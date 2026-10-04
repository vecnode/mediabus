#include "app/control/LuaControllerScript.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace media {
namespace {

/// Lua 5.1 has no LUA_OK; status 0 is success. Spelled out because the usual
/// `status != LUA_OK` does not compile against the 5.1 headers.
constexpr int kLuaOk = 0;

/// The host a Lua API function belongs to. Set for the duration of one script
/// run and read by the instruction hook, which the C API does not give a
/// context pointer for. Lua is single-threaded here, so a thread_local is
/// exactly as safe as the state itself.
thread_local LuaControllerScript* gActiveHost = nullptr;

/// Instruction hook: every `kInstructionBudget` VM instructions, check the
/// remaining allowance and abort with a normal Lua error when it is gone.
/// Without this, `while true do end` would hang the frame loop.
void instructionHook(lua_State* L, lua_Debug*) {
	LuaControllerScript* host = gActiveHost;
	if (host == nullptr) {
		return;
	}
	if (host->spendInstructions(LuaControllerScript::kInstructionBudget)) {
		return;
	}
	luaL_error(L, "controller script exceeded its instruction budget for one tick; "
		"use OnTick to spread work across frames");
}

/// The host object handed to every API function as the Lua `this`.
LuaControllerScript* hostOf(lua_State* L) {
	return static_cast<LuaControllerScript*>(lua_touserdata(L, lua_upvalueindex(1)));
}

/// Build a readable error with a traceback, and pop the Lua error object.
std::string luaErrorText(lua_State* L, const std::string& where) {
	const char* message = lua_tostring(L, -1);
	const std::string raw = (message != nullptr) ? message : "unknown Lua error";
	lua_pop(L, 1);
	return where + ": " + raw;
}

// ---------------------------------------------------------------------------
// API functions
// ---------------------------------------------------------------------------

int apiLog(lua_State* L) {
	LuaControllerScript* host = hostOf(L);
	const char* message = luaL_optstring(L, 1, "");
	if (host != nullptr) {
		host->noteLog(message);
	}
	LOG_NOTICE("ControllerScript") << message;
	lua_pushboolean(L, 1);
	return 1;
}

/// Sleep(ms): charge the tick budget instead of blocking the frame loop.
int apiSleep(lua_State* L) {
	LuaControllerScript* host = hostOf(L);
	const double ms = luaL_optnumber(L, 1, 100.0);
	if (host == nullptr) {
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool within = host->chargeTickBudget(ms);
	lua_pushboolean(L, within ? 1 : 0);
	return 1;
}

/// Every transport helper has the same shape: call through the seam, turn a
/// refusal into a Lua error naming the reason, return true on success.
#define CONTROL_FN(signature, expression)                                     \
	int signature {                                                           \
		LuaControllerScript* host = hostOf(L);                                \
		if (host == nullptr || host->commands() == nullptr) {                 \
			lua_pushboolean(L, 0);                                            \
			return 1;                                                         \
		}                                                                     \
		std::string error;                                                    \
		if (!(expression)) {                                                  \
			if (error.empty()) { error = "the player refused the command"; }   \
			host->noteScriptError(error);                                     \
			host->raisePlayerError(error);                                    \
		}                                                                     \
		lua_pushboolean(L, 1);                                                \
		return 1;                                                             \
	}

CONTROL_FN(apiPlay(lua_State* L),
	host->commands()->openClip(static_cast<std::size_t>(
		std::max<lua_Integer>(0, luaL_checkinteger(L, 1))), error))

CONTROL_FN(apiNext(lua_State* L), host->commands()->next(error))
CONTROL_FN(apiPrevious(lua_State* L), host->commands()->previous(error))
CONTROL_FN(apiPause(lua_State* L), host->commands()->playPause(error))
CONTROL_FN(apiStop(lua_State* L), host->commands()->stop(error))

CONTROL_FN(apiSeekPercent(lua_State* L),
	host->commands()->seekPercent(luaL_checknumber(L, 1), error))

CONTROL_FN(apiSetVolume(lua_State* L),
	host->commands()->setVolume(luaL_checknumber(L, 1), error))

CONTROL_FN(apiSetSpeed(lua_State* L),
	host->commands()->setSpeed(luaL_checknumber(L, 1), error))

CONTROL_FN(apiSetSubtitles(lua_State* L),
	host->commands()->setSubtitles(lua_toboolean(L, 1) != 0, error))

CONTROL_FN(apiShowHud(lua_State* L),
	host->commands()->setHud(lua_toboolean(L, 1) != 0, error))

CONTROL_FN(apiFullscreen(lua_State* L),
	host->commands()->setFullscreen(lua_toboolean(L, 1) != 0, error))

#undef CONTROL_FN

/// Playlist(): an array of {index=, name=, type=}.
int apiPlaylist(lua_State* L) {
	LuaControllerScript* host = hostOf(L);
	lua_newtable(L);
	if (host == nullptr) {
		return 1;
	}
	const std::vector<PlayerClipInfo> clips = host->scriptPlaylist();
	for (std::size_t i = 0; i < clips.size(); ++i) {
		lua_newtable(L);
		lua_pushinteger(L, static_cast<lua_Integer>(clips[i].index));
		lua_setfield(L, -2, "index");
		lua_pushstring(L, clips[i].name.c_str());
		lua_setfield(L, -2, "name");
		lua_pushstring(L, clips[i].mediaType.c_str());
		lua_setfield(L, -2, "type");
		lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

/// CurrentClip(): the Player's identity as the bar last saw it.
int apiCurrentClip(lua_State* L) {
	LuaControllerScript* host = hostOf(L);
	lua_newtable(L);
	if (host == nullptr) {
		return 1;
	}
	const ControllerState state = host->scriptState();
	lua_pushboolean(L, state.online ? 1 : 0);
	lua_setfield(L, -2, "online");
	lua_pushinteger(L, static_cast<lua_Integer>(state.clipIndex));
	lua_setfield(L, -2, "index");
	lua_pushinteger(L, static_cast<lua_Integer>(state.clipCount));
	lua_setfield(L, -2, "count");
	lua_pushstring(L, state.clipName.c_str());
	lua_setfield(L, -2, "name");
	lua_pushboolean(L, state.playing ? 1 : 0);
	lua_setfield(L, -2, "playing");
	lua_pushboolean(L, state.paused ? 1 : 0);
	lua_setfield(L, -2, "paused");
	lua_pushboolean(L, state.isImage ? 1 : 0);
	lua_setfield(L, -2, "isImage");
	lua_pushnumber(L, state.position);
	lua_setfield(L, -2, "position");
	lua_pushnumber(L, state.duration);
	lua_setfield(L, -2, "duration");
	lua_pushnumber(L, state.speed);
	lua_setfield(L, -2, "speed");
	lua_pushnumber(L, state.volume);
	lua_setfield(L, -2, "volume");
	return 1;
}

/// OnTick(fn): register the once-per-frame handler.
int apiOnTick(lua_State* L) {
	LuaControllerScript* host = hostOf(L);
	if (host == nullptr || !lua_isfunction(L, 1)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushvalue(L, 1);
	host->storeTickHandler();
	lua_pushboolean(L, 1);
	return 1;
}

int luaPanic(lua_State* L) {
	const char* message = lua_tostring(L, -1);
	LOG_ERROR("ControllerScript") << "unprotected Lua error: "
		<< (message != nullptr ? message : "(unknown)");
	return 0;
}

/// Current last-write time and size of a file, or false when it cannot be read.
///
/// Both are needed. `std::filesystem::last_write_time` was measured on this
/// toolchain and has **1-second** granularity, so two saves inside one second
/// carry the same timestamp; the size catches the ordinary case of a script
/// whose text changed length.
bool fileStamp(const std::string& path, long long& mtime, std::uintmax_t& size) {
	std::error_code ec;
	const auto writeTime = std::filesystem::last_write_time(path, ec);
	if (ec) {
		return false;
	}
	mtime = static_cast<long long>(writeTime.time_since_epoch().count());
	size = std::filesystem::file_size(path, ec);
	return !ec;
}

} // namespace

std::vector<ControllerScriptFile> discoverControllerScripts() {
	std::vector<ControllerScriptFile> found;
	std::error_code ec;
	const std::filesystem::path root = std::filesystem::path(
		platform::dataDirectory()) / "controller-scripts";
	if (!std::filesystem::is_directory(root, ec)) {
		return found;
	}
	for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
		if (ec) {
			break;
		}
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		const std::string path = entry.path().string();
		if (platform::lowerExtension(path) != ".lua") {
			continue;
		}
		ControllerScriptFile file;
		file.absolutePath = path;
		file.name = entry.path().filename().string();
		found.push_back(std::move(file));
	}
	std::sort(found.begin(), found.end(),
		[](const ControllerScriptFile& a, const ControllerScriptFile& b) {
			return a.name < b.name;
		});
	return found;
}

std::string findControllerScript(const std::string& name) {
	// A name, not a path: reject anything with a directory component before
	// touching the filesystem, so neither "../x.lua" nor "C:\x.lua" can match.
	const std::filesystem::path wanted(name);
	if (wanted.empty() || wanted.filename() != wanted) {
		return {};
	}
	const std::string leaf = wanted.filename().string();
	// Windows filenames are case-insensitive, so a name must resolve whether an
	// operator typed it as it appears in a directory listing or not. ASCII is
	// enough here: the extension filter has already restricted this to .lua.
	const auto same = [](const std::string& a, const std::string& b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			const unsigned char ca = static_cast<unsigned char>(a[i]);
			const unsigned char cb = static_cast<unsigned char>(b[i]);
			const unsigned char la = ca >= 'A' && ca <= 'Z'
				? static_cast<unsigned char>(ca - 'A' + 'a') : ca;
			const unsigned char lb = cb >= 'A' && cb <= 'Z'
				? static_cast<unsigned char>(cb - 'A' + 'a') : cb;
			if (la != lb) {
				return false;
			}
		}
		return true;
	};
	for (const ControllerScriptFile& script : discoverControllerScripts()) {
		if (same(script.name, leaf)) {
			return script.absolutePath;
		}
	}
	return {};
}

LuaControllerScript::LuaControllerScript() = default;

LuaControllerScript::~LuaControllerScript() {
	if (lua_ != nullptr) {
		if (tickRef_ != kTickRefNone) {
			luaL_unref(lua_, LUA_REGISTRYINDEX, tickRef_);
			tickRef_ = kTickRefNone;
		}
		lua_close(lua_);
		lua_ = nullptr;
	}
}

bool LuaControllerScript::initialize() {
	if (lua_ != nullptr) {
		return true;
	}
	lua_ = luaL_newstate();
	if (lua_ == nullptr) {
		LOG_ERROR("ControllerScript") << "luaL_newstate failed; scripting disabled";
		return false;
	}
	lua_atpanic(lua_, luaPanic);
	luaL_openlibs(lua_);
	tickRef_ = kTickRefNone;
	registerApi();

	LOG_NOTICE("ControllerScript") << "scripting ready (" << LUA_RELEASE << ")";
	return true;
}

void LuaControllerScript::registerApi() {
	// One closure per function, each carrying the host as an upvalue. The host
	// is the `this` every API call reads: no registry lookup, no global state,
	// so two controllers in one process could not collide.
	auto pushFunction = [this](int (*fn)(lua_State*)) {
		lua_pushlightuserdata(lua_, this);
		lua_pushcclosure(lua_, fn, 1);
	};

	lua_newtable(lua_);                       // controller
	pushFunction(apiLog);
	lua_setfield(lua_, -2, "Log");
	pushFunction(apiSleep);
	lua_setfield(lua_, -2, "Sleep");
	pushFunction(apiPlay);
	lua_setfield(lua_, -2, "Play");
	pushFunction(apiNext);
	lua_setfield(lua_, -2, "Next");
	pushFunction(apiPrevious);
	lua_setfield(lua_, -2, "Previous");
	pushFunction(apiPause);
	lua_setfield(lua_, -2, "Pause");
	pushFunction(apiStop);
	lua_setfield(lua_, -2, "Stop");
	pushFunction(apiSeekPercent);
	lua_setfield(lua_, -2, "SeekPercent");
	pushFunction(apiSetVolume);
	lua_setfield(lua_, -2, "SetVolume");
	pushFunction(apiSetSpeed);
	lua_setfield(lua_, -2, "SetSpeed");
	pushFunction(apiSetSubtitles);
	lua_setfield(lua_, -2, "SetSubtitles");
	pushFunction(apiShowHud);
	lua_setfield(lua_, -2, "ShowHUD");
	pushFunction(apiFullscreen);
	lua_setfield(lua_, -2, "Fullscreen");
	pushFunction(apiPlaylist);
	lua_setfield(lua_, -2, "Playlist");
	pushFunction(apiCurrentClip);
	lua_setfield(lua_, -2, "CurrentClip");
	pushFunction(apiOnTick);
	lua_setfield(lua_, -2, "OnTick");
	lua_setglobal(lua_, "controller");
}

bool LuaControllerScript::chargeTickBudget(double ms) {
	if (ms < 0.0 || ms > 60000.0) {
		return false;
	}
	tickSpentMs_ += ms;
	return tickSpentMs_ < kTickBudgetClockMs;
}

bool LuaControllerScript::spendInstructions(int count) {
	instructionsLeft_ -= count;
	return instructionsLeft_ >= 0;
}

void LuaControllerScript::raiseBudgetExhausted() {
	luaL_error(lua_, "controller script exceeded its tick budget; "
		"use OnTick and short Sleep steps to spread work across frames");
}

void LuaControllerScript::raisePlayerError(const std::string& error) {
	luaL_error(lua_, "%s", error.c_str());
}

void LuaControllerScript::noteScriptError(const std::string& message) {
	lastError_ = message;
}

void LuaControllerScript::noteLog(const std::string& message) {
	lastLog_ = message;
}

void LuaControllerScript::storeTickHandler() {
	// Stack top is the function. Drop any previous handler first, so a script
	// cannot leak a registry reference by registering twice.
	if (tickRef_ != kTickRefNone) {
		luaL_unref(lua_, LUA_REGISTRYINDEX, tickRef_);
		tickRef_ = kTickRefNone;
	}
	tickRef_ = luaL_ref(lua_, LUA_REGISTRYINDEX);
}

std::vector<PlayerClipInfo> LuaControllerScript::scriptPlaylist() const {
	return commands_ != nullptr ? commands_->playlist() : std::vector<PlayerClipInfo>{};
}

ControllerState LuaControllerScript::scriptState() const {
	return commands_ != nullptr ? commands_->state() : ControllerState{};
}

bool LuaControllerScript::runSource(const std::string& source,
	const std::string& chunkName, std::string& error) {
	if (lua_ == nullptr) {
		error = "scripting is not available";
		return false;
	}
	if (!currentScript_.empty() || tickRef_ != kTickRefNone) {
		stopScript();
	}
	currentScript_ = chunkName;
	lastError_.clear();
	tickSpentMs_ = 0.0;
	instructionsLeft_ = kInstructionBudget;

	const int status = luaL_loadbuffer(lua_, source.c_str(), source.size(),
		chunkName.c_str());
	if (status != kLuaOk) {
		setError(luaErrorText(lua_, chunkName));
		error = lastError_;
		currentScript_.clear();
		return false;
	}

	gActiveHost = this;
	lua_sethook(lua_, instructionHook, LUA_MASKCOUNT, kInstructionBudget);
	const int callStatus = lua_pcall(lua_, 0, 0, 0);
	lua_sethook(lua_, nullptr, 0, 0);
	gActiveHost = nullptr;

	if (callStatus != kLuaOk) {
		setError(luaErrorText(lua_, chunkName));
		error = lastError_;
		currentScript_.clear();
		return false;
	}
	error.clear();
	return true;
}

bool LuaControllerScript::runFile(const std::string& path, std::string& error) {
	// Take a copy first. `path` routinely aliases loadedPath_: reloadIfChanged()
	// and forceReload() pass the member straight through, and runSource() below
	// stops the running script, which clears that member. Without this copy the
	// name is erased mid-call and loadedPath_ is assigned an empty string — so a
	// reload would work exactly once and then silently stop tracking the file.
	const std::string filePath = path;
	std::FILE* file = std::fopen(filePath.c_str(), "rb");
	if (file == nullptr) {
		error = "cannot open script: " + filePath;
		return false;
	}
	std::string source;
	char buffer[4096];
	std::size_t read = 0;
	while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
		source.append(buffer, read);
	}
	std::fclose(file);

	// A byte-order mark is a syntax error to Lua 5.1, so strip it rather than
	// fail with a confusing "unexpected symbol near '<\239>'".
	if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF
		&& static_cast<unsigned char>(source[1]) == 0xBB
		&& static_cast<unsigned char>(source[2]) == 0xBF) {
		source.erase(0, 3);
	}

