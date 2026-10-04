#pragma once

#include "json.hpp"

#include <string>

namespace media {

/// Synchronous JSON client for the Player's localhost control API.
///
/// The Controller is a *client*: it owns no decoder and links no libmpv. That
/// keeps the two applications independent — either can be started first, and
/// the Controller can attach to a Player that was already running.
///
/// Every call is deliberately short-fused (see kTimeoutSeconds). The Controller
/// polls the Player from a worker thread, and a Player that has been killed
/// must not be able to stall that thread: a dead peer has to surface as a fast
/// failure, not as a hung bar.
class HttpJsonClient {
public:
	using Json = nlohmann::json;

	/// Connect/read timeouts per request, in milliseconds. Localhost only, so a
	/// healthy Player answers in well under a millisecond; anything slower is a
	/// failure. These are deliberately generous for loopback (a server may be
	/// mid-frame in a slow decode) and deliberately finite: the point is that a
	/// peer which is gone fails fast and repeatedly, instead of one call
	/// blocking the polling thread forever.
	static constexpr int kConnectTimeoutMs = 250;
	static constexpr int kReadTimeoutMs = 1000;

	HttpJsonClient(std::string host, int port);

	/// POST `path` with `body`. On success `out` holds the parsed JSON and the
	/// function returns true. On any failure (connection refused, timeout,
	/// non-200, unparseable body) it returns false and fills `error` — it never
	/// throws, because every caller is on a path that must keep running.
	bool post(const std::string& path, const Json& body, Json& out, std::string& error) const;

	/// GET `path`.
	bool get(const std::string& path, Json& out, std::string& error) const;

	/// GET /api/health. Cheap liveness probe used when /api/status fails.
	bool ping(std::string& error) const;

	const std::string& host() const { return host_; }
	int port() const { return port_; }

private:
	/// Shared implementation for get/post.
	bool perform(bool isPost, const std::string& path, const Json& body,
		Json& out, std::string& error) const;

	std::string host_;
	int port_ = 0;
};

} // namespace media
