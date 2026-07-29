#pragma once

// App-level handshake (hello / hello_ack / session_ready / handshake_error)
// for the WIIU_GAMEPAD stream type, exchanged as WebSocket text frames
// before any Video/Input binary frame, per finlink's docs/protocol.md.
// Mirrors the sibling melonds-screen-stream fork's own
// src/streaming/FinlinkMessages.h/.cpp (same wire shapes, same
// simplification for a fixed single-slot, audio-less stream type -- no
// redirect step, no audio negotiation) -- hand-written JSON for building
// (this codebase's rapidjson dependency exists, but a handful of fixed-shape
// small objects don't need a DOM library any more than the melonDS/azahar
// versions did), finlink/json.h's span-based reader for parsing hello_ack.
//
// Pure message (de)serialization -- no socket I/O, mirroring
// FinlinkWebSocket.h's own separation of transport from message content.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Cemu::FinlinkStream
{

constexpr int kProtocolVersion = 2;
constexpr char kStreamType[] = "WIIU_GAMEPAD";
// Same combined touch+buttons+dual-analog-stick encoding Azahar's
// N3DS_BOTTOM_SCREEN advertises (finlink/protocol.h's
// finlink_extended_input) -- the Wii U GamePad has real remote-controllable
// buttons and a circle-pad-equivalent stick in addition to touch, unlike a
// touch-only secondary screen.
constexpr char kInputEncoding[] = "n3ds_touch_and_buttons";
constexpr uint32_t kStreamWidth = 854;
constexpr uint32_t kStreamHeight = 480;
// The Wii U's DRC scanout runs at the console's fixed video output refresh
// rate (60Hz NTSC / 50Hz PAL depending on console region) -- 60.0 is used
// here as the common-case approximation, same spirit as the other finlink
// server implementations' native-fps constants; it's advertised, not
// enforced (frames are only ever sent when a new one is actually captured).
constexpr double kStreamFps = 60.0;

struct HandshakeAck
{
	int protocolVersion;
	int requestedSlot;
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

std::string BuildSessionReadyMessage();

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail);

}
