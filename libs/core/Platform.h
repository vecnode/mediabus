#pragma once

#include <string>

/// Cross-platform process/window facts that the app needs before any media
/// library is involved. Implemented per platform in Platform.cpp.

namespace media::platform {

/// Absolute path of the directory containing the running executable.
/// The data root is resolved relative to this, so the app does not depend on
/// the current working directory (a controller may launch it from anywhere).
std::string executableDirectory();

/// Absolute path of `<executableDirectory>/data`.
std::string dataDirectory();

/// Absolute path of `<dataDirectory>/scripts` — the only directory mpv scripts
/// are ever loaded from. Scripts are operator-installed; nothing is discovered
/// implicitly, and the user's own mpv config directory is never consulted.
std::string scriptsDirectory();

/// Lowercased file extension including the dot, e.g. ".mp4".
std::string lowerExtension(const std::string& path);

} // namespace media::platform
