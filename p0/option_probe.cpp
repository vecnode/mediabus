// option_probe — ask libmpv which script-related options exist.
//
// The CLI has --script and --scripts, but libmpv exposes options through a
// different table and rejects unknown ones with "option not found". Guessing
// wastes more time than asking.

#include <mpv/client.h>

#include <clocale>
#include <cstdio>
#include <cstring>

namespace {

const char* status(int rc) {
	return rc >= 0 ? "ACCEPTED" : "rejected";
}

void trySet(mpv_handle* mpv, const char* name, const char* value) {
	const int rc = mpv_set_option_string(mpv, name, value);
	std::printf("  %-28s = %-46s -> %s (%s)\n", name, value, status(rc),
		rc >= 0 ? "ok" : mpv_error_string(rc));
}

void trySetProperty(mpv_handle* mpv, const char* name, const char* value) {
	const int rc = mpv_set_property_string(mpv, name, value);
	std::printf("  [property] %-19s = %-30s -> %s (%s)\n", name, value, status(rc),
		rc >= 0 ? "ok" : mpv_error_string(rc));
}

} // namespace

int main(int argc, char** argv) {
	std::setlocale(LC_NUMERIC, "C");
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const char* scriptPath = argc > 1 ? argv[1] : "C:/tmp/nonexistent.lua";

	std::printf("=== libmpv option probe ===\n");
	std::printf("client API %lu.%lu\n\n",
		(unsigned long)(mpv_client_api_version() >> 16),
		(unsigned long)(mpv_client_api_version() & 0xFFFF));

	// ---- phase 1: plain options, before initialize ----
	std::printf("phase 1: mpv_set_option_string BEFORE mpv_initialize\n");
	mpv_handle* mpv = mpv_create();
	if (mpv == nullptr) {
		std::printf("mpv_create failed\n");
		return 1;
	}

	trySet(mpv, "config", "no");
	trySet(mpv, "load-scripts", "yes");        // allow script dir loading
	trySet(mpv, "script", scriptPath);         // CLI-style single script
	trySet(mpv, "scripts", scriptPath);        // CLI-style list
	trySet(mpv, "script-opts", "x=y");
	trySet(mpv, "config-dir", "C:/tmp/mpv-cfg-probe");
	trySet(mpv, "ytdl", "no");

	std::printf("\nmpv_initialize...\n");
	const int rc = mpv_initialize(mpv);
	std::printf("  mpv_initialize -> %s\n", rc >= 0 ? "ok" : mpv_error_string(rc));
	if (rc < 0) {
		mpv_terminate_destroy(mpv);
		return 1;
	}

	// ---- phase 2: same names as runtime properties, after initialize ----
	std::printf("\nphase 2: same names as PROPERTIES after initialize\n");
	trySetProperty(mpv, "script", scriptPath);
	trySetProperty(mpv, "scripts", scriptPath);

	// ---- phase 3: what does mpv think is loaded? ----
	std::printf("\nphase 3: observable state\n");
	char* clients = mpv_get_property_string(mpv, "client-list");
	if (clients != nullptr) {
		std::printf("  client-list : %s\n", clients);
		mpv_free(clients);
	} else {
		std::printf("  client-list : (unavailable)\n");
	}

	// Drain a moment so any script messages appear.
	for (int i = 0; i < 50; ++i) {
		mpv_event* event = mpv_wait_event(mpv, 0.02);
		if (event != nullptr && event->event_id == MPV_EVENT_LOG_MESSAGE) {
			auto* m = static_cast<mpv_event_log_message*>(event->data);
			std::printf("  [log %s] %s", m->level, m->text);
		}
	}

	mpv_terminate_destroy(mpv);
	std::printf("\ndone\n");
	return 0;
}