	const std::string name = std::filesystem::path(filePath).filename().string();
	if (!runSource(source, name, error)) {
		return false;
	}

	// Remember the file only on success, so a broken reload keeps the previous
	// script rather than pointing at garbage.
	loadedPath_ = filePath;
	if (!fileStamp(filePath, loadedMtime_, loadedSize_)) {
		loadedMtime_ = 0;
		loadedSize_ = 0;
	}
	return true;
}

bool LuaControllerScript::callTickHandler() {
	if (lua_ == nullptr || tickRef_ == kTickRefNone) {
		return true;
	}
	lua_rawgeti(lua_, LUA_REGISTRYINDEX, tickRef_);

	gActiveHost = this;
	lua_sethook(lua_, instructionHook, LUA_MASKCOUNT, kInstructionBudget);
	const int status = lua_pcall(lua_, 0, 0, 0);
	lua_sethook(lua_, nullptr, 0, 0);
	gActiveHost = nullptr;

	if (status != kLuaOk) {
		setError(luaErrorText(lua_, currentScript_));
		return false;
	}
	return true;
}

bool LuaControllerScript::tick() {
	if (lua_ == nullptr || currentScript_.empty()) {
		return true;
	}
	// After a failure, back off rather than re-running a broken script every
	// frame. Once the backoff elapses the script is retried from its OnTick,
	// which is what makes hot-reload usable while debugging.
	if (!lastError_.empty()) {
		const double since = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - errorAt_).count();
		if (since < kErrorBackoffSeconds) {
			return false;
		}
		lastError_.clear();
	}

	tickSpentMs_ = 0.0;
	instructionsLeft_ = kInstructionBudget;
	if (!callTickHandler()) {
		errorAt_ = std::chrono::steady_clock::now();
		stopScript();
		return false;
	}
	return true;
}

