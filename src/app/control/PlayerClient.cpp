#include "app/control/PlayerClient.h"

#include "core/Log.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace media {
namespace {

using Json = HttpJsonClient::Json;

/// Read a field only when it is present and of the expected type. A Player
/// from a different build must degrade to defaults rather than throw: the
/// Controller is supposed to survive a version mismatch, not crash on it.
template <typename T>
void readIf(const Json& source, const char* key, T& target) {
	if (!source.contains(key)) {
		return;
	}
	const Json& value = source[key];
	if (value.is_null()) {
		return;
	}
	try {
		target = value.get<T>();
	} catch (const std::exception&) {
		// Wrong type: keep the default. Not worth failing the whole poll.
	}
}

/// Parse a `/api/status` object (or the `status` object embedded in a command
/// reply) into the bar's snapshot.
ControllerState parseStatus(const Json& status) {
	ControllerState state;
	state.online = true;
	readIf(status, "loaded", state.loaded);
	readIf(status, "playing", state.playing);
	readIf(status, "paused", state.paused);
	readIf(status, "isImage", state.isImage);
	readIf(status, "seekable", state.seekable);
	readIf(status, "clipCount", state.clipCount);
	readIf(status, "clipName", state.clipName);
	readIf(status, "position", state.position);
	readIf(status, "duration", state.duration);
	readIf(status, "speed", state.speed);
	readIf(status, "volume", state.volume);
	readIf(status, "subtitlesEnabled", state.subtitlesEnabled);
	readIf(status, "hudVisible", state.hudVisible);
	readIf(status, "fullscreen", state.fullscreen);
	// Where the playlist is being read from. The Controller draws this, and the
	// Dashboard compares it against the setting in mediaplayer.ini, so it comes
	// from the Player rather than from either client's own config copy.
	readIf(status, "mediaFolder", state.mediaFolder);
	// How many clips that folder holds. clipCount is the same number today, but
	// it is the *playlist* count, which could legitimately drift from the
	// corpus on disk; the corpus field means the latter.
	readIf(status, "corpusClipCount", state.corpusClipCount);
	if (state.corpusClipCount == 0) {
		state.corpusClipCount = static_cast<std::size_t>(
			state.clipCount > 0 ? state.clipCount : 0);
	}

	// clipIndex is triply awkward: it is unsigned in the contract, but a
	// negative value must not wrap around into a huge playlist position.
	if (status.contains("clipIndex") && status["clipIndex"].is_number()) {
		try {
			const long long raw = status["clipIndex"].get<long long>();
			state.clipIndex = raw > 0 ? static_cast<std::size_t>(raw) : 0;
		} catch (const std::exception&) {
			state.clipIndex = 0;
		}
	}
	state.lastError.clear();
	return state;
}

/// Prefer the `status` object from a command reply; fall back to the reply
/// itself, which is what GET /api/status returns.
const Json& statusObject(const Json& reply) {
	if (reply.is_object() && reply.contains("status") && reply["status"].is_object()) {
		return reply["status"];
	}
	return reply;
}

/// A Player answering `{"ok": false, "error": "..."}` refused the command.
bool refused(const Json& reply, std::string& error) {
	if (!reply.is_object() || !reply.contains("ok") || !reply["ok"].is_boolean()) {
		return false;
	}
	if (reply["ok"].get<bool>()) {
		return false;
	}
	readIf(reply, "error", error);
	if (error.empty()) {
		error = "player refused the command";
	}
	return true;
}

} // namespace

PlayerRoute PlayerCommands::routeFor(ControlCommand command, double percent) {
	PlayerRoute route;
	route.isPost = true;
	switch (command) {
		case ControlCommand::None:
			return route;                      // invalid: nothing to send
		case ControlCommand::Previous:
			route.path = "/api/previous";
			break;
		case ControlCommand::PlayPause:
			// No "paused" field means "toggle" to the Player, which is exactly
			// what the bar's play/pause button wants.
			route.path = "/api/pause";
			break;
		case ControlCommand::Stop:
			route.path = "/api/stop";
			break;
		case ControlCommand::Next:
			route.path = "/api/next";
			break;
		case ControlCommand::ToggleHud:
			route.path = "/api/hud";
			break;
		case ControlCommand::ToggleFullscreen:
			route.path = "/api/fullscreen";
			break;
		case ControlCommand::ToggleSubtitles:
			route.path = "/api/subtitles";
			break;
	}
	route.valid = true;
	route.body = Json::object();
	(void)percent;   // Seek is not an enum member: it carries a value and has
	                 // its own method (seekPercent).
	return route;
}

bool PlayerCommands::send(ControlCommand command, double percent, std::string& error) {
	if (command == ControlCommand::None) {
		error = "unknown command";
		return false;
	}
	const ControllerState current = state();
	switch (command) {
		case ControlCommand::Previous: return previous(error);
		case ControlCommand::Next: return next(error);
		case ControlCommand::Stop: return stop(error);
		case ControlCommand::PlayPause: return playPause(error);
		case ControlCommand::ToggleHud:
			return setHud(!current.hudVisible, error);
		case ControlCommand::ToggleFullscreen:
			return setFullscreen(!current.fullscreen, error);
		case ControlCommand::ToggleSubtitles:
			return setSubtitles(!current.subtitlesEnabled, error);
		case ControlCommand::None:
			break;
	}
	(void)percent;
	error = "unknown command";
	return false;
}

PlayerClient::PlayerClient(std::string host, int port)
	: client_(std::move(host), port) {}

PlayerClient::~PlayerClient() {
	stop();
}

void PlayerClient::start() {
	if (started_.exchange(true)) {
		return;
	}
	stopping_ = false;
	thread_ = std::thread([this] { pollLoop(); });
}

