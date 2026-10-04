#pragma once

#include "control/ControllerModel.h"
#include "control/PlayerClient.h"
#include "net/CommandQueue.h"
#include "json.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace media {

class LuaControllerScript;

/// The Controller's own control surface, on its own port.
///
/// The Player has `:8080`; the Controller has `:8081`. This is what lets an
/// operator drive the Controller itself from a shell (or any other client)
/// without going through the Lua script host, and it uses the same
/// submit-and-poll design as the Player's API: HTTP workers never touch the
/// player link, the scene or the Lua state. Each handler builds a ControllerRequest
/// and blocks until the frame loop's poll() has answered it.
///
/// Routes (all localhost-only, like the Player's):
///
///   GET  /api/controller/status             bar + player state
///   GET  /api/controller/scripts            scripts on disk, and which is loaded
///   GET  /api/controller/script-content     ?name=x.lua - the text of one script
///   POST /api/controller/script             {"path":"x.lua"} or {"source":"..."}
///   POST /api/controller/script-save        {"name":"x.lua","text":"..."}
///   POST /api/controller/validate           {"name":"x.lua","text":"..."}
///   POST /api/controller/stop-script        stop the running script
///   POST /api/controller/reload-script      reload the running script from disk
///   POST /api/controller/command            {"command":"next"}, {"open":0}, {"seek":50}
///
/// The script-text routes exist for the Dashboard's editor. They are the only
/// routes in either application that write a file, so they are the strictest:
/// a bare .lua name inside the scripts directory, the text must compile as Lua
/// before it is written, and nothing is ever executed by a save.
///
/// The three things only the host knows. Passed in, never inherited, so this
/// class never sees GL, a window or a Lua state directly and tests can stub the
/// whole surface with value objects.
struct ControllerHost {
	PlayerCommands* player = nullptr;
	ControllerModel* model = nullptr;
	/// Null when scripting could not start; the script routes then report so.
	LuaControllerScript* scripts = nullptr;
};

class ControllerHttpServer {
public:
	/// The Player's control API, from the Controller's side. Declared here as
	/// well as on the Player because this is the single number the two
	/// applications have to agree on, and the Dashboard quotes it too.
	static constexpr int kPlayerPort = 8080;
	static constexpr int kDefaultPort = 8081;
	static constexpr const char* kDefaultBindHint = "127.0.0.1";

	using Json = nlohmann::json;

	/// One unit of work for the main thread. A plain value, so it can be built
	/// and inspected in tests without starting a server.
	struct Request {
		enum class Kind {
			Status,
			Control,
			OpenClip,
			SeekPercent,
			RunScript,
			RunSource,
			StopScript,
			ReloadScript,
			RescanScripts,
			/// Read one script's text, for the Dashboard's editor.
			ReadScript,
			/// Write one script's text back, after checking that it compiles.
			SaveScript,
			/// Compile a draft without running it, and report the line it failed
			/// on. This is what puts an error marker on the right line while the
			/// operator types, instead of only at the next run.
			ValidateScript,
		};

		Kind kind = Kind::Status;
		ControlCommand command = ControlCommand::None;
		std::size_t clipIndex = 0;
		double value = 0.0;
		/// The source text, for RunSource and for the two script-text routes.
		std::string text;
		/// A bare script name, for ReadScript and SaveScript.
		std::string name;
	};

	explicit ControllerHttpServer(ControllerHost host);
	~ControllerHttpServer();

	ControllerHttpServer(const ControllerHttpServer&) = delete;
	ControllerHttpServer& operator=(const ControllerHttpServer&) = delete;

	/// Start listening. Returns false (and logs why) when the port is taken;
	/// the bar keeps working, it just has no external API.
	bool start(int port);
	void stop();
	bool isRunning() const { return running_; }
	int port() const { return port_; }

	/// Main thread: run every queued request. Called once per frame.
	void poll();

	/// Execute one request. Exposed so tests can drive the whole
	/// request -> command translation with a stub host and no socket.
	static Json execute(ControllerHost host, int port, bool running,
		const Request& request);

	/// Translate a wire command name to a ControlCommand. Exposed so the route
	/// table, the Lua host and the tests share one spelling.
	static ControlCommand commandFromName(const std::string& name);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	ControllerHost host_;
	bool running_ = false;
	int port_ = kDefaultPort;
};

} // namespace media
