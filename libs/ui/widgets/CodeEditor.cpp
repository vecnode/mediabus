#include "ui/widgets/CodeEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace media::ui {
namespace {

ImVec4 toImVec4(std::uint32_t rgb, float alpha = 1.0f) {
	return ImVec4(
		static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
		static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
		static_cast<float>(rgb & 0xFFu) / 255.0f,
		alpha);
}

ImU32 toImU32(std::uint32_t rgb, float alpha = 1.0f) {
	return ImGui::GetColorU32(toImVec4(rgb, alpha));
}

/// The colour for one token kind. Plain text uses the style's text colour so a
/// theme change moves the bulk of the interface, not just the interesting parts.
ImU32 tokenColour(const CodeEditorStyle& style, ScriptTokenKind kind) {
	switch (kind) {
		case ScriptTokenKind::Keyword: return toImU32(style.keyword);
		case ScriptTokenKind::Comment: return toImU32(style.comment);
		case ScriptTokenKind::String: return toImU32(style.string);
		case ScriptTokenKind::Number: return toImU32(style.number);
		case ScriptTokenKind::Operator: return toImU32(style.op);
		case ScriptTokenKind::Library: return toImU32(style.library);
		case ScriptTokenKind::Api: return toImU32(style.api);
		case ScriptTokenKind::Plain: break;
	}
	return toImU32(style.text);
}

/// Resize callback for InputTextMultiline over a std::string.
///
/// ImGui's text field wants a fixed char buffer. The documented way to edit a
/// std::string instead is this callback: ImGui asks how much room it needs when
/// the text grows past the current allocation, and the string is resized before
/// the edit is applied. Without it the field silently stops accepting input at
/// the initial capacity, which is a confusing bug to diagnose.
int resizeCallback(ImGuiInputTextCallbackData* data) {
	if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) {
		return 0;
	}
	auto* text = static_cast<std::string*>(data->UserData);
	IM_ASSERT(data->Buf == text->c_str());
	text->resize(static_cast<std::size_t>(data->BufTextLen));
	data->Buf = text->data();
	return 0;
}

/// The widget's identity, so ImGui keeps the caret and scroll state across
/// frames while the surrounding panel comes and goes.
constexpr const char* kInputId = "##code";

} // namespace

void CodeEditor::revealLine(std::size_t line) {
	pendingReveal_ = line;
}

