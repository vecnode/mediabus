#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace media {

class MediaPlayerController;

/// Window presentation the host owns and the API may toggle.
///
/// The render loop lives in main.cpp, so the HTTP layer must never reach for
/// it directly. The host instead supplies these two closures, and every route
/// that touches presentation (`/api/hud`, `/api/fullscreen`) goes through them
/// on the main thread, exactly like every other command.
///
/// Both closures are optional: when one is unset the corresponding route
/// answers with an error instead of pretending to work. That keeps the tests
/// able to construct a server without a window.
struct PresentationHooks {
	/// Current HUD visibility.
	std::function<bool()> getHud;
	/// Set HUD visibility. Returns false when the host refused.
	std::function<bool(bool)> setHud;
	/// Current fullscreen state.
	std::function<bool()> getFullscreen;
	/// Set fullscreen state. Returns false when the host refused.
	std::function<bool(bool)> setFullscreen;
};

/// Localhost-only JSON control API.
///
/// Threading contract (the reason the API stays safe):
///   - HTTP worker threads never touch the controller or the decoder.
///   - A handler submits a closure to a queue and blocks until the poll()ing
///     main thread has run it and handed back the response.
///   - poll() is called once per frame from the main thread.
///
/// Several workers may block on the queue at once, so the queue is fully
/// synchronised and closing the server wakes every waiter.
class HttpControlServer {
public:
	static constexpr int kDefaultPort = 8080;
	static constexpr const char* kDefaultBindHint = "127.0.0.1";

	explicit HttpControlServer(MediaPlayerController& controller,
		PresentationHooks hooks = {});
	~HttpControlServer();

	HttpControlServer(const HttpControlServer&) = delete;
	HttpControlServer& operator=(const HttpControlServer&) = delete;

	bool start(int port);
	void stop();
	bool isRunning() const { return running_; }
	int port() const { return port_; }

	/// Execute queued commands. Main thread only.
	void poll();

	/// Bounded local path accepted by the playlist route.
	bool addClipPath(const std::string& path, std::string* error);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool running_ = false;
	int port_ = kDefaultPort;
};

/// Serialise a status snapshot to the wire shape. The first block of keys is
/// the frozen contract; the rest is additive.
std::string statusToJsonText(const MediaPlayerController& controller);

} // namespace media
