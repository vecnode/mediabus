#pragma once

#include "control/ControllerModel.h"
#include "control/LuaControllerScript.h"
#include "control/PlayerClient.h"

#include <cstddef>
#include <functional>
#include <string>

namespace media {

/// Everything the Controller's interface can ask for, as a value.
///
/// The ImGui panel fills one of these in and hands it back; nothing in the panel
/// touches HTTP, Lua or the configuration file. That split is deliberate and it
/// is what makes the behaviour testable: the tests drive TransportExecutor with
/// a recorder in place of the Player and assert exactly which calls a click
/// produced, with no window, no socket and no ImGui.
///
/// One action per frame at most, because that is what a person can do.
struct TransportAction {
	/// True when the panel did something that needs acting on. A default
	/// constructed action is "nothing happened", which is the common case.
	bool valid = false;

	/// A transport command, or None.
	ControlCommand command = ControlCommand::None;

	/// Set when the operator dragged the seek bar. Only meaningful with
	/// `seeking` true - otherwise a seek to 0% would be indistinguishable from
	/// "no seek".
	bool seeking = false;
	double seekPercent = 0.0;

	/// Set when the volume or speed control was moved.
	bool settingVolume = false;
	double volume = 100.0;
	bool settingSpeed = false;
	double speed = 1.0;

	/// Open a playlist entry by index.
	bool openingClip = false;
	std::size_t clipIndex = 0;

	/// Open the operating system's folder picker for the media corpus. This one
	/// runs a modal Win32 dialog, so it must stay on the thread that owns the
	/// frame loop.
	bool chooseFolder = false;

	/// Script-host actions: reload the running script from disk, or stop it.
	bool reloadScript = false;
	bool stopScript = false;

	/// Ask the launcher to restart the Player. Filled in by the Dashboard's own
	/// interface, not the Controller's, but the same executor reports it.
	bool restartPlayer = false;
};

/// Applies a TransportAction to the Player, the script host and the shared
/// configuration, and reports one line of feedback.
///
/// Contains no ImGui and no GL: it is the Controller's behaviour, separated from
/// the widget that triggers it.
class TransportExecutor {
public:
	/// What the folder picker hands back: the chosen absolute path, empty when
	/// nothing was chosen, and a flag distinguishing "cancelled" from
	/// "unavailable" - a cancel is not an error and must say nothing.
	struct FolderChoice {
		std::string path;
		bool cancelled = false;
	};
	using FolderPicker = std::function<FolderChoice(const std::string& initialDirectory)>;

	/// `player` and `model` must outlive this object. `scripts` may be null when
	/// Lua could not start, in which case the script actions report that.
	///
	/// `picker` is how the "choose the media folder" action reaches the
	/// operating system without this library - which the headless tests link -
	/// having to know about the shell, COM or windows.h. The application
	/// supplies it; a test supplies a stub that returns a path it chose.
	TransportExecutor(PlayerCommands& player, ControllerModel& model,
		LuaControllerScript* scripts);
	void setFolderPicker(FolderPicker picker) { picker_ = std::move(picker); }

	/// Act on `action`. Returns the line the interface should show - empty when
	/// the action either succeeded silently or was a no-op.
	///
	/// Failures are reported, never thrown and never fatal: a Controller whose
	/// Player went away must keep answering.
	std::string perform(const TransportAction& action);

	/// Reload the running script unconditionally (the R key, and the Scripts
	/// panel's button). Returns a message on failure, empty on success.
	std::string reloadScript();

	/// Stop the running script and drop its tick handler.
	std::string stopScript();

	/// True when a script host is available at all.
	bool scripting() const { return scripts_ != nullptr && scripts_->ready(); }
	LuaControllerScript* scripts() { return scripts_; }

private:
	/// The folder-picker path, which also decides who writes mediabus.ini.
	std::string chooseMediaFolder();

	PlayerCommands& player_;
	ControllerModel& model_;
	LuaControllerScript* scripts_ = nullptr;
	FolderPicker picker_;
};

} // namespace media
