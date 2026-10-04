#pragma once

/// A syntax-highlighted text editor widget.
///
/// ImGui has no code editor, and vendoring a whole one (ImGuiColorTextEdit and
/// friends) would mean a second third-party project to track, compile and keep
/// in step with Dear ImGui's API - which changes between releases. So this is a
/// deliberately small one, built from primitives ImGui already has.
///
/// The technique is the one ImGui's own documentation suggests for text that
/// needs colour: an `InputTextMultiline` with a resizable std::string behind it
/// does the editing, so the caret, selection, mouse drag, IME and clipboard all
/// behave exactly as they do anywhere else in the interface; then the text is
/// drawn over it in colour with a draw list, and the editing widget's own label
/// is hidden. Nothing is reimplemented, which is what keeps this to a few
/// hundred lines instead of a few thousand.
///
/// Tokenising happens on change, never per frame: a script is a few hundred
/// lines and the cost of colouring it is only paid when the text actually moves.
///
/// This header includes imgui.h because the widget *is* ImGui. It lives in
/// libs/ui, and no application logic includes it.

#include "control/ScriptDocument.h"

#include <imgui.h>

#include <cstdint>
#include <string>

namespace media::ui {

/// Theme colours and font sizes the editor draws with, so it needs nothing from
/// the fence but a palette. Kept as plain bytes to avoid ImVec4 in a struct the
/// caller builds.
struct CodeEditorStyle {
	std::uint32_t background = 0x0D1017;
	std::uint32_t gutter = 0x11151D;
	std::uint32_t gutterText = 0x5A6474;
	std::uint32_t currentLine = 0x181D27;
	std::uint32_t text = 0xE6EAF2;
	std::uint32_t keyword = 0xC792EA;
	std::uint32_t comment = 0x5F6B7E;
	std::uint32_t string = 0x9ECE6A;
	std::uint32_t number = 0xFF9E64;
	std::uint32_t op = 0x89DDFF;
	std::uint32_t library = 0x7AA2F7;
	std::uint32_t api = 0xE0AF68;
	std::uint32_t errorLine = 0x3A1D1F;
	std::uint32_t errorText = 0xD9534F;
	std::uint32_t caret = 0x7AA2F7;

	/// Width of the line-number gutter, in characters.
	float gutterChars = 4.0f;
	/// Gap between the gutter and the text.
	float gutterPad = 1.0f;
};

/// One editor, with the scroll and caret state ImGui needs between frames.
///
/// Owns no document: the caller passes a ScriptDocument each frame, so the
/// document can be swapped (a different script opened) without recreating the
/// widget, and the document stays testable with no ImGui at all.
class CodeEditor {
public:
	/// Everything the operator did while the editor was on screen.
	struct Result {
		/// The text was modified this frame.
		bool edited = false;
		/// Ctrl+S, or the toolbar's Save. The caller decides what saving means.
		bool saveRequested = false;
		/// The caret is on this line (1-based), for the status bar.
		std::size_t caretLine = 1;
	};

	/// Draw the editor. `height` of 0 fills the remaining space.
	///
	/// `readOnly` is used while a validation error is unresolved, so a broken
	/// draft cannot be typed over the moment the operator looks away - and so the
	/// error marker stays where it was placed.
	Result draw(ScriptDocument& document, const CodeEditorStyle& style,
		float height = 0.0f, bool readOnly = false);

	/// Put the caret on `line` (1-based) and scroll it into view. Used when an
	/// error arrives: the operator's eye should land on the problem, not on
	/// wherever the caret happened to be.
	void revealLine(std::size_t line);

	/// True when the caret should be moved on the next frame.
	bool hasPendingReveal() const { return pendingReveal_ != 0; }

private:
	/// Draw the gutter: line numbers, with the error line marked.
	void drawGutter(ImDrawList* drawList, const ScriptDocument& document,
		const CodeEditorStyle& style, ImVec2 origin, float lineHeight);

	/// Draw the coloured text over the hidden editing widget.
	void drawText(ImDrawList* drawList, const ScriptDocument& document,
		const CodeEditorStyle& style, ImVec2 origin, float lineHeight,
		float textStart);

	/// The 1-based line to scroll to on the next frame, or 0.
	std::size_t pendingReveal_ = 0;

	/// Height the editor had last frame, so the gutter can be sized before the
	/// text widget is drawn.
	float lastHeight_ = 0.0f;
};

} // namespace media::ui
