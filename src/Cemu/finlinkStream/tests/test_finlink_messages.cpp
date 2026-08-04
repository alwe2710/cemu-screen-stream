// FinlinkMessages.{h,cpp} had zero test coverage before this file, despite
// being exactly where hello_ack.video_mode gets parsed and
// session_ready.video_mode gets reported -- the two fields the "Video-mode
// fallback" negotiation feature (finlink/docs/protocol.md) actually runs
// on. Standalone, deliberately not wired into Cemu's own (huge) CMake
// build: FinlinkMessages.cpp only depends on finlink/json.h's span parser
// (see its own header comment, "no socket I/O"), so this links against
// just that one finlink_core source file, not the whole emulator -- see
// tests/CMakeLists.txt in this directory.

#include "../FinlinkMessages.h"

#include "finlink/handshake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                           \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                          \
		{                                                                     \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::exit(1);                                                     \
		}                                                                      \
	} while (0)

using namespace Cemu::FinlinkStream;

namespace
{

std::vector<uint8_t> ToBytes(const std::string& s)
{
	return std::vector<uint8_t>(s.begin(), s.end());
}

void TestBuildHelloMessage()
{
	const std::string hello = BuildHelloMessage();
	CHECK(hello.find("\"message\":\"hello\"") != std::string::npos);
	CHECK(hello.find("\"stream_type\":\"WIIU_GAMEPAD\"") != std::string::npos);
	CHECK(hello.find("\"protocol_version\":2") != std::string::npos);
	CHECK(hello.find("\"width\":854") != std::string::npos);
	CHECK(hello.find("\"height\":480") != std::string::npos);
	CHECK(hello.find("\"input_encoding\":\"n3ds_touch_and_buttons\"") != std::string::npos);

	// Round-trip through the actual shared parser every client uses, not
	// just a substring match -- catches a field that's technically present
	// but not where a real hello parse would look for it.
	finlink_hello parsed;
	CHECK(finlink_parse_hello(reinterpret_cast<const uint8_t*>(hello.data()), hello.size(), &parsed) ==
	      FINLINK_HANDSHAKE_OK);
	CHECK(parsed.protocol_version == kProtocolVersion);
	CHECK(std::strcmp(parsed.stream_type, kStreamType) == 0);
	CHECK(parsed.video.width == kStreamWidth && parsed.video.height == kStreamHeight);
	CHECK(parsed.has_audio);
}

void TestParseHelloAckDefaultsToTiles()
{
	// No video_mode field at all -- an old client, or one that never set
	// it -- must default to "tiles", per HandshakeAck::videoMode's own
	// comment.
	const auto ack = ParseHelloAck(ToBytes(R"({"message":"hello_ack","protocol_version":2,"requested_slot":0})"));
	CHECK(ack.has_value());
	CHECK(ack->protocolVersion == 2);
	CHECK(ack->requestedSlot == 0);
	CHECK(ack->videoMode == "tiles");
}

void TestParseHelloAckRecognizedVideoModes()
{
	// "legacy"/"h264"/"h265" are the three values ParseHelloAck() actually
	// branches on explicitly (see its own implementation) -- each must
	// come through unchanged.
	for (const char* mode : {"legacy", "h264", "h265"})
	{
		std::string json = R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,"video_mode":")";
		json += mode;
		json += "\"}";
		const auto ack = ParseHelloAck(ToBytes(json));
		CHECK(ack.has_value());
		CHECK(ack->videoMode == mode);
	}

	// "tiles" sent explicitly must behave identically to it being the
	// implicit default (see TestParseHelloAckDefaultsToTiles) -- the same
	// requested mode either way.
	const auto explicit_tiles = ParseHelloAck(
	    ToBytes(R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,"video_mode":"tiles"})"));
	CHECK(explicit_tiles.has_value());
	CHECK(explicit_tiles->videoMode == "tiles");
}

void TestParseHelloAckUnrecognizedVideoModeFallsBackToTiles()
{
	// A value ParseHelloAck() doesn't recognize (a future mode this build
	// predates, or outright garbage) must fall back to the same "tiles"
	// default as no field at all -- not silently accepted verbatim (that
	// would let an unrecognized string flow into
	// BuildSessionReadyMessage()'s own JSON output unvalidated) and not an
	// outright parse failure either (an unrecognized mode is exactly the
	// case the whole fallback feature exists to handle gracefully).
	const auto ack = ParseHelloAck(
	    ToBytes(R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,"video_mode":"vp9"})"));
	CHECK(ack.has_value());
	CHECK(ack->videoMode == "tiles");
}

void TestParseHelloAckRejectsMalformed()
{
	CHECK(!ParseHelloAck({}).has_value());
	CHECK(!ParseHelloAck(ToBytes(R"({"message":"session_ready"})")).has_value());
	CHECK(!ParseHelloAck(ToBytes(R"({"message":"hello_ack","requested_slot":0})")).has_value()); // no protocol_version
	CHECK(!ParseHelloAck(ToBytes(R"({"message":"hello_ack","protocol_version":2})")).has_value()); // no requested_slot
}

void TestBuildSessionReadyMessageVideoMode()
{
	// This is the actual fallback-reporting call site (WiiuGamepadStream.cpp
	// passes ack->videoMode straight through) -- round-trip each value
	// through the real shared session_ready parser, same as
	// TestBuildHelloMessage() does for hello.
	for (const char* mode : {"tiles", "legacy", "h264", "h265"})
	{
		const std::string ready_json = BuildSessionReadyMessage(mode);
		CHECK(ready_json.find("\"message\":\"session_ready\"") != std::string::npos);

		finlink_session_ready parsed;
		CHECK(finlink_parse_session_ready(reinterpret_cast<const uint8_t*>(ready_json.data()), ready_json.size(),
		                                   &parsed) == FINLINK_HANDSHAKE_OK);
		CHECK(std::strcmp(parsed.video_mode, mode) == 0);
		CHECK(parsed.video.width == kStreamWidth && parsed.video.height == kStreamHeight);
		CHECK(parsed.has_audio && parsed.audio.sample_rate == 48000 && parsed.audio.channels == 2);
		// This stream type is single-slot, so a redirect must never be
		// present -- catches a copy-paste from a multi-slot host's own
		// BuildSessionReadyMessage() accidentally landing here.
		CHECK(!parsed.has_redirect);
	}
}

void TestBuildHandshakeErrorMessage()
{
	const std::string err_json =
	    BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable, "already has an active client");

