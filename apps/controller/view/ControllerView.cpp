#include "view/ControllerView.h"

#include "ui/UiLayer.h"

// The ImGui fence: this file may include imgui.h because it is application UI.
// It must not include a GL header (that is what UiLayer is for), and it must not
// include the HTTP client or the Lua headers, because drawing is not allowed to
// talk to the Player.
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace media {
namespace {

/// Theme colours arrive as 0xRRGGBB. ui/UiLayer.h has no ImGui type in it, so
/// this conversion lives here rather than there.
ImVec4 toImVec4(std::uint32_t rgb, float alpha = 1.0f) {
	return ImVec4(
		static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
		static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
		static_cast<float>(rgb & 0xFFu) / 255.0f,
		alpha);
}

/// mm:ss, or h:mm:ss past an hour. A negative or unknown time prints as --:--,
/// which is what an image and an unreached Player both want.
std::string clock(double seconds) {
	if (!(seconds >= 0.0) || seconds > 359999.0) {
		return "--:--";
	}
	const int total = static_cast<int>(seconds + 0.5);
	const int s = total % 60;
	const int m = (total / 60) % 60;
	const int h = total / 3600;
	char buffer[32];
	if (h > 0) {
		std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", h, m, s);
	} else {
		std::snprintf(buffer, sizeof(buffer), "%d:%02d", m, s);
	}
	return buffer;
}

/// Begin a row whose first cell takes what it needs and whose second cell is
/// pinned to the right edge.
///
/// This exists because the obvious approach - SetCursorPosX() out to the right
/// edge - is exactly what Dear ImGui refuses to do: it extends the window's
/// content rectangle and logs an "extend window/parent boundaries" error every
/// frame, which is what it did before this was rewritten. A two-column table is
/// the supported way to say "this belongs at the right edge".
void beginRightAlignedRow(const char* id) {
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
	if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
	}
}

/// End a row started by beginRightAlignedRow(). `drawRight` runs with the cursor
/// already at the start of the right-hand cell.
template <typename Fn>
void endRightAlignedRow(Fn&& drawRight) {
	if (ImGui::TableSetColumnIndex(1)) {
		drawRight();
	}
	ImGui::EndTable();
	ImGui::PopStyleVar();
}

/// Move to the right of `trailingWidth` worth of empty space, so the next item
/// ends near the right edge of the current row.
///
/// This is the single-line counterpart of the table above, for rows where a whole
/// table would be heavier than the row deserves. SameLine() with an offset is
/// deliberately used instead of SetCursorPosX(): the offset is measured from
/// where the line already is, so it never asks for space the line does not have,
/// and an item that does not fit simply wraps rather than logging an error.
void sameLineRight(float trailingWidth) {
	const float room = ImGui::GetContentRegionAvail().x;
	const float offset = room - trailingWidth;
	if (offset > 0.0f) {
		ImGui::SameLine(0.0f, offset);
	} else {
		ImGui::SameLine();
	}
}

/// The transport row. The label is what the operator reads; Play/Pause resolves
/// its own label from the current state.
struct TransportButton {
	ControlCommand command;
	const char* label;
	const char* tooltip;
	bool primary;
};

const TransportButton kTransport[] = {
	{ControlCommand::Previous, "|<", "Previous clip", false},
	{ControlCommand::PlayPause, "Play", "Play or pause (Space)", true},
	{ControlCommand::Stop, "Stop", "Stop the Player", false},
	{ControlCommand::Next, ">|", "Next clip", false},
};

} // namespace

ControllerPanel::Frame ControllerPanel::draw(ui::UiLayer& ui, const ControllerModel& model,
	const std::string& scriptName, const std::string& scriptError,
	bool scriptRunning) {
	(void)ui;
	Frame frame;

	const ImGuiIO& io = ImGui::GetIO();
	// Fill the whole window. NoMove/NoResize/NoCollapse because this window IS
	// the application frame: a panel that could be dragged out of its own OS
	// window, or collapsed to a title bar, would look broken.
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##controller", nullptr, flags);
	ImGui::PopStyleVar(2);

	drawStatusRow(model);
	ImGui::Separator();
	drawTransport(const_cast<ControllerModel&>(model), frame);
	drawSeek(model, frame);
	drawCorpus(model, frame);
	drawScripts(model, scriptName, scriptError, scriptRunning, frame);
	drawMessage(model);

	// Esc closes the window, as it did before the interface changed. ImGui owns
	// the keyboard while a field has focus, so a text field's Esc is not a quit.
	if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		frame.requestQuit = true;
	}

	ImGui::End();
	return frame;
}

