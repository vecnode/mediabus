#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace media {

/// The one directory Controller scripts live in, relative to the data
/// directory. Spelled once, because discovery, the API's status payload, the
/// editor's load and its save all have to agree about it - and a save that
/// resolved to a *different* directory than discovery reads would be a very
/// quiet bug.
inline constexpr const char* kControllerScriptsSubdirectory = "\\controller-scripts";
/// The same, for the scripts mpv loads inside the Player.
inline constexpr const char* kPlayerScriptsSubdirectory = "\\scripts";

/// How one run of characters in a script is coloured.
///
/// Deliberately small. A Lua highlighter can be as clever as it likes, but every
/// extra category is another decision the reader has to make while reading, and
/// the point of the colours is that a syntax error is visible at a glance - not
/// that every token has its own hue.
enum class ScriptTokenKind {
	Plain,     ///< identifiers, whitespace, anything not covered below
	Keyword,   ///< local, function, end, if, then, for, while, return, ...
	Comment,   ///< -- to end of line, and --[[ ]] blocks
	String,    ///< 'x', "x", [[x]], [==[x]==]
	Number,    ///< 12, 3.5, 0x1F, 1e9
	Operator,  ///< + - * / % ^ # == ~= <= >= < > = ( ) { } [ ] ; : , . .. ...
	/// A name the Lua standard library provides: print, string, table, ...
	Library,
	/// A name *this* host provides: controller, mp. Coloured separately from
	/// Library because a script that calls the wrong one is a real mistake - a
	/// Player script has no `controller`, and a Controller script has no `mp`.
	Api,
};

/// One coloured run within a single line.
///
/// Spans never cross a line: a long comment or a long string is emitted as one
/// span per line, which is what lets the editor draw with ImGui's per-line text
/// calls and still colour a `--[[ ... ]]` block correctly.
struct ScriptTokenSpan {
	std::size_t begin = 0;   ///< byte offset into the line, inclusive
	std::size_t end = 0;     ///< byte offset into the line, exclusive
	ScriptTokenKind kind = ScriptTokenKind::Plain;
};

/// One line of the buffer, with the colouring computed for it.
struct ScriptLine {
	std::size_t begin = 0;   ///< byte offset into the document text
	std::size_t end = 0;     ///< byte offset of the line terminator, or of the end
	/// Where each line starts, so a byte offset can be turned into a line number
	/// without rescanning. Filled in by ScriptDocument::reindex.
	std::vector<ScriptTokenSpan> spans;

	/// True when this line is the one Lua reported an error on.
	bool error = false;
	/// The error text, on the error line only.
	std::string errorText;

	std::size_t length() const { return end - begin; }
};

/// A Lua script being edited, plus the colouring of its text.
///
/// This is the whole editor's non-visual half: no ImGui, no GL, no file dialog.
/// The visual half (libs/ui/widgets/CodeEditor) draws what this describes, which
/// is what lets the tokenizer and the line index be unit-tested with no window
/// and no font.
///
/// Editing rules that are enforced here rather than in the widget, because they
/// are statements about Lua rather than about drawing:
///
///   - the text must be pure ASCII with no byte-order mark. Lua 5.1 treats a BOM
///     as a syntax error at the very first byte, which is a confusing failure to
///     hand an operator who pasted from an editor that added one.
///   - a script is loaded and saved by *name* under the data directory, never by
///     an arbitrary path, matching the containment rule every other route in
///     this project follows.
class ScriptDocument {
public:
	/// Load `name` from `<data>/<subdirectory>/`, replacing whatever is open.
	///
	/// `name` is a bare file name. Anything with a directory component, or that
	/// resolves outside the directory, is refused. Returns false and fills
	/// `error` when it cannot be read.
	bool load(const std::string& name, const std::string& subdirectory,
		std::string& error);

	/// Adopt `name` under `<data>/<subdirectory>/` WITHOUT reading it, so a script
	/// that does not exist yet can be created by writing one.
	///
	/// The containment check is the same one load() uses. This is the entry point
	/// for "new script": load() cannot serve that purpose, because the file it
	/// would check for is precisely the one that is not there.
	bool create(const std::string& name, const std::string& subdirectory,
		std::string& error);

	/// Where a bare name would resolve to, without reading or writing anything.
	/// Exposed so a caller can ask "is this name acceptable?" and get the same
	/// answer load() and create() would.
	static bool resolve(const std::string& name, const std::string& subdirectory,
		std::string& pathOut, std::string& error);

	/// Write the buffer back to the file it was loaded from. Refuses when
	/// nothing was loaded or the text is not acceptable Lua source.
	bool save(std::string& error);

	/// True when there is a loaded file to save to.
	bool loaded() const { return !name_.empty(); }

	/// The bare name of the open script, empty when none.
	const std::string& name() const { return name_; }
	/// The directory it was loaded from, so a save goes back to the same place.
	const std::string& subdirectory() const { return subdirectory_; }
	/// Absolute path of the open file, empty when none.
	const std::string& path() const { return path_; }

	/// Replace the whole buffer, as an editor's text field would. Re-tokenises.
	void setText(std::string text);
	const std::string& text() const { return text_; }

	/// True when the buffer differs from what was loaded or last saved.
	bool dirty() const { return dirty_; }
	void markSaved() { dirty_ = false; }

	/// The line index and the colouring. Valid until the text changes.
	const std::vector<ScriptLine>& lines() const { return lines_; }
	std::size_t lineCount() const { return lines_.size(); }

	/// 1-based line number of a byte offset, for the status line.
	std::size_t lineForOffset(std::size_t offset) const;

	/// Record the line Lua complained about, and its message. 0 clears it.
	void setErrorLine(std::size_t line1Based, std::string message);
	void clearError();
	/// The 1-based line most recently reported, or 0. Without an error line the
	/// editor shows a marker in the gutter only when this is non-zero.
	std::size_t errorLine() const { return errorLine_; }
	const std::string& errorText() const { return errorText_; }

	/// Copy the buffer to `out` and report whether it is acceptable Lua source
	/// as far as the *text* rules go - ASCII, no BOM, no NUL. This does not
	/// compile it; the script host does that, because only it knows which Lua
	/// dialect is in play.
	static bool validateText(const std::string& text, std::string& error);

	/// Everything a line's spans describe, rebuilt from scratch. Called by
	/// setText and load; exposed so a test can drive it directly.
	void retokenize();

private:
	std::string name_;
	std::string subdirectory_;
	std::string path_;
	std::string text_;
	bool dirty_ = false;

	std::vector<ScriptLine> lines_;
	std::size_t errorLine_ = 0;
	std::string errorText_;
};

/// The Lua keyword list, for the tokenizer and for the editor's own tests.
bool isLuaKeyword(const std::string& word);

/// Run the Lua tokenizer over one line.
///
/// `inLongComment` and `inLongString` carry the multi-line state in and out: a
/// `--[[ ` opened on one line colours every following line until the `]]`, and
/// the tokenizer cannot know that from a single line. `longLevel` is the number
/// of `=` signs in the opening delimiter, because `[==[ ... ]==]` is legal Lua.
void tokenizeLuaLine(const std::string& line, bool& inLongComment,
	bool& inLongString, int& longLevel, std::vector<ScriptTokenSpan>& spans);

/// Extract the line number from a Lua compiler or runtime message and strip it.
///
/// Lua reports `<chunk>:<line>: <message>` for a syntax error and
/// `<chunk>:<line>: <message>` with a traceback for a runtime one. Returns 0 and
/// leaves `message` alone when there is no line in it.
int parseLuaErrorLine(const std::string& message, std::string& messageOut);

} // namespace media
