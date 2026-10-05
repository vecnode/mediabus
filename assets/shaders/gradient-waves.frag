#version 330 core

// gradient-waves.frag - slow horizontal waves through a cool palette.
//
// See README.md for the contract. Deliberately calm and low-contrast: this is the
// one to leave on screen behind a Controller panel, so it must not fight the
// interface for attention or flicker.

in vec2 v_uv;
out vec4 fragColor;

uniform float u_time;
uniform vec2 u_resolution;

void main() {
	float t = u_time * 0.25;

	// Five harmonics, each quieter than the last, which is what makes the band
	// look like water rather than like a single sine wave.
	float wave = 0.0;
	for (int i = 0; i < 5; ++i) {
		float f = float(i) + 1.0;
		wave += sin(v_uv.x * f * 3.0 + t * f * 2.0) / f;
	}

	float band = clamp(v_uv.y + wave * 0.12, 0.0, 1.0);

	vec3 deep = vec3(0.03, 0.06, 0.14);
	vec3 mid = vec3(0.10, 0.35, 0.65);
	vec3 peak = vec3(0.85, 0.95, 1.00);

	vec3 colour = mix(deep, mid, smoothstep(0.0, 0.6, band));
	colour = mix(colour, peak, smoothstep(0.6, 1.0, band));

	fragColor = vec4(colour, 1.0);
}
