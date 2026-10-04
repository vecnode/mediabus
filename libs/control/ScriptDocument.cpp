#include "control/ScriptDocument.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace media {
namespace fs = std::filesystem;

namespace {

/// Names the Lua standard library puts in scope. Coloured as Library so a misspelt
/// one is visible, and so the host API below stands out against them.
const char* const kLuaLibraries[] = {
	"_G", "assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs",
	"load", "loadfile", "loadstring", "next", "pairs", "pcall", "print",
	"rawequal", "rawget", "rawset", "require", "select", "setmetatable",
	"tonumber", "tostring", "type", "unpack", "xpcall",
	"coroutine", "debug", "io", "math", "os", "package", "string", "table",
};

/// Names the two script hosts provide. Coloured as Api.
const char* const kHostApi[] = {
	"controller", "mp", "msg", "utils",
};

/// Names a Lua *global* that is actually the implicit `mp` table in an mpv
/// script. Included so the Player's reference script reads correctly.
const char* const kMpvFns[] = {
	"observe_property", "register_event", "register_script_message", "commandv",
	"command", "get_property", "get_property_number", "get_property_native",
	"set_property", "set_property_number", "get_script_name", "add_hook",
	"add_key_binding", "enable_messages", "get_time", "log", "message",
};

template <std::size_t N>
bool inList(const char* const (&list)[N], const std::string& word) {
	for (std::size_t i = 0; i < N; ++i) {
		if (word == list[i]) {
			return true;
		}
	}
	return false;
}

bool isIdentStart(unsigned char c) {
	return std::isalpha(c) != 0 || c == '_';
}

bool isIdentChar(unsigned char c) {
	return std::isalnum(c) != 0 || c == '_';
}

void pushSpan(std::vector<ScriptTokenSpan>& spans, std::size_t begin, std::size_t end,
	ScriptTokenKind kind) {
	if (end <= begin) {
		return;
	}
	// Coalesce with the previous span when it has the same kind: a run of
	// whitespace and a run of plain identifiers next to each other are one span
	// as far as drawing is concerned, and fewer spans means fewer draw commands.
	if (!spans.empty() && spans.back().kind == kind && spans.back().end == begin) {
		spans.back().end = end;
		return;
	}
	spans.push_back({begin, end, kind});
}

/// Length of a long bracket at `i`, or 0 when there is none.
///
/// A long bracket is `[`, N `=` signs, `[`. `level` receives N, because the
/// closing delimiter must match it exactly: `[==[ x ]==]`.
std::size_t longBracketLength(const std::string& line, std::size_t i, int& level) {
	if (i >= line.size() || line[i] != '[') {
		return 0;
	}
	std::size_t j = i + 1;
	int equals = 0;
	while (j < line.size() && line[j] == '=') {
		++equals;
		++j;
	}
	if (j >= line.size() || line[j] != '[') {
		return 0;
	}
	level = equals;
	return (j - i) + 1;
}

/// Length of the matching close bracket for `level`, or 0 when this line has
/// none.
std::size_t longCloseLength(const std::string& line, std::size_t i, int level) {
	if (i >= line.size() || line[i] != ']') {
		return 0;
	}
	std::size_t j = i + 1;
	int equals = 0;
	while (j < line.size() && line[j] == '=') {
		++equals;
		++j;
	}
	if (equals != level || j >= line.size() || line[j] != ']') {
		return 0;
	}
	return (j - i) + 1;
}

/// Resolve a bare script name to a path inside the scripts directory.
///
/// This is the containment rule, and it is the one every script route uses: a
/// bare `.lua` file name, no directory component, no drive, and the resolved path
/// must still be inside the directory.
///
/// It deliberately does NOT require the file to exist. The editor creates
/// scripts, and a check that has to read a file before it will authorise writing
/// one can never authorise creating a new one - which is exactly the bug this
/// replaced: save() used load() as its containment check, so saving a script
/// that did not exist yet failed with "cannot read ...".
bool resolveScriptPath(const std::string& name, const std::string& subdirectory,
	std::string& pathOut, std::string& error) {
	pathOut.clear();
	error.clear();

	if (name.empty()) {
		error = "no script name given";
		return false;
	}
	if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos
		|| name == "." || name == "..") {
		error = "a script is named, not pathed: '" + name + "' has a directory in it";
		return false;
	}
	if (platform::lowerExtension(name) != ".lua") {
		error = "only .lua scripts can be opened here";
		return false;
	}

