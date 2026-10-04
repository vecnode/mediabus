#include "core/Log.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace media::log {
namespace {

std::mutex gMutex;
Level gThreshold = Level::Notice;

const char* levelName(Level level) {
	switch (level) {
		case Level::Verbose: return "verbose";
		case Level::Notice:  return "notice";
		case Level::Warning: return "warning";
		case Level::Error:   return "error";
	}
	return "?";
}

} // namespace

Level& threshold() {
	return gThreshold;
}

bool enabled(Level level) {
	return static_cast<int>(level) >= static_cast<int>(gThreshold);
}

void setThresholdFromEnv() {
	const char* raw = std::getenv("MEDIA_LOG_LEVEL");
	if (raw == nullptr) {
		return;
	}
	if (std::strcmp(raw, "verbose") == 0 || std::strcmp(raw, "trace") == 0) {
		gThreshold = Level::Verbose;
	} else if (std::strcmp(raw, "notice") == 0 || std::strcmp(raw, "info") == 0) {
		gThreshold = Level::Notice;
	} else if (std::strcmp(raw, "warning") == 0 || std::strcmp(raw, "warn") == 0) {
		gThreshold = Level::Warning;
	} else if (std::strcmp(raw, "error") == 0) {
		gThreshold = Level::Error;
	}
}

Line::Line(Level level, const char* category)
	: level_(level), category_(category), active_(enabled(level)) {}

Line::~Line() {
	if (!active_) {
		return;
	}
	// One locked write so concurrent lines never interleave mid-message.
	std::lock_guard<std::mutex> lock(gMutex);
	std::cerr << '[' << levelName(level_) << "] " << category_ << ": "
		<< stream_.str() << '\n';
	std::cerr.flush();
}

} // namespace media::log
