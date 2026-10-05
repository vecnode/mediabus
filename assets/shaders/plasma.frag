#version 330 core

// plasma.frag - a colour-cycling plasma field.
//
// See README.md for the contract. The shape is the classic interference of a few
// sine waves; mapping the result onto a phase-shifted palette rather than using
// it as RGB directly is what keeps the colours cycling instead of clipping to
// grey mud in the middle.

in vec2 v_uv;
out vec4 fragColor;

uniform float u_time;
uniform vec2 u_resolution;

void main() {
	// Correct for aspect ratio first, so the field does not stretch with the
	// window: a plasma that is obviously squashed reads as a bug in the Player.
	float aspect = u_resolution.x / max(u_resolution.y, 1.0);
	vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0) * 4.0;
	float t = u_time * 0.6;

	float v = sin(p.x + t);
	v += sin(p.y + t * 1.3);
	v += sin(p.x + p.y + t * 0.7);
	v += sin(length(p) * 3.0 - t * 2.0);
	v *= 0.25;

	// 0.5 + 0.5*sin() keeps every channel inside 0..1 by construction.
	const float kPi = 3.14159265;
	vec3 colour = vec3(
		0.5 + 0.5 * sin(kPi * v + 0.0),
		0.5 + 0.5 * sin(kPi * v + 2.0),
		0.5 + 0.5 * sin(kPi * v + 4.0));

	fragColor = vec4(colour, 1.0);
}