	const std::string directory = platform::dataDirectory() + subdirectory;
	std::error_code ec;
	fs::path candidate = fs::path(directory) / name;
	candidate = fs::weakly_canonical(candidate, ec);
	if (ec) {
		// weakly_canonical fails when the file does not exist, which is the normal
		// case for a script being created. Fall back to the joined path, which is
		// still what the containment comparison needs.
		ec.clear();
		candidate = fs::path(directory) / name;
	}
	const fs::path root = fs::weakly_canonical(fs::path(directory), ec);
	if (!ec) {
		// A plain prefix test is not enough: a sibling directory named
		// "controller-scripts-extra" starts with "controller-scripts". Compare
		// against the directory WITH its separator, so only a real child passes.
		const std::string candidateText = candidate.string();
		std::string rootText = root.string();
		if (!rootText.empty() && rootText.back() != '\\' && rootText.back() != '/') {
			rootText.push_back(fs::path::preferred_separator);
		}
		if (candidateText.compare(0, rootText.size(), rootText) != 0) {
			error = "that script is outside " + directory;
			return false;
		}
	}

	pathOut = candidate.string();
	return true;
}

} // namespace

bool isLuaKeyword(const std::string& word) {
	static const char* const kKeywords[] = {
		"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
		"if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
		"true", "until", "while",
	};
	return inList(kKeywords, word);
}

