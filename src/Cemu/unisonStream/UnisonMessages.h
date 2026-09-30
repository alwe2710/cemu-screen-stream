#pragma once

// App-level handshake (hello / hello_ack / session_ready / handshake_error)
// for the WIIU_GAMEPAD stream type, exchanged as WebSocket text frames
// before any Video/Input binary frame, per Unison's docs/protocol.md.
// Mirrors the sibling melonds-screen-stream fork's own
// src/streaming/UnisonMessages.h/.cpp (same wire shapes, same
// simplification for a fixed single-slot, audio-less stream type -- no
// redirect step, no audio negotiation) -- hand-written JSON for building
// (this codebase's rapidjson dependency exists, but a handful of fixed-shape
// small objects don't need a DOM library any more than the melonDS/azahar
// versions did), unison/json.h's span-based reader for parsing hello_ack.
//
// Pure message (de)serialization -- no socket I/O, mirroring
// UnisonWebSocket.h's own separation of transport from message content.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Cemu::UnisonStream
{

// Mirrors unison/handshake.h's UNISON_PROTOCOL_VERSION, which must stay
// numerically in sync -- see that macro's own comment on the 2 -> 3 and
// 3 -> 4 bumps (session_ready.video_port, docs/protocol.md's "Dedicated
// video/audio channel (UDP)").
constexpr int kProtocolVersion = 4;
constexpr char kStreamType[] = "WIIU_GAMEPAD";
// Same combined touch+buttons+dual-analog-stick encoding Azahar's
// N3DS_BOTTOM_SCREEN advertises (unison/protocol.h's
// unison_extended_input) -- the Wii U GamePad has real remote-controllable
// buttons and a circle-pad-equivalent stick in addition to touch, unlike a
// touch-only secondary screen.
constexpr char kInputEncoding[] = "n3ds_touch_and_buttons";
constexpr uint32_t kStreamWidth = 854;
constexpr uint32_t kStreamHeight = 480;
// The Wii U's DRC scanout runs at the console's fixed video output refresh
// rate (60Hz NTSC / 50Hz PAL depending on console region) -- 60.0 is used
// here as the common-case approximation, same spirit as the other Unison
// server implementations' native-fps constants; it's advertised, not
// enforced (frames are only ever sent when a new one is actually captured).
constexpr double kStreamFps = 60.0;

struct HandshakeAck
{
	int protocolVersion;
	int requestedSlot;
	// Client's requested video encoding ("tiles", "legacy", "h264", or
	// "h265", see docs/protocol.md's hello_ack.video_mode) -- defaults to
	// "tiles" when the client didn't send the field at all (an old
	// client, or one that never picked a non-default mode) or sent a
	// value this server doesn't recognize.
	std::string videoMode = "tiles";
	// Opt-out from the dedicated video/audio channel (docs/protocol.md,
	// "Dedicated video/audio channel (UDP)", protocol_version 4) -- see
	// core/'s unison_hello_ack_request::no_udp_video for the full
	// rationale (clients/web is the one real client that ever sets this,
	// no raw socket API exists in a browser at all). Defaults to false
	// (can use UDP) when the client doesn't send the field, matching
	// every client already converted to protocol_version 4 here.
	bool noUdpVideo = false;
};

enum class HandshakeErrorCode
{
	VersionMismatch,
	SlotUnavailable,
	MalformedRequest,
};

std::string BuildHelloMessage();

// Parses a `hello_ack` text frame payload. Returns nullopt if the JSON is
// malformed or missing required fields -- caller should treat that as
// HandshakeErrorCode::MalformedRequest.
std::optional<HandshakeAck> ParseHelloAck(const std::vector<uint8_t>& payload);

// videoPort is nullopt for a client that set hello_ack.no_udp_video --
// Video/Audio then stay multiplexed on this same WebSocket connection
// instead (WiiuGamepadStream::RunSession's tcpFallback path), the same
// wire format this stream type used before protocol_version 4 (docs/
// protocol.md, "Dedicated video/audio channel (UDP)" -> "Opting out").
std::string BuildSessionReadyMessage(const std::string& videoMode, std::optional<uint16_t> videoPort);

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail);

}
