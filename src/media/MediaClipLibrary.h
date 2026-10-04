#pragma once

#include "media/IClipSource.h"

#include <string>
#include <vector>

namespace media {

/// Discovers images and videos from disk (default IClipSource implementation).
///
/// Ordering and resolution rules:
///   - roots are resolved relative to the executable, not the cwd;
///   - only `<exeDir>/data` is scanned (not the whole bin/ tree);
///   - images sort before videos, then by absolute path;
///   - duplicates (same path, case-insensitive) collapse.
class MediaClipLibrary final : public IClipSource {
public:
	/// Rescan the data directory. Safe to call repeatedly.
	void scan();

	/// Override the directory that scan() walks.
	///
	/// Defaults to `<exeDir>/data`, which is what the running player uses. Tests
	/// point it at a controlled temporary directory instead of the live media
	/// folder: otherwise the number of assertions depends on whatever clips
	/// happen to be installed, and a suite that silently checks less is worse
	/// than one that fails.
	void setRoot(std::string root) { rootOverride_ = std::move(root); }
	const std::string& root() const;

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
	/// Empty means "use the executable's data directory".
	std::string rootOverride_;
};

} // namespace media