void tokenizeLuaLine(const std::string& line, bool& inLongComment,
	bool& inLongString, int& longLevel, std::vector<ScriptTokenSpan>& spans) {
	spans.clear();
	std::size_t i = 0;

	// --- continuation of a long comment or a long string --------------------
	if (inLongComment || inLongString) {
		const bool wasComment = inLongComment;
		const std::size_t close = longCloseLength(line, 0, longLevel);
		if (close == 0) {
			// The whole line belongs to the block that started earlier.
			spans.push_back({0, line.size(),
				wasComment ? ScriptTokenKind::Comment : ScriptTokenKind::String});
			return;
		}
		inLongComment = false;
		inLongString = false;
		i = close;
		pushSpan(spans, 0, i,
			wasComment ? ScriptTokenKind::Comment : ScriptTokenKind::String);
	}

	while (i < line.size()) {
		const char c = line[i];

		// -- line comment, --[[ long comment
		if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
			const std::size_t start = i;
			i += 2;
			int level = 0;
			const std::size_t open = longBracketLength(line, i, level);
			if (open != 0) {
				i += open;
				const std::size_t close = longCloseLength(line, i, level);
				if (close != 0) {
					i += close;
				} else {
					// Runs past this line.
					inLongComment = true;
					longLevel = level;
				}
			} else {
				i = line.size();   // to end of line
			}
			pushSpan(spans, start, i, ScriptTokenKind::Comment);
			continue;
		}

		// [[ long string ]]
		{
			int level = 0;
			const std::size_t open = longBracketLength(line, i, level);
			if (open != 0) {
				const std::size_t start = i;
				i += open;
				const std::size_t close = longCloseLength(line, i, level);
				if (close != 0) {
					i += close;
				} else {
					inLongString = true;
					longLevel = level;
				}
				pushSpan(spans, start, i, ScriptTokenKind::String);
				continue;
			}
		}

		// 'x' or "x", with backslash escapes
		if (c == '\'' || c == '"') {
			const std::size_t start = i;
			const char quote = c;
			++i;
			while (i < line.size()) {
				if (line[i] == '\\' && i + 1 < line.size()) {
					i += 2;
					continue;
				}
				if (line[i] == quote) {
					++i;
					break;
				}
				++i;
			}
			pushSpan(spans, start, i, ScriptTokenKind::String);
			continue;
		}

		// A number: decimal, hex, or with an exponent. Lua 5.1 has no separators.
		if (std::isdigit(static_cast<unsigned char>(c)) != 0
			|| (c == '.' && i + 1 < line.size()
				&& std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0)) {
			const std::size_t start = i;
			if (c == '0' && i + 1 < line.size()
				&& (line[i + 1] == 'x' || line[i + 1] == 'X')) {
				i += 2;
				while (i < line.size()
					&& std::isxdigit(static_cast<unsigned char>(line[i])) != 0) {
					++i;
				}
			} else {
				while (i < line.size()
					&& (std::isdigit(static_cast<unsigned char>(line[i])) != 0
						|| line[i] == '.')) {
					++i;
				}
				if (i < line.size() && (line[i] == 'e' || line[i] == 'E')) {
					std::size_t j = i + 1;
					if (j < line.size() && (line[j] == '+' || line[j] == '-')) {
						++j;
					}
					if (j < line.size()
						&& std::isdigit(static_cast<unsigned char>(line[j])) != 0) {
						i = j;
						while (i < line.size()
							&& std::isdigit(static_cast<unsigned char>(line[i])) != 0) {
							++i;
						}
					}
				}
			}
			pushSpan(spans, start, i, ScriptTokenKind::Number);
			continue;
		}

		// An identifier, a keyword, or one of the two API tables.
		if (isIdentStart(static_cast<unsigned char>(c))) {
			const std::size_t start = i;
			while (i < line.size() && isIdentChar(static_cast<unsigned char>(line[i]))) {
				++i;
			}
			const std::string word = line.substr(start, i - start);
			ScriptTokenKind kind = ScriptTokenKind::Plain;
			if (isLuaKeyword(word)) {
				kind = ScriptTokenKind::Keyword;
			} else if (inList(kHostApi, word) || inList(kMpvFns, word)) {
				kind = ScriptTokenKind::Api;
			} else if (inList(kLuaLibraries, word)) {
				kind = ScriptTokenKind::Library;
			}
			pushSpan(spans, start, i, kind);
			continue;
		}

		// An operator or punctuation run. Grouped so `==` and `..` and `...` are
		// one span rather than three.
		if (std::isspace(static_cast<unsigned char>(c)) == 0) {
			const std::size_t start = i;
			static const char* const kOps[] = {
				"...", "..", "==", "~=", "<=", ">=", "::",
			};
			bool matched = false;
			for (const char* op : kOps) {
				const std::size_t length = std::char_traits<char>::length(op);
				if (line.compare(i, length, op) == 0) {
					i += length;
					matched = true;
					break;
				}
			}
			if (!matched) {
				++i;
			}
			pushSpan(spans, start, i, ScriptTokenKind::Operator);
			continue;
		}

		// Whitespace: plain, and coalesced with whatever plain text follows.
		{
			const std::size_t start = i;
			while (i < line.size()
				&& std::isspace(static_cast<unsigned char>(line[i])) != 0) {
				++i;
			}
			pushSpan(spans, start, i, ScriptTokenKind::Plain);
		}
	}
}

