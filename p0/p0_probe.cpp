/*
 * p0_probe.cpp ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â P0 preflight proof for the libmpv rewrite.
 *
 * Proves, on this machine, the four things the whole plan rests on:
 *   1. libmpv links and the client API version is usable.
 *   2. The render API is available with the OPENGL backend (vo=libmpv).
 *   3. mpv renders real video frames into an FBO we own.
 *   4. Subtitles are composited into that same FBO, and seeking works.
 *
 * Throwaway: not part of the app. Build with build.sh, run with libmpv on PATH.
 */

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

// On Windows, MSYS2's GL/gl.h declares only OpenGL 1.1 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â FBO entry points need
// a loader. GLEW first, and GLFW told not to pull in its own GL headers.
#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <clocale>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static void sleepMs(int ms) { Sleep(ms); }
#else
#include <chrono>
#include <thread>
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

static void* get_proc_address_mpv(void*, const char* name) {
	void* p = (void*)glfwGetProcAddress(name);
	if (!p) {
		std::fprintf(stderr, "  [render] glfwGetProcAddress failed: %s\n", name);
	}
	return p;
}

struct Fbo {
	GLuint fbo = 0, tex = 0, depth = 0;
	int w = 0, h = 0;

	void create(int width, int height) {
		w = width;
		h = height;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glGenRenderbuffers(1, &depth);
		glBindRenderbuffer(GL_RENDERBUFFER, depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

		const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		std::printf("  FBO %dx%d status=%s\n", w, h,
			st == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
};

// Average luma + max deviation across a band of the FBO. Used to distinguish
// "video is rendering" and "subtitle text is composited" from a black frame.
struct BandStat {
	double mean = 0.0;
	int maxv = 0;
	int bright = 0;  // pixels above 200 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â subtitle glyph coverage
};

static BandStat readBand(const Fbo& f, int y0, int y1) {
	std::vector<unsigned char> px(static_cast<size_t>(f.w) * (y1 - y0) * 4);
	BandStat s;
	glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, y0, f.w, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	long long acc = 0;
	for (size_t i = 0; i < px.size(); i += 4) {
		const int v = (px[i] * 30 + px[i + 1] * 59 + px[i + 2] * 11) / 100;
		acc += v;
		if (v > s.maxv) s.maxv = v;
		if (v > 200) ++s.bright;
	}
	s.mean = double(acc) / double(px.size() / 4);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return s;
}

static double getNumber(mpv_handle* mpv, const char* prop, double fallback = -1.0) {
	double v = fallback;
	if (mpv_get_property(mpv, prop, MPV_FORMAT_DOUBLE, &v) < 0) return fallback;
	return v;
}

static std::string getString(mpv_handle* mpv, const char* prop) {
	char* s = mpv_get_property_string(mpv, prop);
	if (!s) return "(n/a)";
	std::string out(s);
	mpv_free(s);
	return out;
}

int main(int argc, char** argv) {
	// REQUIRED by the libmpv API: mpv_create() fails (NULL) unless LC_NUMERIC is
	// "C". On a machine whose locale uses ',' as the decimal separator this is
	// otherwise a silent, baffling startup failure.
	std::setlocale(LC_NUMERIC, "C");

	const char* clip = (argc > 1) ? argv[1] : "sync-bip.mp4";
	const char* srt = (argc > 2) ? argv[2] : "test.srt";
	std::printf("=== P0 libmpv probe ===\nclip: %s\n\n", clip);
	std::fflush(stdout);

	// ---------------------------------------------------------------- 1. version
	const unsigned long api = mpv_client_api_version();
	std::printf("[1] client API version: %lu.%lu\n",
		(unsigned long)(api >> 16), (unsigned long)(api & 0xFFFF));

	// ---------------------------------------------------------------- 2. GL + render API
	if (!glfwInit()) {
		std::fprintf(stderr, "glfwInit failed\n");
		return 1;
	}
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* win = glfwCreateWindow(640, 360, "p0", nullptr, nullptr);
	if (!win) {
		std::fprintf(stderr, "glfwCreateWindow failed\n");
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(win);
	const GLenum glewStatus = glewInit();
	std::printf("[2] GL context: %s | %s\n",
		(const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
	std::printf("    glewInit: %s\n",
		glewStatus == GLEW_OK ? "OK" : (const char*)glewGetErrorString(glewStatus));
	if (glewStatus != GLEW_OK) {
		std::fprintf(stderr, "glewInit failed\n");
		return 1;
	}

	Fbo fbo;
	fbo.create(1280, 720);

	// ---------------------------------------------------------------- 3. mpv instance
	mpv_handle* mpv = mpv_create();
	if (!mpv) {
		std::fprintf(stderr, "mpv_create failed\n");
		return 1;
	}

	// Hardening + render contract, exactly as the plan specifies.
	struct Opt { const char* k; const char* v; };
	const Opt opts[] = {
		{"config", "no"},
		{"load-scripts", "no"},
		{"ytdl", "no"},
		{"load-unsafe-playlists", "no"},
		{"access-references", "no"},
		{"autoload-files", "no"},
		{"terminal", "no"},
		{"input-default-bindings", "no"},
		{"vo", "libmpv"},
		{"hwdec", "auto-safe"},
		{"keep-open", "yes"},
		{"video-timing-offset", "0"},
	};
	for (const Opt& o : opts) {
		const int rc = mpv_set_option_string(mpv, o.k, o.v);
		std::printf("  set %-24s = %-12s rc=%d\n", o.k, o.v, rc);
	}
	std::printf("\n[4] options set; calling mpv_initialize...\n");
	std::fflush(stdout);

	const int irc = mpv_initialize(mpv);
	std::printf("[5] mpv_initialize rc=%d (%s)\n", irc, mpv_error_string(irc));
	std::fflush(stdout);
	if (irc < 0) {
		std::fprintf(stderr, "mpv_initialize failed\n");
		return 1;
	}

	if (mpv_request_log_messages(mpv, "warn") < 0) {
		std::fprintf(stderr, "mpv_request_log_messages failed\n");
	}

	// ---------------------------------------------------------------- 4. render context
	mpv_opengl_init_params gl_init = { get_proc_address_mpv, nullptr };
	int advanced = 0;  // deliberately OFF: plan enables it only after thread stress
	mpv_render_param params[] = {
		{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
		{MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
		{MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
		{MPV_RENDER_PARAM_INVALID, nullptr},
	};
	mpv_render_context* rctx = nullptr;
	const int rrc = mpv_render_context_create(&rctx, mpv, params);
	std::printf("\n[3] render context (OPENGL): rc=%d %s\n", rrc, rrc < 0 ? "FAILED" : "OK");
	if (rrc < 0) {
		std::fprintf(stderr, "  -> %s\n", mpv_error_string(rrc));
		return 1;
	}

	// ---------------------------------------------------------------- 5. load + render
	std::printf("[4] loading %s\n", clip);
	const char* cmd[] = {"loadfile", clip, nullptr};
	mpv_command(mpv, cmd);

	mpv_opengl_fbo fbo_param = { (int)fbo.fbo, fbo.w, fbo.h, 0 };
	int flip = 1;
	mpv_render_param render_params[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &fbo_param},
		{MPV_RENDER_PARAM_FLIP_Y, &flip},
		{MPV_RENDER_PARAM_INVALID, nullptr},
	};

	bool loaded = false;
	for (int i = 0; i < 300 && !loaded; ++i) {
		while (true) {
			mpv_event* ev = mpv_wait_event(mpv, 0);
			if (!ev || ev->event_id == MPV_EVENT_NONE) break;
			if (ev->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
			if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
				mpv_event_log_message* m = (mpv_event_log_message*)ev->data;
				std::printf("  [mpv %s] %s", m->level, m->text);
			}
			if (ev->event_id == MPV_EVENT_END_FILE) {
				mpv_event_end_file* e = (mpv_event_end_file*)ev->data;
				std::printf("  [mpv] END_FILE reason=%d error=%s\n",
					(int)e->reason, mpv_error_string(e->error));
			}
		}
		if (!loaded) sleepMs(20);
	}
	std::printf("  FILE_LOADED: %s\n", loaded ? "yes" : "NO");

	// Let the decode chain actually produce frames, then draw.
	int drew = 0;
	BandStat full{}, subBand{}, base{}, baseSub{};
	for (int i = 0; i < 200 && drew < 12; ++i) {
		const uint64_t flags = mpv_render_context_update(rctx);
		if (flags & MPV_RENDER_UPDATE_FRAME) {
			const int rc = mpv_render_context_render(rctx, render_params);
			if (rc < 0) {
				std::fprintf(stderr, "  render failed: %s\n", mpv_error_string(rc));
				break;
			}
			++drew;
		}
		sleepMs(10);
	}
	std::printf("  frames rendered: %d\n", drew);
	const GLenum glerr = glGetError();
	std::printf("  glGetError after render: 0x%04X %s\n", glerr, glerr == GL_NO_ERROR ? "(clean)" : "(ERROR)");

	full = readBand(fbo, 0, fbo.h);              // whole frame
	subBand = readBand(fbo, 0, fbo.h / 5);       // bottom band (pre-flip: subtitle area)

	std::printf("\n[5] frame statistics\n");
	std::printf("  full frame      : mean luma %6.2f  max %3d  bright %d\n", full.mean, full.maxv, full.bright);
	std::printf("  bottom band     : mean luma %6.2f  max %3d  bright %d\n", subBand.mean, subBand.maxv, subBand.bright);

	// ---------------------------------------------------------------- 6. properties
	std::printf("\n[6] mpv reports\n");
	std::printf("  video-params/w    : %.0f\n", getNumber(mpv, "video-params/w"));
	std::printf("  video-params/h    : %.0f\n", getNumber(mpv, "video-params/h"));
	std::printf("  video-codec       : %s\n", getString(mpv, "video-codec").c_str());
	std::printf("  hwdec-current     : %s\n", getString(mpv, "hwdec-current").c_str());
	std::printf("  container-fps     : %.3f\n", getNumber(mpv, "container-fps"));
	std::printf("  duration          : %.3f\n", getNumber(mpv, "duration"));
	std::printf("  demuxer           : %s\n", getString(mpv, "demuxer").c_str());

	// ---------------------------------------------------------------- 7. audio + seek + subs
	std::printf("\n[7] seek + subtitles\n");
	const char* subAdd[] = {"sub-add", srt, nullptr};
	std::printf("  sub-add rc=%d\n", mpv_command(mpv, subAdd));
	sleepMs(150);

	const char* seekCmd[] = {"seek", "3.0", "absolute", nullptr};
	std::printf("  seek 3.0 rc=%d\n", mpv_command(mpv, seekCmd));
	for (int i = 0; i < 60; ++i) {
		const uint64_t flags = mpv_render_context_update(rctx);
		if (flags & MPV_RENDER_UPDATE_FRAME) mpv_render_context_render(rctx, render_params);
		sleepMs(10);
	}
	std::printf("  time-pos          : %.3f\n", getNumber(mpv, "time-pos"));
	std::printf("  sub-text          : %s\n", getString(mpv, "sub-text").c_str());
	std::printf("  sid               : %s\n", getString(mpv, "sid").c_str());
	std::printf("  aid               : %s\n", getString(mpv, "aid").c_str());
	std::printf("  audio-codec       : %s\n", getString(mpv, "audio-codec").c_str());
	std::printf("  audio-params/ch   : %s\n", getString(mpv, "audio-params/channel-count").c_str());

	const BandStat subAfter = readBand(fbo, 0, fbo.h / 5);
	std::printf("  bottom after subs : mean luma %6.2f  bright %d\n", subAfter.mean, subAfter.bright);

	// 7b. Controlled A/B on the SAME frozen frame: the only variable is whether
	// mpv composites the subtitle into our FBO. Comparing different frames was
	// meaningless because scene brightness swamped the overlay.
	//
	// Both states are given a long settle: toggling sub-visibility makes mpv
	// request an OSD redraw asynchronously, and rendering before that request
	// lands simply re-blits the previous (stale) frame.
	std::printf("\n[7b] subtitle A/B on one frozen frame\n");
	const char* pauseCmd[] = {"set", "pause", "yes", nullptr};
	mpv_command(mpv, pauseCmd);
	for (int i = 0; i < 30; ++i) {
		const uint64_t fl = mpv_render_context_update(rctx);
		if (fl & MPV_RENDER_UPDATE_FRAME) mpv_render_context_render(rctx, render_params);
		sleepMs(10);
	}

	auto renderSettled = [&](int frames, int settleMs) {
		mpv_command_string(mpv, "video-redraw yes");
		for (int i = 0; i < frames; ++i) {
			if (mpv_render_context_update(rctx) & MPV_RENDER_UPDATE_FRAME) {
				mpv_render_context_render(rctx, render_params);
			}
			sleepMs(settleMs);
		}
		// Force one final draw so the readback cannot be a stale previous frame.
		mpv_render_context_render(rctx, render_params);
	};

	mpv_command_string(mpv, "set sub-visibility no");
	renderSettled(20, 25);
	const BandStat withoutSubs = readBand(fbo, 0, fbo.h / 4);

	mpv_command_string(mpv, "set sub-visibility yes");
	renderSettled(20, 25);
	const BandStat withSubs = readBand(fbo, 0, fbo.h / 4);

	const int deltaBright = withSubs.bright - withoutSubs.bright;
	std::printf("  subtitles OFF : bottom bright %d  (mean %.2f)\n", withoutSubs.bright, withoutSubs.mean);
	std::printf("  subtitles ON  : bottom bright %d  (mean %.2f)\n", withSubs.bright, withSubs.mean);
	std::printf("  delta bright  : %+d  (subtitle glyph coverage)\n", deltaBright);
	const bool subsInFbo = deltaBright > 50;
	std::printf("  sub-visibility now: %s\n", getString(mpv, "sub-visibility").c_str());

	// 7c. Control: is it bottom-band mapping, or is the subtitle genuinely not
	// composited? Put an OSD message in the BOTTOM band. If that registers, the
	// readback band is right and the subtitle track is the problem.
	std::printf("\n[7c] bottom-band OSD control + track state\n");
	mpv_command_string(mpv, "set sub-visibility no");
	mpv_command_string(mpv, "set osd-level 3");
	mpv_command_string(mpv, "set osd-align-y bottom");
	mpv_command_string(mpv, "set osd-margin-y 40");
	mpv_command_string(mpv, "show-text BOTTOMBAND");
	renderSettled(12, 25);
	const BandStat osdBottomOn = readBand(fbo, 0, fbo.h / 4);
	mpv_command_string(mpv, "set osd-level 0");
	renderSettled(12, 25);
	const BandStat osdBottomOff = readBand(fbo, 0, fbo.h / 4);
	std::printf("  bottom OSD on  : bright %d (mean %.2f)\n", osdBottomOn.bright, osdBottomOn.mean);
	std::printf("  bottom OSD off : bright %d (mean %.2f)\n", osdBottomOff.bright, osdBottomOff.mean);
	const int deltaBottomOsd = osdBottomOn.bright - osdBottomOff.bright;
	std::printf("  delta bottom   : %+d   <- proves band mapping\n", deltaBottomOsd);

	// 7d. Subtitles WHILE PLAYING. mpv's subtitle path is tied to the video
	// redraw/OSD state; a paused player may keep an old frame, so test the way
	// the app actually plays media.
	std::printf("\n[7d] subtitle A/B while PLAYING\n");
	mpv_command_string(mpv, "set pause no");
	mpv_command_string(mpv, "set osd-level 0");
	mpv_command_string(mpv, "seek 1.0 absolute");
	renderSettled(20, 25);  // let playback actually start producing frames

	int playingDelta = 0;
	BandStat playOff{}, playOn{};
	for (int pass = 0; pass < 2; ++pass) {
		mpv_command_string(mpv, pass == 0 ? "set sub-visibility no" : "set sub-visibility yes");
		renderSettled(20, 25);
		const BandStat s = readBand(fbo, 0, fbo.h / 4);
		if (pass == 0) playOff = s; else playOn = s;
	}
	playingDelta = playOn.bright - playOff.bright;
	std::printf("  playing, subs off : bright %d (mean %.2f)\n", playOff.bright, playOff.mean);
	std::printf("  playing, subs on  : bright %d (mean %.2f)\n", playOn.bright, playOn.mean);
	std::printf("  delta playing     : %+d\n", playingDelta);

	mpv_command_string(mpv, "set sub-visibility yes");
	renderSettled(12, 25);
	std::printf("  track-list/count    : %s\n", getString(mpv, "track-list/count").c_str());
	std::printf("  track-list/2/type   : %s\n", getString(mpv, "track-list/2/type").c_str());
	std::printf("  sub-text            : %s\n", getString(mpv, "sub-text").c_str());
	std::printf("  sid / sub-vis     : %s / %s\n",
		getString(mpv, "sid").c_str(), getString(mpv, "sub-visibility").c_str());

	const bool subsInFbo2 = subsInFbo || playingDelta > 50;

	// ---------------------------------------------------------------- verdict
	std::printf("\n=== VERDICT ===\n");
	std::printf("  client API usable      : %s\n", api >= MPV_MAKE_VERSION(2, 0) ? "yes" : "NO");
	std::printf("  render ctx (OPENGL)    : %s\n", rrc >= 0 ? "yes" : "NO");
	std::printf("  video rendered to FBO  : %s\n", full.mean > 1.0 ? "yes" : "NO");
	std::printf("  subtitles in FBO       : %s (paused %+d / playing %+d)\n", subsInFbo2 ? "yes" : "NO", deltaBright, playingDelta);
	std::printf("  seek honoured          : %s\n", getNumber(mpv, "time-pos") > 2.0 ? "yes" : "NO");

	mpv_render_context_free(rctx);
	mpv_terminate_destroy(mpv);
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}
