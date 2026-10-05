#pragma once

#include "media/IClipSource.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace media {

/// Discovers images, videos and shader clips from one or more folders.
///
/// MULTIPLE ROOTS, MERGED. The corpus is the union of every root this library
/// was pointed at, de-duplicated by absolute path and sorted once. An operator
/// with media in three places configures three folders and gets one playlist,
/// rather than three players or a folder full of shortcuts.
///
/// PLUS A BUILT-IN SHADER LIBRARY that is not a folder anybody chose. It ships
/// with the application (assets/shaders), so it is always present, and it is
/// deliberately NOT one of the roots: see setBuiltinRoot().
///
/// Ordering and resolution rules:
///   - roots are resolved relative to the executable, not the cwd;
///   - images first, then videos, then shaders, each group by absolute path;
///   - duplicates (same path, case-insensitive) collapse, so two roots that
///     overlap cannot show the same file twice;
///   - nothing is walked unless a root was set: no roots and no built-in library
///     means an empty playlist and no filesystem work at all.
///
/// THE WALK IS INTERRUPTIBLE, AND THAT IS LOAD-BEARING. A "folder" is not
/// guaranteed to be a media corpus: point this at a home directory and it walks
/// hundreds of thousands of files. A synchronous walk of that is a window that
/// never paints, which is indistinguishable from a hang - and that was exactly
/// the bug that stopped the Player opening from the Dashboard. So the walk is
/// split into scanBegin()/scanStep() and a windowed caller advances it a slice
/// per frame. scan() remains the one-call version for tests and non-windowed
/// callers.
class MediaClipLibrary final : public IClipSource {
public:
	/// How far one scanStep() may get before handing control back.
	///
	/// Two budgets because either can bind first: a few thousand entries on a
	/// spinning disk are bounded by time, and a million on an NVMe by count.
	/// Whichever is exhausted first ends the step. A zero field means "no limit
	/// on this one".
	struct ScanBudget {
		std::size_t entries = 0;
		std::chrono::milliseconds time{0};

		/// The budget a frame loop uses: small enough that the rest of the frame
		/// - and the next one - fit comfortably inside 16 ms, so the window keeps
		/// painting while a large folder is walked.
		static ScanBudget perFrame() {
			return ScanBudget{4000, std::chrono::milliseconds(6)};
		}

		/// The budget a request handler uses. Generous enough that a real corpus
		/// finishes within the one call, which is what keeps the count those
		/// routes return final - the behaviour every existing caller relies on.
		static ScanBudget perRequest() {
			return ScanBudget{20000, std::chrono::milliseconds(250)};
		}

		/// No limit at all: finish the walk whatever it costs. What scan() uses.
		static ScanBudget unlimited() {
			return ScanBudget{kMaxEntriesSentinel, std::chrono::hours(1)};
		}

		/// "No entry limit" for the budget above. Not a real ceiling: the walk's
		/// own runaway guard is what actually bounds it.
		static constexpr std::size_t kMaxEntriesSentinel =
			static_cast<std::size_t>(-1);
	};

	/// The runaway guard. A media corpus is thousands of entries, not millions.
	/// Past this the walk stops and the playlist is REFUSED rather than truncated
	/// - see setScanEntryLimit() - so a root that turns out to be a whole drive,
	/// or a directory junction pointing at one, costs a bounded wait and a clear
	/// message instead of an endless walk and an arbitrary prefix of somebody's
	/// home directory being presented as a playlist.
	static constexpr std::size_t kMaxScanEntries = 500000;

	/// Tighten (or lift) the runaway guard. 0 restores kMaxScanEntries.
	///
	/// A host may want a smaller ceiling than the default; a test needs one to
	/// reach the guard at all without seeding half a million files.
	void setScanEntryLimit(std::size_t limit) {
		scanEntryLimit_ = limit == 0 ? kMaxScanEntries : limit;
	}
	std::size_t scanEntryLimit() const { return scanEntryLimit_; }

	/// Walk every root to completion. Blocks until it is done.
	///
	/// This is the contract the test suite and any non-interactive caller wants:
	/// when it returns, the playlist is complete and sorted.
	void scan();

