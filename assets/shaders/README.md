# vn-mediabus shaders

Fragment shaders that the Player can use as media: a clip that is generated
rather than decoded, so there is always something to play and always something
to check the render path against.

This folder lives under `assets/`, which `.gitignore` puts back with `!assets/**`
— so everything here is tracked source and travels with a clone. That is the
point: a fresh checkout has usable media with no downloads and no media files in
the repository.

## The contract every shader here follows

A file ending in `.frag` is a complete fragment shader. The Player supplies a
full-screen triangle, so a shader only has to answer "what colour is this pixel".

```glsl
#version 330 core            // first line; GL 3.3 core, like the rest of the tree

in  vec2  v_uv;              // 0..1 across the frame, (0,0) top-left
uniform float u_time;        // seconds since the clip was opened
uniform vec2  u_resolution;  // framebuffer size in pixels
out vec4  fragColor;         // the pixel
```

Rules, all of which exist to keep a shader playable:

- **`#version 330 core` on the first line.** Anything older cannot assume the
  core profile's rules and anything newer may not compile on the target driver.
- **No textures and no files.** A clip is one self-contained file, so it cannot
  fail to open a second one, and it works with no media folder chosen at all.
- **`u_time` is the only thing that makes it move.** A shader that ignores it is
  a still image, which is perfectly valid — it just behaves like a photo clip.
- **Write `fragColor` on every path.** A shader that falls off the end of `main`
  leaves the pixel undefined and the frame torn.
- **Keep it cheap.** This runs once per pixel, every frame, at whatever the
  window size is. Loops of 4–8 iterations are fine; a loop over hundreds is not.
- **Pure ASCII, no byte-order mark.** The same rule the Lua scripts live under:
  a BOM in the first line breaks the `#version` directive.

## What is here

| File | What it is |
| ---- | ---------- |
| `plasma.frag` | A colour-cycling plasma field. Good default, obviously animated. |
| `gradient-waves.frag` | Slow horizontal waves through a cool palette. Calm backdrop. |
| `starfield.frag` | Three parallax star layers drifting upward. |
| `test-pattern.frag` | Colour bars, a centre crosshair and a moving sweep. Use it to check that a frame is live, correctly scaled and not being resampled. |