void ControllerPanel::drawStatusRow(const ControllerModel& model) {
	const ControllerState& s = model.state();

	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(toImVec4(s.online ? 0x5CB85C : 0xD9534F), "%s",
		s.online ? "ONLINE" : "OFFLINE");
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	// The title carries the clip, its index and its transport state, so the row
	// answers "what is playing" without a second look.
	const std::string title = model.titleText();
	ImGui::TextUnformatted(title.c_str());

	if (!playerEndpoint_.empty()) {
		sameLineRight(ImGui::CalcTextSize(playerEndpoint_.c_str()).x);
		ImGui::TextDisabled("%s", playerEndpoint_.c_str());
	}
}

void ControllerPanel::drawTransport(ControllerModel& model, Frame& frame) {
	const ControllerState& s = model.state();
	// Everything except Play/Pause needs a loaded clip. Play/Pause is safe to
	// offer whenever the Player answers: "play" against an empty playlist is a
	// no-op there rather than an error.
	const bool hasClip = s.online && s.loaded && s.clipCount > 0;
	const float buttonWidth = std::max(84.0f, ImGui::GetFontSize() * 5.0f);

	bool first = true;
	for (const TransportButton& spec : kTransport) {
		if (!first) {
			ImGui::SameLine();
		}
		first = false;

		const bool enabled = s.online
			&& (spec.command == ControlCommand::PlayPause || hasClip);
		if (!enabled) {
			ImGui::BeginDisabled();
		}
		if (spec.primary) {
			ImGui::PushStyleColor(ImGuiCol_Button, toImVec4(0x3A76A6));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toImVec4(0x4B8BBE));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, toImVec4(0x2C5F87));
		}
		const std::string label = spec.primary
			? ((s.playing && !s.paused) ? "Pause" : "Play") : spec.label;
		if (ImGui::Button(label.c_str(), ImVec2(buttonWidth, 0.0f))) {
			frame.action.valid = true;
			frame.action.command = spec.command;
		}
		if (spec.primary) {
			ImGui::PopStyleColor(3);
		}
		if (!enabled) {
			ImGui::EndDisabled();
		}
		// Only a button that can actually be pressed gets a tooltip. Without the
		// `enabled` half, a disabled button still pops its tooltip when the
		// pointer happens to be over it - and because the pointer does not move,
		// that tooltip stays on screen and ends up in a screenshot.
		if (enabled && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", spec.tooltip);
		}
	}

	// The three Player toggles as labelled checkboxes, on their own row.
	//
	// They used to share the line with the transport buttons, which does not fit:
	// four buttons plus three labelled checkboxes plus both sliders overran the
	// window and the last checkbox was cut off. Giving the toggles their own row
	// is what makes the whole panel visible at its default size rather than only
	// when it is dragged wider.
	struct ToggleSpec { ControlCommand command; const char* label; bool on; };
	const ToggleSpec toggles[] = {
		{ControlCommand::ToggleHud, "HUD", s.hudVisible},
		{ControlCommand::ToggleFullscreen, "Fullscreen", s.fullscreen},
		{ControlCommand::ToggleSubtitles, "Subtitles", s.subtitlesEnabled},
	};
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Playback");
	ImGui::SameLine();
	ImGui::Dummy(ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
	for (const ToggleSpec& toggle : toggles) {
		ImGui::SameLine();
		if (!s.online) {
			ImGui::BeginDisabled();
		}
		bool value = toggle.on;
		if (ImGui::Checkbox(toggle.label, &value)) {
			frame.action.valid = true;
			frame.action.command = toggle.command;
		}
		if (!s.online) {
			ImGui::EndDisabled();
		}
	}

	// --- volume and speed ---------------------------------------------------
	//
	// The draft is kept while the operator is working the control, and only
	// replaced when the PLAYER'S OWN value changes. It used to be reset from the
	// player whenever the widget was not held or hovered, which broke the control
	// outright: ImGui deactivates a slider on the same frame the mouse is
	// released, so the draft was thrown away and the handle snapped back to the
	// player's old value - and the new value was sent from the discarded draft,
	// so the player received a number the operator never chose and the handle
	// never moved. Volume and speed both did this.
	//
	// Comparing against the last value SEEN from the player is what makes an
	// external change (a Lua script, say) still show up here, without fighting
	// the operator's hand.
	const double zero = 0.0;
	const double hundred = 100.0;
	const double slowest = 0.1;
	const double fastest = 4.0;

	if (volumeDraft_ < 0.0 || std::abs(s.volume - playerVolume_) > 0.01) {
		volumeDraft_ = s.volume;
		playerVolume_ = s.volume;
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Volume");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::SliderScalar("##volume", ImGuiDataType_Double, &volumeDraft_, &zero, &hundred,
		"vol %.0f%%", ImGuiSliderFlags_AlwaysClamp);
	// Sent while the operator drags, not only on release: a slider that only acts
	// when the mouse comes up feels broken, and the Player coalesces a rapid
	// stream of volume sets harmlessly.
	if (ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit()) {
		if (std::abs(volumeDraft_ - sentVolume_) > 0.01) {
			sentVolume_ = volumeDraft_;
			frame.action.valid = true;
			frame.action.settingVolume = true;
			frame.action.volume = volumeDraft_;
		}
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Volume %d%%  (the Player reports %.0f%%)",
			static_cast<int>(volumeDraft_ + 0.5), s.volume);
	}

	ImGui::SameLine();
	if (speedDraft_ < 0.0 || std::abs(s.speed - playerSpeed_) > 0.005) {
		speedDraft_ = s.speed;
		playerSpeed_ = s.speed;
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Speed");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	ImGui::SliderScalar("##speed", ImGuiDataType_Double, &speedDraft_, &slowest, &fastest,
		"%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit()) {
		if (std::abs(speedDraft_ - sentSpeed_) > 0.005) {
			sentSpeed_ = speedDraft_;
			frame.action.valid = true;
			frame.action.settingSpeed = true;
			frame.action.speed = speedDraft_;
		}
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Playback speed (the Player reports %.2fx)", s.speed);
	}

	// A one-line status of what the Player itself says about the transport, so
	// the effect of a click is visible rather than inferred from a position that
	// only moves once a second.
	ImGui::TextDisabled("%s%s%s",
		s.loaded ? (s.isImage ? "image" : "clip loaded") : "nothing loaded",
		s.isImage ? "" : (s.playing && !s.paused ? " - playing" : " - paused"),
		s.seekable ? " - seekable" : "");
}

