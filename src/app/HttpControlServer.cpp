#include "app/HttpControlServer.h"

#include "app/http/CommandQueue.h"
#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "media/IClipSource.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"

#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace media {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxRequestBody = 8192;

bool isLoopback(const std::string& ip) {
	return ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1"
		|| ip == "localhost" || ip.empty();
}

bool hasMediaExtension(const std::string& path) {
	static const char* kExtensions[] = {
		".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v", ".mpg", ".mpeg", ".wmv", ".flv", ".ts",
		".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff"
	};
	const std::string ext = platform::lowerExtension(path);
	for (const char* candidate : kExtensions) {
		if (ext == candidate) {
			return true;
		}
	}
	return false;
}

/// True when `candidate` resolves inside `root`. Guards the playlist route so a
/// request can never point the player at an arbitrary file on the machine.
bool isInsideDirectory(const std::filesystem::path& candidate,
	const std::filesystem::path& root) {
	std::error_code ec;
	const std::filesystem::path canonicalCandidate =
		std::filesystem::weakly_canonical(candidate, ec);
	if (ec) {
		return false;
	}
	const std::filesystem::path canonicalRoot =
		std::filesystem::weakly_canonical(root, ec);
	if (ec) {
		return false;
	}
	auto rootIt = canonicalRoot.begin();
	auto candIt = canonicalCandidate.begin();
	for (; rootIt != canonicalRoot.end(); ++rootIt, ++candIt) {
		if (candIt == canonicalCandidate.end() || *candIt != *rootIt) {
			return false;
		}
	}
	return true;
}

Json statusJson(const MediaPlayerStatus& status,
	const PresentationHooks& hooks) {
	// The media folder is read through the same host closure as the window
	// state: main.cpp owns the render loop and the library, and this layer must
	// not reach for either directly. Unset means "this host has no library to
	// report", which the tests use, and which is reported as an empty string.
	const std::string mediaFolder =
		hooks.getMediaFolder ? hooks.getMediaFolder() : std::string();
	return Json{
		// frozen contract
		{"loaded", status.loaded},
		{"playing", status.playing},
		{"isImage", status.isImage},
		{"clipIndex", status.clipIndex},
		{"clipCount", status.clipCount},
		{"clipName", status.clipName},
		{"subtitlesEnabled", status.subtitlesEnabled},
		{"subtitleText", status.subtitleText},
		// additive
		{"position", status.position},
		{"duration", status.duration},
		{"seekable", status.seekable},
		{"speed", status.speed},
		{"volume", status.volume},
		{"paused", status.paused},
		{"decoder", status.decoder},
		{"scriptsLoaded", status.scriptsLoaded},
		// additive: where the playlist came from. The Controller draws this in
		// its corpus rectangle, and the Dashboard checks it against the setting
		// in mediaplayer.ini, so both read it from the one process that owns
		// the library rather than from their own possibly-stale config copy.
		{"mediaFolder", mediaFolder},
		// additive: presentation the host owns. Reported as null when the host
		// has no window (the test harness), so a client can tell "hidden" from
		// "not applicable".
		{"hudVisible", hooks.getHud ? Json(hooks.getHud()) : Json(nullptr)},
		{"fullscreen", hooks.getFullscreen ? Json(hooks.getFullscreen()) : Json(nullptr)},
	};
}

Json errorJson(const std::string& message) {
	return Json{{"ok", false}, {"error", message}};
}

Json okWithStatus(const MediaPlayerController& controller,
	const PresentationHooks& hooks) {
	return Json{{"ok", true}, {"status", statusJson(controller.getStatus(), hooks)}};
}

} // namespace

std::string statusToJsonText(const MediaPlayerController& controller) {
	return statusJson(controller.getStatus(), PresentationHooks{}).dump();
}

// ---------------------------------------------------------------------------
// Impl: owns the command queue, the httplib server and its listen thread.
//
// The queue itself is CommandQueue<Json>: HTTP workers submit closures and
// block, poll() drains them on the main thread. See app/http/CommandQueue.h.
// ---------------------------------------------------------------------------
struct HttpControlServer::Impl {
	Impl(MediaPlayerController& c, PresentationHooks h)
		: controller(c), hooks(std::move(h)) {}

	~Impl() { stopServer(); }

	CommandQueue<Json> queue;

	std::mutex serverMutex;
	std::unique_ptr<httplib::Server> server;
	std::thread serverThread;
	std::atomic<bool> running{false};

