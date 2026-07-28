#include "FinlinkMessages.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "finlink/json.h"

namespace Cemu::FinlinkStream
{

namespace
{

const char* ErrorCodeToString(HandshakeErrorCode code)
{
	switch (code)
	{
	case HandshakeErrorCode::VersionMismatch: return "version_mismatch";
	case HandshakeErrorCode::SlotUnavailable: return "slot_unavailable";
	case HandshakeErrorCode::MalformedRequest: return "malformed_request";
	}
	return "malformed_request";
}

// Escapes a string for embedding as a JSON string literal. Only `code`'s
// values (fixed literals above) and hardcoded `detail` text are ever passed
// through this in practice, but handshake_error's detail is meant to be
// human-readable free text, so this is defensive rather than provably
// unnecessary.
std::string JsonEscape(const std::string& in)
{
	std::string out;
	out.reserve(in.size() + 8);
	for (char c : in)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)c < 0x20)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else
			{
				out += c;
			}
		}
	}
	return out;
}

// whole_object(): the top-level JSON object always spans the entire
// payload -- finlink_json_find_member() skips leading whitespace itself, so
// passing (0, size) directly works without locating the braces by hand.
finlink_json_span WholeObject(size_t size)
{
	finlink_json_span span;
	span.found = 1;
	span.start = 0;
	span.end = size;
	return span;
}

}

std::string BuildHelloMessage()
{
	std::ostringstream out;
	out.precision(10);
	out << "{"
		<< "\"message\":\"hello\","
		<< "\"protocol_version\":" << kProtocolVersion << ","
		<< "\"stream_type\":\"" << kStreamType << "\","
		<< "\"slots\":[{\"index\":0,\"label\":\"GamePad\",\"occupied\":false}],"
		<< "\"video\":{"
		<< "\"width\":" << kStreamWidth << ","
		<< "\"height\":" << kStreamHeight << ","
		<< "\"pixel_format\":\"rgb565\","
		<< "\"fps\":" << kStreamFps
		<< "},"
		<< "\"input_encoding\":\"" << kInputEncoding << "\""
		<< "}";
	return out.str();
}

std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload)
{
	if (payload.empty())
		return std::nullopt;

	const char* text = reinterpret_cast<const char*>(payload.data());
	const finlink_json_span obj = WholeObject(payload.size());

	char message[16];
	if (finlink_json_get_string(text, finlink_json_find_member(text, obj.start, obj.end, "message"), message, sizeof(message)) == (size_t)-1)
		return std::nullopt;
	if (strcmp(message, "hello_ack") != 0)
		return std::nullopt;

	const finlink_json_span versionSpan = finlink_json_find_member(text, obj.start, obj.end, "protocol_version");
	const finlink_json_span slotSpan = finlink_json_find_member(text, obj.start, obj.end, "requested_slot");
	if (!versionSpan.found || !slotSpan.found)
		return std::nullopt;

	HandshakeAck ack;
	ack.protocolVersion = (int)finlink_json_get_number(text, versionSpan);
	ack.requestedSlot = (int)finlink_json_get_number(text, slotSpan);
	return ack;
}

std::string BuildSessionReadyMessage()
{
	// No real video negotiation for this stream type: fixed 854x480, no
	// audio, no redirect (single slot) -- same simplification as the
	// azahar/melonDS implementations of the same feature.
	std::ostringstream out;
	out.precision(10);
	out << "{"
		<< "\"message\":\"session_ready\","
		<< "\"slot\":0,"
		<< "\"video\":{"
		<< "\"width\":" << kStreamWidth << ","
		<< "\"height\":" << kStreamHeight << ","
		<< "\"fps\":" << kStreamFps
		<< "}"
		<< "}";
	return out.str();
}

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail)
{
	std::ostringstream out;
	out << "{"
		<< "\"message\":\"handshake_error\","
		<< "\"code\":\"" << ErrorCodeToString(code) << "\","
		<< "\"detail\":\"" << JsonEscape(detail) << "\""
		<< "}";
	return out.str();
}

}
