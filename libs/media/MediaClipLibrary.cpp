#include "media/MediaClipLibrary.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <unordered_set>

namespace media {
namespace fs = std::filesystem;

namespace {

constexpr const char* kVideoExtensions[] = {
	".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v", ".mpg", ".mpeg", ".wmv", ".flv", ".ts"
};

constexpr const char* kImageExtensions[] = {
	".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff"
};

/// Fragment shaders: clips that are GENERATED rather than decoded.
constexpr const char* kShaderExtensions[] = {
	".frag", ".glsl"
};

bool hasExtension(const std::string& extension, const char* const* table, std::size_t count) {
	if (extension.empty()) {
		return false;
	}
	for (std::size_t i = 0; i < count; ++i) {
		if (extension == table[i]) {
			return true;
		}
	}
	return false;
}

/// Playlist order: pictures, then moving pictures, then generated ones.
///
/// Shaders sort LAST on purpose. Choosing a media folder should show the media
/// rather than a plasma field; and a Player with no folder chosen still opens the
/// first shader instead of an empty screen, because the shader library is then
/// the only thing in the playlist.
int typeRank(ClipMediaType type) {
	switch (type) {
		case ClipMediaType::Image: return 0;
		case ClipMediaType::Video: return 1;
		case ClipMediaType::Shader: return 2;
	}
	return 3;
}

ClipMediaType mediaTypeForPath(const fs::path& path) {
	const std::string ext = platform::lowerExtension(path.string());
	// Shaders are tested first: a ".frag" has no other reading, and the image
	// branch below is the catch-all, so the order of these two is the decision.
	if (hasExtension(ext, kShaderExtensions, std::size(kShaderExtensions))) {
		return ClipMediaType::Shader;
	}
	if (hasExtension(ext, kVideoExtensions, std::size(kVideoExtensions))) {
		return ClipMediaType::Video;
	}
	// Anything with a known image extension, and any unknown-but-listed file,
	// is treated as an image: the image path is the safe default because it
	// cannot fail to open an audio device.
	return ClipMediaType::Image;
}

bool isMediaPath(const fs::path& path) {
	const std::string ext = platform::lowerExtension(path.string());
	return hasExtension(ext, kVideoExtensions, std::size(kVideoExtensions))
		|| hasExtension(ext, kImageExtensions, std::size(kImageExtensions))
		|| hasExtension(ext, kShaderExtensions, std::size(kShaderExtensions));
}

/// Case-insensitive absolute key, so the same file reached by two spellings is
/// only listed once - and so two roots that OVERLAP cannot list one file twice.
std::string dedupKey(const fs::path& path) {
	std::error_code ec;
	fs::path abs = fs::absolute(path, ec);
	if (ec) {
		abs = path;
	}
	abs.make_preferred();
	std::string key = abs.string();
	std::transform(key.begin(), key.end(), key.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return key;
}

std::string preferredString(const fs::path& path) {
	fs::path copy = path;
	copy.make_preferred();
	return copy.string();
}

} // namespace

const char* toString(ClipMediaType type) {
	switch (type) {
		case ClipMediaType::Image: return "image";
		case ClipMediaType::Video: return "video";
		case ClipMediaType::Shader: return "shader";
	}
	return "unknown";
}

const MediaClip& MediaClipLibrary::clipAt(std::size_t index) const {
	static const MediaClip kEmpty;
	if (index >= clips_.size()) {
		return kEmpty;
	}
	return clips_[index];
}

std::size_t MediaClipLibrary::nextIndex(std::size_t currentIndex) const {
	if (clips_.empty()) {
		return 0;
	}
	return (currentIndex + 1) % clips_.size();
}

std::size_t MediaClipLibrary::previousIndex(std::size_t currentIndex) const {
	if (clips_.empty()) {
		return 0;
	}
	if (currentIndex == 0) {
		return clips_.size() - 1;
	}
	return currentIndex - 1;
}

std::size_t MediaClipLibrary::imageCount() const {
	return static_cast<std::size_t>(std::count_if(clips_.begin(), clips_.end(),
		[](const MediaClip& clip) { return clip.mediaType == ClipMediaType::Image; }));
}

std::size_t MediaClipLibrary::shaderCount() const {
	return static_cast<std::size_t>(std::count_if(clips_.begin(), clips_.end(),
		[](const MediaClip& clip) { return clip.mediaType == ClipMediaType::Shader; }));
}

bool MediaClipLibrary::indexForName(const std::string& name, std::size_t& outIndex) const {
	std::string wanted = name;
	std::transform(wanted.begin(), wanted.end(), wanted.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (std::size_t i = 0; i < clips_.size(); ++i) {
		std::string have = clips_[i].displayName;
		std::transform(have.begin(), have.end(), have.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (have == wanted) {
			outIndex = i;
			return true;
		}
	}
	return false;
}

const std::string& MediaClipLibrary::root() const {
	if (!chosenRoots_.empty()) {
		return chosenRoots_.front();
	}
	static const std::string kDefault = platform::dataDirectory();
	return kDefault;
}

void MediaClipLibrary::scan() {
	// The one-call contract: when this returns the playlist is complete. It is
	// scanBegin() plus one unlimited slice, so the incremental path and this one
	// cannot drift apart - there is only one walk implementation.
	scanBegin();
	scanStep(ScanBudget::unlimited());
}

void MediaClipLibrary::scanBegin() {
	clips_.clear();
	searchLog_.clear();
	seen_.clear();
	seen_.reserve(1024);
	scanEntries_ = 0;
	scanTruncated_ = false;
	scanError_.clear();
	iterator_.reset();
	roots_.clear();
	rootIndex_ = 0;
	walkActive_ = false;

	// The built-in shader library goes FIRST: a handful of files that ship with
	// the application, so collecting it is nearly free, and doing it first means a
	// large chosen folder cannot delay the moment the playlist stops being empty.
	if (hasBuiltinRoot()) {
		roots_.push_back(builtinRoot_);
	}
	for (const std::string& chosen : chosenRoots_) {
		if (!chosen.empty()) {
			roots_.push_back(chosen);
		}
	}

	if (roots_.empty()) {
		// Nothing was chosen and this build ships no shader library, so there is
		// nothing to walk. The default data folder is a convenient place to LOOK
		// for a corpus, not a licence to go walking one: this is the rule that
		// keeps a fresh install from silently playing whatever happened to ship in
		// the build, and keeps "no clips" from being indistinguishable from "the
		// folder moved".
		searchLog_ = "no folder chosen";
		LOG_NOTICE("MediaClipLibrary") << "no folder chosen; nothing scanned";
		return;
	}

	if (!openNextRoot()) {
		// Every root was missing or unreadable. Close the walk out here so the
		// summary still describes what was attempted.
		finishScan();
		return;
	}
	walkActive_ = true;
}

bool MediaClipLibrary::openNextRoot() {
	while (rootIndex_ < roots_.size()) {
		const std::string candidate = roots_[rootIndex_++];

		std::error_code ec;
		if (!fs::is_directory(candidate, ec)) {
			// One bad root must not cost the others: an operator with three good
			// folders and a stale fourth still wants the three.
			LOG_WARN("MediaClipLibrary") << "skipping " << preferredString(candidate)
				<< ": not a folder";
			continue;
		}

		scanError_.clear();
		iterator_.emplace(candidate, scanError_);
		if (scanError_) {
			LOG_ERROR("MediaClipLibrary") << "cannot read " << preferredString(candidate)
				<< ": " << scanError_.message();
			scanError_.clear();
			iterator_.reset();
			continue;
		}
		return true;
	}
	return false;
}

bool MediaClipLibrary::scanStep(ScanBudget budget) {
	if (!walkActive_) {
		// Nothing running, and the summary was written by whichever call ended it.
		return true;
	}

	const fs::recursive_directory_iterator end;
	const bool timed = budget.time.count() > 0;
	const auto started = std::chrono::steady_clock::now();
	std::size_t visited = 0;

	while (walkActive_) {
		if (!iterator_.has_value() || *iterator_ == end) {
			// This root is exhausted (or was empty). Move to the next, or finish.
			iterator_.reset();
			if (!openNextRoot()) {
				break;
			}
			continue;
		}

		// Budgets are checked BEFORE the work, not after, so a budget is a ceiling
		// on the work done rather than on the work started - and at least one entry
		// always gets done, so a tiny budget still makes progress instead of
		// spinning on the same frame forever.
		if (budget.entries != 0 && visited >= budget.entries) {
			return false;
		}
		if (timed && visited != 0 && (visited % 64) == 0
			&& std::chrono::steady_clock::now() - started >= budget.time) {
			return false;
		}
		if (scanEntries_ >= scanEntryLimit_) {
			scanTruncated_ = true;
			// The playlist is thrown away rather than cut short. An arbitrary
			// prefix of a tree this large is not a corpus, and offering a slice of
			// somebody's home directory as their playlist would be worse than
			// offering nothing: it looks like the folder was read and holds those
			// files. Empty plus a reason is a state an operator can act on.
			clips_.clear();
			LOG_ERROR("MediaClipLibrary") << "refusing this root set: more than "
				<< scanEntryLimit_ << " entries across " << roots_.size()
				<< " root(s). That is a whole directory tree rather than a media"
				<< " folder, so no playlist was built. Point the corpus at the"
				<< " folders that hold the media.";
			break;
		}

		// Note the double dereference: optional::operator-> reaches the ITERATOR,
		// and the entry is one -> further in, because the iterator's own
		// operator-> is not applied twice. The path is taken BEFORE incrementing,
		// because an increment that fails leaves the iterator in no state to be
		// asked for an error message.
		const fs::path current = (*iterator_)->path();
		considerEntry(current);
		++scanEntries_;
		++visited;

		iterator_->increment(scanError_);
		if (scanError_) {
			// An unreadable directory ends THIS root. The others still deserve to
			// be read: discarding a good playlist because one subfolder is locked
			// would be the wrong trade.
			LOG_ERROR("MediaClipLibrary") << "stopping " << roots_[rootIndex_ - 1]
				<< " at " << preferredString(current) << ": " << scanError_.message();
			scanError_.clear();
			iterator_.reset();
			if (!openNextRoot()) {
				break;
			}
		}
	}

	walkActive_ = false;
	iterator_.reset();
	finishScan();
	return true;
}

void MediaClipLibrary::considerEntry(const fs::path& path) {
	std::error_code ec;
	if (!fs::is_regular_file(path, ec)) {
		return;
	}
	if (!isMediaPath(path)) {
		return;
	}
	if (!seen_.insert(dedupKey(path)).second) {
		return;
	}

	MediaClip clip;
	clip.absolutePath = preferredString(fs::absolute(path, ec));
	if (ec) {
		clip.absolutePath = preferredString(path);
		ec.clear();
	}
	clip.displayName = path.filename().string();
	clip.mediaType = mediaTypeForPath(path);
	clips_.push_back(std::move(clip));
	LOG_VERBOSE("MediaClipLibrary") << "  found " << path.filename().string();
}

void MediaClipLibrary::finishScan() {
	// One sort over the merged set: the ordering rule is a property of the
	// playlist, not of the order the roots happened to be walked in.
	std::sort(clips_.begin(), clips_.end(),
		[](const MediaClip& a, const MediaClip& b) {
			if (a.mediaType != b.mediaType) {
				return typeRank(a.mediaType) < typeRank(b.mediaType);
			}
			return a.absolutePath < b.absolutePath;
		});

	// Every root that was walked, so the summary names the folders the merged
	// playlist actually came from. A single root prints exactly as it always did.
	searchLog_.clear();
	for (std::size_t i = 0; i < roots_.size(); ++i) {
		if (i != 0) {
			searchLog_ += " + ";
		}
		searchLog_ += preferredString(roots_[i]);
	}
	searchLog_ += " (" + std::to_string(clips_.size()) + ")";
	if (scanTruncated_) {
		searchLog_ += " [refused: more than " + std::to_string(scanEntryLimit_)
			+ " entries]";
	}

	const std::size_t images = imageCount();
	const std::size_t shaders = shaderCount();
	const std::size_t videos = clips_.size() - images - shaders;
	LOG_NOTICE("MediaClipLibrary") << "Searched " << searchLog_;
	LOG_NOTICE("MediaClipLibrary") << "Total: " << clips_.size() << " media file(s) ("
		<< images << " image(s), " << videos << " video(s), " << shaders
		<< " shader(s))";
}

} // namespace media
