#pragma once

#include "net/HttpJsonClient.h"

#include <cstddef>
#include <string>
#include <vector>

namespace media {

/// One script the Controller can run, as its API reports it.
struct ScriptEntry {
	/// The bare file name, e.g. "controller-example.lua". Never a path.
	std::string name;
	/// True for the script the Controller has loaded right now.
	bool running = false;
	/// True when this is the file the editor holds, so the panel can mark it.
	bool open = false;
};

/// The answer to "does this text compile?".
struct ScriptValidation {
	bool ok = false;
	/// The 1-based line Lua blamed, or 0 when the message carries no line.
	std::size_t errorLine = 0;
	/// The message with the `chunk:line:` prefix already stripped.
	std::string message;
};

/// The Dashboard's view of the Controller's script directory.
///
/// The Dashboard is a *client* here, exactly as the Controller is a client of
/// the Player: it does not read or write the scripts directory itself. It asks
/// the Controller to list a script, read it, validate a draft and reload it, and
/// every one of those goes through the Controller's own containment rule. That
/// keeps one implementation of "which files may be touched" instead of two that
/// can drift, and it means the editor cannot open anything the Controller would
/// refuse to run.
///
/// Every call is short-fused (see HttpJsonClient): a Controller that is not
/// running fails in milliseconds rather than freezing the launcher.
class ScriptLibrary {
public:
	/// What went wrong, in one line, when a call fails.
	struct Result {
		bool ok = false;
		std::string error;
	};

	explicit ScriptLibrary(std::string host, int port);

	/// GET /api/controller/scripts - the scripts on disk.
	Result list(std::vector<ScriptEntry>& out);

	/// GET /api/controller/script-content?name=... - the text of one script.
	Result read(const std::string& name, std::string& text);

	/// POST /api/controller/script-save - write the text back.
	Result write(const std::string& name, const std::string& text);

	/// POST /api/controller/validate - compile the text without running it.
	/// A failed validation is a SUCCESSFUL call: `ok` is true when the
	/// Controller answered at all, and the answer says whether the Lua compiled.
	Result validate(const std::string& name, const std::string& text,
		ScriptValidation& out);

	/// POST /api/controller/reload-script - make the Controller re-read the
	/// running script from disk.
	Result reload();

	/// POST /api/controller/script - run a script by name.
	Result run(const std::string& name);

	const std::string& host() const { return client_.host(); }
	int port() const { return client_.port(); }

private:
	HttpJsonClient client_;
};

} // namespace media
