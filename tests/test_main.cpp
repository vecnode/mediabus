// media_tests - tiny dependency-free assertion runner.
//
// A test framework is not worth a dependency here; this is enough to verify the
// HTTP contract, the playlist logic and the Controller's request mapping, and it
// keeps the binary linkable without a GL context.
//
// Nothing here creates a window, needs a Player, or opens a socket: the
// Controller is exercised through its value types (`PlayerCommands`,
// `ControllerHttpServer::execute`) and the script host through a recording
// stand-in for the Player.

#include "app/HttpControlServer.h"
#include "app/control/ControllerHttpServer.h"
#include "app/control/ControllerModel.h"
#include "app/control/LuaControllerScript.h"
#include "app/control/PlayerClient.h"
#include "app/dashboard/DashboardModel.h"
#include "core/AppConfig.h"
#include "core/Platform.h"
#include "core/UiScale.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"

#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
namespace {

using Json = nlohmann::json;

int gFailures = 0;
int gChecks = 0;
const char* gCurrentTest = "";

void check(bool condition, const std::string& what) {
	++gChecks;
	if (!condition) {
		++gFailures;
		std::printf("  FAIL  [%s] %s\n", gCurrentTest, what.c_str());
	}
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const std::string& what) {
	++gChecks;
	if (!(actual == expected)) {
		++gFailures;
		std::printf("  FAIL  [%s] %s\n", gCurrentTest, what.c_str());
		std::printf("        expected: %s\n", std::to_string(expected).c_str());
		std::printf("        actual  : %s\n", std::to_string(actual).c_str());
	}
}

void checkEqStr(const std::string& actual, const std::string& expected, const std::string& what) {
	++gChecks;
	if (actual != expected) {
		++gFailures;
		std::printf("  FAIL  [%s] %s\n", gCurrentTest, what.c_str());
		std::printf("        expected: \"%s\"\n", expected.c_str());
		std::printf("        actual  : \"%s\"\n", actual.c_str());
	}
}

struct TestCase {
	const char* name;
	std::function<void()> fn;
};

std::vector<TestCase>& registry() {
	static std::vector<TestCase> tests;
	return tests;
}

struct Registrar {
	Registrar(const char* name, std::function<void()> fn) {
		registry().push_back({name, std::move(fn)});
	}
};

#define TEST(name) \
	void name(); \
	const Registrar reg_##name(#name, name); \
	void name()

// ---------------------------------------------------------------------------
// A PlayerCommands that records instead of talking HTTP, so a Lua script can be
// asked what it actually requested.
// ---------------------------------------------------------------------------
class RecordingCommands final : public media::PlayerCommands {
public:
	std::vector<std::string> calls;
	std::vector<media::PlayerClipInfo> clips;
	media::ControllerState snapshot;
	bool refuseEverything = false;
	std::string refusal = "refused by the test";

	bool playPause(std::string& error) override { return record("play-pause", error); }
	bool next(std::string& error) override { return record("next", error); }
	bool previous(std::string& error) override { return record("previous", error); }
	bool stop(std::string& error) override { return record("stop", error); }
	bool openClip(std::size_t index, std::string& error) override {
		return record("open:" + std::to_string(index), error);
	}
	bool seekPercent(double percent, std::string& error) override {
		return record("seek:" + std::to_string(static_cast<int>(percent)), error);
	}
	bool setVolume(double percent, std::string& error) override {
		return record("volume:" + std::to_string(static_cast<int>(percent)), error);
	}
	bool setSpeed(double factor, std::string& error) override {
		return record("speed:" + std::to_string(static_cast<int>(factor * 100)), error);
	}
	bool setHud(bool visible, std::string& error) override {
		return record(visible ? "hud:on" : "hud:off", error);
	}
	bool setFullscreen(bool visible, std::string& error) override {
		return record(visible ? "fullscreen:on" : "fullscreen:off", error);
	}
	bool setSubtitles(bool enabled, std::string& error) override {
		return record(enabled ? "subtitles:on" : "subtitles:off", error);
	}
	std::vector<media::PlayerClipInfo> playlist() const override { return clips; }
	media::ControllerState state() const override { return snapshot; }

	/// True when `needle` was requested.
	bool saw(const std::string& needle) const {
		for (const std::string& call : calls) {
			if (call == needle) {
				return true;
			}
		}
		return false;
	}

	void clear() { calls.clear(); }

private:
	bool record(const std::string& call, std::string& error) {
		calls.push_back(call);
		if (refuseEverything) {
			error = refusal;
			return false;
		}
		error.clear();
		return true;
	}
};

// ---------------------------------------------------------------------------
// A playback backend that records what it was asked to do. Lets the transport
// routes be verified before the real libmpv surface exists.
// ---------------------------------------------------------------------------
class RecordingBackend final : public media::IPlaybackBackend {
public:
	bool open(const media::MediaClip& clip) override {
		if (failNextOpen) {
			failNextOpen = false;
			return false;
		}
		opened = clip.displayName;
		state_ = media::TransportState{};
		state_.loaded = true;
		state_.isImage = clip.mediaType == media::ClipMediaType::Image;
		if (state_.isImage) {
			// A held still has no timeline. MPVSurface reports exactly this
			// (see its state()), so the stub mirrors it rather than being
			// conveniently permissive.
			state_.seekable = false;
			state_.playing = false;
			state_.paused = true;
			state_.position = 0.0;
			state_.duration = 0.0;
		} else {
			state_.duration = 60.0;
			state_.seekable = true;
			state_.position = 0.0;
		}
		return true;
	}
	void close() override { state_ = media::TransportState{}; }
	void play() override { state_.playing = true; state_.paused = false; }
	void pause() override { state_.playing = false; state_.paused = true; }
	void stopToPreview() override { state_.playing = false; state_.paused = true; }
	media::TransportState state() const override { return state_; }
	void seekAbsolute(double seconds) override { lastSeek = seconds; state_.position = seconds; }
	void seekRelative(double seconds) override { lastSeek = seconds; state_.position += seconds; }
	void seekPercent(double percent) override {
		lastSeek = percent;
		state_.position = state_.duration * percent / 100.0;
	}
	void setSpeed(double factor) override { state_.speed = factor; }
	void setVolume(double percent) override { state_.volume = percent; }
	void setSubtitlesEnabled(bool enabled) override { state_.subtitlesEnabled = enabled; }
	void setScripts(const std::vector<media::scripts::ScriptFile>& scripts) override {
		scripts_ = scripts;
	}
	std::vector<std::string> loadedScripts() const override {
		std::vector<std::string> names;
		names.reserve(scripts_.size());
		for (const auto& script : scripts_) {
			names.push_back(script.name);
		}
		return names;
	}

	media::TransportState state_{};
	std::string opened;
	double lastSeek = 0.0;
	bool failNextOpen = false;
	std::vector<media::scripts::ScriptFile> scripts_;
};

/// A controlled media directory for the tests.
///
/// Previously the tests scanned the real bin/data, so the number of assertions
/// changed with whatever media happened to be installed (170 checks with a bare
/// folder, 185 with the codec fixtures present). A suite whose coverage depends
/// on ambient files is not trustworthy. This seeds a fixed set in a temp
/// directory instead, and MediaClipLibrary::setRoot points at it.
struct ScopedDataDir {
	std::filesystem::path root;
	std::vector<std::filesystem::path> created;

	explicit ScopedDataDir(bool makeVideos) {
		// Build the directory next to the executable rather than in the system
		// temp directory: the MinGW runtime's temp path here resolves to a short
		// name that the process is denied access to.
		root = std::filesystem::path(media::platform::executableDirectory())
			/ ("tests-tmp-" + std::to_string(::GetCurrentProcessId()));
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		std::filesystem::create_directories(root);
		std::filesystem::create_directories(root / "scripts");

		// A tiny PNG: enough for the extension-based scan, no decode involved.
		const std::filesystem::path png = root / "zz-test-image.png";
		{
			std::ofstream out(png, std::ios::binary);
			const unsigned char kPng[] = {
				0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
			out.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
			created.push_back(png);
		}
		const std::filesystem::path a = root / "zz-test-a.mp4";
		std::ofstream(a).put('\0');
		created.push_back(a);

		if (makeVideos) {
			const std::filesystem::path b = root / "zz-test-b.mkv";
			std::ofstream(b).put('\0');
			created.push_back(b);
			// A non-media file, to prove the scanner ignores it.
			std::ofstream(root / "notes.txt").put('\0');
		}

		// Seed a script so discovery can be asserted against this directory
		// instead of whatever happens to be installed in bin/data/scripts.
		// scripts::discover() resolves production paths, so the copy is checked
		// through a rooted path below.
		{
			const std::filesystem::path script = root / "scripts" / "test-script.lua";
			std::ofstream(script) << "-- test fixture\n";
			created.push_back(script);
			// A non-script file, to prove the filter works.
			std::ofstream(root / "scripts" / "README.txt") << "not a script\n";
		}
	}

	~ScopedDataDir() {
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}
};

/// Point a library at a controlled directory and scan it.
void scanInto(media::MediaClipLibrary& library, const ScopedDataDir& data) {
	library.setRoot(data.root.string());
	library.scan();
}

/// Drive the server's main-thread queue while an HTTP call is outstanding.
/// The call itself runs on another thread, so the test can keep polling.
template <typename Fn>
auto withServer(media::HttpControlServer& server, Fn&& call) {
	std::atomic<bool> done{false};
	using Result = decltype(call());
	Result result{};
	std::thread worker([&] {
		result = call();
		done = true;
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!done.load() && std::chrono::steady_clock::now() < deadline) {
		server.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	worker.join();
	server.poll();
	return result;
}

} // namespace

// ---------------------------------------------------------------------------
TEST(platform_paths_are_absolute) {
	const std::string exeDir = media::platform::executableDirectory();
	check(!exeDir.empty(), "executableDirectory is not empty");
	check(exeDir.find("data") == std::string::npos, "exe dir does not already end in data");
	check(exeDir.back() == '\\' || exeDir.back() == '/',
		"executableDirectory ends with a separator");

	const std::string dataDir = media::platform::dataDirectory();
	// Regression: a naive concatenation produced "...\bindata" and still
	// resolved by accident, which silently hid the scripts directory.
	check(dataDir.find("bindata") == std::string::npos,
		"data dir does not run the separator into the name");
	check(dataDir.size() > exeDir.size(), "data dir is longer than the exe dir");
	checkEqStr(dataDir.substr(0, exeDir.size()), exeDir, "data dir starts with the exe dir");
	checkEqStr(dataDir.substr(exeDir.size()), "data",
		"data dir appends exactly 'data' (exe dir already ends in a separator)");

	const std::string scriptsDir = media::platform::scriptsDirectory();
	check(scriptsDir.find("datascripts") == std::string::npos,
		"scripts dir does not run the separator into the name");
	checkEqStr(scriptsDir.substr(0, dataDir.size()), dataDir,
		"scripts dir starts with the data dir");
	// dataDirectory() has no trailing separator, so joinPath supplies one; this
	// single separator is what makes the path real rather than "datascripts".
	checkEqStr(scriptsDir.substr(dataDir.size()), "\\scripts",
		"scripts dir appends a separator then 'scripts'");

	checkEqStr(media::platform::lowerExtension("C:/a/B.MP4"), ".mp4", "extension lowercased");
	checkEqStr(media::platform::lowerExtension("noext"), "", "no extension yields empty");
	checkEqStr(media::platform::lowerExtension("dir.with.dots/file"), "", "dot in dir is not an extension");
}

TEST(script_host_filters_and_reports_consistently) {
	// Non-script extensions must never be runnable.
	check(media::scripts::isScriptPath("a.lua"), ".lua is a script");
	check(media::scripts::isScriptPath("a.JS"), ".JS is a script (case-insensitive)");
	check(!media::scripts::isScriptPath("a.txt"), ".txt is not a script");
	check(!media::scripts::isScriptPath("a.luac"), ".luac is not loaded");
	check(!media::scripts::isScriptPath(""), "empty path is not a script");

	// discover() resolves the executable's data directory, which the build
	// populates. Assert properties that hold for whatever is there rather than a
	// specific filename, so the suite does not depend on ambient state.
	const std::vector<media::scripts::ScriptFile> found = media::scripts::discover();
	std::printf("        (discovered %zu script(s) in the data directory)\n", found.size());
	for (const auto& script : found) {
		check(!script.absolutePath.empty(), "script path populated");
		check(!script.name.empty(), "script name populated");
		check(script.language == "lua" || script.language == "js",
			"discovered script has a classified language");
		check(media::scripts::isScriptPath(script.absolutePath),
			"every discovered script has a script extension");
		// Regression: a missing path separator once produced "datascripts".
		check(script.absolutePath.find("datascripts") == std::string::npos,
			"script path has a real separator before 'scripts'");
	}
}

/// Images and video share the mpv surface, but they must not share transport
/// semantics: a held still has no timeline. These checks pin that down, because
/// the tempting shortcut (mark everything seekable) would make a host's seek
/// call look like it worked on an image.
TEST(image_clips_have_no_transport) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);

