#pragma once

#include "control/DashboardModel.h"
#include "control/ScriptDocument.h"
#include "control/ScriptLibrary.h"
#include "ui/widgets/CodeEditor.h"

#include <string>
#include <vector>

namespace media::ui {
class UiLayer;
}

namespace media {

/// Draws the Dashboard: the two application rows, the media corpus panel, an
/// activity log and the Lua script editor.
///
/// This is the second and last file in the project that includes <imgui.h>. It
/// reads a DashboardModel and reports what the operator asked for; it never
/// launches a process, never sends an HTTP request and never writes a file.
/// Those are AppLauncher's, ScriptLibrary's and the Controller's jobs - which is
/// what keeps the launcher's behaviour testable with no window.
class DashboardPanel {
public:
	/// Which panel is on screen.
	enum class Tab { Applications, Scripts, Log };

	struct Frame {
		/// An action to run, or None.
		DashboardAction action = DashboardAction::None;
		/// True when the operator asked to quit. Only meaningful when there is no
		/// tray icon, because otherwise QUIT lives in the tray menu.
		bool requestQuit = false;
		/// The tab that is on screen after this frame.
		Tab tab = Tab::Applications;
	};

	/// Draw the whole Dashboard for one frame.
	///
	/// `library` is the Controller's script API and may be null: the Scripts tab
	/// then explains that the Controller is not running rather than disappearing.
	/// An operator who went looking for the editor deserves to be told why it is
	/// not there.
	Frame draw(ui::UiLayer& ui, const DashboardModel& model,
		ScriptLibrary* library, ScriptDocument& document, bool controllerOnline);

	Tab tab() const { return tab_; }
	void setTab(Tab tab) { tab_ = tab; }

	/// Move the editor's caret to an error line on the next frame, so the
	/// operator's eye lands on the problem rather than wherever the caret was.
	void revealError(std::size_t line) { editor_.revealLine(line); }

	/// One line reporting what the last script operation did. Empty shows
	/// nothing. Set by the application, which is what actually calls the API.
	void setScriptStatus(std::string status) { scriptStatus_ = std::move(status); }
	const std::string& scriptStatus() const { return scriptStatus_; }

	/// The script the operator selected.
	void setSelection(std::string name) { selected_ = std::move(name); }
	const std::string& selection() const { return selected_; }

	/// The list the Scripts tab shows, filled by the application from the
	/// Controller's API so this class never talks to the network.
	void setScriptList(std::vector<ScriptEntry> entries) {
		scripts_ = std::move(entries);
	}

	void setDirty(bool dirty) { dirty_ = dirty; }

	// --- one-shot requests, cleared by clearRequests() ----------------------
	bool saveRequested() const { return saveRequested_; }
	bool validateRequested() const { return validateRequested_; }
	bool reloadRequested() const { return reloadRequested_; }
	bool runRequested() const { return runRequested_; }
	/// A new script was named and created blank; empty when none.
	const std::string& newScriptName() const { return newScriptName_; }

	void clearRequests() {
		saveRequested_ = false;
		validateRequested_ = false;
		reloadRequested_ = false;
		runRequested_ = false;
		newScriptName_.clear();
	}

	ui::CodeEditorStyle& editorStyle() { return editorStyle_; }

private:
	void drawApplications(const DashboardModel& model, Frame& frame);
	void drawCorpus(const DashboardModel& model, Frame& frame);
	void drawScripts(const DashboardModel& model, ScriptLibrary* library,
		ScriptDocument& document, bool controllerOnline, Frame& frame);
	void drawLog(const DashboardModel& model);

	Tab tab_ = Tab::Applications;
	std::string scriptStatus_;
	std::string selected_;
	std::vector<ScriptEntry> scripts_;
	bool dirty_ = false;

	bool saveRequested_ = false;
	bool validateRequested_ = false;
	bool reloadRequested_ = false;
	bool runRequested_ = false;
	std::string newScriptName_;

	ui::CodeEditor editor_;
	ui::CodeEditorStyle editorStyle_;

	/// A name being typed into the New Script field.
	char newName_[96] = {0};
};

} // namespace media