void ControllerPanel::drawSeek(const ControllerModel& model, Frame& frame) {
	const ControllerState& s = model.state();
	const bool active = model.seekBarActive();

	// While the operator drags, the draft is the truth; otherwise the Player's
	// own position is. This is the one piece of state the panel keeps, and it
	// lasts exactly as long as a drag.
	if (!seekDragging_) {
		seekDraft_ = model.seekPercent();
	}

	ImGui::BeginDisabled(!active);
	ImGui::SetNextItemWidth(-1.0f);
	// A percentage in a float: ample precision for a scrub bar, and it keeps the
	// widget to the common type.
	float percent = static_cast<float>(seekDraft_);
	if (ImGui::SliderFloat("##seek", &percent, 0.0f, 100.0f, "",
		ImGuiSliderFlags_AlwaysClamp)) {
		seekDraft_ = percent;
	}
	if (ImGui::IsItemActive()) {
		seekDragging_ = true;
	} else if (seekDragging_) {
		// The drag ended: this is the moment to send it. Sending on every frame of
		// a drag would be a seek storm against the decoder.
		seekDragging_ = false;
		frame.action.valid = true;
		frame.action.seeking = true;
		frame.action.seekPercent = seekDraft_;
	}
	ImGui::EndDisabled();

	// Under the bar: elapsed on the left, duration on the right. An image has no
	// timeline, so both read as dashes rather than as a misleading 0:00.
	const std::string elapsed = active ? clock(s.position) : std::string("--:--");
	const std::string total = (active && s.duration > 0.0) ? clock(s.duration)
		: std::string("--:--");
	ImGui::TextDisabled("%s", elapsed.c_str());
	sameLineRight(ImGui::CalcTextSize(total.c_str()).x);
	ImGui::TextDisabled("%s", total.c_str());

	// A still image is the one case worth explaining: without this, a disabled
	// seek bar looks like a bug rather than a fact about the clip.
	if (s.online && s.loaded && s.isImage) {
		ImGui::TextDisabled("a still image has no timeline");
	}
}

