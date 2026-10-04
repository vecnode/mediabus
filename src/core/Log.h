#pragma once

/// Minimal logging facade.
///
/// Deliberately tiny: the openFrameworks build used `ofLogNotice("X") << ...`
/// throughout, and this preserves that call shape so the ported code reads the
/// same without dragging in a logging framework.
///
/// Usage:
///     log::notice("HttpControlServer") << "listening on port " << port;
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
