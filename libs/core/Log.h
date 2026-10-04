#pragma once

/// Minimal logging facade.
///
/// Deliberately tiny: one stream-style statement per line, with no logging
/// framework behind it. Both applications share it.
///
/// Usage:
///     LOG_NOTICE("HttpControlServer") << "listening on port " << port;
///
/// Output goes to stderr so stdout stays clean for structured output.

#include <iostream>
#include <sstream>
#include <string>

namespace media::log {

enum class Level { Verbose, Notice, Warning, Error };

/// Messages below this level are discarded. Defaults to Notice; set to Verbose
/// with MEDIA_LOG_LEVEL=verbose in the environment.
Level& threshold();

/// True when messages at `level` would be emitted.
bool enabled(Level level);

void setThresholdFromEnv();

/// Choose the log's destination explicitly, rather than on the first line.
///
/// Normally there is no need to call this: the first logged line does it. It
/// exists for a caller that wants the decision made - and the log file opened -
/// before anything is written, so a startup failure is never the thing that
/// arrives without a destination.
///
/// The destination is stderr when the process has a console or its stderr has
/// been redirected (a shell, a script), and `<exeDir>/mediabus-<name>.log`
/// otherwise. `name` is the application, and it names that file.
void useFileSink(const char* name);

/// Write one finished line to whichever sink is in use. Exposed so a caller that
/// has its own message to emit - the ImGui assert path, for instance - goes to
/// the same place as everything else instead of to a stderr that may not exist.
void writeLine(const char* level, const char* category, const std::string& text);

/// Accumulates one message and emits it on destruction, so a statement is
/// always a single atomic line even when several threads log at once.
class Line {
public:
	Line(Level level, const char* category);
	~Line();

	Line(const Line&) = delete;
	Line& operator=(const Line&) = delete;

	template <typename T>
	Line& operator<<(const T& value) {
		if (active_) {
			stream_ << value;
		}
		return *this;
	}

private:
	Level level_;
	const char* category_;
	std::ostringstream stream_;
	bool active_;
};

} // namespace media::log

#define MEDIA_LOG(level, category) \
	::media::log::Line(::media::log::Level::level, category)

#define LOG_VERBOSE(cat) MEDIA_LOG(Verbose, cat)
#define LOG_NOTICE(cat)  MEDIA_LOG(Notice, cat)
#define LOG_WARN(cat)    MEDIA_LOG(Warning, cat)
#define LOG_ERROR(cat)   MEDIA_LOG(Error, cat)