	finlink_handshake_error parsed;
	CHECK(finlink_parse_handshake_error(reinterpret_cast<const uint8_t*>(err_json.data()), err_json.size(),
	                                     &parsed) == FINLINK_HANDSHAKE_OK);
	CHECK(std::strcmp(parsed.code, "slot_unavailable") == 0);
	CHECK(std::strcmp(parsed.detail, "already has an active client") == 0);

	// JsonEscape() (internal, only reachable through this) -- a detail
	// string containing characters that would otherwise break the JSON
	// (a literal quote and backslash) must still round-trip intact rather
	// than corrupting the message or ending the JSON string early.
	const std::string tricky_json =
	    BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest, R"(bad "field" \ value)");
	CHECK(finlink_parse_handshake_error(reinterpret_cast<const uint8_t*>(tricky_json.data()), tricky_json.size(),
	                                     &parsed) == FINLINK_HANDSHAKE_OK);
	CHECK(std::strcmp(parsed.detail, R"(bad "field" \ value)") == 0);
}

} // namespace

int main()
{
	TestBuildHelloMessage();
	TestParseHelloAckDefaultsToTiles();
	TestParseHelloAckRecognizedVideoModes();
	TestParseHelloAckUnrecognizedVideoModeFallsBackToTiles();
	TestParseHelloAckRejectsMalformed();
	TestBuildSessionReadyMessageVideoMode();
	TestBuildHandshakeErrorMessage();
	std::printf("finlink_messages: all tests passed\n");
	return 0;
}