	RecordingBackend backend;
	media::MediaPlayerController controller(library, &backend);
	check(controller.setup(), "setup opens the first clip");

	// Images sort before videos, so whatever media happens to be in bin/data,
	// the first clip must be an image and all images must precede all videos.
	const media::MediaPlayerStatus image = controller.getStatus();
	check(image.isImage, "the first clip is an image");
	check(image.loaded, "the image counts as loaded");
	check(!image.playing, "a held image is never 'playing'");
	check(!image.seekable, "a held image is not seekable");
	checkEq(image.duration, 0.0, "a held image has no duration");
	checkEq(image.position, 0.0, "a held image has no position");

	// The image is still a navigable playlist entry.
	const std::size_t imageIndex = image.clipIndex;
	controller.nextClip();
	const media::MediaPlayerStatus next = controller.getStatus();
	check(next.clipIndex != imageIndex, "nextClip moves off the image");
	check(!next.clipName.empty(), "the next clip has a name");
}

TEST(playlist_mixes_images_and_videos_in_a_stable_order) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);
	check(library.size() >= 3, "seeded three clips");

	// Images before videos, then by path: this ordering is what makes index 0
	// predictable for a controller that does not want to inspect media types.
	bool seenVideo = false;
	for (std::size_t i = 0; i < library.size(); ++i) {
		const media::ClipMediaType type = library.clipAt(i).mediaType;
		if (type == media::ClipMediaType::Video) {
			seenVideo = true;
		} else if (seenVideo) {
			check(false, "an image appeared after a video, breaking the ordering");
			break;
		}
		// Every discovered clip must be a media file the scanner recognises.
		// This list must stay a superset of MediaClipLibrary's kVideoExtensions
		// and kImageExtensions: when it lags, the test fails on a format the
		// product actually supports (which is how .avi and .mkv got caught).
		const std::string ext = media::platform::lowerExtension(library.clipAt(i).absolutePath);
		check(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"
				|| ext == ".gif" || ext == ".webp" || ext == ".tif" || ext == ".tiff"
				|| ext == ".mp4" || ext == ".mkv" || ext == ".mov" || ext == ".webm"
				|| ext == ".avi" || ext == ".m4v" || ext == ".mpg" || ext == ".mpeg"
				|| ext == ".wmv" || ext == ".flv" || ext == ".ts",
			"clip has a recognised media extension: " + ext);
	}
	check(seenVideo, "at least one video is present");
}

TEST(controller_forwards_scripts_to_the_backend) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);
	RecordingBackend backend;
	media::MediaPlayerController controller(library, &backend);

	// Mirror how main() wires this: discover, hand to the controller, then set
	// up. The controller must both forward them to the backend and keep the
	// list so /api/scripts can report what is on disk.
	const std::vector<media::scripts::ScriptFile> discovered = media::scripts::discover();
	controller.setScripts(discovered);

	// The controller's own list is what /api/scripts reports as "onDisk"; an
	// earlier wiring bug left it empty while the backend had loaded the script.
	checkEq(controller.scriptFiles().size(), discovered.size(),
		"controller retains the script list it was given");
	check(!controller.scriptFiles().empty(), "controller script list is populated");

	check(controller.setup(), "setup succeeds with scripts");

	// The backend must have been handed the scripts before opening a clip,
	// because mpv only accepts them pre-initialization.
	check(!backend.scripts_.empty(), "backend received the discovered scripts");
	checkEq(backend.scripts_.size(), discovered.size(),
		"backend received every discovered script");

	const media::MediaPlayerStatus status = controller.getStatus();
	// scriptsLoaded mirrors whatever discover() found, so assert the two agree
	// rather than naming a file that may not be installed.
	checkEq(status.scriptsLoaded.size(), controller.scriptFiles().size(),
		"loaded scripts mirror the discovered list");
	checkEq(controller.loadedScripts().size(), controller.scriptFiles().size(),
		"loaded count matches the on-disk count");
}

TEST(clip_library_orders_images_first_and_dedups) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);

	// The scanner must ignore non-media files even though they sit in the root.
	checkEq(library.size(), std::size_t{3}, "found exactly the three seeded clips");
	check(library.size() > 0 && library.clipAt(0).mediaType == media::ClipMediaType::Image,
		"images sort before videos");

	bool sawVideo = false;
	for (std::size_t i = 0; i < library.size(); ++i) {
		if (library.clipAt(i).mediaType == media::ClipMediaType::Video) {
			sawVideo = true;
		}
		check(!library.clipAt(i).absolutePath.empty(), "clip path populated");
		check(!library.clipAt(i).displayName.empty(), "clip name populated");
	}
	check(sawVideo, "found at least one video");

	// Cyclic navigation.
	checkEq(library.nextIndex(library.size() - 1), std::size_t{0}, "next wraps to 0");
	checkEq(library.previousIndex(0), library.size() - 1, "previous wraps to last");
	checkEq(library.nextIndex(0), std::size_t{1}, "next advances");

	std::size_t found = 999;
	check(library.indexForName("zz-test-image.png", found), "lookup by name works");
	check(found < library.size(), "lookup index in range");
	check(!library.indexForName("nope.png", found), "unknown name is rejected");
}

TEST(controller_transport_state) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);

	RecordingBackend backend;
	media::MediaPlayerController controller(library, &backend);
	check(controller.setup(), "controller setup opens the first clip");

	const media::MediaPlayerStatus first = controller.getStatus();
	check(first.loaded, "status reports loaded");
	checkEq(first.clipCount, library.size(), "status reports the clip count");
	checkEq(first.clipIndex, std::size_t{0}, "starts at index 0");
	check(!first.clipName.empty(), "status carries the clip name");

	const std::size_t before = first.clipIndex;
	controller.nextClip();
	const media::MediaPlayerStatus after = controller.getStatus();
	check(after.clipIndex != before, "nextClip moved off the first clip");

	// Index round trip.
	check(controller.openClipAtIndex(0), "openClipAtIndex(0) succeeds");
	checkEq(controller.getStatus().clipIndex, std::size_t{0}, "index round trips");
	check(!controller.openClipAtIndex(99999), "out-of-range index is rejected");

	// A backend that refuses to load must not change the current clip.
	controller.openClipAtIndex(1);
	const std::size_t good = controller.getStatus().clipIndex;
	backend.failNextOpen = true;
	check(!controller.openClipAtIndex(0), "refused open reports failure");
	checkEq(controller.getStatus().clipIndex, good, "refused open leaves the clip unchanged");

	// Transport. Do this on a VIDEO clip explicitly: play/stop/seek are no-ops
	// on a held image (by design), so depending on which clip happens to be
	// current would make this test depend on the contents of bin/data.
	std::size_t videoIndex = library.size();
	for (std::size_t i = 0; i < library.size(); ++i) {
		if (library.clipAt(i).mediaType == media::ClipMediaType::Video) {
			videoIndex = i;
			break;
		}
	}
	check(videoIndex < library.size(), "a video clip is available for transport tests");
	check(controller.openClipAtIndex(videoIndex), "opened the video clip");

	controller.play();
	check(controller.getStatus().playing, "play sets playing");
	controller.stop();
	check(!controller.getStatus().playing, "stop clears playing");
	check(controller.seekAbsolute(12.5), "seekAbsolute accepted");
	checkEq(backend.lastSeek, 12.5, "seek value reached the backend");
	check(controller.seekPercent(50.0), "seekPercent accepted");
	check(!controller.seekPercent(500.0), "seekPercent out of range rejected");
	check(controller.setSpeed(2.0), "setSpeed accepted");
	check(!controller.setSpeed(0.0), "setSpeed zero rejected");
	check(controller.setVolume(30.0), "setVolume accepted");
	check(!controller.setVolume(101.0), "setVolume out of range rejected");

	controller.setSubtitlesEnabled(false);
	check(!controller.getStatus().subtitlesEnabled, "subtitles can be disabled");
	controller.setSubtitleText("hello");
	checkEqStr(controller.getSubtitleText(), "hello", "subtitle override applied");
	controller.clearSubtitleOverride();
	checkEqStr(controller.getSubtitleText(), "", "subtitle override cleared");

	const auto clips = controller.getClips();
	checkEq(clips.size(), library.size(), "clip info list matches the library");
	if (!clips.empty()) {
		check(!clips[0].mediaType.empty(), "clip info carries a media type");
		checkEqStr(clips[0].mediaType, "image", "first clip is an image here");
	}
}

