#include "view/DashboardView.h"

#include "ui/UiLayer.h"

// The ImGui fence. This file draws; it must not include a GL header (UiLayer is
// for that) and must not reach for AppLauncher, the HTTP client or the file
// system, because drawing is not allowed to act.
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace media {
namespace {

ImVec4 toImVec4(std::uint32_t rgb, float alpha = 1.0f) {
	return ImVec4(
		static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
		static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
		static_cast<float>(rgb & 0xFFu) / 255.0f,
		alpha);
}

constexpr std::uint32_t kOk = 0x5CB85C;
constexpr std::uint32_t kWarn = 0xD9A441;
constexpr std::uint32_t kError = 0xD9534F;
constexpr std::uint32_t kAccent = 0x4B8BBE;

/// A filled status pill: a coloured dot and a word. Cheaper to read at a glance
/// than a sentence, and it is the first thing an operator looks for.
void statusPill(bool running, bool available) {
	const std::uint32_t colour = !available ? 0x555C6B : (running ? kOk : 0x6B7484);
	const char* text = !available ? "missing" : (running ? "running" : "stopped");
	ImGui::TextColored(toImVec4(colour), "%s", "\xe2\x97\x8f");   // U+25CF
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextColored(toImVec4(colour), "%s", text);
}

/// Move to the right of `trailingWidth` worth of empty space, so the next item
/// ends near the right edge of the current row.
///
/// SameLine() with an offset is used rather than SetCursorPosX(): the offset is
/// measured from where the line already is, so it never asks for space the line
/// does not have. Moving the cursor out to the right edge instead extends the
/// window's content rectangle, which Dear ImGui logs as an "extend
/// window/parent boundaries" error on every frame - and it did, until this was
/// rewritten.
void sameLineRight(float trailingWidth) {
	const float room = ImGui::GetContentRegionAvail().x;
	const float offset = room - trailingWidth;
	if (offset > 0.0f) {
		ImGui::SameLine(0.0f, offset);
	} else {
		ImGui::SameLine();
	}
}

/// A right-aligned button of a fixed width, so a column of them lines up.
bool rightButton(const char* label, float width, bool enabled,
	const char* tooltip) {
	sameLineRight(width);
	if (!enabled) {
		ImGui::BeginDisabled();
	}
	const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
	if (!enabled) {
		ImGui::EndDisabled();
	}
	if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", tooltip);
	}
	return pressed;
}

} // namespace

