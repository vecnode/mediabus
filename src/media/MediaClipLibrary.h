#pragma once

#include "media/IClipSource.h"

#include <string>
#include <vector>

namespace media {

/// Discovers images and videos from disk (default IClipSource implementation).
///
/// Behaviour preserved from the openFrameworks build:
///   - roots are resolved relative to the executable, not the cwd;
///   - only `<exeDir>/data` is scanned (not the whole bin/ tree);
///   - images sort before videos, then by absolute path;
///   - duplicates (same path, case-insensitive) collapse.
class MediaClipLibrary final : public IClipSource {
public:
	/// Rescan the data directory. Safe to call repeatedly.
	void scan();

	bool empty() const override { return clips_.empty(); }
	std::size_t size() const override { return clips_.size(); }

	const MediaClip& clipAt(std::size_t index) const override;
	std::size_t nextIndex(std::size_t currentIndex) const override;
	std::size_t previousIndex(std::size_t currentIndex) const override;

	const std::vector<MediaClip>& allClips() const { return clips_; }

	/// Human-readable summary of what was searched, for diagnostics and the
	/// HTTP status payload.
	const std::string& searchLog() const { return searchLog_; }

	/// Number of entries of each type, for logging.
	std::size_t imageCount() const;

	/// Resolve a playlist index for a clip by display name (case-insensitive).
	/// Returns false when no clip matches.
	bool indexForName(const std::string& name, std::size_t& outIndex) const;

private:
	std::vector<MediaClip> clips_;
	std::string searchLog_;
};

} // namespace media
