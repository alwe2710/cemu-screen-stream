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
// Touch/button/stick injection: deliberately NOT applied from OnDrcFrame()
// or from the network thread directly -- VPADController::VPADRead() (which
// already safely runs on the CPU/game thread) is where GetInputOverride()
// is consumed instead, mirroring exactly how azahar/melonDS's own input
// overrides work: the network thread only ever writes the latched struct
// below under a mutex, never calls into game/console state directly.

#include "Common/socket.h"

#include <finlink/protocol.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class LatteTextureView;

namespace Cemu::FinlinkStream
{

class Beacon;

class WiiuGamepadStream
{
public:
	explicit WiiuGamepadStream(uint16_t port);
	~WiiuGamepadStream();

	WiiuGamepadStream(const WiiuGamepadStream&) = delete;
	WiiuGamepadStream& operator=(const WiiuGamepadStream&) = delete;

	void OnDrcFrame(LatteTextureView* texView);

	[[nodiscard]] std::optional<finlink_extended_input> GetInputOverride() const;

	// True whenever a client is connected and streaming (single slot, see
	// m_active below). GeneralSettings2 reads this once at dialog-open time
	// to decide whether to gray out the GamePad audio device/channels
	// controls -- same "snapshot at construction" pattern that dialog
	// already uses for its game_launched parameter, not a live-updated
	// state.
	[[nodiscard]] bool IsActive() const { return m_active.load(std::memory_order_relaxed); }

	struct TextInputResult
	{
		bool confirmed;
		std::string text;
	};

	// Text input (Wii U's software keyboard, swkbd.cpp) -- server->client
	// request / client->server response. A much rarer, one-shot traffic
	// pattern than video/input, so this is a simple "arm a request, poll
	// for the response" API rather than a continuous stream like
	// GetInputOverride(). Safe to call from any thread (swkbd.cpp calls it
	// from the CPU/game thread) -- the actual WS send happens on the
	// session thread, same "network thread owns the socket" separation as
	// everything else here.
	void RequestTextInput(const std::string& initialText, uint32_t maxLength);

	// Returns and clears the latest response once, or nullopt if none is
	// pending. Non-blocking; swkbd.cpp polls this once per SwkbdCalc() call
	// while its own keyboard state is active.
	[[nodiscard]] std::optional<TextInputResult> PollTextInputResponse();

	// Wii U GamePad speaker audio -- called from the audio (AX) thread via a
	// hook in ax_out.cpp's AIInitDRCDMA(), once per accumulated audio block.
	// Returns true if a client is connected, meaning these samples are now
	// this stream's to deliver: the caller must NOT also feed them to the
	// local g_padAudio device, so GamePad audio plays exclusively on the
	// finlink client while it's connected instead of also locally. Returns
	// false (no client connected) if the caller should play them back
	// locally as usual -- mirrors dolphin-gba-stream's own
	// ForwardAudioSamples() "take ownership" contract.
	bool SubmitGamepadAudio(const int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint8_t channels);

	// GamePad microphone input -- the reverse direction of SubmitGamepadAudio
	// above. Called from FinlinkInputAPI (src/audio/FinlinkInputAPI.h), the
	// IAudioInputAPI backend a user selects as the GamePad's microphone
	// device in General Settings, exactly like a real Cubeb device.
	//
	// SetMicWanted mirrors real mic hardware: only actually captures while
	// the game has the mic open (mic.cpp's MICStatus.drc[x].isOpen, via
	// mic_updateDevicePlayState() -> IAudioInputAPI::Play()/Stop()), not
	// continuously just because a client is connected -- causes RunSession
	// to send a FINLINK_MSG_MIC_ENABLE to the client on the next loop
	// iteration if the wanted state actually changed.
	void SetMicWanted(bool wanted, uint32_t sampleRate);

	// Drains and returns whatever mic audio the client has sent since the
	// last call (never blocks) -- FinlinkInputAPI::ConsumeBlock() polls this
	// once per AX tick. Empty if nothing new has arrived. Raw s16le bytes,
	// mono (the Wii U GamePad mic, like the 3DS's, is mono-only).
	[[nodiscard]] std::vector<uint8_t> PollMicAudio();

private:
	void AcceptLoop();
	void ServeConnection(SOCKET fd);
	void RunSession(SOCKET fd, const std::string& videoMode);

	const uint16_t m_port;
	SOCKET m_listenSocket;
	std::thread m_acceptThread;
	std::atomic_bool m_stop{false};
	std::unique_ptr<Beacon> m_beacon;

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

	std::atomic_bool m_streaming{false}; // session_ready sent, input override live
	std::atomic_bool m_inputActive{false};
	mutable std::mutex m_inputMutex;
	finlink_extended_input m_latestInput{};

	std::mutex m_textInputMutex;
	bool m_textInputRequestPending = false;
	std::string m_textInputRequestInitialText;
	uint32_t m_textInputRequestMaxLength = 0;
	std::optional<TextInputResult> m_textInputResponse;

	// GamePad speaker audio pending delivery to the client, appended to by
	// SubmitGamepadAudio() (audio thread) and drained by RunSession()
	// (network thread) once per loop iteration -- a FIFO queue rather than
	// a "latest wins" latch like m_latestFrameRgba above, since audio is a
	// continuous stream where dropping anything but a bounded backlog would
	// produce audible gaps.
	std::mutex m_audioMutex;
	std::vector<int16_t> m_pendingAudioSamples;
	uint32_t m_audioSampleRate = 48000;
	uint8_t m_audioChannels = 2;

	// GamePad microphone input pending delivery to FinlinkInputAPI, plus the
	// want-state going the other way -- separate mutex/fields from the
	// speaker-audio ones above since these are two independent directions
	// (out vs in), not reusable state.
	std::mutex m_micMutex;
	bool m_micWanted = false;
	uint32_t m_micWantedSampleRate = 0;
	std::vector<uint8_t> m_pendingMicAudio; // raw s16le bytes, mono, FIFO
};

extern std::unique_ptr<WiiuGamepadStream> g_wiiuGamepadStream;

}
