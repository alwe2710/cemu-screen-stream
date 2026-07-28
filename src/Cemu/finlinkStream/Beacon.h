#pragma once

// UDP discovery beacon for the WIIU_GAMEPAD finlink stream, so finlink
// clients (3DS/Switch/Android/web) can find this Cemu instance on the LAN
// the same way they find an azahar or dolphin-gba-stream instance -- all
// broadcast the same `finlink_beacon` JSON shape on the same fixed port
// (finlink/discovery.h's FINLINK_BEACON_PORT), so one client-side listener
// works against any of them without caring which it's looking at (see
// finlink docs/protocol.md's "Discovery-Beacon" section). Ported from
// azahar's core/streaming/beacon.h/.cpp: same message shape and broadcast
// cadence, raw Berkeley sockets (Common/socket.h) instead of boost::asio,
// matching WiiuGamepadStream.cpp's own networking style.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace Cemu::FinlinkStream
{

class Beacon
{
public:
	// `handshakePort` is what gets advertised as the beacon's
	// "handshake_port" field -- the port a discovering client should then
	// open its own WebSocket connection to (WiiuGamepadStream's own port).
	explicit Beacon(uint16_t handshakePort);
	~Beacon();

	Beacon(const Beacon&) = delete;
	Beacon& operator=(const Beacon&) = delete;

private:
	void Run();
	std::string BuildMessage() const;

	const uint16_t m_handshakePort;
	// Resolved once at construction (see Beacon.cpp) rather than on every
	// tick -- the local outbound-facing address essentially never changes
	// mid-session, unlike the game title, which BuildMessage() does refresh
	// every tick.
	std::string m_localHost;
	std::atomic_bool m_stop{false};
	std::thread m_thread;
};

}
