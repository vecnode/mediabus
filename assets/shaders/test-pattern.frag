#version 330 core

// test-pattern.frag - colour bars, a crosshair and a moving sweep.
//
// See README.md for the contract. This is the diagnostic clip: the bars make a
// wrong colour space or a channel swap obvious, the crosshair shows whether the
// frame is centred and correctly scaled, and the sweep proves the frame is live
// rather than a still. If something looks wrong in the Player, this is the clip
// to open first.

in vec2 v_uv;
out vec4 fragColor;

uniform float u_time;
uniform vec2 u_resolution;

void main() {
	vec2 uv = v_uv;

	// Eight vertical bars, written as a chain rather than an array so the shader
	// stays readable and cannot trip over array-construction differences between
	// drivers.
	float bar = floor(uv.x * 8.0);
	vec3 colour = vec3(0.0);
	if (bar < 0.5) {
		colour = vec3(1.0, 1.0, 1.0);
	} else if (bar < 1.5) {
		colour = vec3(1.0, 1.0, 0.0);
	} else if (bar < 2.5) {
		colour = vec3(0.0, 1.0, 1.0);
	} else if (bar < 3.5) {
		colour = vec3(0.0, 1.0, 0.0);
	} else if (bar < 4.5) {
		colour = vec3(1.0, 0.0, 1.0);
	} else if (bar < 5.5) {
		colour = vec3(1.0, 0.0, 0.0);
	} else if (bar < 6.5) {
		colour = vec3(0.0, 0.0, 1.0);
	} else {
		colour = vec3(0.0, 0.0, 0.0);
	}

	// A sweep across the lower third: one traversal every four seconds.
	float sweep = fract(u_time * 0.25);
	if (uv.y > 0.66 && abs(uv.x - sweep) < 0.006) {
		colour = vec3(0.90, 0.20, 0.20);
	}

	// Centre crosshair, so a half-pixel offset or a stretch is visible.
	if (abs(uv.x - 0.5) < 0.0015 || abs(uv.y - 0.5) < 0.0015) {
		colour = vec3(0.90, 0.90, 0.90);
	}

	fragColor = vec4(colour, 1.0);
}
