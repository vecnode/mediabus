#pragma once

#include <string>

/// The small amount of state the three applications share across runs.
///
/// At the moment that is one thing: **which folder the Player's media corpus
/// lives in**. The Dashboard is where it is set and displayed, the Controller
/// can ask for it to change, and the Player reads it before its first scan.
///
/// It lives in a text file next to the executable (`bin\mediaplayer.ini`), not
/// in the registry and not in `%APPDATA%`: the whole point of this repository is
/// a folder you can copy somewhere and run, so the setting has to travel with
/// it. The format is deliberately dumb - `key = value`, one per line, with the
/// four characters that would break parsing backslash-escaped - so a person can
/// read and edit it, and so it needs no JSON dependency in the core library.
///
/// Nothing here touches GL, HTTP or the window system, so it is testable
/// headless and safe to call from any thread.
namespace media::config {

/// Keys this build understands. Anything else in the file is ignored rather
/// than treated as an error, so an older binary does not refuse to start on a
/// file a newer one wrote.
inline constexpr const char* kKeyMediaFolder = "mediaFolder";
inline constexpr const char* kKeyUiScale = "uiScale";

/// The in-memory view of the file. Absent keys keep their default.
struct Config {
	/// Absolute path of the folder the Player scans for media. Empty means
	/// "whatever the caller's default is", which for the Player is
	/// `<exeDir>/data`.
	std::string mediaFolder;

	/// Multiplier applied on top of the DPI-derived text scale. 0 means "not
	/// set", which leaves the DPI decision alone. An operator who still finds
	/// the text small can set this in the file.
	float uiScale = 0.0f;
};

/// Absolute path of `<exeDir>/mediaplayer.ini`.
std::string configPath();

/// Point every subsequent load()/save() at `path` instead of configPath().
///
/// This exists for the tests, and it is not a nicety. The media-folder route
/// persists its choice, so a test that exercises that route would otherwise
/// write the *real* bin\mediaplayer.ini and leave the Player installed next to
/// it scanning a temporary test directory. Every test that can write the config
/// must redirect it first.
///
/// An empty path restores the default, which is what a test should do when it is
/// finished. Not thread-safe on purpose: it is a test fixture, set before the
/// work starts, and pretending otherwise would suggest it is safe to flip at
/// runtime.
void setConfigPathOverride(std::string path);

/// Read `path`. A missing or unreadable file yields the defaults, not an error:
/// a first run has no file and must not be a failure. Returns false only when
/// the file exists and could not be read.
bool load(const std::string& path, Config& out);

/// Convenience overload for configPath().
bool load(Config& out);

/// Write `path`, creating the parent directory when needed. Persisting an empty
/// mediaFolder writes the key with no value, which reads back as empty - that
/// is a meaningful state ("use the default"), so it is not omitted.
bool save(const std::string& path, const Config& in);

/// Convenience overload for configPath().
bool save(const Config& in);

/// Parse `text` in the file format. Exposed so the round trip and the escaping
/// rules can be tested without touching the disk.
void parse(const std::string& text, Config& out);

/// Serialise to the file format. Exposed for the same reason.
std::string serialize(const Config& in);

} // namespace media::config
