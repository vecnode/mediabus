#version 330 core

// starfield.frag - three parallax star layers drifting upward.
//
// See README.md for the contract. The grid-and-hash trick is used rather than a
// particle list because a shader clip has no memory between frames: every pixel
// has to be derivable from u_time alone, and a hash of the cell it falls in is
// the cheapest way to place stars that stay put while the field moves.

in vec2 v_uv;
out vec4 fragColor;

uniform float u_time;
uniform vec2 u_resolution;

/// Deterministic pseudo-random value for a cell. Pure arithmetic, so the same
/// cell holds the same star on every frame and for every pixel in it.
float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void main() {
	float aspect = u_resolution.x / max(u_resolution.y, 1.0);
	vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0);
	float t = u_time * 0.08;

	vec3 colour = vec3(0.0);
	for (int layer = 0; layer < 3; ++layer) {
		float depth = float(layer) + 1.0;
		// Nearer layers move faster, which is the whole of the parallax.
		vec2 q = p * depth + vec2(0.0, -t * depth * 2.0);
		vec2 cell = floor(q * 12.0);
		vec2 local = fract(q * 12.0) - 0.5;

		// Only the brightest few cells hold a star, or the field fills in and
		// stops reading as a starfield at all.
		float bright = step(0.94, hash(cell));
		float glow = bright * smoothstep(0.35, 0.0, length(local)) / depth;
		colour += vec3(glow);
	}

	// Fade the top and bottom edges so the field does not look cropped when it
	// scrolls off.
	colour *= smoothstep(1.0, 0.2, abs(v_uv.y - 0.5) * 2.0);

	fragColor = vec4(colour, 1.0);
}
