// media_tests — tiny dependency-free assertion runner.
//
// A test framework is not worth a dependency here; this is enough to verify the
// HTTP contract and the playlist logic, and it keeps the binary linkable
// without a GL context.

#include "app/HttpControlServer.h"
#include "core/Platform.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"

#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
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

/// Create a temporary data directory with a couple of fake clips in it.
/// MediaClipLibrary resolves bin/data from the executable, and the test binary
/// lives in bin/, so this writes into the real location and cleans up after.
struct ScopedDataDir {
	std::filesystem::path root;
	std::vector<std::filesystem::path> created;

	explicit ScopedDataDir(bool makeVideos) {
		root = media::platform::dataDirectory();
		std::filesystem::create_directories(root);

		// A 1x1 PNG: enough for the extension-based scan, no decoder involved.
		const std::filesystem::path png = root / "zz-test-image.png";
		if (!std::filesystem::exists(png)) {
			std::ofstream out(png, std::ios::binary);
			const unsigned char kPng[] = {
				0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
			out.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
			created.push_back(png);
		}
		std::filesystem::path a = root / "zz-test-a.mp4";
		if (!std::filesystem::exists(a)) {
			std::ofstream(a).put('\0');
			created.push_back(a);
		}
		if (makeVideos) {
			std::filesystem::path b = root / "zz-test-b.mkv";
			if (!std::filesystem::exists(b)) {
				std::ofstream(b).put('\0');
				created.push_back(b);
			}
		}
	}

	~ScopedDataDir() {
		std::error_code ec;
		for (const auto& p : created) {
			std::filesystem::remove(p, ec);
		}
	}
};

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

TEST(script_host_discovers_the_reference_script) {
	// The seeded data dir plus the installed reference script.
	const std::vector<media::scripts::ScriptFile> found = media::scripts::discover();
	check(!found.empty(), "at least one script discovered under data/scripts");

	bool sawLua = false;
	for (const auto& script : found) {
		check(!script.absolutePath.empty(), "script path populated");
		check(!script.name.empty(), "script name populated");
		check(script.language == "lua" || script.language == "js", "script language classified");
		check(script.absolutePath.find("datascripts") == std::string::npos,
			"script path has a real separator before 'scripts'");
		if (script.name == "media-player.lua") {
			sawLua = true;
			checkEqStr(script.language, "lua", "reference script classified as lua");
		}
	}
	check(sawLua, "reference script media-player.lua found");

	// Non-script extensions are not runnable.
	check(media::scripts::isScriptPath("a.lua"), ".lua is a script");
	check(media::scripts::isScriptPath("a.JS"), ".JS is a script (case-insensitive)");
	check(!media::scripts::isScriptPath("a.txt"), ".txt is not a script");
	check(!media::scripts::isScriptPath("a.luac"), ".luac is not loaded");
	check(!media::scripts::isScriptPath(""), "empty path is not a script");
}

/// Images and video share the mpv surface, but they must not share transport
/// semantics: a held still has no timeline. These checks pin that down, because
/// the tempting shortcut (mark everything seekable) would make a host's seek
/// call look like it worked on an image.
TEST(image_clips_have_no_transport) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	library.scan();

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
	library.scan();
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
	library.scan();
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
	check(!status.scriptsLoaded.empty(), "status reports loaded scripts");

	bool sawReference = false;
	for (const std::string& name : status.scriptsLoaded) {
		if (name == "media-player.lua") {
			sawReference = true;
		}
	}
	check(sawReference, "reference script reported as loaded");

	// Every script the backend was given must also be listed as on-disk, so the
	// two views can never disagree.
	checkEq(controller.loadedScripts().size(), controller.scriptFiles().size(),
		"loaded count matches the on-disk count");
}

TEST(clip_library_orders_images_first_and_dedups) {
	ScopedDataDir data(true);
	media::MediaClipLibrary library;
	library.scan();

	check(library.size() >= 3, "found the three seeded clips");
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
	library.scan();

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
	library.scan();

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
	library.scan();
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
	library.scan();
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

	// A real, in-data-root media file is accepted.
	check(server.addClipPath(media::platform::dataDirectory() + "\\zz-test-a.mp4", &error),
		"in-root media file accepted");
}

// ---------------------------------------------------------------------------
int main() {
	std::printf("media_tests\n");
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
