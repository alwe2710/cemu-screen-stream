#include "UnisonMessages.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "unison/handshake.h"
#include "unison/json.h"

namespace Cemu::UnisonStream
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
// payload -- unison_json_find_member() skips leading whitespace itself, so
// passing (0, size) directly works without locating the braces by hand.
unison_json_span WholeObject(size_t size)
{
	unison_json_span span;
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
		// Advisory only, like video above -- the actual sample_rate/channels
		// of every UNISON_MSG_AUDIO frame are carried in that frame's own
		// header (see ax_out.cpp's AIInitDRCDMA()/WiiuGamepadStream::
		// SubmitGamepadAudio()), this is just a hint for a client that wants
		// to prepare its audio pipeline ahead of the first frame.
		<< "\"audio\":{\"sample_rate\":48000,\"channels\":2},"
		<< "\"input_encoding\":\"" << kInputEncoding << "\""
		<< "}";
	return out.str();
}

std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload)
{
	if (payload.empty())
		return std::nullopt;

	const char* text = reinterpret_cast<const char*>(payload.data());
	const unison_json_span obj = WholeObject(payload.size());

	char message[16];
	if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "message"), message, sizeof(message)) == (size_t)-1)
		return std::nullopt;
	if (strcmp(message, "hello_ack") != 0)
		return std::nullopt;

	const unison_json_span versionSpan = unison_json_find_member(text, obj.start, obj.end, "protocol_version");
	const unison_json_span slotSpan = unison_json_find_member(text, obj.start, obj.end, "requested_slot");
	if (!versionSpan.found || !slotSpan.found)
		return std::nullopt;

	HandshakeAck ack;
	ack.protocolVersion = (int)unison_json_get_number(text, versionSpan);
	ack.requestedSlot = (int)unison_json_get_number(text, slotSpan);

	char videoMode[UNISON_VIDEO_MODE_LEN];
	if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "video_mode"), videoMode, sizeof(videoMode)) != (size_t)-1
		&& (strcmp(videoMode, "legacy") == 0 || strcmp(videoMode, "h264") == 0 || strcmp(videoMode, "h265") == 0))
		ack.videoMode = videoMode;
	return ack;
}

std::string BuildSessionReadyMessage(const std::string& videoMode)
{
	// No real video/audio negotiation for this stream type: fixed 854x480
	// and 48kHz/stereo, no redirect (single slot) -- same simplification as
	// the azahar/melonDS implementations of the same feature.
	//
	// videoMode is ack->videoMode, i.e. exactly what RunSession() will be
	// called with right after this message is sent (WiiuGamepadStream.cpp) --
	// reported here as the session's committed choice per docs/protocol.md's
	// "Video-mode fallback". Known gap, deliberately not fixed in this pass:
	// for h264/h265 this is reported before the SoftwareVideoEncoder is
	// actually constructed (RunSession does that), so if construction fails
	// the frames that follow silently fall back to TILES per-frame
	// (SendVideoFrame) while session_ready already claimed h264/h265. Fine
	// for now since encoder construction failing is not a case that's been
	// observed in practice; hoisting construction earlier so this can be
	// honest even in that case is a separate follow-up, not required for
	// the fallback-reporting feature itself to work correctly in the
	// common case (server doesn't support the mode at all).
	std::ostringstream out;
	out.precision(10);
	out << "{"
		<< "\"message\":\"session_ready\","
		<< "\"slot\":0,"
		<< "\"video\":{"
		<< "\"width\":" << kStreamWidth << ","
		<< "\"height\":" << kStreamHeight << ","
		<< "\"fps\":" << kStreamFps
		<< "},"
		<< "\"audio\":{\"sample_rate\":48000,\"channels\":2},"
		<< "\"video_mode\":\"" << videoMode << "\""
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
