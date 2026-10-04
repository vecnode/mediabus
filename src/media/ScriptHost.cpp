#include "media/ScriptHost.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace media::scripts {
namespace fs = std::filesystem;

bool isScriptPath(const std::string& path) {
	const std::string ext = platform::lowerExtension(path);
	return ext == ".lua" || ext == ".js";
}

std::vector<ScriptFile> discover() {
	std::vector<ScriptFile> found;

	const fs::path directory = platform::scriptsDirectory();
	std::error_code ec;
	if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
		// Absent directory is normal: scripting is opt-in.
		LOG_VERBOSE("ScriptHost") << "no script directory at " << directory.string();
		return found;
	}

	for (fs::directory_iterator it(directory, ec), end; it != end; it.increment(ec)) {
		if (ec) {
			LOG_WARN("ScriptHost") << "cannot list " << directory.string() << ": "
				<< ec.message();
			break;
		}
		if (!it->is_regular_file(ec)) {
			continue;
		}
		const fs::path& path = it->path();
		if (!isScriptPath(path.string())) {
			LOG_VERBOSE("ScriptHost") << "skipping non-script file "
				<< path.filename().string();
			continue;
		}

		ScriptFile script;
		script.absolutePath = fs::absolute(path, ec).string();
		if (ec) {
			script.absolutePath = path.string();
			ec.clear();
		}
		script.name = path.filename().string();
		script.language = platform::lowerExtension(path.string()) == ".lua" ? "lua" : "js";
		found.push_back(std::move(script));
	}

	// Deterministic load order: several scripts messaging each other should see
	// the same ordering on every run.
	std::sort(found.begin(), found.end(),
		[](const ScriptFile& a, const ScriptFile& b) { return a.name < b.name; });

	if (!found.empty()) {
		for (const ScriptFile& script : found) {
			LOG_NOTICE("ScriptHost") << "found script " << script.name
				<< " (" << script.language << ")";
		}
	}
	return found;
}

} // namespace media::scripts
