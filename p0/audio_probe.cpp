// audio_probe — confirm libmpv actually opens an audio output, not just decodes.
//
// Run against a playing file: this creates its own mpv instance (no GL, no
// render context) so it needs no window, then reports the audio properties that
// prove the AO is live.

#include "backends/mpv/MPVSurface.h"

#include "core/Log.h"
#include "media/MediaClipLibrary.h"

#include <mpv/client.h>

#include <clocale>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

namespace {

std::string propString(mpv_handle* mpv, const char* name) {
	char* raw = mpv_get_property_string(mpv, name);
	if (raw == nullptr) {
		return "(n/a)";
	}
	std::string out(raw);
	mpv_free(raw);
	return out;
}

std::string propNumber(mpv_handle* mpv, const char* name) {
	double value = 0.0;
	if (mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &value) < 0) {
		return "(n/a)";
	}
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.3f", value);
	return buffer;
}

} // namespace

int main(int argc, char** argv) {
	std::setlocale(LC_NUMERIC, "C");
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 2) {
		std::printf("usage: audio_probe <media-file>\n");
		return 2;
	}

	mpv_handle* mpv = mpv_create();
	if (mpv == nullptr) {
		std::printf("mpv_create failed\n");
		return 1;
	}

	// Same hardening set the app uses; vo=null because there is no window here.
	mpv_set_option_string(mpv, "config", "no");
	mpv_set_option_string(mpv, "load-scripts", "no");
	mpv_set_option_string(mpv, "ytdl", "no");
	mpv_set_option_string(mpv, "terminal", "no");
	mpv_set_option_string(mpv, "vo", "null");
	mpv_set_option_string(mpv, "hwdec", "auto-safe");

	if (mpv_initialize(mpv) < 0) {
		std::printf("mpv_initialize failed\n");
		return 1;
	}

	const char* command[] = {"loadfile", argv[1], "replace", nullptr};
	if (mpv_command(mpv, command) < 0) {
		std::printf("loadfile failed\n");
		return 1;
	}

	std::printf("=== libmpv audio probe ===\nfile: %s\n\n", argv[1]);

	bool loaded = false;
	for (int i = 0; i < 300 && !loaded; ++i) {
		while (true) {
			mpv_event* event = mpv_wait_event(mpv, 0);
			if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
				break;
			}
			if (event->event_id == MPV_EVENT_FILE_LOADED) {
				loaded = true;
			}
			if (event->event_id == MPV_EVENT_END_FILE) {
				auto* end = static_cast<mpv_event_end_file*>(event->data);
				if (end != nullptr && end->error < 0) {
					std::printf("load error: %s\n", mpv_error_string(end->error));
					mpv_terminate_destroy(mpv);
					return 1;
				}
			}
		}
		if (!loaded) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	std::printf("file loaded        : %s\n", loaded ? "yes" : "NO");

	std::printf("audio-codec        : %s\n", propString(mpv, "audio-codec").c_str());
	std::printf("audio-params/ch    : %s\n", propString(mpv, "audio-params/channel-count").c_str());
	std::printf("audio-params/rate  : %s\n", propString(mpv, "audio-params/samplerate").c_str());
	std::printf("audio-out-params/ch: %s\n", propString(mpv, "audio-out-params/channel-count").c_str());
	std::printf("audio-device       : %s\n", propString(mpv, "audio-device").c_str());
	std::printf("aid                : %s\n", propString(mpv, "aid").c_str());

	// The decisive check: these two are only populated once an audio output has
	// actually been opened.
	std::printf("current-ao         : %s\n", propString(mpv, "current-ao").c_str());
	std::printf("ao-vol             : %s\n", propNumber(mpv, "ao-volume").c_str());

	// Let it play briefly and confirm the audio clock advances, which only
	// happens when samples are being consumed by a live AO.
	std::printf("\nplaying ~1.5s...\n");
	for (int i = 0; i < 75; ++i) {
		mpv_wait_event(mpv, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::printf("time-pos           : %s\n", propNumber(mpv, "time-pos").c_str());
	std::printf("audio-pts          : %s\n", propNumber(mpv, "audio-pts").c_str());
	std::printf("mute               : %s\n", propString(mpv, "mute").c_str());

	const std::string ao = propString(mpv, "current-ao");
	const std::string codec = propString(mpv, "audio-codec");
	const bool audioOk = codec != "(n/a)" && !codec.empty() && ao != "(n/a)" && !ao.empty();

	std::printf("\n=== VERDICT ===\n");
	std::printf("  audio decoded      : %s\n", codec != "(n/a)" ? "yes" : "NO");
	std::printf("  audio output open  : %s\n", ao != "(n/a)" ? "yes" : "NO");
	std::printf("  AUDIO PLAYING      : %s\n", audioOk ? "yes" : "NO");

	mpv_terminate_destroy(mpv);
	return audioOk ? 0 : 1;
}
