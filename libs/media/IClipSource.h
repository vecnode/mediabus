#pragma once

#include <cstddef>
#include <string>

namespace media {

enum class ClipMediaType {
	Video,
	Image,
	/// A fragment shader: a clip that is GENERATED rather than decoded. It has no
	/// decoder, no timeline and no audio, so nothing in the playback path may
	/// hand one to mpv. See libs/shader/ShaderClipRenderer.
	Shader
};

const char* toString(ClipMediaType type);

/// Metadata for one item in the media playlist.
struct MediaClip {
	std::string absolutePath;
	std::string displayName;
	ClipMediaType mediaType = ClipMediaType::Video;
};

/// Playlist source abstraction — swap implementations (disk scan, JSON, M3U)
/// without touching the playback engine.
class IClipSource {
public:
	virtual ~IClipSource() = default;

	virtual bool empty() const = 0;
	virtual std::size_t size() const = 0;
	virtual const MediaClip& clipAt(std::size_t index) const = 0;
	virtual std::size_t nextIndex(std::size_t currentIndex) const = 0;
	virtual std::size_t previousIndex(std::size_t currentIndex) const = 0;
};

} // namespace media
