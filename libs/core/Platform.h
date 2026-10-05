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

/// Absolute path of the built-in shader library — the one media directory the
/// operator never chose — or empty when this build ships none.
///
/// Candidates are tried in order so a built tree and a packaged release both
/// work without the caller knowing which it is in:
///
///   `<exeDir>/data/shaders`       installed: data/ sits beside the executables
///   `<exeDir>/../assets/shaders`  built tree: bin/ holds the exes, assets/ is up
///                                 one level at the repository root
///   `<exeDir>/assets/shaders`     executables and assets side by side
///
/// The tracked source of truth is `assets/shaders/`, which `.gitignore` keeps
/// (with `!assets/**`), so a clone has usable media with no download. An empty
/// result is not an error: it means the playlist is simply whatever the operator
/// chose, and the Player says so and carries on.
std::string shaderDirectory();

/// Lowercased file extension including the dot, e.g. ".mp4".
std::string lowerExtension(const std::string& path);

} // namespace media::platform
