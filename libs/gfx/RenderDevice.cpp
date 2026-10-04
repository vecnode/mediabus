#include "gfx/RenderDevice.h"

// Factory lives here so callers can create a device without pulling in GLEW or
// any other API header: only GlRenderDevice.cpp knows about OpenGL.
//
// When a second backend is added (Vulkan, D3D12, SDL3_GPU), this is the one
// place that selects it.

namespace media {

std::unique_ptr<RenderDevice> createGlRenderDevice();

} // namespace media
