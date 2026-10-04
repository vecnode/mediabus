#include "control/ScriptLibrary.h"

#include <string>
#include <utility>

namespace media {
namespace {

/// Percent-encode a query-string value.
///
/// JSON escaping is not query escaping: a name with a space would reach the
/// server as a second word and a name with an `&` would start a second
/// parameter. Script names are normally plain, but the editor lets a person type
/// one, so this cannot assume that.
std::string urlEncode(const std::string& value) {
	static const char* const kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (const char c : value) {
		const unsigned char uc = static_cast<unsigned char>(c);
		const bool unreserved = (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z')
			|| (uc >= '0' && uc <= '9') || uc == '-' || uc == '_' || uc == '.'
			|| uc == '~';
		if (unreserved) {
			out.push_back(static_cast<char>(uc));
		} else {
			out.push_back('%');
			out.push_back(kHex[(uc >> 4) & 0x0F]);
			out.push_back(kHex[uc & 0x0F]);
		}
	}
	return out;
}

/// A failed call reports the client's own message; a call that succeeded but
/// whose body says `{"ok":false}` carries the server's reason, which is the one
/// worth showing.
std::string failureText(const HttpJsonClient::Json& body, const std::string& fallback) {
	if (body.is_object()) {
		const auto it = body.find("error");
		if (it != body.end() && it->is_string()) {
			const std::string text = it->get<std::string>();
			if (!text.empty()) {
				return text;
			}
		}
	}
	return fallback;
}

} // namespace

ScriptLibrary::ScriptLibrary(std::string host, int port)
	: client_(std::move(host), port) {}

ScriptLibrary::Result ScriptLibrary::list(std::vector<ScriptEntry>& out) {
	out.clear();
	HttpJsonClient::Json body;
	std::string error;
	if (!client_.get("/api/controller/scripts", body, error)) {
		return {false, error};
	}

	const auto disk = body.find("onDisk");
	if (disk != body.end() && disk->is_array()) {
		for (const auto& item : *disk) {
			if (!item.is_string()) {
				continue;
			}
			ScriptEntry entry;
			entry.name = item.get<std::string>();
			out.push_back(std::move(entry));
		}
	}
	return {true, {}};
}

ScriptLibrary::Result ScriptLibrary::read(const std::string& name, std::string& text) {
	text.clear();
	HttpJsonClient::Json body;
	std::string error;
	// A GET carries its argument in the path, so the name is percent-encoded
	// here: see urlEncode for why JSON escaping is not enough.
	const std::string path = "/api/controller/script-content?name=" + urlEncode(name);
	if (!client_.get(path, body, error)) {
		return {false, error};
	}
	const auto it = body.find("text");
	if (it == body.end() || !it->is_string()) {
		return {false, failureText(body, "the Controller returned no script text")};
	}
	text = it->get<std::string>();
	return {true, {}};
}

ScriptLibrary::Result ScriptLibrary::write(const std::string& name,
	const std::string& text) {
	HttpJsonClient::Json request;
	request["name"] = name;
	request["text"] = text;

	HttpJsonClient::Json body;
	std::string error;
	if (!client_.post("/api/controller/script-save", request, body, error)) {
		return {false, error};
	}
	if (body.is_object()) {
		const auto ok = body.find("ok");
		if (ok != body.end() && ok->is_boolean() && !ok->get<bool>()) {
			return {false, failureText(body, "the Controller refused the script")};
		}
	}
	return {true, {}};
}

ScriptLibrary::Result ScriptLibrary::validate(const std::string& name,
	const std::string& text, ScriptValidation& out) {
	out = ScriptValidation{};
	HttpJsonClient::Json request;
	request["name"] = name;
	request["text"] = text;

	HttpJsonClient::Json body;
	std::string error;
	if (!client_.post("/api/controller/validate", request, body, error)) {
		return {false, error};
	}

	const auto ok = body.find("ok");
	out.ok = (ok != body.end() && ok->is_boolean()) ? ok->get<bool>() : false;

	const auto line = body.find("line");
	if (line != body.end() && line->is_number_integer()) {
		const int value = line->get<int>();
		out.errorLine = value > 0 ? static_cast<std::size_t>(value) : 0;
	}
	const auto message = body.find("error");
	if (message != body.end() && message->is_string()) {
		out.message = message->get<std::string>();
	}
	return {true, {}};
}

ScriptLibrary::Result ScriptLibrary::reload() {
	HttpJsonClient::Json body;
	std::string error;
	if (!client_.post("/api/controller/reload-script", HttpJsonClient::Json::object(),
			body, error)) {
		return {false, error};
	}
	if (body.is_object()) {
		const auto ok = body.find("ok");
		if (ok != body.end() && ok->is_boolean() && !ok->get<bool>()) {
			return {false, failureText(body, "the Controller refused the reload")};
		}
	}
	return {true, {}};
}

ScriptLibrary::Result ScriptLibrary::run(const std::string& name) {
	HttpJsonClient::Json request;
	request["path"] = name;

	HttpJsonClient::Json body;
	std::string error;
	if (!client_.post("/api/controller/script", request, body, error)) {
		return {false, error};
	}
	if (body.is_object()) {
		const auto ok = body.find("ok");
		if (ok != body.end() && ok->is_boolean() && !ok->get<bool>()) {
			return {false, failureText(body, "the Controller refused to run the script")};
		}
	}
	return {true, {}};
}

} // namespace media