TEST(http_contract_shape) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);

	RecordingBackend backend;
	media::MediaPlayerController controller(library, &backend);
	controller.setup();

	media::HttpControlServer server(controller);
	// 18080 keeps the test off the production port so both can run at once.
	check(server.start(18080), "server binds port 18080");

	httplib::Client client("127.0.0.1", 18080);
	client.set_connection_timeout(5, 0);
	client.set_read_timeout(5, 0);

	// --- health ---
	const auto health = withServer(server, [&] { return client.Get("/api/health"); });
	check(health && health->status == 200, "GET /api/health is 200");
	if (health) {
		const Json body = Json::parse(health->body);
		check(body.contains("ok") && body["ok"].get<bool>(), "health reports ok");
	}

	// --- status: the frozen keys must all be present with the right types ---
	const auto status = withServer(server, [&] { return client.Get("/api/status"); });
	check(status && status->status == 200, "GET /api/status is 200");
	if (status) {
		const Json body = Json::parse(status->body);
		const char* required[] = {"loaded", "playing", "isImage", "clipIndex",
			"clipCount", "clipName", "subtitlesEnabled", "subtitleText"};
		for (const char* key : required) {
			check(body.contains(key), std::string("status has frozen key '") + key + "'");
		}
		check(body["loaded"].is_boolean(), "loaded is boolean");
		check(body["playing"].is_boolean(), "playing is boolean");
		check(body["isImage"].is_boolean(), "isImage is boolean");
		check(body["clipIndex"].is_number_unsigned() || body["clipIndex"].is_number_integer(),
			"clipIndex is an integer");
		check(body["clipCount"].is_number_unsigned() || body["clipCount"].is_number_integer(),
			"clipCount is an integer");
		check(body["clipName"].is_string(), "clipName is a string");
		check(body["subtitlesEnabled"].is_boolean(), "subtitlesEnabled is boolean");
		check(body["subtitleText"].is_string(), "subtitleText is a string");
		// additive keys
		check(body.contains("position"), "status has additive 'position'");
		check(body.contains("duration"), "status has additive 'duration'");
		check(body.contains("decoder"), "status has additive 'decoder'");
	}

	// --- clips array ---
	const auto clips = withServer(server, [&] { return client.Get("/api/clips"); });
	check(clips && clips->status == 200, "GET /api/clips is 200");
	if (clips) {
		const Json body = Json::parse(clips->body);
		check(body.is_array(), "clips payload is an array");
		checkEq(body.size(), library.size(), "clips length matches the library");
		if (!body.empty()) {
			for (const char* key : {"index", "name", "path", "mediaType"}) {
				check(body[0].contains(key), std::string("clip entry has '") + key + "'");
			}
		}
	}

	// --- transport routes answer with ok + status ---
	const auto next = withServer(server, [&] { return client.Post("/api/next", "", "application/json"); });
	check(next && next->status == 200, "POST /api/next is 200");
	if (next) {
		const Json body = Json::parse(next->body);
		check(body.contains("ok") && body["ok"].get<bool>(), "next reports ok");
		check(body.contains("status"), "next embeds status");
	}

	// --- seek validation ---
	const auto badSeek = withServer(server, [&] {
		return client.Post("/api/seek", "{\"nonsense\": 1}", "application/json");
	});
	check(badSeek && badSeek->status == 200, "seek with unknown field is handled");
	if (badSeek) {
		const Json body = Json::parse(badSeek->body);
		check(body.contains("error"), "unknown seek field reports an error");
	}

	const auto goodSeek = withServer(server, [&] {
		return client.Post("/api/seek", "{\"time\": 5.0}", "application/json");
	});
	check(goodSeek && goodSeek->status == 200, "seek with time is 200");
	if (goodSeek) {
		const Json body = Json::parse(goodSeek->body);
		check(body["ok"].get<bool>(), "seek reports ok");
	}
	checkEq(backend.lastSeek, 5.0, "seek reached the backend");

	const auto malformed = withServer(server, [&] {
		return client.Post("/api/seek", "{not json", "application/json");
	});
	check(malformed && malformed->status == 400, "malformed JSON body is 400");

	// --- subtitles: the documented request shape ---
	const auto subs = withServer(server, [&] {
		return client.Post("/api/subtitles", "{\"enabled\": true}", "application/json");
	});
	check(subs && subs->status == 200, "POST /api/subtitles is 200");
	if (subs) {
		const Json body = Json::parse(subs->body);
		check(body.contains("subtitlesEnabled"), "subtitle reply carries enabled flag");
		check(body.contains("subtitleText"), "subtitle reply carries text");
	}

	const auto badSubs = withServer(server, [&] {
		return client.Post("/api/subtitles", "{\"enabled\": \"yes\"}", "application/json");
	});
	check(badSubs && badSubs->status == 200, "non-boolean enabled is handled");
	if (badSubs) {
		const Json body = Json::parse(badSubs->body);
		check(body.contains("error"), "non-boolean enabled reports an error");
	}

	// --- clip index route, including the regex capture ---
	const auto openZero = withServer(server, [&] {
		return client.Post("/api/clips/0", "", "application/json");
	});
	check(openZero && openZero->status == 200, "POST /api/clips/0 is 200");
	if (openZero) {
		check(Json::parse(openZero->body)["ok"].get<bool>(), "clip open reports ok");
	}

	const auto openTooBig = withServer(server, [&] {
		return client.Post("/api/clips/99999", "", "application/json");
	});
	check(openTooBig && openTooBig->status == 200, "out-of-range clip returns a body");
	if (openTooBig) {
		check(Json::parse(openTooBig->body).contains("error"),
			"out-of-range clip reports an error");
	}

	// --- unknown route is 404 ---
	const auto missing = withServer(server, [&] { return client.Get("/api/nope"); });
	check(missing && missing->status == 404, "unknown endpoint is 404");

	server.stop();
	check(!server.isRunning(), "server reports stopped");
}

TEST(http_position_route) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);
	RecordingBackend backend;
	media::MediaPlayerController controller(library, &backend);
	controller.setup();

	media::HttpControlServer server(controller);
	check(server.start(18081), "server binds port 18081");

	httplib::Client client("127.0.0.1", 18081);
	client.set_read_timeout(5, 0);
	client.set_connection_timeout(5, 0);

	const auto pos = withServer(server, [&] { return client.Get("/api/position"); });
	check(pos && pos->status == 200, "GET /api/position is 200");
	if (pos) {
		const Json body = Json::parse(pos->body);
		for (const char* key : {"position", "duration", "seekable", "speed", "paused"}) {
			check(body.contains(key), std::string("position payload has '") + key + "'");
		}
	}

	const auto pause = withServer(server, [&] {
		return client.Post("/api/pause", "{\"paused\": true}", "application/json");
	});
	check(pause && pause->status == 200, "POST /api/pause is 200");
	check(controller.getStatus().paused, "pause reached the controller");

	const auto speed = withServer(server, [&] {
		return client.Post("/api/speed", "{\"speed\": 1.5}", "application/json");
	});
	check(speed && speed->status == 200, "POST /api/speed is 200");
	checkEq(backend.state_.speed, 1.5, "speed reached the backend");

	const auto volume = withServer(server, [&] {
		return client.Post("/api/volume", "{\"volume\": 40}", "application/json");
	});
	check(volume && volume->status == 200, "POST /api/volume is 200");
	checkEq(backend.state_.volume, 40.0, "volume reached the backend");

	server.stop();
}

TEST(add_clip_path_rejects_outside_data_dir) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	scanInto(library, data);
	media::MediaPlayerController controller(library, nullptr);
	media::HttpControlServer server(controller);

	std::string error;
	// Escaping the data root must be refused: this route must never let a
	// request point the player at an arbitrary file.
	check(!server.addClipPath(media::platform::dataDirectory() + "\\..\\..\\Windows\\notepad.exe", &error),
		"path escaping the data dir is rejected");
	check(!error.empty(), "rejection explains itself");

	check(!server.addClipPath("C:\\Windows\\notepad.exe", &error),
		"absolute path outside the data dir is rejected");

	check(!server.addClipPath("", &error), "empty path rejected");

	check(!server.addClipPath(media::platform::dataDirectory() + "\\missing.mp4", &error),
		"non-existent file rejected");

	check(!server.addClipPath(media::platform::dataDirectory() + "\\notes.txt", &error),
		"non-media extension rejected");

	// The acceptance case needs a real file inside the PRODUCTION data
	// directory, because HttpControlServer::addClipPath resolves its containment
	// boundary from platform::dataDirectory() rather than from the library under
	// test. Create one and remove it again so the test is self-contained.
	const std::string probe = media::platform::dataDirectory() + "\\zz-containment-probe.mp4";
	{
		std::ofstream out(probe, std::ios::binary);
		out.put('\0');
	}
	check(server.addClipPath(probe, &error),
		"in-root media file accepted");
	std::error_code cleanup;
	std::filesystem::remove(probe, cleanup);

	// And the same path once removed must be refused, proving the check is
	// about the file's existence and location rather than its name.
	check(!server.addClipPath(probe, &error),
		"the probe file is rejected once deleted");
}