void ControllerPanel::drawCorpus(const ControllerModel& model, Frame& frame) {
	const ControllerState& s = model.state();
	const float buttonWidth = ImGui::GetFontSize() * 6.5f;

	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", model.corpusLabel().c_str());
	ImGui::SameLine();

	// The value carries the meaning, so its colour does: a real folder is normal
	// text, "not set" is dim, and an offline Player is a warning rather than an
	// error - nothing is wrong, there is simply nothing to ask yet.
	const std::string value = model.corpusValue();
	if (!s.online) {
		ImGui::TextColored(toImVec4(0xD9A441), "%s", value.c_str());
	} else if (!model.corpusChosen()) {
		ImGui::TextDisabled("%s", value.c_str());
	} else {
		ImGui::TextUnformatted(value.c_str());
		if (ImGui::IsItemHovered()) {
			// The field shows the tail of the path; the tooltip shows all of it.
			ImGui::SetTooltip("%s", s.mediaFolder.c_str());
		}
	}

	sameLineRight(buttonWidth);
	if (ImGui::Button("Change...", ImVec2(buttonWidth, 0.0f))) {
		frame.action.valid = true;
		frame.action.chooseFolder = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Choose the folder the Player reads its media from");
	}
}

void ControllerPanel::drawScripts(const ControllerModel& model,
	const std::string& scriptName, const std::string& scriptError,
	bool scriptRunning, Frame& frame) {
	ImGui::Separator();
	ImGui::AlignTextToFramePadding();

	if (scriptName.empty()) {
		ImGui::TextDisabled("Script: none running");
	} else {
		ImGui::TextColored(toImVec4(0x5CB85C), "Script: %s", scriptName.c_str());
		ImGui::SameLine();
		if (scriptRunning) {
			// How much of this tick's budget the script has spent. An operator
			// watching a sequence wants to know how close it is to being cut off,
			// and a bar is quicker to read than a number.
			ImGui::ProgressBar(std::min(1.0f, std::max(0.0f, scriptBudget_)),
				ImVec2(ImGui::GetFontSize() * 6.0f, ImGui::GetFontSize()));
			ImGui::SameLine();
		}
		ImGui::TextDisabled("%s", scriptRunning ? "running" : "stopped");
	}

	const float buttonWidth = ImGui::GetFontSize() * 5.0f;
	sameLineRight(buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x);
	ImGui::BeginDisabled(scriptName.empty());
	if (ImGui::Button("Reload", ImVec2(buttonWidth, 0.0f))) {
		frame.action.valid = true;
		frame.action.reloadScript = true;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Re-read the script from disk now (R)");
	}
	ImGui::SameLine();
	if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f))) {
		frame.action.valid = true;
		frame.action.stopScript = true;
	}
	ImGui::EndDisabled();

	// The script's last failure, or failing that its last log line: one line
	// either way, so a chatty script cannot push the transport controls off the
	// window.
	if (!scriptError.empty()) {
		ImGui::TextColored(toImVec4(0xD9534F), "%s", scriptError.c_str());
	} else if (!scriptLog_.empty()) {
		ImGui::TextDisabled("%s", scriptLog_.c_str());
	}
	(void)model;
}

void ControllerPanel::drawMessage(const ControllerModel& model) {
	const std::string& message = model.message();
	if (message.empty()) {
		return;
	}
	ImGui::Separator();
	ImGui::TextWrapped("%s", message.c_str());
}

} // namespace media
