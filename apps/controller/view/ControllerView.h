#pragma once

#include "control/ControllerModel.h"
#include "control/TransportAction.h"

#include <string>

namespace media::ui {
class UiLayer;
}

namespace media {

/// Draws the Controller: status, transport, seek, volume, speed, the media
/// corpus field and the message strip.
///
/// This is one of only two files in the project that includes <imgui.h>, and it
/// is the reason only an application links ImGui. It reads a ControllerModel and
/// returns a TransportAction; it never sends an HTTP request, never touches Lua
/// and never writes a configuration file. All of that is the executor's job
/// (control/TransportAction.h), which is what keeps the behaviour testable with
/// no window.
///
/// ImGui remembers nothing between frames here: the model is the only state, so
/// the panel is a function from the model to widgets, plus whatever the operator
/// touched.
class ControllerPanel {
public:
	/// Draw the whole Controller interface for one frame. The action is `valid`
	/// only when the operator did something that needs acting on; the caller
	/// hands it straight to TransportExecutor.
	struct Frame {
		TransportAction action;
		/// True when the operator asked to quit (Esc). The application decides
		/// what that means.
		bool requestQuit = false;
	};

	/// `scriptName` is what the script host reports as running, empty when
	/// nothing is; `scriptError` is its last failure, empty when there is none.
	Frame draw(ui::UiLayer& ui, const ControllerModel& model,
		const std::string& scriptName, const std::string& scriptError,
		bool scriptRunning);

	/// Fraction of the per-tick budget the running script has consumed, 0..1.
	void setScriptBudget(float fraction) { scriptBudget_ = fraction; }

	/// The last line the script logged, for the Scripts strip.
	void setScriptLog(std::string line) { scriptLog_ = std::move(line); }

	/// The Player's host:port, shown in the status row so two Controllers can be
	/// told apart at a glance.
	void setPlayerEndpoint(std::string endpoint) { playerEndpoint_ = std::move(endpoint); }

private:
	void drawStatusRow(const ControllerModel& model);
	void drawTransport(ControllerModel& model, Frame& frame);
	void drawSeek(const ControllerModel& model, Frame& frame);
	void drawReadouts(const ControllerModel& model, Frame& frame);
	void drawCorpus(const ControllerModel& model, Frame& frame);
	void drawMessage(const ControllerModel& model);
	void drawScripts(const ControllerModel& model, const std::string& scriptName,
		const std::string& scriptError, bool scriptRunning, Frame& frame);

	std::string playerEndpoint_;
	std::string scriptLog_;
	float scriptBudget_ = 0.0f;

	// The seek bar is a widget the operator drags while the Player keeps
	// reporting a position. `seekDraft_` is the value under the operator's hand,
	// held here for exactly as long as the drag lasts, so the incoming position
	// cannot fight it. See drawSeek.
	double seekDraft_ = 0.0;
	bool seekDragging_ = false;

	// The volume and speed controls need three numbers each, for the same reason:
	//
	//   draft   what the operator is setting right now (the widget's value)
	//   seen    the value the PLAYER last reported, so an external change still
	//           shows up here
	//   sent    the value already dispatched, so a held slider does not post the
	//           same number sixty times a second
	//
	// -1 for a draft means "nothing has been read from the Player yet".
	double volumeDraft_ = -1.0;
	double playerVolume_ = -1.0;
	double sentVolume_ = -1.0;
	double speedDraft_ = -1.0;
	double playerSpeed_ = -1.0;
	double sentSpeed_ = -1.0;
};

} // namespace media
