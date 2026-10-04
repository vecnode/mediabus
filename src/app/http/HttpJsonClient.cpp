#include "app/http/HttpJsonClient.h"

#include "core/Log.h"

#include "httplib.h"

#include <chrono>
#include <utility>

namespace media {
namespace {

constexpr std::size_t kMaxResponseBytes = 256 * 1024;

} // namespace

HttpJsonClient::HttpJsonClient(std::string host, int port)
	: host_(std::move(host)), port_(port) {}

bool HttpJsonClient::perform(bool isPost, const std::string& path, const Json& body,
	Json& out, std::string& error) const {
	httplib::Client client(host_, port_);

	// The chrono overloads are the only correct way to ask for a sub-second
	// timeout here: the `time_t sec, time_t usec = 0` overloads truncate a
	// fractional double to ZERO seconds, and zero means "wait forever" — which
	// is precisely the behaviour a dead peer must not produce. Passing
	// std::chrono::milliseconds selects the overload that cannot be truncated.
	client.set_connection_timeout(std::chrono::milliseconds(kConnectTimeoutMs));
	client.set_read_timeout(std::chrono::milliseconds(kReadTimeoutMs));
	client.set_write_timeout(std::chrono::milliseconds(kReadTimeoutMs));
	client.set_follow_location(false);

	httplib::Result result;
	if (isPost) {
		const std::string payload = body.is_null() ? std::string("{}") : body.dump();
		result = client.Post(path, payload, "application/json");
	} else {
		result = client.Get(path);
	}

	if (!result) {
		error = httplib::to_string(result.error());
		LOG_VERBOSE("PlayerClient") << "GET/POST " << path << " to " << host_ << ":"
			<< port_ << " failed: " << error;
		return false;
	}
	if (result->status != 200) {
		error = "HTTP " + std::to_string(result->status);
		LOG_VERBOSE("PlayerClient") << path << " answered " << result->status;
		return false;
	}
	if (result->body.size() > kMaxResponseBytes) {
		error = "response too large";
		return false;
	}

	out = Json::parse(result->body, nullptr, false);
	if (out.is_discarded()) {
		error = "response was not JSON";
		return false;
	}
	error.clear();
	return true;
}

bool HttpJsonClient::post(const std::string& path, const Json& body, Json& out,
	std::string& error) const {
	return perform(true, path, body, out, error);
}

bool HttpJsonClient::get(const std::string& path, Json& out, std::string& error) const {
	return perform(false, path, Json(), out, error);
}

bool HttpJsonClient::ping(std::string& error) const {
	Json out;
	return get("/api/health", out, error);
}

} // namespace media
