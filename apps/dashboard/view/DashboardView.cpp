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
		// Corpus sits second because it is the question the Applications tab
		// raises: that tab says where the media comes from, this one says what
		// arrived. Scripts and Activity are the working tabs and stay last.
		if (ImGui::BeginTabItem("Corpus", nullptr, selectFlags(Tab::Corpus))) {
			frame.tab = Tab::Corpus;
			drawMediaCorpus(model, frame);
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

	// Esc ASKS before it quits, and what it quits is the LAUNCHER - not the
	// session. The Player and the Controller keep running and keep answering
	// their APIs, because Esc is also the key that pulls the Player out of
	// fullscreen and must never be the key that tears everything down. Stopping
	// the other two stays with QUIT in the tray menu, which is labelled for it.
	if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		quitConfirmOpen_ = true;
	}

	ImGui::End();

	// After End(): a modal is its own top-level window, so it is declared outside
	// the launcher frame rather than nested inside it.
	drawQuitConfirm(frame);

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
			// Three separate calls rather than one nested conditional: the middle
			// case carries a format string, and a ternary that mixes a format with
			// plain literals is how "A running %s ..." ended up with no argument
			// to go with it - which SetTooltip passes straight to vsnprintf. The
			// name is row.title, because a tooltip that cannot say WHICH
			// application it means is not worth showing.
			if (canStop) {
				ImGui::SetTooltip("Stop the copy this launcher started");
			} else if (row.running) {
				ImGui::SetTooltip(
					"A running %s this launcher did not start is not ours to stop",
					row.title.c_str());
			} else {
				ImGui::SetTooltip("Not running");
			}
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
	const float fontSize = ImGui::GetFontSize();
	const float buttonWidth = fontSize * 7.0f;

	// AutoResizeY rather than a formula: the card holds a header, one row per
	// folder, a count line and a button, and a fixed height either clips the last
	// folder or leaves a scrollbar and dead space - both of which were visible
	// with a mere two folders. Letting ImGui measure its own content cannot drift
	// from what it draws.
	ImGui::BeginChild("##corpus", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

	ImGui::TextUnformatted(corpus.folders.size() == 1 ? "Media folder" : "Media folders");
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	if (corpus.chosen()) {
		ImGui::TextDisabled("%zu merged into one playlist", corpus.folders.size());
	} else {
		ImGui::TextDisabled("none set - only the built-in shader library is read");
	}

	// Every folder, each with its own Remove: this panel is also the only place to
	// drop one that has moved or was added by mistake.
	if (corpus.folders.empty()) {
		ImGui::TextDisabled("nothing chosen");
	} else {
		for (const std::string& folder : corpus.folders) {
			ImGui::PushID(folder.c_str());
			if (ImGui::SmallButton("Remove")) {
				frame.action = DashboardAction::RemoveMediaFolder;
				frame.actionFolder = folder;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Stop merging %s", folder.c_str());
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(folder.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", folder.c_str());
			}
			ImGui::PopID();
		}
	}

	// Which folders the count refers to. The Player's answer is authoritative
	// while it is up: it is the process doing the scanning.
	if (corpus.playerOnline) {
		ImGui::TextColored(toImVec4(kOk), "%zu %s from %zu folder(s) the Player is reading",
			corpus.clipCount, corpus.clipCount == 1 ? "clip" : "clips",
			corpus.playerFolders.size());
	} else {
		ImGui::TextDisabled("the Player is not running, so the count is what was "
			"last written down");
	}

	ImGui::Spacing();
	if (rightButton("Add folder...", buttonWidth, true,
			"Add another folder: every folder listed is merged into one playlist")) {
		frame.action = DashboardAction::ChooseMediaFolder;
	}

	ImGui::EndChild();
}

void DashboardPanel::drawMediaCorpus(const DashboardModel& model, Frame& frame) {
	(void)frame;
	const DashboardCorpus& corpus = model.corpus();
	const std::vector<CorpusEntry>& clips = model.clipList();

	ImGui::TextUnformatted(corpus.chosen() ? "Corpus" : "Corpus (built-in shaders only)");
	ImGui::Separator();

	// EVERY folder, not just the first. This is the tab whose whole job is to
	// explain where the playlist came from, and with several folders merged a
	// single line naming one of them is worse than useless: it makes a clip that
	// came from another look like it came from nowhere.
	if (corpus.folders.empty()) {
		ImGui::TextColored(toImVec4(kWarn),
			"No media folder chosen, so the playlist is the built-in shader library.");
	} else {
		for (const std::string& folder : corpus.folders) {
			ImGui::TextDisabled("  %s", folder.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", folder.c_str());
			}
		}
	}

	// What the running Player says it is reading, when that disagrees with the
	// list above - which happens the moment someone edits mediabus.ini by hand
	// while a Player is already up.
	if (corpus.playerOnline && !corpus.playerFolders.empty()
		&& corpus.playerFolders != corpus.folders) {
		ImGui::TextColored(toImVec4(kWarn), "the running Player is reading:");
		for (const std::string& folder : corpus.playerFolders) {
			ImGui::TextDisabled("  %s", folder.c_str());
		}
	}

	if (!model.corpusStatus().empty()) {
		ImGui::TextDisabled("%s", model.corpusStatus().c_str());
	}

	ImGui::Separator();

	// "The list is empty" is three different situations with three different
	// fixes, and an empty list on its own distinguishes none of them. Say which.
	if (clips.empty()) {
		ImGui::Spacing();
		if (!corpus.playerOnline) {
			ImGui::TextDisabled("The Player is not running, so there is no playlist "
				"to read. Start it from the Applications tab.");
		} else if (corpus.chosen()) {
			ImGui::TextDisabled("This folder holds no media. Use Change... on the "
				"Applications tab to point the Player somewhere else.");
		} else {
			ImGui::TextDisabled("Nothing is loaded. Choose a media folder with "
				"Change... on the Applications tab.");
		}
		return;
	}

	ImGui::TextDisabled("%zu %s, newest scan", clips.size(),
		clips.size() == 1 ? "clip" : "clips");
	ImGui::Spacing();

	ImGui::BeginChild("##cliplist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

	// A real corpus runs to thousands of entries, and ImGui would submit every
	// one of them every frame. The clipper submits only the rows actually on
	// screen, which is what keeps a large folder from making the launcher slow.
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(clips.size()));
	while (clipper.Step()) {
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
			const CorpusEntry& clip = clips[static_cast<std::size_t>(row)];
			ImGui::PushID(static_cast<int>(clip.index));

			ImGui::TextDisabled("%5zu", clip.index);
			ImGui::SameLine();
			ImGui::TextUnformatted(clip.name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", clip.mediaType.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", clip.path.c_str());
			}

			ImGui::PopID();
		}
	}

	ImGui::EndChild();
}

