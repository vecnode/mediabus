#include "core/AppConfig.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace media::config {
namespace fs = std::filesystem;

namespace {

/// Test-only redirection of configPath(). Empty means "use the default".
std::string gConfigPathOverride;

/// Escape the four characters that would otherwise make the line ambiguous.
/// Backslash first, or the escapes introduced below would be doubled.
std::string escape(const std::string& value) {
	std::string out;
	out.reserve(value.size() + 8);
	for (char c : value) {
		switch (c) {
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '=':  out += "\\e"; break;
			default:   out.push_back(c); break;
		}
	}
	return out;
}

std::string unescape(const std::string& value) {
	std::string out;
	out.reserve(value.size());
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] != '\\' || i + 1 >= value.size()) {
			out.push_back(value[i]);
			continue;
		}
		switch (value[++i]) {
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 'e': out.push_back('='); break;
			case '\\': out.push_back('\\'); break;
			// Unknown escape: keep both characters rather than losing one.
			default:
				out.push_back('\\');
				out.push_back(value[i]);
				break;
		}
	}
	return out;
}

std::string trim(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return {};
	}
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

} // namespace

std::string configPath() {
	if (!gConfigPathOverride.empty()) {
		return gConfigPathOverride;
	}
	return platform::executableDirectory() + "mediabus.ini";
}

void setConfigPathOverride(std::string path) {
	gConfigPathOverride = std::move(path);
}

void parse(const std::string& text, Config& out) {
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line)) {
		const std::string trimmed = trim(line);
		if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
			continue;
		}
		const std::size_t equals = trimmed.find('=');
		if (equals == std::string::npos) {
			continue;   // not a key/value line; ignore rather than refuse
		}
		const std::string key = trim(trimmed.substr(0, equals));
		const std::string value = unescape(trim(trimmed.substr(equals + 1)));

		if (key == kKeyMediaFolder) {
			// Repeated lines, one folder each, in order. An empty value is not a
			// folder: it is what a file written with "nothing chosen" contains, so
			// it is skipped rather than added as an empty path. That also makes a
			// later line win over an earlier empty one, which is what a person
			// editing the file by hand would expect.
			if (!value.empty()) {
				out.mediaFolders.push_back(value);
			}
		} else if (key == kKeyUiScale) {
			// strtof, not stof: a malformed number must not throw out of a
			// config load. Out-of-range values are rejected by the caller's
			// sanitizeScale; here only "is it a number" matters.
			const char* begin = value.c_str();
			char* end = nullptr;
			const float parsed = std::strtof(begin, &end);
			if (end != begin) {
				out.uiScale = parsed;
			}
		}
		// Any other key: ignored on purpose, see the header.
	}
}

std::string serialize(const Config& in) {
	std::ostringstream out;
	out << "# vn-mediabus settings.\n"
		<< "# Written by the Dashboard and the Controller. Safe to edit by hand.\n"
		<< "#\n"
		<< "# mediaFolder  one line PER FOLDER the Player reads. The corpus is the\n"
		<< "#              merge of all of them, de-duplicated, so two folders that\n"
		<< "#              overlap cannot list a file twice. Repeat the line to add\n"
		<< "#              another; delete a line to drop that folder. NO line at all\n"
		<< "#              (or an empty value) means no folder has been chosen: the\n"
		<< "#              Player then reads only the shader library that ships with\n"
		<< "#              it, and never falls back to <exeDir>\\data - a fresh\n"
		<< "#              install must not silently play whatever the build shipped.\n"
		<< "#              A folder that does not exist is skipped with a warning and\n"
		<< "#              the others are still read. A set of folders far larger than\n"
		<< "#              a media corpus is refused whole rather than walked, so a\n"
		<< "#              home directory here cannot make the Player look hung.\n"
		<< "# uiScale      extra text size multiplier on top of the monitor DPI.\n"
		<< "#              0 or absent leaves the DPI decision alone.\n"
		<< "\n";
	if (in.mediaFolders.empty()) {
		// The key is still written with no value: "nothing chosen" is a real state
		// and the file should say so rather than leaving a reader to guess whether
		// the key is absent because it was never set.
		out << kKeyMediaFolder << " = \n";
	} else {
		for (const std::string& folder : in.mediaFolders) {
			out << kKeyMediaFolder << " = " << escape(folder) << "\n";
		}
	}
	out << kKeyUiScale << " = " << in.uiScale << "\n";
	return out.str();
}

bool load(const std::string& path, Config& out) {
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		return false;   // first run: defaults are the answer, not an error
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		LOG_WARN("AppConfig") << "cannot read " << path << "; using defaults";
		return false;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	parse(buffer.str(), out);
	return true;
}

bool load(Config& out) {
	return load(configPath(), out);
}

bool save(const std::string& path, const Config& in) {
	std::error_code ec;
	const fs::path target(path);
	if (target.has_parent_path()) {
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			LOG_ERROR("AppConfig") << "cannot create " << target.parent_path().string()
				<< ": " << ec.message();
			return false;
		}
	}
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		LOG_ERROR("AppConfig") << "cannot write " << path;
		return false;
	}
	file << serialize(in);
	if (!file) {
		LOG_ERROR("AppConfig") << "write failed for " << path;
		return false;
	}
	return true;
}

bool save(const Config& in) {
	return save(configPath(), in);
}

} // namespace media::config