// ---------------------------------------------------------------------------
// Controller: the command -> route table.
//
// This is the guard against the two applications drifting apart: every command
// the bar can issue must map to a route the Player documents.
// ---------------------------------------------------------------------------
TEST(controller_command_routes_match_the_players_api) {
	const media::ControlCommand commands[] = {
		media::ControlCommand::Previous,
		media::ControlCommand::PlayPause,
		media::ControlCommand::Stop,
		media::ControlCommand::Next,
		media::ControlCommand::ToggleHud,
		media::ControlCommand::ToggleFullscreen,
		media::ControlCommand::ToggleSubtitles,
	};
	// The routes PlayerClient.cpp is allowed to speak. A new command that is
	// not in this list is a new dependency on the Player, and has to be
	// deliberate.
	const char* documented[] = {
		"/api/play", "/api/stop", "/api/next", "/api/previous", "/api/pause",
		"/api/seek", "/api/speed", "/api/volume", "/api/subtitles", "/api/hud",
		"/api/fullscreen", "/api/clips/rescan", "/api/status", "/api/clips",
		"/api/position", "/api/health", "/api/scripts",
	};

	for (const media::ControlCommand command : commands) {
		const media::PlayerRoute route = media::PlayerCommands::routeFor(command, 0.0);
		check(route.valid, std::string("route exists for ") + media::toString(command));
		check(route.isPost, std::string("route is a POST for ") + media::toString(command));
		check(!route.path.empty(), std::string("route has a path for ") + media::toString(command));
		check(route.body.is_object(),
			std::string("route carries a JSON object for ") + media::toString(command));

		bool known = false;
		for (const char* candidate : documented) {
			if (route.path == candidate) {
				known = true;
				break;
			}
		}
		check(known, std::string("documented route for ") + media::toString(command)
			+ ": " + route.path);
	}

	// None is not a command: it must never produce a request.
	check(!media::PlayerCommands::routeFor(media::ControlCommand::None, 0.0).valid,
		"ControlCommand::None maps to no route");
}

TEST(controller_forwards_every_command_to_the_player) {
	RecordingCommands recorder;
	media::ControllerModel model;
	media::ControllerHost host;
	host.player = &recorder;
	host.model = &model;

	std::string error;
	const auto send = [&](media::ControlCommand command, double value) {
		recorder.clear();
		media::ControllerHttpServer::Request request;
		request.kind = media::ControllerHttpServer::Request::Kind::Control;
		request.command = command;
		request.value = value;
		const Json reply = media::ControllerHttpServer::execute(host, 8081, true, request);
		checkEq(reply["ok"].get<bool>(), true,
			std::string("controller accepted ") + media::toString(command));
		return recorder.calls;
	};

	check(send(media::ControlCommand::Next, 0.0).at(0) == "next", "next reaches the player");
	check(send(media::ControlCommand::Previous, 0.0).at(0) == "previous",
		"previous reaches the player");
	check(send(media::ControlCommand::Stop, 0.0).at(0) == "stop", "stop reaches the player");
	check(send(media::ControlCommand::PlayPause, 0.0).at(0) == "play-pause",
		"play/pause reaches the player");

	// The toggles resolve against the last snapshot: the wire protocol carries
	// booleans, so a toggle has to become an absolute value somewhere.
	model.applyState(media::ControllerState{});
	recorder.snapshot.hudVisible = true;
	check(send(media::ControlCommand::ToggleHud, 0.0).at(0) == "hud:off",
		"hiding a visible HUD sends visible=false");
	recorder.snapshot.fullscreen = false;
	check(send(media::ControlCommand::ToggleFullscreen, 0.0).at(0) == "fullscreen:on",
		"toggling fullscreen from off sends visible=true");
	recorder.snapshot.subtitlesEnabled = true;
	check(send(media::ControlCommand::ToggleSubtitles, 0.0).at(0) == "subtitles:off",
		"toggling subtitles from on sends enabled=false");

	// A refusal must surface as an error object rather than a silent success.
	recorder.refuseEverything = true;
	recorder.clear();
	media::ControllerHttpServer::Request request;
	request.kind = media::ControllerHttpServer::Request::Kind::Control;
	request.command = media::ControlCommand::Next;
	const Json refused = media::ControllerHttpServer::execute(host, 8081, true, request);
	checkEq(refused["ok"].get<bool>(), false, "a refused command reports not-ok");
	checkEqStr(refused["error"].get<std::string>(), recorder.refusal,
		"a refused command carries the player's reason");
	(void)error;
}

TEST(controller_script_routes_report_when_scripting_is_unavailable) {
	RecordingCommands recorder;
	media::ControllerModel model;
	media::ControllerHost host;      // scripts == nullptr: Lua failed to start
	host.player = &recorder;
	host.model = &model;

	media::ControllerHttpServer::Request request;
	request.kind = media::ControllerHttpServer::Request::Kind::RunSource;
	request.text = "controller.Next()";

	const Json reply = media::ControllerHttpServer::execute(host, 8081, true, request);
	checkEq(reply["ok"].get<bool>(), false, "running a script without Lua reports failure");
	check(reply["error"].get<std::string>().find("scripting") != std::string::npos,
		"the failure names scripting as the cause");
	check(recorder.calls.empty(), "a refused script runs nothing");
}

TEST(controller_script_names_stay_inside_the_script_directory) {
	// The route takes a *name*, and this resolver is the whole guard: it is
	// what keeps "..\..\evil.lua" from being opened, and also what makes a
	// script that discovery listed actually loadable. Opening the bare name
	// resolved it against the process working directory instead of the data
	// directory, so every script the API listed failed to run.
	check(media::findControllerScript("").empty(), "an empty name resolves to nothing");
	check(media::findControllerScript("..\\..\\evil.lua").empty(),
		"a backslash traversal is refused");
	check(media::findControllerScript("../../evil.lua").empty(),
		"a forward-slash traversal is refused");
	check(media::findControllerScript("sub\\evil.lua").empty(),
		"a name with a directory component is refused");
	check(media::findControllerScript("C:\\Windows\\evil.lua").empty(),
		"an absolute path is refused");
	check(media::findControllerScript("no-such-script.lua").empty(),
		"an unknown name resolves to nothing");

	// The positive case: the example script the build installs is found by the
	// name GET /api/controller/scripts reports. Skipped (not failed) in a tree
	// that has never been built, since the file is a build output.
	const std::string found = media::findControllerScript("controller-example.lua");
	if (found.empty()) {
		check(true, "controller-example.lua is not installed here; positive case skipped");
	} else {
		check(std::filesystem::exists(found),
			"a listed script name resolves to a file that exists");
	}
}

TEST(controller_script_route_refuses_a_name_it_cannot_resolve) {
	RecordingCommands recorder;
	media::ControllerModel model;
	media::LuaControllerScript scripts;
	if (!scripts.initialize()) {
		check(false, "Lua should be available in a build with the Controller enabled");
		return;
	}

	media::ControllerHost host;
	host.player = &recorder;
	host.model = &model;
	host.scripts = &scripts;

	media::ControllerHttpServer::Request request;
	request.kind = media::ControllerHttpServer::Request::Kind::RunScript;
	request.text = "..\\..\\media_tests.exe";

	const Json reply = media::ControllerHttpServer::execute(host, 8081, true, request);
	checkEq(reply["ok"].get<bool>(), false,
		"running a path outside the script directory reports failure");
	check(reply["error"].get<std::string>().find("no such script") != std::string::npos,
		"the failure says the script was not found");
	check(!scripts.scriptRunning(), "a refused script is not running");
	check(recorder.calls.empty(), "a refused script issues no player commands");
}

TEST(controller_script_reloads_from_disk_and_survives_a_broken_edit) {
	// The Controller's host owns its Lua state, so — unlike the Player's mpv
	// scripts — a reload is a real reload. That is what the R key and the
	// reload route rely on, and what the frame loop's change check calls.
	media::LuaControllerScript scripts;
	if (!scripts.initialize()) {
		check(false, "Lua should be available in a build with the Controller enabled");
		return;
	}
	RecordingCommands recorder;
	scripts.setCommands(&recorder);

	// The production script directory, exactly as the containment test uses the
	// production data directory: the resolver and the reload path both resolve
	// against platform::dataDirectory(), so a temp directory would test nothing.
	const std::string dir = media::platform::dataDirectory() + "\\controller-scripts";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const std::string path = dir + "\\zz-reload-test.lua";

	const auto write = [&path](const std::string& source) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << source;
	};

	write("controller.Log('v1')\n");
	std::string error;
	check(scripts.runFile(path, error), "the test script loads");
	checkEqStr(scripts.lastLog(), "v1", "the first version ran");

	// Nothing touched it, so the change check must be a no-op rather than a
	// reload: the frame loop calls this every half second.
	check(!scripts.reloadIfChanged(), "an unmodified script is not reloaded");

	// A save in the same second is caught by the size, because
	// last_write_time has 1-second granularity here (see fileStamp). This
	// edit is deliberately longer than the one before it.
	write("controller.Log('version two')\n");
	check(scripts.reloadIfChanged(), "a changed script is reloaded in place");
	checkEqStr(scripts.lastLog(), "version two", "the new version ran");

	// A same-length save in the same second is only visible through the
	// timestamp, so this leg waits for the clock rather than for the code.
	// (The body below is exactly as long as the one above, on purpose.)
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	write("controller.Log('version 3!!')\n");
	check(scripts.reloadIfChanged(), "a same-length script is caught by the timestamp");
	checkEqStr(scripts.lastLog(), "version 3!!", "the same-length version ran");

	// A broken edit must be reported and must not leave a half-loaded script
	// pretending to run.
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	write("this is not lua\n");
	check(!scripts.reloadIfChanged(), "a broken edit is not reported as a reload");
	check(!scripts.lastError().empty(), "a broken edit records why it failed");

	// Remove the probe file but keep the directory: the build installs the
	// example script there, and a test must not delete a build output.
	std::filesystem::remove(path, ec);
}

TEST(controller_command_names_round_trip) {
	const media::ControlCommand commands[] = {
		media::ControlCommand::Previous,
		media::ControlCommand::PlayPause,
		media::ControlCommand::Stop,
		media::ControlCommand::Next,
		media::ControlCommand::ToggleHud,
		media::ControlCommand::ToggleFullscreen,
		media::ControlCommand::ToggleSubtitles,
	};
	for (const media::ControlCommand command : commands) {
		// toString is the wire spelling, and commandFromName is its inverse.
		check(media::ControllerHttpServer::commandFromName(media::toString(command)) == command,
			std::string("name round-trips for ") + media::toString(command));
	}
	check(media::ControllerHttpServer::commandFromName("nonsense") == media::ControlCommand::None,
		"an unknown command name is rejected");
	check(media::ControllerHttpServer::commandFromName("pause") == media::ControlCommand::PlayPause,
		"the 'pause' alias resolves to play/pause");
}