DashboardPanel::Frame DashboardPanel::draw(ui::UiLayer& ui, const DashboardModel& model,
	ScriptLibrary* library, ScriptDocument& document, bool controllerOnline) {
	(void)ui;
	Frame frame;

	const ImGuiIO& io = ImGui::GetIO();
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
	// NoScrollbar only while the editor is not on screen: the editor needs the
	// child region to scroll, and a window that cannot scroll would clip it.
	const bool editing = (tab_ == Tab::Scripts);

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##dashboard", nullptr, editing ? (flags)
		: (flags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
	ImGui::PopStyleVar(2);

	// --- header ------------------------------------------------------------
	ImGui::TextUnformatted("vn-mediabus");
	ImGui::SameLine();
	ImGui::TextDisabled("media bus launcher");
	if (!model.message().empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::TextColored(toImVec4(kAccent), "%s", model.message().c_str());
	}
	ImGui::Separator();

	// --- tabs --------------------------------------------------------------
	// Applications is first because it is what the launcher is for; Scripts
	// second because editing a script is the thing an operator comes back for.
	//
	// Which tab is OPEN is Dear ImGui's state, not this class's. A setTab()
	// request is therefore handed over as ImGuiTabItemFlags_SetSelected on one
	// frame and then forgotten - mirroring a tab index into `tab_` did not work,
	// because the first frame opened Applications and overwrote the request
	// before it could take effect.
	const auto selectFlags = [this](Tab which) {
		return (applyRequestedTab_ && tab_ == which) ? ImGuiTabItemFlags_SetSelected : 0;
	};
	if (ImGui::BeginTabBar("##tabs")) {
		if (ImGui::BeginTabItem("Applications", nullptr, selectFlags(Tab::Applications))) {
			frame.tab = Tab::Applications;
			drawApplications(model, frame);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Scripts", nullptr, selectFlags(Tab::Scripts))) {
			frame.tab = Tab::Scripts;
			drawScripts(model, library, document, controllerOnline, frame);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Activity", nullptr, selectFlags(Tab::Log))) {
			frame.tab = Tab::Log;
			drawLog(model);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	} else {
		frame.tab = tab_;
	}
	// The request has been delivered (or there was no tab bar to deliver it to,
	// in which case it is still the best answer available).
	applyRequestedTab_ = false;

	// Esc closes the launcher window. Whether that hides it or exits is the
	// application's decision - it depends on whether a tray icon exists - so it
	// is reported rather than acted on.
	if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		frame.requestQuit = true;
	}

	ImGui::End();
	return frame;
}

void DashboardPanel::drawApplications(const DashboardModel& model, Frame& frame) {
	const float buttonWidth = ImGui::GetFontSize() * 5.5f;
	const float twoButtons = buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;

	for (const DashboardRow& row : model.rows()) {
		ImGui::PushID(static_cast<int>(row.app));

		// Each application gets its own bordered card rather than a table row:
		// the two are peers with different roles, and a card can carry the path
		// and the port without a column wide enough to hold both. The height
		// leaves room for the title line, the subtitle line and the button row
		// beneath them - it was too short to show its own buttons.
		ImGui::BeginChild("##card", ImVec2(0.0f, ImGui::GetFontSize() * 6.2f),
			ImGuiChildFlags_Borders);

		ImGui::TextUnformatted(row.title.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		statusPill(row.running, row.available);

		if (row.port > 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			ImGui::TextDisabled("API :%d", row.port);
		}

		ImGui::TextDisabled("%s", row.subtitle.c_str());
		if (!row.path.empty() && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", row.path.c_str());
		}

		// The buttons follow the text rather than being pinned to the bottom of
		// the card. Pinning them meant SetCursorPosY() to a position the content
		// had already passed, which Dear ImGui refuses - it extends the window's
		// content rectangle and logs an error every frame. Letting the card grow
		// to fit its contents is both quieter and more robust: a longer subtitle
		// no longer pushes the buttons off the bottom.
		ImGui::Spacing();
		sameLineRight(twoButtons);

		// A Stop that is present but inert is the honest statement for a process
		// this launcher did not start: hiding it would suggest the launcher could
		// stop a Player it has no handle on.
		const bool canStop = row.canStop();
		ImGui::BeginDisabled(!canStop);
		if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f)) && canStop) {
			frame.action = row.stop;
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip(canStop
				? "Stop the copy this launcher started"
				: (row.running
					? "A running %s this launcher did not start is not ours to stop"
					: "Not running"));
		}

		ImGui::SameLine();

		const bool canLaunch = row.canLaunch();
		ImGui::BeginDisabled(!canLaunch);
		ImGui::PushStyleColor(ImGuiCol_Button, toImVec4(0x2C5F87));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toImVec4(kAccent));
		if (ImGui::Button("Launch", ImVec2(buttonWidth, 0.0f)) && canLaunch) {
			frame.action = row.launch;
		}
		ImGui::PopStyleColor(2);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip(row.available
				? (row.running ? "Already running" : "Start it and watch its API come up")
				: "Not found next to this launcher");
		}

		ImGui::EndChild();
		ImGui::PopID();
	}

	ImGui::Spacing();
	drawCorpus(model, frame);

	if (model.availableCount() == 0) {
		ImGui::Spacing();
		ImGui::TextColored(toImVec4(kError),
			"Neither application was found next to this launcher. "
			"Build the tree (scripts\\build.bat) so all three binaries sit in bin\\.");
	}
}

void DashboardPanel::drawCorpus(const DashboardModel& model, Frame& frame) {
	const DashboardCorpus& corpus = model.corpus();
	const float buttonWidth = ImGui::GetFontSize() * 7.0f;

	// The card has to hold three things: the name line with its status pill, the
	// "running - libmpv video and audio - API :8080" line, and the button row
	// under them. At 4.4 font-heights the button row fell outside the card and
	// the Change... button was invisible - a card that clips its own call to
	// action. 6.4 leaves room for all three at any font size, because the height
	// is expressed in font-heights rather than in pixels.
	ImGui::BeginChild("##corpus", ImVec2(0.0f, ImGui::GetFontSize() * 6.4f),
		ImGuiChildFlags_Borders);

	ImGui::TextUnformatted("Media folder");
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	if (corpus.chosen()) {
		ImGui::TextUnformatted(corpus.folder.c_str());
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", corpus.folder.c_str());
		}
	} else {
		ImGui::TextDisabled("not set - the Player uses its own data folder");
	}

	// Which folder the count refers to. The Player's answer is authoritative
	// while it is up: it is the process doing the scanning.
	if (corpus.playerOnline) {
		ImGui::TextColored(toImVec4(kOk), "%zu %s in the folder the Player is reading",
			corpus.clipCount, corpus.clipCount == 1 ? "clip" : "clips");
		if (!corpus.playerFolder.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("(%s)", corpus.playerFolder.c_str());
		}
	} else {
		ImGui::TextDisabled("the Player is not running, so the count is what was "
			"last written down");
	}

	ImGui::Spacing();
	if (rightButton("Change...", buttonWidth, true,
			"Choose the folder the Player reads its media from")) {
		frame.action = DashboardAction::ChooseMediaFolder;
	}

	ImGui::EndChild();
}

