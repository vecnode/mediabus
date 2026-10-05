#include "core/Platform.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace media::platform {
namespace fs = std::filesystem;

namespace {

fs::path rawExecutablePath() {
#ifdef _WIN32
	// GetModuleFileNameW handles long paths and non-ASCII install locations.
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;) {
		const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
			static_cast<DWORD>(buffer.size()));
		if (written == 0) {
			return {};
		}
		if (written < buffer.size() - 1) {
			buffer.resize(written);
			return fs::path(buffer);
		}
		buffer.resize(buffer.size() * 2);
	}
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buffer(size + 1, '\0');
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
		return {};
	}
	return fs::path(buffer.data());
#else
	std::vector<char> buffer(PATH_MAX, '\0');
	const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	if (written <= 0) {
		return {};
	}
	buffer.resize(static_cast<std::size_t>(written));
	return fs::path(buffer.data());
#endif
}

} // namespace

std::string executableDirectory() {
	std::error_code ec;
	fs::path path = rawExecutablePath();
	if (path.empty()) {
		// Fall back to the current directory rather than returning nothing: a
		// wrong-but-usable data root beats no data root at all.
		path = fs::current_path(ec);
	} else {
		path = path.parent_path();
	}
	path = fs::absolute(path, ec);
	if (ec) {
		return path.string();
	}
	path.make_preferred();
	std::string out = path.string();
	// Keep a trailing separator so callers can concatenate safely.
	if (!out.empty() && out.back() != '\\' && out.back() != '/') {
		out.push_back(fs::path::preferred_separator);
	}
	return out;
}

namespace {

/// Join a directory (which may or may not end in a separator) with a child
/// name. Guards against the "bindata" class of bug.
std::string joinPath(const std::string& directory, const std::string& child) {
	if (directory.empty()) {
		return child;
	}
	std::string out = directory;
	if (out.back() != '\\' && out.back() != '/') {
		out.push_back(fs::path::preferred_separator);
	}
	out += child;
	return out;
}

} // namespace

std::string dataDirectory() {
	return joinPath(executableDirectory(), "data");
}

std::string scriptsDirectory() {
	return joinPath(dataDirectory(), "scripts");
}

std::string shaderDirectory() {
	// Fixed candidates rather than configuration: this is the one directory the
	// Player reads that nobody chose, so it must resolve predictably. See the
	// header for why each candidate exists.
	const std::string exe = executableDirectory();
	const std::string candidates[] = {
		joinPath(exe, "data") + "/shaders",
		joinPath(exe, "..") + "/assets/shaders",
		joinPath(exe, "assets") + "/shaders",
	};

	std::error_code ec;
	for (const std::string& candidate : candidates) {
		if (!fs::is_directory(candidate, ec)) {
			ec.clear();
			continue;
		}
		// weakly_canonical rather than absolute: it collapses the ".." in the
		// built-tree candidate, so the path the log prints is the real folder
		// rather than one with "bin\..\assets" left in the middle of it.
		const fs::path resolved = fs::weakly_canonical(candidate, ec);
		if (ec) {
			return candidate;
		}
		fs::path preferred = resolved;
		preferred.make_preferred();
		return preferred.string();
	}
	return {};
}

std::string lowerExtension(const std::string& path) {
	const std::size_t slash = path.find_last_of("/\\");
	const std::size_t dot = path.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
		return {};
	}
	std::string ext = path.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext;
}

} // namespace media::platform
