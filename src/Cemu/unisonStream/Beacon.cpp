#include "Beacon.h"

#include "Common/socket.h"

#if BOOST_OS_UNIX
#include <arpa/inet.h>
#endif

#include <chrono>
#include <sstream>
#include <thread>

#include "unison/discovery.h"
#include "UnisonMessages.h"

#include "Cafe/CafeSystem.h"

namespace Cemu::UnisonStream
{

namespace
{

constexpr std::chrono::milliseconds kBeaconInterval{2000};

// UDP "connect" (no packet actually leaves for a connectionless socket --
// it just resolves local routing) to a well-known external address, then
// reads back which local interface/address the OS picked for that route.
// Same trick as azahar's Core::Streaming::ProbeLocalHost(); doesn't require
// the address to be reachable, only routable.
std::string ProbeLocalHost()
{
	SOCKET probe = socket(PF_INET, SOCK_DGRAM, 0);
	if (probe == INVALID_SOCKET)
		return {};

	sockaddr_in remoteAddr{};
	remoteAddr.sin_family = AF_INET;
	remoteAddr.sin_port = htons(80);
	remoteAddr.sin_addr.s_addr = inet_addr("8.8.8.8");

	if (connect(probe, (sockaddr*)&remoteAddr, sizeof(remoteAddr)) == SOCKET_ERROR)
	{
		closesocket(probe);
		return {};
	}

	sockaddr_in localAddr{};
	socklen_t localAddrLen = sizeof(localAddr);
	if (getsockname(probe, (sockaddr*)&localAddr, &localAddrLen) == SOCKET_ERROR)
	{
		closesocket(probe);
		return {};
	}
	closesocket(probe);

	char buf[INET_ADDRSTRLEN] = {};
#if BOOST_OS_WINDOWS
	InetNtopA(AF_INET, &localAddr.sin_addr, buf, sizeof(buf));
#else
	inet_ntop(AF_INET, &localAddr.sin_addr, buf, sizeof(buf));
#endif
	return std::string(buf);
}

// Escapes a string for embedding as a JSON string literal. Mirrors
// UnisonMessages.cpp's own JsonEscape() (anonymous-namespace, not shared
// across translation units) -- only the game title ever passes through
// here, but titles are free-form metadata text, not a fixed literal set.
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

}

Beacon::Beacon(uint16_t handshakePort) : m_handshakePort(handshakePort), m_localHost(ProbeLocalHost())
{
	m_thread = std::thread(&Beacon::Run, this);
}

Beacon::~Beacon()
{
	m_stop = true;
	if (m_thread.joinable())
		m_thread.join();
}

std::string Beacon::BuildMessage() const
{
	// Same source the RPC/telemetry title getters use elsewhere in this
	// codebase -- the loaded title, not any concept of "what's on the
	// GamePad screen right now" (there's no separate content selection here
	// the way GC_GBA_LINK has a GBA cartridge distinct from the GC game).
	// GetForegroundTitleName() already falls back to "Unknown Game" on its
	// own, but Beacon only ever lives bracketed by a running title (see
	// WiiuGamepadStream.h's own lifecycle comment), so that fallback is
	// effectively dead here -- kept anyway as a second line of defense.
	std::string title = CafeSystem::GetForegroundTitleName();
	if (title.empty())
		title = "Cemu";

	std::ostringstream out;
	out << "{"
		<< "\"type\":\"unison_beacon\","
		<< "\"protocol_version\":" << kProtocolVersion << ","
		<< "\"emulator_identifier\":\"Cemu\","
		<< "\"game_title\":\"" << JsonEscape(title) << "\","
		<< "\"stream_type\":\"" << kStreamType << "\","
		<< "\"host\":\"" << m_localHost << "\","
		<< "\"handshake_port\":" << m_handshakePort
		<< "}";
	return out.str();
}

void Beacon::Run()
{
	SOCKET sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET)
		return;

	int broadcastEnabled = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnabled, sizeof(broadcastEnabled));

	sockaddr_in broadcastAddr{};
	broadcastAddr.sin_family = AF_INET;
	broadcastAddr.sin_port = htons(UNISON_BEACON_PORT);
	broadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	while (!m_stop)
	{
		const std::string message = BuildMessage();
		// Best-effort: a dropped/failed broadcast just means this tick's
		// beacon didn't go out, no different from ordinary UDP loss -- the
		// next tick covers for it.
		sendto(sock, message.data(), (int)message.size(), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

		// Polls m_stop every 100ms instead of sleeping the full interval in
		// one call, so the destructor doesn't have to wait out an
		// in-progress interval.
		for (auto waited = std::chrono::milliseconds::zero();
			 waited < kBeaconInterval && !m_stop; waited += std::chrono::milliseconds(100))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	closesocket(sock);
}

}
