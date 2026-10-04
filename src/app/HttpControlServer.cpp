#include "app/HttpControlServer.h"

#include "core/Log.h"
#include "core/Platform.h"
#include "media/IClipSource.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"

#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace media {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxRequestBody = 8192;

/// One unit of work submitted by an HTTP worker, executed on the main thread.
struct Command {
	std::function<Json()> run;
	Json result;
	bool done = false;
};

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

Json statusJson(const MediaPlayerStatus& status) {
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
	};
}

Json errorJson(const std::string& message) {
	return Json{{"ok", false}, {"error", message}};
}

Json okWithStatus(const MediaPlayerController& controller) {
	return Json{{"ok", true}, {"status", statusJson(controller.getStatus())}};
}

} // namespace

std::string statusToJsonText(const MediaPlayerController& controller) {
	return statusJson(controller.getStatus()).dump();
}

// ---------------------------------------------------------------------------
// Impl: owns the queue, the httplib server and its listen thread.
// ---------------------------------------------------------------------------
struct HttpControlServer::Impl {
	explicit Impl(MediaPlayerController& c) : controller(c) {}

	~Impl() { stopServer(); }

	std::mutex queueMutex;
	std::condition_variable queueCv;    // wakes the main thread
	std::condition_variable resultCv;   // wakes blocked workers
	std::deque<std::shared_ptr<Command>> queue;
	bool shuttingDown = false;

	std::mutex serverMutex;
	std::unique_ptr<httplib::Server> server;
	std::thread serverThread;
	std::atomic<bool> running{false};

	MediaPlayerController& controller;

	/// Submit work and block until the main thread produces a result.
	/// Returns nullopt when shutting down, so a worker can never wait forever
	/// for a poll() that will not come.
	std::optional<Json> submit(std::function<Json()> work) {
		auto command = std::make_shared<Command>();
		command->run = std::move(work);

		{
			std::unique_lock<std::mutex> lock(queueMutex);
			if (shuttingDown) {
				return std::nullopt;
			}
			queue.push_back(command);
		}
		queueCv.notify_one();

		std::unique_lock<std::mutex> lock(queueMutex);
		resultCv.wait(lock, [&] { return command->done || shuttingDown; });
		if (!command->done) {
			return std::nullopt;
		}
		return command->result;
	}

	/// Main thread: run everything queued, outside the lock so one long command
	/// cannot stall other submitters.
	void drain() {
		std::deque<std::shared_ptr<Command>> batch;
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			batch.swap(queue);
		}
		for (auto& command : batch) {
			Json result;
			try {
				result = command->run();
			} catch (const std::exception& e) {
				result = errorJson(std::string("command failed: ") + e.what());
			}
			{
				std::lock_guard<std::mutex> lock(queueMutex);
				command->result = std::move(result);
				command->done = true;
			}
			resultCv.notify_all();
		}
	}

	/// Main thread: block briefly when there is nothing to do. Keeps the API
	/// responsive without busy-spinning the render loop.
	void waitForWork(int timeoutMs) {
		std::unique_lock<std::mutex> lock(queueMutex);
		if (!queue.empty() || shuttingDown) {
			return;
		}
		queueCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[&] { return !queue.empty() || shuttingDown; });
	}

	void failAllWaiters() {
		std::lock_guard<std::mutex> lock(queueMutex);
		shuttingDown = true;
		queue.clear();
		resultCv.notify_all();
		queueCv.notify_all();
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
HttpControlServer::HttpControlServer(MediaPlayerController& controller)
	: impl_(std::make_unique<Impl>(controller)) {}

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

	server->Get("/api/status", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) { return statusJson(c.getStatus()); });
	});

	server->Get("/api/position", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) {
			const MediaPlayerStatus s = c.getStatus();
			return Json{{"position", s.position}, {"duration", s.duration},
				{"seekable", s.seekable}, {"speed", s.speed}, {"paused", s.paused}};
		});
	});

	server->Get("/api/clips", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
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
	server->Post("/api/play", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) { c.play(); return okWithStatus(c); });
	});

	server->Post("/api/stop", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) { c.stop(); return okWithStatus(c); });
	});

	server->Post("/api/next", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) { c.nextClip(); return okWithStatus(c); });
	});

	server->Post("/api/previous", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) { c.previousClip(); return okWithStatus(c); });
	});

	server->Post("/api/pause", [guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body = Json::object();
		// A pause request with no body means "toggle".
		if (!req.body.empty() && !parseBody(req, res, body)) return;
		dispatch(res, [body](MediaPlayerController& c) -> Json {
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
			return okWithStatus(c);
		});
	});

	// ---- seek / rate / volume -------------------------------------------
	server->Post("/api/seek", [guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		dispatch(res, [body](MediaPlayerController& c) -> Json {
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
			return okWithStatus(c);
		});
	});

	server->Post("/api/speed", [guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		if (!body.contains("speed") || !body["speed"].is_number()) {
			res.status = 400;
			res.set_content(errorJson("expected {\"speed\": <number>}").dump(), "application/json");
			return;
		}
		const double speed = body["speed"].get<double>();
		dispatch(res, [speed](MediaPlayerController& c) -> Json {
			if (!c.setSpeed(speed)) {
				return errorJson("speed out of range (0.01..100) or nothing loaded");
			}
			return okWithStatus(c);
		});
	});

	server->Post("/api/volume", [guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		if (!body.contains("volume") || !body["volume"].is_number()) {
			res.status = 400;
			res.set_content(errorJson("expected {\"volume\": <0..100>}").dump(), "application/json");
			return;
		}
		const double volume = body["volume"].get<double>();
		dispatch(res, [volume](MediaPlayerController& c) -> Json {
			if (!c.setVolume(volume)) {
				return errorJson("volume out of range (0..100)");
			}
			return okWithStatus(c);
		});
	});

	// ---- subtitles -------------------------------------------------------
	server->Post("/api/subtitles", [guard, dispatch, parseBody](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		dispatch(res, [body](MediaPlayerController& c) -> Json {
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
				{"status", statusJson(c.getStatus())}};
		});
	});

	// ---- playlist --------------------------------------------------------
	server->Post(R"(/api/clips/(\d+))", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
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
		dispatch(res, [index](MediaPlayerController& c) -> Json {
			if (!c.openClipAtIndex(index)) {
				return errorJson("clip index out of range or failed to load");
			}
			return okWithStatus(c);
		});
	});

	// Rescan the data directory for new media, then report the new list.
	server->Post("/api/clips/rescan", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, [](MediaPlayerController& c) -> Json {
			auto* library = dynamic_cast<MediaClipLibrary*>(&c.clipSource());
			if (library == nullptr) {
				return errorJson("clip source does not support rescan");
			}
			library->scan();
			return Json{{"ok", true},
				{"clipCount", library->size()},
				{"searchLog", library->searchLog()}};
		});
	});

	// ---- scripts ---------------------------------------------------------
	// Lists what is on disk and what actually loaded. Scripts are discovered
	// only under <data>/scripts and only loaded at startup, because mpv cannot
	// attach or detach a script once running — so this route reports, it does
	// not pretend to hot-reload.
	server->Get("/api/scripts", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
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

	server->Post("/api/scripts/rescan", [guard, dispatch](const httplib::Request& req, httplib::Response& res) {
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
	LOG_NOTICE("HttpControlServer") << "  POST /api/clips/{index} | /api/clips/rescan";
	return true;
}

void HttpControlServer::stop() {
	if (!impl_) {
		return;
	}
	running_ = false;
	impl_->stopServer();
}

} // namespace media