// ---------------------------------------------------------------------------
// Controller: layout and hit testing. Pure geometry, no window.
// ---------------------------------------------------------------------------
TEST(controller_layout_fits_the_bar_and_buttons_do_not_overlap) {
	media::ControllerModel model;
	const float widths[] = {480.0f, 720.0f, 1200.0f, 1920.0f};
	for (const float width : widths) {
		model.layout(width, static_cast<float>(media::ControllerModel::kDefaultHeight));
		const media::ControllerLayout& layout = model.layout();

		check(!layout.buttons.empty(), "there are transport buttons");
		check(layout.seekBar.w > 0.0f, "the seek bar has width");

		// Every button stays inside the window and clear of the one before it.
		float previousRight = -1.0f;
		for (const media::ControlButton& button : layout.buttons) {
			check(button.rect.x >= 0.0f, "a button starts inside the bar");
			check(button.rect.x + button.rect.w <= width + 0.5f,
				"a button ends inside the bar");
			check(button.rect.y + button.rect.h <= media::ControllerModel::kDefaultHeight + 0.5f,
				"a button ends above the bottom edge");
			check(button.rect.x >= previousRight - 0.5f, "buttons do not overlap");
			previousRight = button.rect.x + button.rect.w;
		}

		// The seek bar must not sit on top of the buttons, or a click meant for
		// one would scrub instead.
		for (const media::ControlButton& button : layout.buttons) {
			check(!layout.seekBar.hit(button.rect.centreX(), button.rect.centreY()),
				"the seek bar does not overlap the transport row");
		}
	}
}

TEST(controller_hit_test_returns_the_button_under_the_pointer) {
	media::ControllerModel model;
	model.layout(static_cast<float>(media::ControllerModel::kDefaultWidth),
		static_cast<float>(media::ControllerModel::kDefaultHeight));

	for (const media::ControlButton& button : model.layout().buttons) {
		const media::ControlCommand hit = model.hitTest(button.rect.centreX(),
			button.rect.centreY());
		check(hit == button.command,
			std::string("centre of '") + button.label + "' hits its own command");
	}

	// Outside every button, including the padding around the bar.
	check(model.hitTest(1.0f, 1.0f) == media::ControlCommand::None,
		"a click in the padding hits nothing");
	check(model.hitTest(-5.0f, -5.0f) == media::ControlCommand::None,
		"a click outside the window hits nothing");
	check(model.hitTest(10000.0f, 10000.0f) == media::ControlCommand::None,
		"a click past the right edge hits nothing");
}

TEST(controller_seek_bar_maps_clicks_to_percentages) {
	media::ControllerModel model;
	model.layout(static_cast<float>(media::ControllerModel::kDefaultWidth),
		static_cast<float>(media::ControllerModel::kDefaultHeight));
	const media::Rect bar = model.layout().seekBar;

	const float midY = bar.centreY();
	double percent = -1.0;
	check(model.seekPercentAt(bar.x, midY, percent), "a click at the left edge is on the bar");
	checkEq(percent, 0.0, "the left edge is 0%");

	check(model.seekPercentAt(bar.x + bar.w * 0.5f, midY, percent), "the middle is on the bar");
	checkEq(percent, 50.0, "the middle is 50%");

	check(model.seekPercentAt(bar.x + bar.w - 0.5f, midY, percent),
		"a click at the right edge is on the bar");
	check(percent > 99.0 && percent <= 100.0, "the right edge is ~100%");

	check(!model.seekPercentAt(bar.x, bar.y - 40.0f, percent),
		"a click above the bar is not a seek");
	check(!model.seekPercentAt(-10.0f, midY, percent),
		"a click left of the bar is not a seek");
}

TEST(controller_seek_bar_is_inactive_for_a_still_image) {
	media::ControllerModel model;
	model.layout(720.0f, static_cast<float>(media::ControllerModel::kDefaultHeight));
	check(!model.seekBarActive(), "with nothing loaded the seek bar is inactive");

	media::ControllerState video;
	video.online = true;
	video.loaded = true;
	video.seekable = true;
	video.duration = 120.0;
	video.clipCount = 2;
	video.clipName = "clip.mp4";
	model.applyState(video);
	check(model.seekBarActive(), "a seekable clip makes the seek bar active");

	media::ControllerState image = video;
	image.isImage = true;
	image.seekable = false;
	image.duration = 0.0;
	model.applyState(image);
	check(!model.seekBarActive(), "a held image never offers a seek");
	check(model.titleText().find("[IMAGE]") != std::string::npos,
		"the title says the clip is an image");

	media::ControllerState offline = video;
	offline.online = false;
	model.applyState(offline);
	check(!model.seekBarActive(), "an offline player never offers a seek");
	check(model.titleText().find("NOT RUNNING") != std::string::npos,
		"the title reports an offline player");
}

TEST(controller_state_reports_change_only_for_visible_fields) {
	media::ControllerModel model;
	media::ControllerState state;
	state.online = true;
	state.loaded = true;
	state.clipName = "a.mp4";
	state.clipCount = 3;

	check(model.applyState(state), "the first snapshot is a change");
	check(!model.applyState(state), "an identical snapshot is not a change");

	// A sub-50ms position tick must not be reported as a change: the bar does
	// not draw that precision, and reporting it would redraw every frame.
	media::ControllerState ticked = state;
	ticked.position = state.position + 0.01;
	check(!model.applyState(ticked), "a sub-frame position tick is not a change");

	media::ControllerState moved = state;
	moved.position = state.position + 1.0;
	check(model.applyState(moved), "a whole-second position move is a change");

	media::ControllerState renamed = state;
	renamed.clipName = "b.mp4";
	check(model.applyState(renamed), "a clip change is a change");

	model.markOffline("connection refused");
	check(!model.state().online, "markOffline clears online");
	checkEqStr(model.state().lastError, "connection refused", "markOffline keeps the reason");
}

// ---------------------------------------------------------------------------
// Controller: the script host. Drives a recording player, so no HTTP and no
// window are involved.
// ---------------------------------------------------------------------------
TEST(lua_script_drives_the_player_and_registers_a_tick_handler) {
	media::LuaControllerScript script;
	if (!script.initialize()) {
		check(false, "the Lua host must initialise");
		return;
	}
	RecordingCommands recorder;
	media::PlayerClipInfo clip;
	clip.index = 1;
	clip.name = "b.mp4";
	clip.mediaType = "video";
	recorder.clips.push_back(clip);
	script.setCommands(&recorder);

	std::string error;
	const bool loaded = script.runSource(
		"controller.Log('hello') "
		"controller.Play(1) "
		"controller.SetVolume(45) "
		"controller.SetSpeed(2) "
		"controller.ShowHUD(false) "
		"local list = controller.Playlist() "
		"controller.Log('clips=' .. tostring(#list)) "
		"controller.OnTick(function() controller.Next() end)",
		"test.lua", error);
	check(loaded, "the script loads: " + error);
	check(script.scriptRunning(), "the host reports a running script");
	check(script.hasTickHandler(), "the OnTick handler was registered");
	check(script.lastError().empty(), "a clean script leaves no error");

	check(recorder.saw("open:1"), "Play(1) opened clip 1");
	check(recorder.saw("volume:45"), "SetVolume(45) reached the player");
	check(recorder.saw("speed:200"), "SetSpeed(2) reached the player");
	check(recorder.saw("hud:off"), "ShowHUD(false) reached the player");

	// The tick handler runs once per tick and is repeatable.
	recorder.clear();
	check(script.tick(), "the first tick succeeds");
	check(recorder.saw("next"), "the tick handler ran");
	recorder.clear();
	check(script.tick(), "the second tick succeeds");
	check(recorder.saw("next"), "the tick handler is repeatable");

	script.stopScript();
	check(!script.scriptRunning(), "stopScript clears the running script");
	check(!script.hasTickHandler(), "stopScript drops the tick handler");
	recorder.clear();
	check(script.tick(), "ticking a stopped host is a no-op");
	check(recorder.calls.empty(), "a stopped script issues nothing");
}

TEST(lua_script_failure_is_contained_and_the_host_survives) {
	media::LuaControllerScript script;
	if (!script.initialize()) {
		check(false, "the Lua host must initialise");
		return;
	}
	RecordingCommands recorder;
	script.setCommands(&recorder);

	// A syntax error must not leave a half-loaded script behind.
	std::string error;
	check(!script.runSource("this is not lua at all (", "broken.lua", error),
		"a syntax error fails the load");
	check(!error.empty(), "the syntax error explains itself");
	check(!script.scriptRunning(), "a failed load leaves no running script");

	// A runtime error inside OnTick must stop that script, not the host.
	check(script.runSource("controller.OnTick(function() error('boom') end)",
		"runtime.lua", error), "a script that raises in OnTick still loads");
	check(!script.tick(), "the tick reports the failure");
	check(!script.lastError().empty(), "the failure is recorded");
	check(!script.scriptRunning(), "the failing script is stopped, not retried forever");

	// The host is still usable afterwards: a bad script must not poison it.
	check(script.runSource("controller.OnTick(function() controller.Next() end)",
		"good.lua", error), "a good script loads after a bad one: " + error);
	recorder.clear();
	check(script.tick(), "the good script ticks");
	check(recorder.saw("next"), "the good script still reaches the player");
}

TEST(lua_script_cannot_freeze_the_bar) {
	media::LuaControllerScript script;
	if (!script.initialize()) {
		check(false, "the Lua host must initialise");
		return;
	}
	RecordingCommands recorder;
	script.setCommands(&recorder);

	// A loop that never yields must be aborted by the instruction budget, and
	// must abort quickly enough that this test finishes.
	std::string error;
	const auto started = std::chrono::steady_clock::now();
	const bool loaded = script.runSource("while true do end", "spin.lua", error);
	const double elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - started).count();

	check(!loaded, "an endless loop is aborted");
	check(!error.empty(), "the abort explains itself");
	check(elapsed < 10.0, "the abort happens promptly");
	check(!script.scriptRunning(), "the aborted script is not left running");

	// A long Sleep sequence is capped per tick instead: each call that exceeds
	// the budget reports false, which is how a script observes the window.
	check(script.runSource(
		"controller.OnTick(function() "
		"  local a = controller.Sleep(50) "
		"  local b = controller.Sleep(50) "
		"  controller.Log(tostring(a) .. ',' .. tostring(b)) "
		"end)", "sleep.lua", error), "the sleep script loads: " + error);
	check(script.tick(), "the sleep script ticks");
	checkEqStr(script.lastLog(), "true,false",
		"Sleep charges a rolling budget: the first call fits, the second does not");
}

