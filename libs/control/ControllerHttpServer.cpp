#include "control/ControllerHttpServer.h"

#include "control/LuaControllerScript.h"
#include "core/Log.h"
#include "core/Platform.h"

#include "httplib.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace media {
namespace {

using Json = ControllerHttpServer::Json;
using Request = ControllerHttpServer::Request;
using Kind = ControllerHttpServer::Request::Kind;

constexpr std::size_t kMaxRequestBody = 32768;

bool isLoopback(const std::string& ip) {
	return ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1"
		|| ip == "localhost" || ip.empty();
}

Json errorJson(const std::string& message) {
	return Json{{"ok", false}, {"error", message}};
}

Json stateJson(const ControllerState& state) {
	return Json{
		{"online", state.online},
		{"loaded", state.loaded},
		{"playing", state.playing},
		{"paused", state.paused},
		{"isImage", state.isImage},
		{"seekable", state.seekable},
		{"clipIndex", state.clipIndex},
		{"clipCount", state.clipCount},
		{"clipName", state.clipName},
		{"position", state.position},
		{"duration", state.duration},
		{"speed", state.speed},
		{"volume", state.volume},
		{"subtitlesEnabled", state.subtitlesEnabled},
		{"hudVisible", state.hudVisible},
		{"fullscreen", state.fullscreen},
		{"lastError", state.lastError},
	};
}

/// The directory scripts are discovered under, for the status payload.
std::string scriptsRoot() {
	return platform::dataDirectory() + "\\controller-scripts";
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
//
// The queue is CommandQueue<Json>: an HTTP worker hands over a Request and
// blocks; the frame loop's poll() runs it through execute() below on the main
// thread. The back-pointer to the host is set by start(), after the vtable is
// established, so no virtual is ever called during construction.
// ---------------------------------------------------------------------------
struct ControllerHttpServer::Impl {
	/// Port the server bound, reported in /api/controller/status.
	int port = ControllerHttpServer::kDefaultPort;
	CommandQueue<Json> queue;
	std::unique_ptr<httplib::Server> server;
	std::thread serverThread;
	std::atomic<bool> running{false};

	void stopServer() {
		running = false;
		queue.failAllWaiters();
		if (server) {
			server->stop();
		}
		if (serverThread.joinable()) {
			serverThread.join();
		}
		server.reset();
	}
};

Json ControllerHttpServer::execute(ControllerHost host, int port, bool running,
	const Request& request) {
	LuaControllerScript* scripts = host.scripts;
	const bool needsScripts = request.kind == Kind::RunScript
		|| request.kind == Kind::RunSource || request.kind == Kind::StopScript
		|| request.kind == Kind::ReloadScript;
	if (needsScripts && scripts == nullptr) {
		return errorJson("scripting is not available in this build");
	}
	if (host.player == nullptr || host.model == nullptr) {
		return errorJson("controller is not wired up");
	}

	switch (request.kind) {
		case Kind::Status:
			return Json{{"ok", true},
				{"controller", {
					{"port", port},
					{"running", running},
					{"scriptDirectory", scriptsRoot()},
					{"script", scripts != nullptr ? Json(scripts->currentScript()) : Json(nullptr)},
					{"scriptError", scripts != nullptr ? Json(scripts->lastError()) : Json(nullptr)},
					{"tickHandler", scripts != nullptr ? Json(scripts->hasTickHandler()) : Json(nullptr)},
				}},
				{"player", stateJson(host.model->state())}};

		case Kind::RescanScripts: {
			Json names = Json::array();
			for (const ControllerScriptFile& script : discoverControllerScripts()) {
				names.push_back(script.name);
			}
			return Json{{"ok", true},
				{"directory", scriptsRoot()},
				{"onDisk", names},
				{"loaded", scripts != nullptr ? scripts->currentScript() : std::string()}};
		}

		case Kind::RunScript: {
			// The body carries a name from GET /api/controller/scripts, not a
			// path: resolve it against the one directory scripts live in, so
			// this route cannot open anything else on disk.
			const std::string path = findControllerScript(request.text);
			if (path.empty()) {
				return errorJson("no such script: " + request.text
					+ " (see GET /api/controller/scripts)");
			}
			std::string error;
			if (!scripts->runFile(path, error)) {
				return errorJson(error);
			}
			return Json{{"ok", true}, {"script", scripts->currentScript()},
				{"tickHandler", scripts->hasTickHandler()}};
		}

		case Kind::RunSource: {
			std::string error;
			if (!scripts->runSource(request.text, "controller-api", error)) {
				return errorJson(error);
			}
			return Json{{"ok", true}, {"script", scripts->currentScript()},
				{"tickHandler", scripts->hasTickHandler()}};
		}

		case Kind::StopScript:
			scripts->stopScript();
			return Json{{"ok", true}};

		case Kind::ReloadScript: {
			std::string error;
			if (!scripts->forceReload(error)) {
				return errorJson(error);
			}
			return Json{{"ok", true}, {"script", scripts->currentScript()}};
		}

		case Kind::Control:
		case Kind::OpenClip:
		case Kind::SeekPercent: {
			PlayerCommands& player = *host.player;
			std::string error;
			bool ok = false;
			switch (request.kind) {
				case Kind::Control:
					ok = player.send(request.command, request.value, error);
					break;
				case Kind::OpenClip:
					ok = player.openClip(request.clipIndex, error);
					break;
				case Kind::SeekPercent:
					ok = player.seekPercent(request.value, error);
					break;
				default:
					break;
			}
			if (!ok) {
				return errorJson(error.empty() ? "the player did not accept the request" : error);
			}
			return Json{{"ok", true}, {"player", stateJson(host.model->state())}};
		}
	}

	return errorJson("unsupported request");
}

ControlCommand ControllerHttpServer::commandFromName(const std::string& name) {
	// One spelling per command, shared by the wire protocol, the tests and the
	// reply payloads. Aliases are accepted because an operator types them.
	if (name == "play-pause" || name == "playpause" || name == "pause") {
		return ControlCommand::PlayPause;
	}
	if (name == "play") return ControlCommand::PlayPause;
	if (name == "next") return ControlCommand::Next;
	if (name == "previous" || name == "prev") return ControlCommand::Previous;
	if (name == "stop") return ControlCommand::Stop;
	if (name == "toggle-hud" || name == "hud") return ControlCommand::ToggleHud;
	if (name == "toggle-fullscreen" || name == "fullscreen") {
		return ControlCommand::ToggleFullscreen;
	}
	if (name == "toggle-subtitles" || name == "subtitles") {
		return ControlCommand::ToggleSubtitles;
	}
	return ControlCommand::None;
}

ControllerHttpServer::ControllerHttpServer(ControllerHost host)
	: impl_(std::make_unique<Impl>()), host_(host) {}

ControllerHttpServer::~ControllerHttpServer() {
	stop();
}

bool ControllerHttpServer::start(int port) {
	if (running_) {
		return true;
	}
	port_ = port;
	impl_->port = port;

	auto server = std::make_unique<httplib::Server>();
	server->set_payload_max_length(kMaxRequestBody);
	server->set_read_timeout(5, 0);
	server->set_write_timeout(5, 0);
	server->set_keep_alive_max_count(1);

	auto guard = [](const httplib::Request& req, httplib::Response& res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		if (!isLoopback(req.remote_addr)) {
			LOG_WARN("ControllerApi") << "rejected non-local client " << req.remote_addr;
			res.status = 403;
			res.set_content(errorJson("localhost only").dump(), "application/json");
			return false;
		}
		return true;
	};

	// A worker submits and blocks; the main thread answers. The thunk carries
	// the host by value, so it never refers to a worker's stack frame.
	auto dispatch = [this](httplib::Response& res, const Request& request) {
		const ControllerHost host = host_;
		const int port = port_;
		auto result = impl_->queue.submit([host, port, request] {
			return ControllerHttpServer::execute(host, port, true, request);
		});
		if (!result) {
			res.status = 503;
			res.set_content(errorJson("controller is shutting down").dump(),
				"application/json");
			return;
		}
		res.status = 200;
		res.set_content(result->dump(), "application/json");
	};

	auto parseBody = [](const httplib::Request& req, httplib::Response& res, Json& out) {
		out = Json::parse(req.body, nullptr, false);
		if (out.is_discarded()) {
			res.status = 400;
			res.set_content(errorJson("expected JSON body").dump(), "application/json");
			return false;
		}
		return true;
	};

	server->Get("/api/controller/status", [guard, dispatch](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		dispatch(res, Request{});
	});

	server->Get("/api/controller/scripts", [guard, dispatch](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Request request;
		request.kind = Kind::RescanScripts;
		dispatch(res, request);
	});

	server->Post("/api/controller/scripts/rescan", [guard, dispatch](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Request request;
		request.kind = Kind::RescanScripts;
		dispatch(res, request);
	});

	server->Post("/api/controller/script", [guard, dispatch, parseBody](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		Request request;
		if (body.contains("source") && body["source"].is_string()) {
			request.kind = Kind::RunSource;
			request.text = body["source"].get<std::string>();
		} else if (body.contains("path") && body["path"].is_string()) {
			request.kind = Kind::RunScript;
			request.text = body["path"].get<std::string>();
		} else {
			res.status = 400;
			res.set_content(errorJson(
				"expected {\"path\": \"name.lua\"} or {\"source\": \"...\"}").dump(),
				"application/json");
			return;
		}
		dispatch(res, request);
	});

	server->Post("/api/controller/stop-script", [guard, dispatch](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Request request;
		request.kind = Kind::StopScript;
		dispatch(res, request);
	});

	server->Post("/api/controller/reload-script", [guard, dispatch](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Request request;
		request.kind = Kind::ReloadScript;
		dispatch(res, request);
	});

	server->Post("/api/controller/command", [guard, dispatch, parseBody](
		const httplib::Request& req, httplib::Response& res) {
		if (!guard(req, res)) return;
		Json body;
		if (!parseBody(req, res, body)) return;
		Request request;

		// "open" carries a clip index and "seek" a percentage; every other
		// command is a bare name.
		if (body.contains("open") && body["open"].is_number_integer()) {
			request.kind = Kind::OpenClip;
			request.clipIndex = static_cast<std::size_t>(
				std::max<long long>(0, body["open"].get<long long>()));
		} else if (body.contains("seek") && body["seek"].is_number()) {
			request.kind = Kind::SeekPercent;
			request.value = body["seek"].get<double>();
		} else if (body.contains("command") && body["command"].is_string()) {
			const std::string name = body["command"].get<std::string>();
			const ControlCommand command = commandFromName(name);
			if (command == ControlCommand::None) {
				res.status = 400;
				res.set_content(errorJson("unknown command: " + name).dump(),
					"application/json");
				return;
			}
			request.kind = Kind::Control;
			request.command = command;
		} else {
			res.status = 400;
			res.set_content(errorJson(
				"expected {\"command\": \"next\"}, {\"open\": 0} or {\"seek\": 50}").dump(),
				"application/json");
			return;
		}
		dispatch(res, request);
	});

	impl_->server = std::move(server);
	if (!impl_->server->bind_to_port(kDefaultBindHint, port_)) {
		LOG_WARN("ControllerApi") << "port " << port_
			<< " unavailable; the Controller API is disabled";
		impl_->server.reset();
		running_ = false;
		return false;
	}

	impl_->running = true;
	running_ = true;
	impl_->serverThread = std::thread([this] {
		impl_->server->listen_after_bind();
	});

	LOG_NOTICE("ControllerApi") << "Controller API on http://127.0.0.1:" << port_;
	LOG_NOTICE("ControllerApi") << "  GET  /api/controller/status | /api/controller/scripts";
	LOG_NOTICE("ControllerApi") << "  POST /api/controller/script | /api/controller/stop-script";
	LOG_NOTICE("ControllerApi") << "  POST /api/controller/reload-script | /api/controller/command";
	return true;
}

void ControllerHttpServer::poll() {
	if (!running_) {
		return;
	}
	impl_->queue.drain(errorJson);
}
void ControllerHttpServer::stop() {
	if (!impl_) {
		return;
	}
	running_ = false;
	impl_->stopServer();
}

} // namespace media
