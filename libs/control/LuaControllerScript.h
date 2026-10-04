#pragma once

#include "control/ControllerModel.h"
#include "control/PlayerClient.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace media {

/// A Lua script discovered under `<data>/controller-scripts`.
struct ControllerScriptFile {
	std::string absolutePath;
	std::string name;
};

/// Every loadable script in `<data>/controller-scripts`, sorted by name.
///
/// Same security posture as the Player's script host: exactly one named
/// directory, never the user's own Lua paths, and nothing is executed
/// implicitly. Returns empty (not an error) when the directory is absent.
std::vector<ControllerScriptFile> discoverControllerScripts();

/// Resolve a script *name* to a file under `<data>/controller-scripts`.
///
/// The name must be one discovery actually found — a bare `x.lua`, never a
/// path with a directory component — so a route that takes a name cannot reach
/// `..\..\anything.lua` or an absolute path elsewhere on disk. Returns an
/// empty string when there is no such script.
///
/// This exists because "just open the name you were given" resolves against
/// the process's working directory, not the data directory, so the scripts
/// route 404'd every script that discovery had just listed.
std::string findControllerScript(const std::string& name);

/// The Controller's scripting host: an embedded Lua 5.1 state that drives the
/// Player through the PlayerCommands seam.
///
/// Threading: one instance, owned and used only by the frame loop. HTTP workers
/// never touch the Lua state — the Controller's own API queues its work like
/// every other request.
///
/// The API a script sees is deliberately small and media-shaped:
///
///     controller.Log(msg)            controller.Play(index)
///     controller.Sleep(ms)           controller.SeekPercent(0..100)
///     controller.Next()              controller.Previous()
///     controller.Pause()             controller.SetVolume(0..100)
///     controller.SetSpeed(factor)    controller.SetSubtitles(bool)
///     controller.ShowHUD(bool)       controller.Fullscreen(bool)
///     controller.CurrentClip()       controller.Playlist()
///     controller.OnTick(function() end)
///
/// Release semantics: mpv's scripts cannot be unloaded once attached, so the
/// Player's host reports honestly that a reload needs a restart. This host owns
/// its own state, so a reload here really is a reload — there is no restart.
///
/// Two guards keep the bar responsive whatever a script does:
///   - a per-tick VM instruction budget, which aborts a loop that never yields;
///   - `Sleep(ms)` charges a wall-clock budget instead of blocking, so a script
///     observes a rolling window and a long sequence runs across frames.
class LuaControllerScript {
public:
	/// Per-tick wall-clock budget for `Sleep`, in milliseconds: 100ms is about
	/// six frames at 60fps, which is plenty for a sequence step and far short of
	/// a stall a person would notice.
	static constexpr double kTickBudgetClockMs = 100.0;
	/// Per-tick VM instruction budget, for scripts that never sleep.
	static constexpr int kInstructionBudget = 2000000;
	/// After a failure, do not retry the script for this long.
	static constexpr double kErrorBackoffSeconds = 2.0;

	LuaControllerScript();
	~LuaControllerScript();

	LuaControllerScript(const LuaControllerScript&) = delete;
	LuaControllerScript& operator=(const LuaControllerScript&) = delete;

	/// Where command calls go. Must outlive this object. Never null after
	/// initialize() in practice, but every call is guarded anyway.
	void setCommands(PlayerCommands* commands) { commands_ = commands; }

	/// Create the Lua state and register the API. Returns false only when Lua
	/// itself could not start, in which case the bar runs without scripting.
	bool initialize();
	bool ready() const { return lua_ != nullptr; }

	/// Load and run `path` now, replacing any running script. Returns false and
	/// fills `error` on a syntax or runtime failure.
	bool runFile(const std::string& path, std::string& error);
	/// Run a source string verbatim. Used by tests and by the Controller's API.
	bool runSource(const std::string& source, const std::string& chunkName,
		std::string& error);

	/// Stop the running script and drop its OnTick handler.
	void stopScript();
	bool scriptRunning() const { return !currentScript_.empty(); }
	const std::string& currentScript() const { return currentScript_; }
	/// The loaded script registered an OnTick handler.
	bool hasTickHandler() const { return tickRef_ != kTickRefNone; }

	/// Called once per frame. Runs the OnTick handler with a fresh budget.
	/// Returns false when the script raised an error this tick; it is reported
	/// through lastError() and the script is stopped with a backoff.
	bool tick();

	const std::string& lastError() const { return lastError_; }
	/// The last `Log()` line a script emitted, for the bar's status strip.
	const std::string& lastLog() const { return lastLog_; }

	/// Reload the loaded file when its modification time changed. Called by the
	/// frame loop a few times a second, which is what makes editing a script on
	/// disk take effect without a restart. Returns true when it reloaded.
	bool reloadIfChanged();

	/// Reload unconditionally, for the R key. Keeps the previous script on
	/// failure — the file association survives, so a fix can be reloaded — and
	/// reports why it failed.
	bool forceReload(std::string& error);

	/// The file currently loaded, as reloadIfChanged/forceReload remember it.
	const std::string& loadedPath() const { return loadedPath_; }

	// ---------------------------------------------------------------------
	// Host surface. Public because the Lua C functions reach it through the
	// light-userdata upvalue and cannot be members of the class.
	// ---------------------------------------------------------------------

	/// Charge the current tick's wall-clock budget. True while time remains.
	bool chargeTickBudget(double ms);

	/// Called by the instruction hook once per `count` VM instructions with
	/// `count` = kInstructionBudget. Returns false once the tick's instruction
	/// allowance is spent, which makes the hook abort the script.
	bool spendInstructions(int count);

	/// Raise a Lua error when the budget is exhausted; used by Sleep. Lua's
	/// luaL_error longjmps, but the 5.1 header does not say so, hence no
	/// [[noreturn]] here.
	void raiseBudgetExhausted();
	void raisePlayerError(const std::string& error);

	/// Record a command failure from a script, for the bar's status strip.
	void noteScriptError(const std::string& message);
	void noteLog(const std::string& message);
	void storeTickHandler();
	std::vector<PlayerClipInfo> scriptPlaylist() const;
	ControllerState scriptState() const;
	PlayerCommands* commands() { return commands_; }

private:
	void registerApi();
	bool callTickHandler();
	void setError(std::string message);

	lua_State* lua_ = nullptr;
	PlayerCommands* commands_ = nullptr;

	std::string currentScript_;
	std::string lastError_;
	std::string lastLog_;

	/// Registry reference to the OnTick function, or kTickRefNone.
	int tickRef_ = kTickRefNone;
	/// Wall-clock charged for the current tick.
	double tickSpentMs_ = 0.0;
	/// Instructions remaining before the hook aborts the script.
	int instructionsLeft_ = 0;

	std::string loadedPath_;
	/// Last-write time and size of `loadedPath_` as they were when it loaded.
	/// Both are compared, because `std::filesystem::last_write_time` has
	/// **1-second** granularity on MinGW (measured, not assumed): two saves in
	/// the same second share a timestamp, and the size catches the usual case
	/// of a script that changed length. The R key is the explicit fallback.
	long long loadedMtime_ = 0;
	std::uintmax_t loadedSize_ = 0;
	std::chrono::steady_clock::time_point errorAt_{};

	static constexpr int kTickRefNone = -2;
};

} // namespace media