void PlayerClient::stop() {
	if (!started_.exchange(false)) {
		return;
	}
	stopping_ = true;
	if (thread_.joinable()) {
		thread_.join();
	}
}

ControllerState PlayerClient::state() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return snapshot_;
}

std::vector<PlayerClipInfo> PlayerClient::playlist() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return playlist_;
}

void PlayerClient::pollLoop() {
	while (!stopping_.load()) {
		pollOnce();
		// Sleep in small slices so stop() is not delayed by a long interval.
		const int total = pollIntervalMs_.load();
		for (int slept = 0; slept < total && !stopping_.load(); slept += 20) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
}

void PlayerClient::pollOnce() {
	Json reply;
	std::string error;
	if (!client_.get("/api/status", reply, error)) {
		// One cheap retry against /api/health, so a Player that is up but
		// mid-restart is reported as reachable rather than broken.
		std::string healthError;
		const bool alive = client_.ping(healthError);
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.online = false;
		snapshot_.lastError = alive ? "player is starting up" : error;
		return;
	}

	const ControllerState parsed = parseStatus(statusObject(reply));

	// The playlist is a larger payload than the status poll, so fetch it only
	// when the clip count disagrees with what is cached. A disagreement in
	// either direction counts: a folder change that leaves 0 clips must clear
	// the cached list, or the Controller would keep reporting the previous
	// folder's clips.
	std::vector<PlayerClipInfo> clips;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (parsed.clipCount != playlist_.size()) {
			clips = playlist_;
		}
	}
	if (parsed.clipCount != clips.size()) {
		Json list;
		std::string listError;
		if (client_.get("/api/clips", list, listError) && list.is_array()) {
			clips.clear();
			clips.reserve(list.size());
			for (const Json& entry : list) {
				PlayerClipInfo info;
				readIf(entry, "index", info.index);
				readIf(entry, "name", info.name);
				readIf(entry, "mediaType", info.mediaType);
				clips.push_back(std::move(info));
			}
		}
	}

	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_ = parsed;
	// Always adopt, including an empty list: see above. This assignment is what
	// makes "0 clips" mean 0 clips rather than "unchanged".
	playlist_ = std::move(clips);
}

bool PlayerClient::postAndAdopt(const std::string& path, const Json& body,
	const char* what, std::string& error) {
	Json reply;
	if (!client_.post(path, body, reply, error)) {
		return false;
	}
	if (refused(reply, error)) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_ = parseStatus(statusObject(reply));
	}
	LOG_VERBOSE("PlayerClient") << what << " -> " << path;
	return true;
}

bool PlayerClient::playPause(std::string& error) {
	// No "paused" field means "toggle" to the Player.
	return postAndAdopt("/api/pause", Json::object(), "play-pause", error);
}

bool PlayerClient::next(std::string& error) {
	return postAndAdopt("/api/next", Json::object(), "next", error);
}

bool PlayerClient::previous(std::string& error) {
	return postAndAdopt("/api/previous", Json::object(), "previous", error);
}

bool PlayerClient::stop(std::string& error) {
	return postAndAdopt("/api/stop", Json::object(), "stop", error);
}

bool PlayerClient::openClip(std::size_t index, std::string& error) {
	return postAndAdopt("/api/clips/" + std::to_string(index), Json::object(),
		"open clip", error);
}

bool PlayerClient::seekPercent(double percent, std::string& error) {
	return postAndAdopt("/api/seek", Json{{"percent", percent}}, "seek", error);
}

bool PlayerClient::setVolume(double percent, std::string& error) {
	return postAndAdopt("/api/volume", Json{{"volume", percent}}, "volume", error);
}

bool PlayerClient::setSpeed(double factor, std::string& error) {
	return postAndAdopt("/api/speed", Json{{"speed", factor}}, "speed", error);
}

bool PlayerClient::setHud(bool visible, std::string& error) {
	return postAndAdopt("/api/hud", Json{{"visible", visible}}, "hud", error);
}

bool PlayerClient::setFullscreen(bool visible, std::string& error) {
	return postAndAdopt("/api/fullscreen", Json{{"visible", visible}}, "fullscreen", error);
}

bool PlayerClient::setSubtitles(bool enabled, std::string& error) {
	return postAndAdopt("/api/subtitles", Json{{"enabled", enabled}}, "subtitles", error);
}

bool PlayerClient::rescanClips(std::string& error) {
	Json reply;
	if (!client_.post("/api/clips/rescan", Json::object(), reply, error)) {
		return false;
	}
	if (refused(reply, error)) {
		return false;
	}
	// Adopt the reply itself, not its `status` object: this route answers at
	// the top level, and its mediaFolder/clipCount are exactly what the corpus
	// field needs to redraw with.
	adoptCorpusReply(reply);
	return true;
}

bool PlayerClient::setMediaFolder(const std::string& directory, std::string& error) {
	Json reply;
	if (!client_.post("/api/media-dir", Json{{"path", directory}}, reply, error)) {
		return false;
	}
	if (refused(reply, error)) {
		return false;
	}
	adoptCorpusReply(reply);
	LOG_NOTICE("PlayerClient") << "media folder set to "
		<< (directory.empty() ? std::string("(default)") : directory);
	return true;
}

void PlayerClient::adoptCorpusReply(const Json& reply) {
	// parseStatus on the reply object works because the corpus routes return a
	// superset: every status key plus mediaFolder. statusObject() would unwrap
	// to a status object that lacks mediaFolder on /api/media-dir, which is why
	// this is a separate path.
	const ControllerState parsed = parseStatus(statusObject(reply));
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_ = parsed;
}

} // namespace media
