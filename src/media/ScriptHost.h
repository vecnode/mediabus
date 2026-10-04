#pragma once

#include <string>
#include <vector>

/// Discovery and validation of mpv scripts.
///
/// Scripts are a product feature: an operator can change player behaviour by
/// dropping a Lua or JS file into `<data>/scripts/` without rebuilding. The
/// security posture is deliberate and narrow:
///
///   * Only this one directory is ever scanned. mpv's own `load-scripts`
///     option (which would auto-run everything in the user's
///     `~/.config/mpv/scripts/`) stays OFF, so a file appearing elsewhere on
///     the machine can never execute.
///   * Paths are passed to mpv explicitly, one `--script=` per file.
///   * A script is a trusted, operator-installed artefact — it runs with the
///     full authority of the player by design. What is *not* permitted is
///     implicit execution, which is why `ytdl` also stays off: that is the one
///     built-in path by which media content could spawn a subprocess.

namespace media::scripts {

/// A script discovered on disk.
struct ScriptFile {
	/// Absolute path handed to mpv.
	std::string absolutePath;
	/// File name, for diagnostics and the HTTP status payload.
	std::string name;
	/// "lua" or "js", inferred from the extension.
	std::string language;

	/// `--script=<absolutePath>`.
	std::string optionValue() const { return absolutePath; }
};

/// Every runnable script in `<data>/scripts`, sorted by name for determinism.
/// Returns an empty vector (not an error) when the directory is absent, which
/// is the normal case for a fresh install.
std::vector<ScriptFile> discover();

/// Extensions mpv can execute here.
bool isScriptPath(const std::string& path);

} // namespace media::scripts
