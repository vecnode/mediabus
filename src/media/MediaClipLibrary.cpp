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

ClipMediaType mediaTypeForPath(const fs::path& path) {
	const std::string ext = platform::lowerExtension(path.string());
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
		|| hasExtension(ext, kImageExtensions, std::size(kImageExtensions));
}

/// Case-insensitive absolute key, so the same file reached by two spellings is
/// only listed once (Windows paths differ in case routinely).
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
	return type == ClipMediaType::Image ? "image" : "video";
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

void MediaClipLibrary::scan() {
	clips_.clear();
	searchLog_.clear();

	std::error_code ec;
	const fs::path root = platform::dataDirectory();
	if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
		searchLog_ = "no data folder at " + preferredString(root);
		LOG_ERROR("MediaClipLibrary") << searchLog_;
		return;
	}

	std::unordered_set<std::string> seen;
	seen.reserve(1024);
	std::size_t found = 0;

	for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
		if (ec) {
			LOG_ERROR("MediaClipLibrary") << "scan failed for " << preferredString(root)
				<< ": " << ec.message();
			break;
		}
		if (!it->is_regular_file(ec)) {
			continue;
		}

		const fs::path filePath = it->path();
		if (!isMediaPath(filePath)) {
			continue;
		}
		if (!seen.insert(dedupKey(filePath)).second) {
			continue;
		}

		MediaClip clip;
		clip.absolutePath = preferredString(fs::absolute(filePath, ec));
		if (ec) {
			clip.absolutePath = preferredString(filePath);
			ec.clear();
		}
		clip.displayName = filePath.filename().string();
		clip.mediaType = mediaTypeForPath(filePath);
		clips_.push_back(std::move(clip));
		++found;
		LOG_VERBOSE("MediaClipLibrary") << "  found " << filePath.filename().string();
	}

	// Stable order: images before videos, then by path.
	std::sort(clips_.begin(), clips_.end(),
		[](const MediaClip& a, const MediaClip& b) {
			if (a.mediaType != b.mediaType) {
				return a.mediaType == ClipMediaType::Image;
			}
			return a.absolutePath < b.absolutePath;
		});

	searchLog_ = preferredString(root) + " (" + std::to_string(found) + ")";
	const std::size_t images = imageCount();
	LOG_NOTICE("MediaClipLibrary") << "Searched " << searchLog_;
	LOG_NOTICE("MediaClipLibrary") << "Total: " << clips_.size() << " media file(s) ("
		<< images << " image(s), " << (clips_.size() - images) << " video(s))";
}

} // namespace media