CodeEditor::Result CodeEditor::draw(ScriptDocument& document,
	const CodeEditorStyle& style, float height, bool readOnly) {
	Result result;
	const ImGuiStyle& imStyle = ImGui::GetStyle();
	const float lineHeight = ImGui::GetTextLineHeight();
	const float glyphWidth = ImGui::CalcTextSize("M").x;
	// Four characters of line numbers is 9999 lines, which is far more Lua than
	// anyone writes here; longer documents simply widen the gutter.
	const float gutterWidth = glyphWidth * style.gutterChars
		+ glyphWidth * style.gutterPad;

	const float wanted = (height > 0.0f) ? height : lastHeight_;
	lastHeight_ = wanted;

	// The whole editor is one scrollable region: gutter and text scroll together
	// vertically, and a long line scrolls horizontally. ImGui gives that for
	// free, which is why the text field is sized to its content instead of being
	// given its own scrollbar - two nested scroll regions in one small widget is
	// a worse experience than one.
	ImGui::PushStyleColor(ImGuiCol_ChildBg, toImVec4(style.background));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::BeginChild("##editor", ImVec2(0.0f, wanted),
		ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

	ImDrawList* drawList = ImGui::GetWindowDrawList();

	// Content size: the taller of the text and the visible area, so the gutter
	// fill reaches the bottom of the widget even in a short document.
	const float contentHeight = std::max(
		static_cast<float>(document.lineCount()) * lineHeight,
		ImGui::GetContentRegionAvail().y);
	const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();

	// --- gutter ------------------------------------------------------------
	drawList->AddRectFilled(contentOrigin,
		ImVec2(contentOrigin.x + gutterWidth, contentOrigin.y + contentHeight),
		toImU32(style.gutter));

	// --- the editing surface ----------------------------------------------
	// No frame, no padding, no background: this is a text field being used as an
	// input sink, and everything visible is drawn by the code below. Pushing the
	// padding to zero is what makes the drawn text and the invisible real text
	// share an origin, so the caret lands exactly under the glyphs.
	ImGui::SetCursorScreenPos(ImVec2(contentOrigin.x + gutterWidth, contentOrigin.y));
	const float textWidth = std::max(1.0f,
		ImGui::GetContentRegionAvail().x - glyphWidth * 2.0f);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(glyphWidth, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
	ImGui::PushTabStop(!readOnly);

	// The field edits its own copy and the document is updated when it reports a
	// change, rather than the field pointing into the document. That keeps the
	// tokenised line index consistent with the text at all times: setText()
	// re-tokenises, so the colouring drawn below always describes exactly what is
	// on screen, and a half-applied edit is not a state this can be in.
	std::string editable = document.text();
	// One spare byte for the terminating NUL ImGui writes.
	editable.reserve(editable.size() + 1);

	ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize
		| ImGuiInputTextFlags_AllowTabInput;
	if (readOnly) {
		flags |= ImGuiInputTextFlags_ReadOnly;
	}

	const bool changed = ImGui::InputTextMultiline(kInputId, editable.data(),
		editable.capacity() + 1, ImVec2(textWidth, contentHeight),
		flags, resizeCallback, &editable);

	const ImVec2 textOrigin = ImGui::GetItemRectMin();
	ImGui::PopTabStop();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar();

	if (changed) {
		document.setText(std::move(editable));
		result.edited = true;
	}

	// --- the coloured text over the invisible real text ---------------------
	// Both are at the same origin and drawn with the same font, which is what
	// makes the caret land exactly under the glyphs.
	drawText(drawList, document, style, textOrigin, lineHeight, 0.0f);
	drawGutter(drawList, document, style, contentOrigin, lineHeight);

	// --- caret position and the reveal request ------------------------------
	// ImGui does not expose the caret's line, so it is recovered from the text
	// field's own scroll and cursor state: the field keeps the caret on screen,
	// and the offset of the visible text from the top of the content is the
	// caret's line.
	const float scrollY = ImGui::GetScrollY();
	result.caretLine = static_cast<std::size_t>(
		std::max(0.0f, std::floor(scrollY / lineHeight))) + 1;
	result.caretLine = std::min(result.caretLine, std::max<std::size_t>(1,
		document.lineCount()));

	if (pendingReveal_ > 0) {
		const float target = static_cast<float>(pendingReveal_ - 1) * lineHeight;
		ImGui::SetScrollY(std::max(0.0f, target - ImGui::GetWindowHeight() * 0.3f));
		pendingReveal_ = 0;
	}

	// Ctrl+S is handled here because this is where the keyboard focus is. The
	// caller decides what saving means, so the widget only reports the intent.
	if (!readOnly && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
		&& ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		result.saveRequested = true;
	}

	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	(void)imStyle;
	return result;
}

void CodeEditor::drawGutter(ImDrawList* drawList, const ScriptDocument& document,
	const CodeEditorStyle& style, ImVec2 origin, float lineHeight) {
	const float glyphWidth = ImGui::CalcTextSize("M").x;
	const float rightEdge = origin.x + glyphWidth * style.gutterChars;
	const ImU32 numberColour = toImU32(style.gutterText);
	const ImU32 errorColour = toImU32(style.errorText);

	const std::vector<ScriptLine>& lines = document.lines();
	// Only the visible lines are drawn: a ten-thousand-line document must not
	// cost ten thousand draw commands per frame.
	const float top = ImGui::GetScrollY();
	const float bottom = top + ImGui::GetWindowHeight();
	const std::size_t first = static_cast<std::size_t>(
		std::max(0.0f, std::floor(top / lineHeight)));
	const std::size_t last = std::min(lines.size(),
		static_cast<std::size_t>(std::ceil(bottom / lineHeight)) + 1);

	char label[16];
	for (std::size_t i = first; i < last; ++i) {
		const float y = origin.y + static_cast<float>(i) * lineHeight;
		const bool isError = lines[i].error;
		if (isError) {
			// A marker in the margin, not just a coloured number: the point is to
			// be findable while scrolling past.
			drawList->AddRectFilled(ImVec2(origin.x, y),
				ImVec2(origin.x + glyphWidth * 0.6f, y + lineHeight),
				errorColour);
		}
		std::snprintf(label, sizeof(label), "%zu", i + 1);
		const float width = ImGui::CalcTextSize(label).x;
		drawList->AddText(ImVec2(rightEdge - width, y),
			isError ? errorColour : numberColour, label);
	}
}

void CodeEditor::drawText(ImDrawList* drawList, const ScriptDocument& document,
	const CodeEditorStyle& style, ImVec2 origin, float lineHeight, float textStart) {
	const ImU32 errorBg = toImU32(style.errorLine, 0.55f);
	const std::vector<ScriptLine>& lines = document.lines();
	const std::string& text = document.text();

	const float top = ImGui::GetScrollY();
	const float bottom = top + ImGui::GetWindowHeight();
	const std::size_t first = static_cast<std::size_t>(
		std::max(0.0f, std::floor(top / lineHeight)));
	const std::size_t last = std::min(lines.size(),
		static_cast<std::size_t>(std::ceil(bottom / lineHeight)) + 1);

	for (std::size_t i = first; i < last; ++i) {
		const ScriptLine& line = lines[i];
		const float y = origin.y + static_cast<float>(i) * lineHeight;

		if (line.error) {
			// A full-width wash behind the line, so the problem line is obvious
			// even at the far side of a long window.
			drawList->AddRectFilled(ImVec2(origin.x - 1000.0f, y),
				ImVec2(origin.x + 4000.0f, y + lineHeight), errorBg);
		}

		// A blank line still needs nothing drawn, but the loop below handles it
		// by producing no spans.
		float x = origin.x + textStart;
		for (const ScriptTokenSpan& span : line.spans) {
			const std::size_t begin = std::min(span.begin, line.length());
			const std::size_t end = std::min(span.end, line.length());
			if (end <= begin) {
				continue;
			}
			const std::string piece = text.substr(line.begin + begin, end - begin);
			drawList->AddText(ImVec2(x, y), tokenColour(style, span.kind), piece.c_str());
			x += ImGui::CalcTextSize(piece.c_str()).x;
		}
	}
}

} // namespace media::ui