	MediaPlayerController& controller;
	/// The host's window state, reached only from the main thread.
	PresentationHooks hooks;

	/// Submit work and block until the main thread produces a result.
	/// Returns nullopt when shutting down, so a worker can never wait forever
	/// for a poll() that will not come.
	std::optional<Json> submit(std::function<Json()> work) {
		return queue.submit(std::move(work));
	}

	/// Main thread: run everything queued.
	void drain() {
		queue.drain(errorJson);
	}

	/// Main thread: block briefly when there is nothing to do.
	void waitForWork(int timeoutMs) {
		queue.waitForWork(timeoutMs);
	}

	void failAllWaiters() {
		queue.failAllWaiters();
	}

	void stopServer() {
		running = false;
		failAllWaiters();
		if (server) {
			server->stop();
		}
		if (serverThread.joinable()) {
			serverThread.join();
		}
		server.reset();
	}
};

// ---------------------------------------------------------------------------
// HttpControlServer
// ---------------------------------------------------------------------------
HttpControlServer::HttpControlServer(MediaPlayerController& controller,
	PresentationHooks hooks)
	: impl_(std::make_unique<Impl>(controller, std::move(hooks))) {}

HttpControlServer::~HttpControlServer() {
	stop();
}

void HttpControlServer::poll() {
	if (running_) {
		impl_->drain();
	}
}

bool HttpControlServer::addClipPath(const std::string& path, std::string* error) {
	if (path.empty()) {
		if (error) *error = "path is required";
		return false;
	}
	if (!hasMediaExtension(path)) {
		if (error) *error = "unsupported media extension";
		return false;
	}
	std::error_code ec;
	const std::filesystem::path candidate(path);
	if (!std::filesystem::exists(candidate, ec)
		|| !std::filesystem::is_regular_file(candidate, ec)) {
		if (error) *error = "no such file";
		return false;
	}
	if (!isInsideDirectory(candidate, platform::dataDirectory())) {
		if (error) *error = "path must be inside the data directory";
		return false;
	}
	if (error) error->clear();
	return true;
}