TEST(lua_script_reports_a_refused_player_call) {
	media::LuaControllerScript script;
	if (!script.initialize()) {
		check(false, "the Lua host must initialise");
		return;
	}
	RecordingCommands recorder;
	recorder.refuseEverything = true;
	recorder.refusal = "nothing loaded";
	script.setCommands(&recorder);

	// A refused call raises inside the script, so the operator sees why rather
	// than a silent no-op.
	std::string error;
	const bool loaded = script.runSource("controller.Next()", "refused.lua", error);
	check(!loaded, "a refused player call fails the script");
	check(error.find("nothing loaded") != std::string::npos,
		"the refusal reason survives into the script error: " + error);
}

// ---------------------------------------------------------------------------
// Dashboard: rows, availability and hit tests.
// ---------------------------------------------------------------------------
TEST(dashboard_rows_report_availability_and_running_state) {
	media::DashboardModel model;
	model.layout(static_cast<float>(media::DashboardModel::kDefaultWidth),
		static_cast<float>(media::DashboardModel::kDefaultHeight));
	checkEq(model.rows().size(), media::DashboardModel::kRowCount, "there are two rows");
	checkEq(model.availableCount(), std::size_t{0}, "nothing is available before any probe");
	checkEqStr(model.rows()[0].title, "Player", "the first row is the Player");
	checkEqStr(model.rows()[1].title, "Controller", "the second row is the Controller");

	// A missing executable: the row exists, is not launchable, and says why.
	media::AppStatus absent;
	model.setStatus(media::DashboardApp::Player, absent, "", media::AppProbe::kPlayerPort);
	check(!model.rows()[0].available, "a missing executable is reported unavailable");
	check(model.rows()[0].subtitle.find("not found") != std::string::npos,
		"the row explains that the executable is missing");
	checkEq(model.availableCount(), std::size_t{0}, "a missing app is not counted as available");

	// Found but stopped.
	media::AppStatus stopped;
	model.setStatus(media::DashboardApp::Player, stopped, "C:\\bin\\media-player-cpp.exe",
		media::AppProbe::kPlayerPort);
	check(model.rows()[0].available, "a found executable is available");
	check(!model.rows()[0].running, "a stopped app is not running");
	checkEq(model.availableCount(), std::size_t{1}, "the available app is counted");

	// Found and running, started by someone else: running, but not ours to stop.
	media::AppStatus external;
	external.apiUp = true;
	external.childPid = 0;
	model.setStatus(media::DashboardApp::Player, external, "C:\\bin\\media-player-cpp.exe",
		media::AppProbe::kPlayerPort);
	check(model.rows()[0].running, "an app answering its API is running");
	check(!model.rows()[0].managed, "an app this Dashboard did not start is unmanaged");
}

TEST(dashboard_hit_test_only_offers_actions_that_make_sense) {
	media::DashboardModel model;
	model.layout(static_cast<float>(media::DashboardModel::kDefaultWidth),
		static_cast<float>(media::DashboardModel::kDefaultHeight));

	// Nothing available: neither button does anything.
	check(model.hitTest(model.rows()[0].launchButton.centreX(),
		model.rows()[0].launchButton.centreY()) == media::DashboardAction::None,
		"an unavailable app cannot be launched");

	media::AppStatus stopped;
	model.setStatus(media::DashboardApp::Player, stopped, "C:\\bin\\media-player-cpp.exe",
		media::AppProbe::kPlayerPort);
	check(model.hitTest(model.rows()[0].launchButton.centreX(),
		model.rows()[0].launchButton.centreY()) == media::DashboardAction::LaunchPlayer,
		"a stopped Player row offers LAUNCH");
	check(model.hitTest(model.rows()[0].stopButton.centreX(),
		model.rows()[0].stopButton.centreY()) == media::DashboardAction::None,
		"a stopped row offers no STOP");

	// Running but unmanaged: LAUNCH is inert and STOP must not be offered, or
	// the Dashboard would kill an app it did not start.
	media::AppStatus external;
	external.apiUp = true;
	model.setStatus(media::DashboardApp::Player, external, "C:\\bin\\media-player-cpp.exe",
		media::AppProbe::kPlayerPort);
	check(model.hitTest(model.rows()[0].launchButton.centreX(),
		model.rows()[0].launchButton.centreY()) == media::DashboardAction::None,
		"a running app is not launched again");
	check(model.hitTest(model.rows()[0].stopButton.centreX(),
		model.rows()[0].stopButton.centreY()) == media::DashboardAction::None,
		"an unmanaged app is not stopped");

	// Running and managed: STOP is offered.
	media::AppStatus managed;
	managed.apiUp = true;
	managed.childPid = 4242;
	model.setStatus(media::DashboardApp::Controller, managed,
		"C:\\bin\\media-controller-cpp.exe", media::AppProbe::kControllerPort);
	check(model.hitTest(model.rows()[1].stopButton.centreX(),
		model.rows()[1].stopButton.centreY()) == media::DashboardAction::StopController,
		"a managed app row offers STOP");

	// Rows do not claim clicks that belong to nothing.
	check(model.hitTest(1.0f, 1.0f) == media::DashboardAction::None,
		"a click in the title area acts on nothing");
}

TEST(dashboard_layout_keeps_rows_inside_the_window) {
	media::DashboardModel model;
	model.layout(static_cast<float>(media::DashboardModel::kDefaultWidth),
		static_cast<float>(media::DashboardModel::kDefaultHeight));
	for (const media::DashboardRow& row : model.rows()) {
		check(row.card.x >= 0.0f, "a card starts inside the window");
		check(row.card.x + row.card.w <= media::DashboardModel::kDefaultWidth + 0.5f,
			"a card ends inside the window");
		check(row.card.y >= 0.0f, "a card starts below the top edge");
		check(row.card.y + row.card.h <= media::DashboardModel::kDefaultHeight + 0.5f,
			"a card ends above the bottom edge");
		// Buttons sit inside their card and in the order STOP then LAUNCH.
		check(row.stopButton.x >= row.card.x, "STOP starts inside its card");
		check(row.launchButton.x + row.launchButton.w <= row.card.x + row.card.w + 0.5f,
			"LAUNCH ends inside its card");
		check(row.stopButton.x + row.stopButton.w <= row.launchButton.x + 0.5f,
			"STOP does not overlap LAUNCH");
	}
	check(model.messageArea().y + model.messageArea().h
			<= media::DashboardModel::kDefaultHeight + 0.5f,
		"the message line sits inside the window");
}

TEST(controller_client_commands_reach_the_player_over_http) {
	// End-to-end over a real socket: a stand-in Player server on a private port,
	// a real PlayerClient, and the base send() dispatch in between.
	//
	// This test exists because that dispatch path is easy to get subtly wrong:
	// `PlayerCommands::send` works by calling the virtual command methods, so a
	// client whose `next()` calls back into `send()` recurses until the stack is
	// gone -- a silent, instant death rather than a diagnosable failure.
	//
	// Every call goes through withServer(), which is what pumps the server's
	// command queue: the queue only runs on the thread that calls poll(), so a
	// synchronous call from this thread would wait for a poll() that this thread
	// is itself blocking on.
	const int port = 18099;
	media::MediaClipLibrary library;
	ScopedDataDir data(true);
	scanInto(library, data);
	RecordingBackend backend;
	media::MediaPlayerController player(library, &backend);
	check(player.setup(), "the stand-in player has a clip to open");
	// A stand-in needs window state too: /api/hud and /api/fullscreen answer
	// "this player has no window to change" when the host supplies no hooks,
	// which is correct but would make the toggle commands untestable here.
	bool standInHud = true;
	bool standInFullscreen = false;
	media::PresentationHooks hooks;
	hooks.getHud = [&standInHud] { return standInHud; };
	hooks.setHud = [&standInHud](bool visible) { standInHud = visible; return true; };
	hooks.getFullscreen = [&standInFullscreen] { return standInFullscreen; };
	hooks.setFullscreen = [&standInFullscreen](bool visible) {
		standInFullscreen = visible;
		return true;
	};
	media::HttpControlServer server(player, hooks);
	if (!server.start(port)) {
		check(false, "the stand-in player server must bind");
		return;
	}

	media::PlayerClient client("127.0.0.1", port);
	// Small poll interval: this test drives its own client, never start().
	std::string error;

	// A plain GET must work before anything else, so a failure below is about
	// the command path and not about reaching the server at all.
	{
		std::string pingError;
		const bool reachable = withServer(server, [&] { return client.ping(pingError); });
		check(reachable, "the stand-in player answers a plain GET: " + pingError);
	}

	// Every enum command goes through send(), which is the recursion hazard.
	const media::ControlCommand commands[] = {
		media::ControlCommand::Next,
		media::ControlCommand::Previous,
		media::ControlCommand::PlayPause,
		media::ControlCommand::Stop,
		media::ControlCommand::ToggleHud,
		media::ControlCommand::ToggleFullscreen,
		media::ControlCommand::ToggleSubtitles,
	};
	for (const media::ControlCommand command : commands) {
		error.clear();
		const bool ok = withServer(server, [&] {
			return client.send(command, 0.0, error);
		});
		check(ok, std::string("send(") + media::toString(command)
			+ ") reached the player: " + error);
	}

	// The explicit command methods must reach the player too, not just dispatch.
	{
		error.clear();
		check(withServer(server, [&] { return client.openClip(1, error); }),
			"openClip reaches the player: " + error);
		error.clear();
		check(withServer(server, [&] { return client.seekPercent(25.0, error); }),
			"seekPercent reaches the player: " + error);
		error.clear();
		check(withServer(server, [&] { return client.setVolume(40.0, error); }),
			"setVolume reaches the player: " + error);
		error.clear();
		check(withServer(server, [&] { return client.setSpeed(1.5, error); }),
			"setSpeed reaches the player: " + error);
	}

	// A command reply is adopted, so the client's snapshot tracks the Player.
	check(client.state().online, "a command reply marks the player online");
	checkEq(client.state().clipIndex, std::size_t{1}, "the client adopted the new clip index");
	checkEq(client.state().volume, 40.0, "the client adopted the new volume");

	// An unknown command must fail without touching the network.
	error.clear();
	check(!client.send(media::ControlCommand::None, 0.0, error), "an unknown command fails");
	check(!error.empty(), "the failure explains itself");

	server.stop();
}