void DashboardPanel::drawQuitConfirm(Frame& frame) {
	static const char* const kPopup = "Quit the launcher?";

	// Was it ALREADY open before this frame? The press that opens the modal must
	// not also cancel it, and ImGui's own Escape-closes-a-popup path only runs
	// when keyboard navigation is active, which this interface does not turn on.
	const bool wasOpen = ImGui::IsPopupOpen(kPopup);

	if (quitConfirmOpen_) {
		ImGui::OpenPopup(kPopup);
		quitConfirmOpen_ = false;
	}

	// A ceiling on the width, so a longer sentence can never make the modal wider
	// than the window it belongs to. The lines below are short enough not to need
	// wrapping, which is deliberate: TextWrapped and AlwaysAutoResize fight each
	// other over the width.
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
		ImVec2(display.x * 0.8f, display.y * 0.8f));

	if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}

	// Centre from the size ImGui reports AFTER Begin, rather than trusting a pivot
	// on SetNextWindowPos. A pivot is resolved against the size known before this
	// frame's layout, and ImGui defers the actual move while it measures - a lot of
	// machinery to depend on for a dialog that must simply be in the middle. The
	// size here is the size the modal really has, and because this runs every frame
	// the modal also stays centred if the window is resized.
	const ImVec2 size = ImGui::GetWindowSize();
	ImGui::SetWindowPos(ImVec2((display.x - size.x) * 0.5f,
		(display.y - size.y) * 0.5f));

	ImGui::TextUnformatted("Close the launcher?");
	ImGui::TextDisabled("The Player and the Controller keep running.");
	ImGui::TextDisabled("QUIT in the tray menu stops them too.");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float buttonWidth = ImGui::GetFontSize() * 5.0f;
	if (ImGui::Button("Yes", ImVec2(buttonWidth, 0.0f))) {
		frame.requestQuit = true;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("No", ImVec2(buttonWidth, 0.0f))
		|| (wasOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void DashboardPanel::drawScripts(const DashboardModel& model, ScriptLibrary* library,
	ScriptDocument& document, bool controllerOnline, Frame& frame) {
	(void)model;
	// The editor reports through its own one-shot requests rather than through
	// Frame, so the frame is not this function's to fill in.
	(void)frame;

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