bool LuaControllerScript::reloadIfChanged() {
	if (loadedPath_.empty()) {
		return false;
	}
	long long mtime = 0;
	std::uintmax_t size = 0;
	if (!fileStamp(loadedPath_, mtime, size)) {
		return false;
	}
	if (mtime == loadedMtime_ && size == loadedSize_) {
		return false;
	}
	std::string error;
	// Safe despite the aliasing hazard in runFile: the copy there is what keeps
	// this member intact across the reload.
	if (!runFile(loadedPath_, error)) {
		LOG_WARN("ControllerScript") << "reload failed: " << error;
		return false;
	}
	LOG_NOTICE("ControllerScript") << "reloaded " << currentScript();
	return true;
}

bool LuaControllerScript::forceReload(std::string& error) {
	if (loadedPath_.empty()) {
		error = "no script loaded";
		return false;
	}
	return runFile(loadedPath_, error);
}

void LuaControllerScript::stopScript() {
	if (lua_ != nullptr && tickRef_ != kTickRefNone) {
		luaL_unref(lua_, LUA_REGISTRYINDEX, tickRef_);
	}
	tickRef_ = kTickRefNone;
	currentScript_.clear();
	loadedPath_.clear();
	loadedMtime_ = 0;
	loadedSize_ = 0;
}

void LuaControllerScript::setError(std::string message) {
	lastError_ = std::move(message);
	LOG_WARN("ControllerScript") << lastError_;
}

} // namespace media