	/// Start a walk without doing any of it. The playlist is emptied here.
	///
	/// When there is nothing to walk - no roots and no built-in library - this
	/// finishes immediately (scanning() stays false) with searchLog() explaining
	/// why, so a caller driving this from a frame loop is never left waiting on a
	/// walk that has nothing to do.
	void scanBegin();

	/// Carry a walk started by scanBegin() forward. Returns true when finished.
	///
	/// The playlist is sorted and searchLog() is set exactly once, on the call
	/// that returns true. Safe to call when no walk is running: it returns true
	/// immediately.
	bool scanStep(ScanBudget budget);

	/// True while a walk begun by scanBegin() has not finished.
	bool scanning() const { return walkActive_; }

	/// Entries (files and directories) examined by the current walk.
	std::size_t scanEntries() const { return scanEntries_; }

	/// Media files accepted by the current walk, i.e. the playlist as it stands
	/// while the walk is still running.
	std::size_t scanFound() const { return clips_.size(); }

	/// True when the runaway guard stopped the walk, in which case the playlist
	/// was REFUSED whole: the folder is reported as holding 0 clips rather than
	/// however many happened to be found before the limit.
	bool scanTruncated() const { return scanTruncated_; }

	// --- the roots ---------------------------------------------------------

	/// Replace the set of folders to merge. Empty means nothing was chosen, and
	/// nothing of the operator's is walked.
	void setRoots(std::vector<std::string> roots) { chosenRoots_ = std::move(roots); }
	const std::vector<std::string>& roots() const { return chosenRoots_; }

	/// Convenience for the single-folder case: replaces the set with one folder.
	/// An empty string clears the set, which is what "no folder chosen" means.
	void setRoot(std::string root) {
		chosenRoots_.clear();
		if (!root.empty()) {
			chosenRoots_.push_back(std::move(root));
		}
	}

	/// True when at least one folder was chosen. The built-in shader library is
	/// not a choice and must not make this true, or the Player would claim an
	/// operator had picked a corpus when they had not.
	bool hasRoot() const { return !chosenRoots_.empty(); }

	/// The first chosen root, or the built-in default when none was chosen.
	/// Kept for the callers that have exactly one folder to name; prefer roots().
	const std::string& root() const;

	/// The always-present shader folder, or empty when this build ships none.
	void setBuiltinRoot(std::string root) { builtinRoot_ = std::move(root); }
	const std::string& builtinRoot() const { return builtinRoot_; }
	bool hasBuiltinRoot() const { return !builtinRoot_.empty(); }

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
	std::size_t shaderCount() const;

	/// Resolve a playlist index for a clip by display name (case-insensitive).
	/// Returns false when no clip matches.
	bool indexForName(const std::string& name, std::size_t& outIndex) const;

private:
	/// Offer one path to the playlist. Shared by the incremental walk and the
	/// one-call scan so the two cannot disagree about what a clip is.
	void considerEntry(const std::filesystem::path& path);

	/// Sort, summarise and close the walk. Called exactly once, by the step that
	/// reaches the end of the last root - or by scanBegin() when no root could be
	/// opened at all.
	void finishScan();

	/// Open the next root's iterator. False when there are none left, or when
	/// none of the remaining ones could be opened.
	bool openNextRoot();

	std::vector<MediaClip> clips_;
	std::string searchLog_;
	/// Folders the operator chose. Empty means nothing was chosen.
	std::vector<std::string> chosenRoots_;
	/// The shader library that ships with the application: never a "chosen" root,
	/// and walked even when nothing was chosen.
	std::string builtinRoot_;

	// --- walk state, live only between scanBegin() and the step that ends it ---
	/// Every root this walk will visit: the built-in library first (a handful of
	/// files, and it guarantees the playlist is never empty for no reason), then
	/// each chosen folder.
	std::vector<std::string> roots_;
	std::size_t rootIndex_ = 0;
	bool walkActive_ = false;
	std::optional<std::filesystem::recursive_directory_iterator> iterator_;
	std::error_code scanError_;
	std::unordered_set<std::string> seen_;
	std::size_t scanEntries_ = 0;
	std::size_t scanEntryLimit_ = kMaxScanEntries;
	bool scanTruncated_ = false;
};

} // namespace media
