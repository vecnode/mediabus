#pragma once

#include "app/control/ControllerModel.h"
#include "app/http/HttpJsonClient.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {

/// One entry of the Player's playlist, as `GET /api/clips` reports it.
struct PlayerClipInfo {
	std::size_t index = 0;
	std::string name;
	std::string mediaType;
};

/// Where a control command goes on the Player's HTTP API.
struct PlayerRoute {
	bool valid = false;
	bool isPost = true;
	std::string path;
	HttpJsonClient::Json body;
};

/// Everything the Controller can ask of a Player.
///
/// This is the seam that keeps the scripting host testable: the real
/// implementation is PlayerClient (HTTP), and a test substitutes a recorder to
/// prove what a Lua script actually asked for, without a network or a player.
struct PlayerCommands {
	virtual ~PlayerCommands() = default;

	virtual bool playPause(std::string& error) = 0;
	virtual bool next(std::string& error) = 0;
	virtual bool previous(std::string& error) = 0;
	virtual bool stop(std::string& error) = 0;
	virtual bool openClip(std::size_t index, std::string& error) = 0;
	virtual bool seekPercent(double percent, std::string& error) = 0;
	virtual bool setVolume(double percent, std::string& error) = 0;
	virtual bool setSpeed(double factor, std::string& error) = 0;
	virtual bool setHud(bool visible, std::string& error) = 0;
	virtual bool setFullscreen(bool visible, std::string& error) = 0;
	virtual bool setSubtitles(bool enabled, std::string& error) = 0;

	virtual std::vector<PlayerClipInfo> playlist() const = 0;
	virtual ControllerState state() const = 0;

	/// Dispatch one enum command. Implemented here in terms of the virtuals
	/// above, so every implementation — the HTTP client and a test recorder
	/// alike — agrees on what "next" or "toggle the HUD" means.
	///
	/// The toggles resolve to an absolute value from the latest snapshot,
	/// because the Player's wire protocol carries booleans, not toggles.
	/// `percent` is only used by a seek, which is not an enum member.
	///
	/// IMPORTANT: an override must post/act directly and must NOT call back into
	/// send(), or the two call each other forever — this method dispatches by
	/// calling the virtuals, so `next()` calling `send()` is an infinite
	/// recursion that overflows the stack.
	bool send(ControlCommand command, double percent, std::string& error);

	/// Where a command goes on the Player's HTTP API.
	///
	/// The mapping is a value, not a chain of ifs, so it can be unit-tested
	/// without a running Player: the test walks every ControlCommand and asserts
	/// the route is one the Player documents. That is the guard against the two
	/// applications drifting apart silently.
	static PlayerRoute routeFor(ControlCommand command, double percent);
};

/// The Controller's link to the Player: a polling thread for status, and
/// one-shot commands for everything else.
///
/// Threading:
///   - pollLoop() writes `snapshot_` under `mutex_`; the frame loop reads it
///     through state(). Nothing else is shared.
///   - Commands are synchronous and short-fused (see HttpJsonClient): the frame
///     loop issues at most one per click, and a dead Player fails in
///     milliseconds rather than freezing the bar.
///   - Nothing here touches GL, the scene or Lua.
class PlayerClient final : public PlayerCommands {
public:
	PlayerClient(std::string host, int port);
	~PlayerClient() override;

	PlayerClient(const PlayerClient&) = delete;
	PlayerClient& operator=(const PlayerClient&) = delete;

	/// Start polling. Safe to call once; a second call is ignored.
	void start();
	/// Stop the polling thread and wait for it to finish. Idempotent.
	void stop();

	/// Interval between successful polls.
	void setPollIntervalMs(int ms) { pollIntervalMs_ = ms; }

	/// Latest snapshot. Copies under the mutex, so the frame loop never blocks
	/// on an in-flight HTTP request.
	ControllerState state() const override;
	std::vector<PlayerClipInfo> playlist() const override;

	/// Force one poll on the calling thread. Used at startup so the bar does
	/// not show OFFLINE for the first interval.
	void pollOnce();

	/// GET /api/health on the calling thread. The cheapest "is it there?"
	/// question, and what the poll loop falls back to when /api/status fails.
	bool ping(std::string& error) const { return client_.ping(error); }

	// --- commands ---------------------------------------------------------
	bool playPause(std::string& error) override;
	bool next(std::string& error) override;
	bool previous(std::string& error) override;
	bool stop(std::string& error) override;
	bool openClip(std::size_t index, std::string& error) override;
	bool seekPercent(double percent, std::string& error) override;
	bool setVolume(double percent, std::string& error) override;
	bool setSpeed(double factor, std::string& error) override;
	bool setHud(bool visible, std::string& error) override;
	bool setFullscreen(bool visible, std::string& error) override;
	bool setSubtitles(bool enabled, std::string& error) override;
	bool rescanClips(std::string& error);

	/// Ask the Player to switch its media corpus folder and reload.
	///
	/// An empty `directory` means "back to the Player's default data folder".
	/// The Player validates and persists the choice; this returns false, with
	/// `error` filled in, when it refuses or cannot be reached. That distinction
	/// is what lets the caller decide whether to write mediaplayer.ini itself.
	bool setMediaFolder(const std::string& directory, std::string& error);

	const std::string& host() const { return client_.host(); }
	int port() const { return client_.port(); }

private:
	void pollLoop();
	/// Shared tail of every command: post, check ok, adopt the returned status.
	bool postAndAdopt(const std::string& path, const HttpJsonClient::Json& body,
		const char* what, std::string& error);
	/// Adopt a reply that carries the status fields at the top level (the
	/// corpus routes), rather than nested under `status`.
	void adoptCorpusReply(const HttpJsonClient::Json& reply);

	HttpJsonClient client_;
	std::thread thread_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> started_{false};
	std::atomic<int> pollIntervalMs_{200};

	mutable std::mutex mutex_;
	ControllerState snapshot_;
	std::string lastError_;
	std::vector<PlayerClipInfo> playlist_;
};

} // namespace media
