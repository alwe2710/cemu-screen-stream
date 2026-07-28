#pragma once

// Server implementation for the WIIU_GAMEPAD finlink stream type: a
// single-slot WebSocket server that streams the emulated Wii U GamePad
// (DRC) screen (854x480) to one remote client and accepts touch input back,
// per finlink's docs/protocol.md. Sibling implementation to the
// azahar-screen-stream (N3DS_BOTTOM_SCREEN) and melonds-screen-stream
// (NDS_BOTTOM_SCREEN) forks' own streaming servers -- same wire protocol,
// same overall shape.
//
// Lifecycle: owned as a free-standing global (g_wiiuGamepadStream below),
// mirroring src/Cafe/HW/Espresso/Debugger/GDBStub.h's g_gdbstub -- but
// unlike that one (a menu-toggled debug feature with no dependency on a
// running game), this is constructed/destroyed bracketing an actual game
// session (MainWindow::CreateCanvas()+CafeSystem::LaunchForegroundTitle()
// / CafeSystem::ShutdownTitle()+DestroyCanvas()), since it needs a live DRC
// framebuffer and g_renderer to exist.
//
// Networking: single blocking accept loop on its own thread, mirroring
// GDBServer::ThreadFunc's exact style (this codebase's only other TCP
// server) rather than the non-blocking-poll style used in the sibling
// projects' servers -- appropriate here since, like GDBStub, this only ever
// serves one client at a time (s_maxGDBClients-equivalent = 1).
//
// Video capture: LatteRenderTarget_itHLECopyColorBufferToScanBuffer() (see
// that function's own comment) calls OnDrcFrame() once per DRC scan-buffer
// copy, on the render/GPU thread, independent of whether the local
// "GamePad View" window is open. That capture is a real (but rate-limited)
// GPU stall -- see Renderer::CaptureStreamFrame()'s own comment -- so
// OnDrcFrame() throttles how often it actually triggers one.
//
// Touch injection: deliberately NOT applied from OnDrcFrame() or from the
// network thread directly -- VPADController::update_touch() (which already
// safely runs on the CPU/game thread) is where GetTouchOverride() is
// consumed instead, mirroring exactly how azahar/melonDS's own touch
// overrides work: the network thread only ever writes lock-free atomics,
// never calls into game/console state directly.

#include "Common/socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class LatteTextureView;

namespace Cemu::FinlinkStream
{

struct TouchOverride
{
	bool pressed;
	uint16_t x;
	uint16_t y;
};

class WiiuGamepadStream
{
public:
	explicit WiiuGamepadStream(uint16_t port);
	~WiiuGamepadStream();

	WiiuGamepadStream(const WiiuGamepadStream&) = delete;
	WiiuGamepadStream& operator=(const WiiuGamepadStream&) = delete;

	void OnDrcFrame(LatteTextureView* texView);

	[[nodiscard]] std::optional<TouchOverride> GetTouchOverride() const noexcept;

private:
	void AcceptLoop();
	void ServeConnection(SOCKET fd);
	void RunSession(SOCKET fd);

	const uint16_t m_port;
	SOCKET m_listenSocket;
	std::thread m_acceptThread;
	std::atomic_bool m_stop{false};

	// Claimed by the one session currently allowed to stream (this stream
	// type has exactly one slot, see FinlinkMessages.cpp).
	std::atomic_bool m_active{false};

	// Minimum interval between GPU readbacks -- see this file's own comment
	// on OnDrcFrame() being a real render-thread stall. 50ms caps the
	// capture rate at 20fps regardless of the game's actual frame rate.
	static constexpr std::chrono::milliseconds kMinCaptureInterval{50};
	std::chrono::steady_clock::time_point m_lastCaptureTime{};

	std::mutex m_frameMutex;
	std::vector<uint8_t> m_latestFrameRgba; // width*height*4, R,G,B,A per pixel
	int m_latestFrameWidth = 0;
	int m_latestFrameHeight = 0;
	uint64_t m_frameId = 0;

	std::atomic_bool m_streaming{false}; // session_ready sent, touch override live
	std::atomic_bool m_touchPressed{false};
	std::atomic<uint16_t> m_touchX{0};
	std::atomic<uint16_t> m_touchY{0};
};

extern std::unique_ptr<WiiuGamepadStream> g_wiiuGamepadStream;

}