void DashboardPanel::drawScripts(const DashboardModel& model, ScriptLibrary* library,
	ScriptDocument& document, bool controllerOnline, Frame& frame) {
	(void)model;

	if (!controllerOnline || library == nullptr) {
		// Not hidden: an operator who came here looking for the editor is told
		// exactly why it is not available and what to do about it. Lua runs
		// inside the Controller, so there is nothing to edit without it.
		ImGui::Spacing();
		ImGui::TextColored(toImVec4(kWarn), "The Controller is not running.");
		ImGui::TextWrapped(
			"Scripts are executed by the Controller, so the editor reads and "
			"writes them through its API on :%d. Start the Controller from the "
			"Applications tab and this tab will list the scripts in "
			"bin\\data\\controller-scripts.",
			AppProbe::kControllerPort);
		return;
	}

	// --- left column: the script list and its actions ----------------------
	const float listWidth = ImGui::GetFontSize() * 15.0f;
	ImGui::BeginChild("##list", ImVec2(listWidth, 0.0f), ImGuiChildFlags_Borders);

	ImGui::TextUnformatted("Scripts");
	ImGui::Separator();

	for (const ScriptEntry& entry : scripts_) {
		ImGui::PushID(entry.name.c_str());
		const bool selected = (entry.name == selected_);
		if (ImGui::Selectable(entry.name.c_str(), selected)) {
			setSelection(entry.name);
		}
		if (entry.running) {
			ImGui::SameLine();
			ImGui::TextColored(toImVec4(kOk), "%s", "running");
		}
		ImGui::PopID();
	}
	if (scripts_.empty()) {
		ImGui::TextDisabled("no scripts on disk");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextDisabled("New script");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##newname", newName_, sizeof(newName_),
		ImGuiInputTextFlags_CharsNoBlank);
	if (rightButton("Create", ImGui::GetFontSize() * 7.0f,
			newName_[0] != '\0',
			"Create an empty script with this name in controller-scripts")) {
		newScriptName_ = newName_;
		newName_[0] = '\0';
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (rightButton("Run", ImGui::GetFontSize() * 7.0f, !selected_.empty(),
			"Run this script in the Controller now")) {
		runRequested_ = true;
	}
	if (rightButton("Reload", ImGui::GetFontSize() * 7.0f, !selected_.empty(),
			"Tell the Controller to re-read the script from disk")) {
		reloadRequested_ = true;
	}

	ImGui::EndChild();
	ImGui::SameLine();

	// --- right column: the editor ------------------------------------------
	ImGui::BeginChild("##editorpane", ImVec2(0.0f, 0.0f));

	if (!document.loaded()) {
		ImGui::Spacing();
		ImGui::TextDisabled("Select a script on the left to edit it.");
		ImGui::EndChild();
		return;
	}

	// Title row: the file, whether it has unsaved changes, and the actions.
	ImGui::TextUnformatted(document.name().c_str());
	if (dirty_) {
		ImGui::SameLine();
		ImGui::TextColored(toImVec4(kWarn), "%s", "(unsaved)");
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	ImGui::TextDisabled("%zu lines", document.lineCount());

	const float buttonWidth = ImGui::GetFontSize() * 5.0f;
	ImGui::SameLine();
	if (rightButton("Validate", buttonWidth, true,
			"Compile the draft without running it, and mark the line it fails on")) {
		validateRequested_ = true;
	}
	ImGui::SameLine();
	if (rightButton("Save", buttonWidth, true, "Write the file back (Ctrl+S)")) {
		saveRequested_ = true;
	}

	// The error, if there is one, above the editor rather than below it: the
	// editor can be tall, and a message under it is a message nobody scrolls to.
	if (!document.errorText().empty()) {
		const std::string& message = document.errorText();
		if (document.errorLine() > 0) {
			ImGui::TextColored(toImVec4(kError), "line %zu: %s",
				document.errorLine(), message.c_str());
		} else {
			ImGui::TextColored(toImVec4(kError), "%s", message.c_str());
		}
	} else if (!scriptStatus_.empty()) {
		ImGui::TextDisabled("%s", scriptStatus_.c_str());
	}

	// The editor takes whatever height is left, so the panel works at any window
	// size without a hard-coded number.
	const float remaining = ImGui::GetContentRegionAvail().y
		- ImGui::GetFrameHeightWithSpacing();
	ui::CodeEditor::Result edited = editor_.draw(document, editorStyle_,
		std::max(120.0f, remaining), false);
	if (edited.edited) {
		dirty_ = true;
	}
	if (edited.saveRequested) {
		saveRequested_ = true;
	}

	ImGui::EndChild();
}

void DashboardPanel::drawLog(const DashboardModel& model) {
	const std::string& log = model.activityLog();
	if (log.empty()) {
		ImGui::TextDisabled("Nothing has happened yet this session.");
		return;
	}

	ImGui::TextDisabled("%zu bytes retained, newest last", log.size());
	ImGui::Separator();

	// A read-only multiline view rather than a run of text calls: ImGui then
	// handles the scrolling and the selection, so a person can copy a line out.
	// The const_cast is safe - the field is ReadOnly, so ImGui never writes
	// through it - and avoids copying the whole log every frame.
	ImGui::PushStyleColor(ImGuiCol_FrameBg, toImVec4(0x0D1017));
	std::string& mutableLog = const_cast<std::string&>(log);
	ImGui::InputTextMultiline("##log", mutableLog.data(), mutableLog.size() + 1,
		ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
	ImGui::PopStyleColor();
}

} // namespace media