bool HttpControlServer::start(int port) {
	if (running_) {
		return true;
	}
	port_ = port;

	auto server = std::make_unique<httplib::Server>();
	server->set_payload_max_length(kMaxRequestBody);
	server->set_read_timeout(5, 0);
	server->set_write_timeout(5, 0);
	server->set_keep_alive_max_count(1);

	// Enforces localhost-only access and sets the shared response header.
	// Returns false when the response has already been filled in.
	auto guard = [](const httplib::Request& req, httplib::Response& res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		if (!isLoopback(req.remote_addr)) {
			LOG_WARN("HttpControlServer") << "rejected non-local client " << req.remote_addr;
			res.status = 403;
			res.set_content(errorJson("localhost only").dump(), "application/json");
			return false;
		}
		return true;
	};

	// Submit a controller command and answer with its JSON.
	auto dispatch = [this](httplib::Response& res,
		const std::function<Json(MediaPlayerController&)>& work) {
		auto result = impl_->submit([this, work]() { return work(impl_->controller); });
		if (!result) {
			res.status = 503;
			res.set_content(errorJson("server shutting down").dump(), "application/json");
			return;
		}
		res.status = 200;
		res.set_content(result->dump(), "application/json");
	};

	// Parse the body, answering 400 on malformed JSON. Returns false in that case.
	auto parseBody = [](const httplib::Request& req, httplib::Response& res, Json& out) {
		out = Json::parse(req.body, nullptr, false);
		if (out.is_discarded()) {
			LOG_WARN("HttpControlServer") << "unparseable body on " << req.path
				<< " (" << req.body.size() << " byte(s), content-type '"
				<< req.get_header_value("Content-Type") << "')";
			res.status = 400;
			res.set_content(errorJson("expected JSON body").dump(), "application/json");
			return false;
		}
		return true;
	};

	// ---- read-only -------------------------------------------------------
	server->Get("/api/health", [guard](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		res.status = 200;
		res.set_content(Json{{"ok", true}}.dump(), "application/json");
	});

	server->Get("/api/status", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [this](MediaPlayerController& c) { return statusJson(c.getStatus(), impl_->hooks); });
	});

	server->Get("/api/position", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) {
			const MediaPlayerStatus s = c.getStatus();
			return Json{{"position", s.position}, {"duration", s.duration},
				{"seekable", s.seekable}, {"speed", s.speed}, {"paused", s.paused}};
		});
	});

	server->Get("/api/clips", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) {
			Json payload = Json::array();
			for (const MediaPlayerClipInfo& clip : c.getClips()) {
				payload.push_back({{"index", clip.index}, {"name", clip.name},
					{"path", clip.path}, {"mediaType", clip.mediaType}});
			}
			return payload;
		});
	});

	// ---- transport -------------------------------------------------------
	server->Post("/api/play", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [this](MediaPlayerController& c) { c.play(); return okWithStatus(c, impl_->hooks); });
	});

	server->Post("/api/stop", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [this](MediaPlayerController& c) { c.stop(); return okWithStatus(c, impl_->hooks); });
	});

	server->Post("/api/next", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [this](MediaPlayerController& c) { c.nextClip(); return okWithStatus(c, impl_->hooks); });
	});

	server->Post("/api/previous", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [this](MediaPlayerController& c) { c.previousClip(); return okWithStatus(c, impl_->hooks); });
	});

	server->Post("/api/pause", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body = Json::object();
		// A pause request with no body means "toggle".
		if (!req.body.empty() && !parseBody(req, res, body)) return;
		dispatch(res, [this, body](MediaPlayerController& c) -> Json {
			bool paused = !c.getStatus().paused;
			if (body.contains("paused")) {
				if (!body["paused"].is_boolean()) {
					return errorJson("\"paused\" must be a boolean");
				}
				paused = body["paused"].get<bool>();
			}
			if (!c.setPaused(paused)) {
				return errorJson("pause rejected (nothing loaded)");
			}
			return okWithStatus(c, impl_->hooks);
		});
	});

	// ---- seek / rate / volume -------------------------------------------
	server->Post("/api/seek", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		dispatch(res, [this, body](MediaPlayerController& c) -> Json {
			bool ok = false;
			if (body.contains("time") && body["time"].is_number()) {
				ok = c.seekAbsolute(body["time"].get<double>());
			} else if (body.contains("relative") && body["relative"].is_number()) {
				ok = c.seekRelative(body["relative"].get<double>());
			} else if (body.contains("percent") && body["percent"].is_number()) {
				ok = c.seekPercent(body["percent"].get<double>());
			} else {
				return errorJson("expected one of: time, relative, percent (numbers)");
			}
			if (!ok) {
				return errorJson("seek rejected (nothing loaded, or value out of range)");
			}
			return okWithStatus(c, impl_->hooks);
		});
	});

	server->Post("/api/speed", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		if (!body.contains("speed") || !body["speed"].is_number()) {
			res.status = 400;
			res.set_content(errorJson("expected {\"speed\": <number>}").dump(), "application/json");
			return;
		}
		const double speed = body["speed"].get<double>();
		dispatch(res, [this, speed](MediaPlayerController& c) -> Json {
			if (!c.setSpeed(speed)) {
				return errorJson("speed out of range (0.01..100) or nothing loaded");
			}
			return okWithStatus(c, impl_->hooks);
		});
	});

	server->Post("/api/volume", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		if (!body.contains("volume") || !body["volume"].is_number()) {
			res.status = 400;
			res.set_content(errorJson("expected {\"volume\": <0..100>}").dump(), "application/json");
			return;
		}
		const double volume = body["volume"].get<double>();
		dispatch(res, [this, volume](MediaPlayerController& c) -> Json {
			if (!c.setVolume(volume)) {
				return errorJson("volume out of range (0..100)");
			}
			return okWithStatus(c, impl_->hooks);
		});
	});

	// ---- subtitles -------------------------------------------------------
	server->Post("/api/subtitles", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		dispatch(res, [this, body](MediaPlayerController& c) -> Json {
			if (body.contains("enabled")) {
				if (!body["enabled"].is_boolean()) {
					return errorJson("\"enabled\" must be a boolean");
				}
				c.setSubtitlesEnabled(body["enabled"].get<bool>());
			}
			if (body.contains("text")) {
				if (!body["text"].is_string()) {
					return errorJson("\"text\" must be a string");
				}
				const std::string text = body["text"].get<std::string>();
				if (text.empty()) {
					c.clearSubtitleOverride();
				} else {
					c.setSubtitleText(text);
				}
			}
			if (!body.contains("enabled") && !body.contains("text")) {
				return errorJson("expected {\"enabled\": bool} and/or {\"text\": string}");
			}
			return Json{{"ok", true},
				{"subtitlesEnabled", c.isSubtitlesEnabled()},
				{"subtitleText", c.getSubtitleText()},
				{"status", statusJson(c.getStatus(), impl_->hooks)}};
		});
	});

	// ---- presentation ----------------------------------------------------
	// The render loop belongs to main.cpp, so these routes only reach the
	// window through the host's hooks. They are the surface the Controller
	// uses to hide the Player's HUD and go fullscreen.
	auto presentationRoute = [&server, guard, dispatch, parseBody](const std::string& path,
		const std::function<bool()>& getter,
		const std::function<bool(bool)>& setter) {
		server->Get(path, [guard, dispatch, getter](
			const httplib::Request& req, httplib::Response& res) {
			if (!guard(req, res)) return;
			if (!getter) {
				res.status = 200;
				res.set_content(errorJson("this player has no window to report").dump(),
					"application/json");
				return;
			}
			dispatch(res, [getter](MediaPlayerController&) -> Json {
				return Json{{"ok", true}, {"visible", getter()}};
			});
		});

		server->Post(path, [guard, dispatch, parseBody, setter](
			const httplib::Request& req, httplib::Response& res) {
			if (!guard(req, res)) return;
			Json body;
			if (!parseBody(req, res, body)) return;
			if (!body.contains("visible") || !body["visible"].is_boolean()) {
				res.status = 400;
				res.set_content(errorJson("expected {\"visible\": <boolean>}").dump(),
					"application/json");
				return;
			}
			if (!setter) {
				res.status = 200;
				res.set_content(errorJson("this player has no window to change").dump(),
					"application/json");
				return;
			}
			const bool visible = body["visible"].get<bool>();
			dispatch(res, [setter, visible](MediaPlayerController&) -> Json {
				if (!setter(visible)) {
					return errorJson("the host refused the change");
				}
				return Json{{"ok", true}, {"visible", visible}};
			});
		});

		LOG_NOTICE("HttpControlServer") << "  GET/POST " << path
			<< " ({\"visible\": bool} to set)";
	};
	presentationRoute("/api/hud", impl_->hooks.getHud, impl_->hooks.setHud);
	presentationRoute("/api/fullscreen", impl_->hooks.getFullscreen,
		impl_->hooks.setFullscreen);

	// ---- playlist --------------------------------------------------------
	server->Post(R"(/api/clips/(\d+))", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		if (req.matches.size() < 2) {
			res.status = 400;
			res.set_content(errorJson("missing clip index").dump(), "application/json");
			return;
		}
		const std::string indexText = req.matches[1].str();
		for (char ch : indexText) {
			if (ch < '0' || ch > '9') {
				res.status = 400;
				res.set_content(errorJson("invalid clip index").dump(), "application/json");
				return;
			}
		}
		std::size_t index = 0;
		try {
			index = static_cast<std::size_t>(std::stoull(indexText));
		} catch (const std::exception&) {
			res.status = 400;
			res.set_content(errorJson("invalid clip index").dump(), "application/json");
			return;
		}
		dispatch(res, [this, index](MediaPlayerController& c) -> Json {
			if (!c.openClipAtIndex(index)) {
				return errorJson("clip index out of range or failed to load");
			}
			return okWithStatus(c, impl_->hooks);
		});
	});

	// Rescan the data directory for new media, then report the new list.
	server->Post("/api/clips/rescan", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) -> Json {
			const auto* library = dynamic_cast<const MediaClipLibrary*>(&c.clipSource());
			if (library == nullptr) {
				return errorJson("clip source does not support rescan");
			}
			const std::size_t count = c.rescan();
			return Json{{"ok", true},
				{"clipCount", count},
				{"mediaFolder", c.mediaFolder()},
				{"searchLog", library->searchLog()}};
		});
	});

	// ---- media corpus folder --------------------------------------------
	// The folder the playlist is read from. GET is what the Controller draws in
	// its corpus rectangle; POST re-points the library and reloads.
	//
	// There is deliberately NO containment check here, unlike /api/clips/{path}
	// below: choosing the corpus is an operator decision made through a folder
	// picker on their own machine, and it is meaningless to restrict it to one
	// directory. The guard that matters is the one already on every route - the
	// server binds to 127.0.0.1 and refuses any non-loopback client, so this is
	// not a remotely reachable "read any folder" primitive.
	//
	// The Player is the single writer of the setting: it persists the choice to
	// mediaplayer.ini so it survives the next start, whether the request came
	// from the Controller's picker or from curl.
	server->Get("/api/media-dir", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) -> Json {
			return Json{{"ok", true},
				{"mediaFolder", c.mediaFolder()},
				{"clipCount", c.getClips().size()}};
		});
	});

	server->Post("/api/media-dir", [this, guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		if (!body.contains("path") || !body["path"].is_string()) {
			res.status = 400;
			res.set_content(errorJson("expected {\"path\": \"<folder>\"}").dump(),
				"application/json");
			return;
		}
		std::string requested = body["path"].get<std::string>();

		std::error_code ec;
		if (!requested.empty()) {
			// Tolerate forward slashes from a hand-written request; the library
			// compares and reports preferred separators itself.
			requested = std::filesystem::path(requested).make_preferred().string();
			if (!std::filesystem::is_directory(requested, ec)) {
				res.status = 400;
				res.set_content(errorJson("not a folder: " + requested).dump(),
					"application/json");
				return;
			}
		}

		dispatch(res, [this, requested](MediaPlayerController& c) -> Json {
			const std::size_t count = c.setMediaFolder(requested);
			const std::string actual = c.mediaFolder();

			// Persist so the choice survives a restart. A failed write is
			// reported but does not undo the change: the playlist has already
			// moved, and pretending otherwise would be worse than a warning.
			config::Config stored;
			config::load(stored);
			stored.mediaFolder = actual;
			const bool persisted = config::save(stored);

			Json payload = okWithStatus(c, impl_->hooks);
			payload["clipCount"] = count;
			payload["mediaFolder"] = actual;
			payload["persisted"] = persisted;
			if (!persisted) {
				payload["warning"] = "folder changed, but " + config::configPath()
					+ " could not be written; it will not survive a restart";
			}
			return payload;
		});
	});

	// ---- scripts ---------------------------------------------------------
	// Lists what is on disk and what actually loaded. Scripts are discovered
	// only under <data>/scripts and only loaded at startup, because mpv cannot
	// attach or detach a script once running — so this route reports, it does
	// not pretend to hot-reload.
	server->Get("/api/scripts", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) {
			Json onDisk = Json::array();
			for (const scripts::ScriptFile& script : c.scriptFiles()) {
				onDisk.push_back({{"name", script.name},
					{"language", script.language},
					{"path", script.absolutePath}});
			}
			return Json{{"ok", true},
				{"directory", platform::scriptsDirectory()},
				{"onDisk", onDisk},
				{"loaded", c.loadedScripts()},
				{"reloadSupported", false}};
		});
	});

	server->Post("/api/scripts/rescan", [this, guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) -> Json {
			const std::vector<scripts::ScriptFile> found = c.rescanScripts();
			Json names = Json::array();
			for (const scripts::ScriptFile& script : found) {
				names.push_back(script.name);
			}
			return Json{{"ok", true},
				{"onDisk", names},
				{"loaded", c.loadedScripts()},
				{"note", "scripts are attached at startup; restart to apply changes"}};
		});
	});

	impl_->server = std::move(server);

	if (!impl_->server->bind_to_port(kDefaultBindHint, port_)) {
		LOG_ERROR("HttpControlServer") << "failed to bind port " << port_
			<< " — HTTP API disabled";
		impl_->server.reset();
		running_ = false;
		return false;
	}

	impl_->running = true;
	running_ = true;
	impl_->serverThread = std::thread([this] {
		impl_->server->listen_after_bind();   // blocks until stop()
	});

	LOG_NOTICE("HttpControlServer") << "HTTP API listening on http://127.0.0.1:"
		<< port_ << " (localhost clients only)";
	LOG_NOTICE("HttpControlServer") << "  GET  /api/status | /api/clips | /api/position | /api/health";
	LOG_NOTICE("HttpControlServer") << "  POST /api/play | /api/stop | /api/next | /api/previous";
	LOG_NOTICE("HttpControlServer") << "  POST /api/seek | /api/pause | /api/speed | /api/volume | /api/subtitles";
	LOG_NOTICE("HttpControlServer") << "  POST /api/clips/{index} | /api/clips/rescan";	return true;
}

void HttpControlServer::stop() {
	if (!impl_) {
		return;
	}
	running_ = false;
	impl_->stopServer();
}

} // namespace media