TEST(controller_state_survives_a_refused_player_reply) {
	// A Player that answers ok:false must produce an error, not a crash and not
	// a silent success.
	RecordingCommands recorder;
	recorder.refuseEverything = true;
	recorder.refusal = "clip index out of range";
	media::ControllerModel model;
	media::ControllerHost host;
	host.player = &recorder;
	host.model = &model;

	media::ControllerHttpServer::Request request;
	request.kind = media::ControllerHttpServer::Request::Kind::OpenClip;
	request.clipIndex = 999;

	const Json reply = media::ControllerHttpServer::execute(host, 8081, true, request);
	checkEq(reply["ok"].get<bool>(), false, "a refused open is not reported as success");
	checkEqStr(reply["error"].get<std::string>(), "clip index out of range",
		"the player's reason is passed through");
}

// ---------------------------------------------------------------------------
// appconfig_round_trips_the_media_folder
//
// The settings file is the one thing all three processes read, and the only
// way it can fail is silently: a path that comes back different from the one
// that went in means the Player scans a folder nobody chose. The escape rules
// are what would break first, so they are asserted directly.
// ---------------------------------------------------------------------------
TEST(appconfig_round_trips_the_media_folder) {
	media::config::Config parsed;
	// Empty is a meaningful state ("use the Player's default"), not a missing
	// one, so it must survive the round trip rather than being dropped.
	media::config::parse(media::config::serialize(parsed), parsed);
	checkEqStr(parsed.mediaFolder, "", "an unset folder stays unset");

	media::config::Config written;
	written.mediaFolder = "D:\\Media Corpus\\Shows & Films";
	written.uiScale = 1.75f;
	media::config::Config readBack;
	media::config::parse(media::config::serialize(written), readBack);
	checkEqStr(readBack.mediaFolder, written.mediaFolder,
		"an ordinary path survives serialise + parse");
	check(std::abs(readBack.uiScale - 1.75f) < 0.001f, "the uiScale setting survives");

	// '=' is the key/value separator and '\' is the escape character: a folder
	// containing either must not corrupt the file or truncate the path.
	media::config::Config awkward;
	awkward.mediaFolder = "C:\\odd=name\\back\\slash";
	media::config::Config awkwardBack;
	media::config::parse(media::config::serialize(awkward), awkwardBack);
	checkEqStr(awkwardBack.mediaFolder, awkward.mediaFolder,
		"'=' and '\\' in a path survive the round trip");

	// A key this build does not know must be ignored, not treated as an error:
	// a newer binary may have written the file.
	media::config::Config forward;
	forward.mediaFolder = "D:\\kept";
	media::config::parse("someFutureKey = 12\nmediaFolder = D:\\kept\n", forward);
	checkEqStr(forward.mediaFolder, "D:\\kept", "an unknown key does not stop the parse");

	// A real file, so load()/save() are covered and not just the text helpers.
	const std::string path = media::platform::executableDirectory()
		+ "tests-appconfig-" + std::to_string(::GetCurrentProcessId()) + ".ini";
	std::error_code ec;
	std::filesystem::remove(path, ec);

	media::config::Config absent;
	check(!media::config::load(path, absent),
		"a missing file reports false rather than failing");
	media::config::Config target;
	target.mediaFolder = "E:\\Corpus";
	target.uiScale = 0.0f;
	check(media::config::save(path, target), "save writes the file");
	media::config::Config fromDisk;
	check(media::config::load(path, fromDisk), "load reads what save wrote");
	checkEqStr(fromDisk.mediaFolder, "E:\\Corpus", "the folder came back off disk");

	std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// ui_scale_keeps_text_readable_on_a_dense_display
//
// The whole reason for the scale is the 4K case, and the floor is what makes a
// 4K panel at 100% Windows scaling readable. If the floor regresses, the text
// goes back to being a 7-pixel capital and nothing else fails.
// ---------------------------------------------------------------------------
TEST(ui_scale_keeps_text_readable_on_a_dense_display) {
	// A big scale multiplies through.
	check(media::ui::textScale(2.0f, media::ui::kBodyPixels) >= 2.0f,
		"a 2x monitor gets at least 2x text");

	// The floor: 100% DPI must still produce a legible body line. This is the
	// regression guard - the old build drew every one of these at scale 1.
	check(media::ui::bodyScale(1.0f) > 1.0f,
		"body text is larger than the raw 7px cell even at 100% DPI");
	check(media::ui::kGlyphHeight * media::ui::bodyScale(1.0f) >= media::ui::kBodyPixels,
		"body text meets its pixel floor");
	check(media::ui::kGlyphHeight * media::ui::smallScale(1.0f) >= media::ui::kSmallPixels,
		"small print meets its floor");
	check(media::ui::buttonScale(1.0f) > media::ui::bodyScale(1.0f),
		"transport labels are the largest of the roles");

	// Rubbish in: no zeros, no negatives, no NaN, no absurdity.
	check(media::ui::sanitizeScale(0.0f) >= 1.0f, "a zero scale is clamped up");
	check(media::ui::sanitizeScale(-4.0f) >= 1.0f, "a negative scale is clamped up");
	check(media::ui::sanitizeScale(100.0f) <= 4.0f, "an absurd scale is clamped down");
	check(media::ui::fromContentScale(0.0f, 0.0f) >= 1.0f,
		"a monitor that reports nothing still yields a usable scale");

	// The ini override wins over the monitor, in both directions.
	media::config::Config bigger;
	bigger.uiScale = 3.0f;
	check(media::ui::textScale(bigger.uiScale, media::ui::kBodyPixels) >= 3.0f,
		"an explicit ini scale enlarges the text");
	media::config::Config zero;
	check(zero.uiScale == 0.0f, "0 means 'not set', so the DPI decision stands");
}

// ---------------------------------------------------------------------------
// controller_corpus_panel_reports_the_folder_and_the_clip_count
//
// The empty state is the one the request called out: with no folder chosen and
// nothing to play, the bar must say so plainly rather than look broken.
// ---------------------------------------------------------------------------
TEST(controller_corpus_panel_reports_the_folder_and_the_clip_count) {
	media::ControllerModel model;
	model.setUiScale(2.0f);

	// Offline: there is no folder to report, and the panel says what to do
	// instead of showing a stale path.
	check(model.corpusValue().find("START THE PLAYER") != std::string::npos,
		"offline, the panel asks for the player rather than naming a folder");
	check(!model.corpusChosen(), "offline is not a chosen corpus");

	media::ControllerState online;
	online.online = true;
	online.clipCount = 0;
	online.corpusClipCount = 0;
	model.applyState(online);

	// The no-folder case: readable, explicit, and not an error.
	check(!model.corpusChosen(), "the default folder is not a chosen corpus");
	check(model.corpusValue().find("NOT SET") != std::string::npos,
		"an unset folder is spelled out");
	check(model.corpusValue().find("0 VIDEOS") != std::string::npos,
		"an empty corpus reports 0 videos");
	checkEqStr(model.titleText(), "NO CLIPS", "an empty playlist still says NO CLIPS");

	media::ControllerState loaded = online;
	loaded.loaded = true;
	loaded.clipCount = 1;
	loaded.corpusClipCount = 1;
	loaded.clipIndex = 0;
	loaded.clipName = "only.mp4";
	loaded.mediaFolder = "D:\\Corpus\\Shows\\2026";
	model.applyState(loaded);

	check(model.corpusChosen(), "a reported folder counts as chosen");
	check(model.corpusValue().find("1 VIDEO") != std::string::npos,
		"a single clip is reported in the singular");
	check(model.corpusValue().find("2026") != std::string::npos,
		"the panel names the folder");

	// The field must be clickable and must not swallow a transport click.
	const float width = static_cast<float>(media::ControllerModel::kDefaultWidth);
	const float height = static_cast<float>(media::ControllerModel::kDefaultHeight);
	model.layout(width, height);
	const media::Rect corpus = model.layout().corpusArea;
	check(!corpus.empty(), "the corpus field is laid out when the player is up");
	check(model.corpusHit(corpus.centreX(), corpus.centreY()),
		"a click on the corpus field is a corpus click");
	check(model.hitTest(corpus.centreX(), corpus.centreY()) == media::ControlCommand::None,
		"the corpus field is not a transport button");
	check(!model.corpusHit(corpus.x, corpus.y - 60.0f),
		"a click above the corpus field is not a corpus click");
	check(corpus.y + corpus.h <= height + 0.5f, "the corpus field ends inside the bar");

	// The bands must not overlap at any DPI scale. This is the check that
	// matters: the corpus field is fitted between the transport row and the
	// seek bar, and getting the space budget wrong there draws the folder
	// straight through the buttons without failing anything else.
	//
	// Each iteration is laid out at the size the application would really ask
	// for at that scale (default size x scale), because that is the pair the
	// window and the layout are in step at.
	const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
	for (const float scale : scales) {
		media::ControllerModel scaled;
		scaled.setUiScale(scale);
		scaled.applyState(loaded);
		scaled.layout(static_cast<float>(media::ControllerModel::kDefaultWidth) * scale,
			static_cast<float>(media::ControllerModel::kDefaultHeight) * scale);
		const media::ControllerLayout& out = scaled.layout();
		const std::string at = " at " + std::to_string(scale) + "x";

		check(!out.corpusArea.empty(), "the corpus field is laid out" + at);
		check(out.corpusArea.h >= media::ui::kGlyphHeight
				* media::ui::textScale(scale, 15.0f),
			"the corpus field fits its own text" + at);

		float buttonsBottom = 0.0f;
		for (const media::ControlButton& button : out.buttons) {
			check(!out.corpusArea.hit(button.rect.centreX(), button.rect.centreY()),
				"the corpus field does not overlap the transport row" + at);
			buttonsBottom = std::max(buttonsBottom, button.rect.y + button.rect.h);
			check(button.rect.y + button.rect.h <= out.corpusArea.y + 0.5f,
				"a transport button ends above the corpus field" + at);
		}
		check(out.corpusArea.y + out.corpusArea.h <= out.seekBar.y + 0.5f,
			"the corpus field ends above the seek bar" + at);
		check(buttonsBottom > 0.0f, "the transport row has height" + at);
		check(out.seekBar.y + out.seekBar.h
				<= static_cast<float>(media::ControllerModel::kDefaultHeight) * scale + 0.5f,
			"the seek bar ends inside the bar" + at);
	}
}

// ---------------------------------------------------------------------------
// dashboard_corpus_panel_is_laid_out_and_keeps_its_button_inside
// ---------------------------------------------------------------------------
TEST(dashboard_corpus_panel_is_laid_out_and_keeps_its_button_inside) {
	media::DashboardModel model;
	model.setCorpus("D:\\Corpus", 0, false, "");
	model.layout(static_cast<float>(media::DashboardModel::kDefaultWidth),
		static_cast<float>(media::DashboardModel::kDefaultHeight));

	const float windowW = static_cast<float>(media::DashboardModel::kDefaultWidth);
	const float windowH = static_cast<float>(media::DashboardModel::kDefaultHeight);

	check(!model.corpus().card.empty(), "the corpus panel has been laid out");
	check(model.corpus().card.x >= 0.0f, "the panel starts inside the window");
	check(model.corpus().card.x + model.corpus().card.w <= windowW + 0.5f,
		"the panel ends inside the window");
	check(model.corpus().card.y + model.corpus().card.h <= windowH + 0.5f,
		"the panel ends above the bottom edge");
	check(model.corpus().chooseButton.x >= model.corpus().card.x,
		"CHANGE... starts inside its panel");
	check(model.corpus().chooseButton.x + model.corpus().chooseButton.w
			<= model.corpus().card.x + model.corpus().card.w + 0.5f,
		"CHANGE... ends inside its panel");

	// It must not collide with the application rows above it.
	for (const media::DashboardRow& row : model.rows()) {
		check(row.card.y + row.card.h <= model.corpus().card.y + 0.5f,
			"the corpus panel sits below every application row");
	}
	check(model.corpus().card.y + model.corpus().card.h
			<= model.messageArea().y + 0.5f,
		"the corpus panel sits above the message line");

	// The button is live whether or not anything is running: choosing the
	// folder is exactly what you do when nothing is.
	check(model.hitTest(model.corpus().chooseButton.centreX(),
		model.corpus().chooseButton.centreY()) == media::DashboardAction::ChooseMediaFolder,
		"CHANGE... offers the folder action");

	// And it still works at 2.4x, which is the 4K case that motivated the panel.
	// The window grows with the scale, exactly as dashboard_main.cpp sizes it.
	const float bigW = windowW * 2.4f;
	const float bigH = windowH * 2.4f;
	model.layout(bigW, bigH, 2.4f);
	check(model.corpus().chooseButton.x + model.corpus().chooseButton.w
			<= model.corpus().card.x + model.corpus().card.w + 0.5f,
		"CHANGE... stays inside its panel at 2.4x");
	check(model.corpus().card.y + model.corpus().card.h
			<= model.messageArea().y + 0.5f,
		"the panel stays above the message line at 2.4x");
	check(model.corpus().card.x + model.corpus().card.w <= bigW + 0.5f,
		"the panel stays inside a 2.4x window");
	check(model.corpus().card.y + model.corpus().card.h <= bigH + 0.5f,
		"the panel ends above the bottom edge at 2.4x");
}

// ---------------------------------------------------------------------------
// player_media_folder_route_switches_the_corpus
//
// The route the Controller's picker calls. It must actually re-point the
// library - a reply that says ok while the playlist is unchanged is the exact
// failure this is here to catch.
// ---------------------------------------------------------------------------
TEST(player_media_folder_route_switches_the_corpus) {
	const int port = 18097;
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	media::MediaPlayerController player(library, nullptr);
	// Seed the playlist directly: this test is about the route, not about the
	// startup path that reads mediaplayer.ini.
	scanInto(library, data);
	check(player.rescan() > 0, "the fixture folder seeded the playlist");

	// IMPORTANT: the route persists the folder to mediaplayer.ini, and that file
	// is shared state next to the binary. Point the config at a throwaway path
	// for the duration of this test, or the player left in bin/ would come up
	// scanning this test's temporary directory.
	const std::string scratchConfig = media::platform::executableDirectory()
		+ "tests-media-dir-" + std::to_string(::GetCurrentProcessId()) + ".ini";
	media::config::setConfigPathOverride(scratchConfig);
	// Restored on every exit path, including an exception.
	struct ConfigGuard {
		~ConfigGuard() { media::config::setConfigPathOverride({}); }
	} guard;

	media::HttpControlServer server(player);
	if (!server.start(port)) {
		check(false, "the media-folder test server must bind");
		return;
	}
	media::HttpJsonClient client("127.0.0.1", port);

	// A folder that does not exist is refused with a 400, and the playlist is
	// left alone. Refusing at the HTTP level rather than answering ok:false is
	// deliberate: the request was wrong, not the player.
	Json reply;
	std::string error;
	check(!withServer(server, [&] {
		return client.post("/api/media-dir",
			Json{{"path", "C:\\definitely-not-here-12345"}}, reply, error);
	}), "a missing folder is refused");
	check(error.find("400") != std::string::npos,
		"the refusal is reported as a bad request: " + error);
	check(player.getClips().size() > 0, "a refused change leaves the playlist intact");

	// A malformed body is a 400, not a crash and not a silent success.
	Json bad;
	std::string badError;
	check(!withServer(server, [&] {
		return client.post("/api/media-dir", Json{{"nope", 1}}, bad, badError);
	}), "a body with no path is refused");

	// Pointing at a folder with no media yields an empty corpus, not an error.
	std::error_code ec;
	const std::filesystem::path empty = data.root / "empty-corpus";
	std::filesystem::create_directories(empty, ec);
	Json emptyReply;
	std::string emptyError;
	check(withServer(server, [&] {
		return client.post("/api/media-dir", Json{{"path", empty.string()}},
			emptyReply, emptyError);
	}), "switching to an empty folder completes");
	check(emptyReply.contains("mediaFolder"), "the reply names the new folder");
	checkEq(emptyReply["clipCount"].get<std::size_t>(), std::size_t{0},
		"an empty folder reports 0 clips");
	checkEq(player.getClips().size(), std::size_t{0},
		"the playlist is empty after switching to an empty folder");
	check(!player.getStatus().loaded, "nothing is loaded after the corpus emptied");

	// GET reports the same thing, which is what the Controller's panel draws.
	Json status;
	std::string statusError;
	check(withServer(server, [&] {
		return client.get("/api/media-dir", status, statusError);
	}), "GET /api/media-dir answers");
	checkEq(status["clipCount"].get<std::size_t>(), std::size_t{0},
		"the reported count matches the empty folder");

	// Back to a folder with media: the playlist comes back.
	Json back;
	std::string backError;
	check(withServer(server, [&] {
		return client.post("/api/media-dir", Json{{"path", data.root.string()}},
			back, backError);
	}), "switching back completes");
	check(back["clipCount"].get<std::size_t>() > 0,
		"the playlist is repopulated when the folder has media");
	check(player.getStatus().loaded, "something is loaded again");

	server.stop();
	{
		std::error_code cleanupError;
		std::filesystem::remove(scratchConfig, cleanupError);
		std::filesystem::remove_all(empty, cleanupError);
	}
}

// ---------------------------------------------------------------------------
// controller_client_posts_the_media_folder_to_the_players_route
// ---------------------------------------------------------------------------
TEST(controller_client_posts_the_media_folder_to_the_players_route) {
	const int port = 18096;
	httplib::Server server;
	std::string postedPath;
	bool sawRequest = false;

	server.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(Json{{"loaded", false}, {"playing", false}, {"isImage", false},
			{"clipIndex", 0}, {"clipCount", 0}, {"clipName", ""},
			{"subtitlesEnabled", true}, {"subtitleText", ""},
			{"mediaFolder", "D:/Corpus"}}.dump(), "application/json");
	});
	server.Post("/api/media-dir", [&](const httplib::Request& req, httplib::Response& res) {
		const Json body = Json::parse(req.body, nullptr, false);
		postedPath = body.is_object() && body.contains("path") && body["path"].is_string()
			? body["path"].get<std::string>() : std::string();
		sawRequest = true;
		res.set_content(Json{{"ok", true}, {"mediaFolder", postedPath},
			{"clipCount", 3}, {"loaded", false}, {"playing", false},
			{"isImage", false}, {"clipIndex", 0}, {"clipName", ""},
			{"subtitlesEnabled", true}, {"subtitleText", ""}}.dump(),
			"application/json");
	});

	if (!server.bind_to_port("127.0.0.1", port)) {
		check(false, "the media-folder client test server must bind");
		return;
	}
	std::thread listener([&server] { server.listen_after_bind(); });

	media::PlayerClient client("127.0.0.1", port);
	std::string error;
	check(client.setMediaFolder("D:\\Corpus", error),
		"setMediaFolder reaches the player: " + error);
	check(sawRequest, "the route was actually called");
	checkEqStr(postedPath, "D:\\Corpus", "the chosen folder is what was posted");
	checkEqStr(client.state().mediaFolder, "D:\\Corpus",
		"the client adopted the folder the player confirmed");
	checkEq(client.state().corpusClipCount, std::size_t{3},
		"the client adopted the corpus clip count");

	// An empty path is valid: it means "back to the player's default".
	sawRequest = false;
	error.clear();
	check(client.setMediaFolder("", error),
		"an empty folder means 'use the default' and is accepted: " + error);
	checkEqStr(postedPath, "", "the empty path is passed through as empty");

	// A poll adopts the mediaFolder from /api/status, which is where the
	// Controller's panel gets it when it did not set it itself.
	client.pollOnce();
	checkEqStr(client.state().mediaFolder, "D:/Corpus",
		"a status poll reports the player's folder");

	server.stop();
	if (listener.joinable()) {
		listener.join();
	}
}

// ---------------------------------------------------------------------------
int main() {	std::printf("media_tests\n");
	for (const TestCase& test : registry()) {
		gCurrentTest = test.name;
		const int before = gFailures;
		std::printf("- %s\n", test.name);
		try {
			test.fn();
		} catch (const std::exception& e) {
			++gFailures;
			std::printf("  FAIL  [%s] threw: %s\n", test.name, e.what());
		}
		if (gFailures == before) {
			std::printf("  ok\n");
		}
	}

	std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}