int parseLuaErrorLine(const std::string& message, std::string& messageOut) {
	messageOut = message;
	if (message.empty()) {
		return 0;
	}

	// Lua's shape is `<chunk>:<line>: <message>`. The chunk name may itself
	// contain colons (a Windows path, for instance), so the line number is taken
	// from the LAST colon that is followed by digits and then a colon or the end
	// of the prefix.
	std::size_t scan = 0;
	int best = 0;
	std::size_t messageStart = std::string::npos;

	while (scan < message.size()) {
		const std::size_t colon = message.find(':', scan);
		if (colon == std::string::npos) {
			break;
		}
		std::size_t j = colon + 1;
		while (j < message.size() && message[j] == ' ') {
			++j;
		}
		std::size_t digitsBegin = j;
		while (j < message.size() && std::isdigit(static_cast<unsigned char>(message[j])) != 0) {
			++j;
		}
		const bool hasDigits = j > digitsBegin;
		const bool followedByColon = (j < message.size() && message[j] == ':');
		if (hasDigits && followedByColon) {
			best = std::atoi(message.substr(digitsBegin, j - digitsBegin).c_str());
			messageStart = j + 1;
			break;   // the first such prefix is the one Lua emitted
		}
		scan = colon + 1;
	}

	if (best > 0 && messageStart != std::string::npos) {
		std::string rest = message.substr(messageStart);
		const std::size_t first = rest.find_first_not_of(' ');
		if (first != std::string::npos) {
			rest.erase(0, first);
		}
		// A runtime error carries a traceback after a newline; keep the first
		// line, which is the one that names the problem.
		const std::size_t newline = rest.find('\n');
		if (newline != std::string::npos) {
			rest.erase(newline);
		}
		messageOut = rest;
		return best;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// ScriptDocument
// ---------------------------------------------------------------------------

bool ScriptDocument::validateText(const std::string& text, std::string& error) {
	// A byte-order mark is a syntax error in Lua 5.1, at the very first byte, and
	// the message it produces points at line 1 column 1 without saying why. This
	// is the single most common way a script pasted from an editor fails.
	if (text.size() >= 3
		&& static_cast<unsigned char>(text[0]) == 0xEF
		&& static_cast<unsigned char>(text[1]) == 0xBB
		&& static_cast<unsigned char>(text[2]) == 0xBF) {
		error = "the file starts with a UTF-8 byte-order mark, which Lua 5.1 "
			"rejects as a syntax error; save it as plain UTF-8 without a BOM";
		return false;
	}
	if (text.find('\0') != std::string::npos) {
		error = "the text contains a NUL byte, which cannot appear in a Lua source file";
		return false;
	}
	error.clear();
	return true;
}

bool ScriptDocument::load(const std::string& name, const std::string& subdirectory,
	std::string& error) {
	error.clear();

	// Containment first: a bare .lua file name that resolves inside the scripts
	// directory. See resolveScriptPath for why this does not require the file to
	// exist - the writer needs the same check, and a new script does not exist.
	std::string resolved;
	if (!resolveScriptPath(name, subdirectory, resolved, error)) {
		return false;
	}

	std::ifstream in(resolved, std::ios::binary);
	if (!in) {
		error = "cannot read " + resolved;
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	std::string content = buffer.str();
	in.close();

	if (!validateText(content, error)) {
		return false;
	}

	name_ = name;
	subdirectory_ = subdirectory;
	path_ = resolved;
	setText(std::move(content));
	dirty_ = false;
	clearError();
	return true;
}

bool ScriptDocument::resolve(const std::string& name, const std::string& subdirectory,
	std::string& pathOut, std::string& error) {
	return resolveScriptPath(name, subdirectory, pathOut, error);
}

bool ScriptDocument::create(const std::string& name, const std::string& subdirectory,
	std::string& error) {
	error.clear();
	std::string resolved;
	if (!resolveScriptPath(name, subdirectory, resolved, error)) {
		return false;
	}

	// Adopt the name with an empty buffer. save() then writes it, which is how a
	// new script comes into being without a second write path existing.
	name_ = name;
	subdirectory_ = subdirectory;
	path_ = resolved;
	text_.clear();
	retokenize();
	dirty_ = false;
	clearError();
	return true;
}

bool ScriptDocument::save(std::string& error) {
	error.clear();
	if (!loaded()) {
		error = "no script is open";
		return false;
	}
	if (!validateText(text_, error)) {
		return false;
	}

	// Write with an explicit LF and no BOM. The validation above already
	// rejected a BOM on the way in; this is what stops one appearing on the way
	// out, and it also normalises whatever line endings the text field produced.
	std::string out;
	out.reserve(text_.size());
	for (std::size_t i = 0; i < text_.size(); ++i) {
		if (text_[i] == '\r') {
			if (i + 1 < text_.size() && text_[i + 1] == '\n') {
				continue;   // the \n that follows is kept
			}
			out.push_back('\n');
			continue;
		}
		out.push_back(text_[i]);
	}

	// Write beside the target and rename, so a crash mid-write cannot leave a
	// half-written script where a valid one used to be.
	const fs::path target(path_);
	const fs::path temporary = target.string() + ".tmp-write";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file) {
			error = "cannot write " + temporary.string();
			return false;
		}
		file.write(out.data(), static_cast<std::streamsize>(out.size()));
		file.flush();
		if (!file) {
			error = "writing " + temporary.string() + " failed";
			return false;
		}
	}
	std::error_code ec;
	fs::rename(temporary, target, ec);
	if (ec) {
		fs::remove(temporary, ec);
		error = "could not replace " + path_;
		return false;
	}
	dirty_ = false;
	LOG_NOTICE("ScriptDocument") << "saved " << path_ << " ("
		<< out.size() << " bytes)";
	return true;
}

void ScriptDocument::setText(std::string text) {
	// The editor's text field hands back whatever it was given; the document is
	// where a NUL or a BOM is refused, so nothing downstream has to check again.
	text_ = std::move(text);
	dirty_ = true;
	retokenize();
}

void ScriptDocument::retokenize() {
	lines_.clear();

	// Split on LF and drop a trailing CR, so a file with CRLF endings indexes the
	// same way as one with LF endings. The editor never shows the CR.
	bool inLongComment = false;
	bool inLongString = false;
	int longLevel = 0;

	std::size_t lineStart = 0;
	while (lineStart <= text_.size()) {
		std::size_t lineEnd = text_.find('\n', lineStart);
		const bool lastLine = (lineEnd == std::string::npos);
		if (lastLine) {
			lineEnd = text_.size();
		}

		std::string content = text_.substr(lineStart, lineEnd - lineStart);
		if (!content.empty() && content.back() == '\r') {
			content.pop_back();
		}

		ScriptLine line;
		line.begin = lineStart;
		line.end = lineStart + content.size();
		tokenizeLuaLine(content, inLongComment, inLongString, longLevel, line.spans);
		lines_.push_back(std::move(line));

		if (lastLine) {
			break;
		}
		lineStart = lineEnd + 1;
	}

	// An empty document still has one line, so the editor shows a caret.
	if (lines_.empty()) {
		lines_.push_back(ScriptLine{});
	}

	// Re-apply the error marker: retokenising must not lose it, and the line it
	// names must stay highlighted while the text is edited around it.
	if (errorLine_ > 0 && errorLine_ <= lines_.size()) {
		lines_[errorLine_ - 1].error = true;
		lines_[errorLine_ - 1].errorText = errorText_;
	}
}

std::size_t ScriptDocument::lineForOffset(std::size_t offset) const {
	// Binary search: the document can be a few thousand lines and this runs for
	// the status line on every frame the caret moves.
	std::size_t low = 0;
	std::size_t high = lines_.size();
	while (low < high) {
		const std::size_t mid = low + (high - low) / 2;
		if (lines_[mid].begin <= offset && (offset <= lines_[mid].end
				|| mid + 1 == lines_.size())) {
			return mid + 1;
		}
		if (lines_[mid].begin > offset) {
			high = mid;
		} else {
			low = mid + 1;
		}
	}
	return lines_.empty() ? 1 : lines_.size();
}

void ScriptDocument::setErrorLine(std::size_t line1Based, std::string message) {
	clearError();
	if (line1Based == 0 || line1Based > lines_.size()) {
		// An error the editor cannot place is still worth showing: it goes on the
		// status line with no gutter marker rather than being dropped.
		errorText_ = std::move(message);
		return;
	}
	errorLine_ = line1Based;
	errorText_ = std::move(message);
	lines_[line1Based - 1].error = true;
	lines_[line1Based - 1].errorText = errorText_;
}

void ScriptDocument::clearError() {
	if (errorLine_ > 0 && errorLine_ <= lines_.size()) {
		lines_[errorLine_ - 1].error = false;
		lines_[errorLine_ - 1].errorText.clear();
	}
	errorLine_ = 0;
	errorText_.clear();
}

} // namespace media
