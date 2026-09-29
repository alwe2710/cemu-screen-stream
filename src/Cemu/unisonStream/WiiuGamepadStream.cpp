#include "WiiuGamepadStream.h"

#include "Common/socket.h"

#include <array>
#include <cstring>

#include "unison/protocol.h"
#include "Beacon.h"
#include "UnisonMessages.h"
#include "UnisonWebSocket.h"
#include "SoftwareVideoEncoder.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

namespace Cemu::UnisonStream
{

std::unique_ptr<WiiuGamepadStream> g_wiiuGamepadStream;

namespace
{

void AppendU32LE(std::vector<uint8_t>& out, uint32_t value)
{
	out.push_back((uint8_t)(value & 0xFF));
	out.push_back((uint8_t)((value >> 8) & 0xFF));
	out.push_back((uint8_t)((value >> 16) & 0xFF));
	out.push_back((uint8_t)((value >> 24) & 0xFF));
}

void AppendS16LE(std::vector<uint8_t>& out, int16_t value)
{
	out.push_back((uint8_t)(value & 0xFF));
	out.push_back((uint8_t)((value >> 8) & 0xFF));
}

// Shared by both m_listenSocket and m_videoListenSocket (WiiuGamepadStream's
// constructor) -- identical bind+listen setup, just a different port.
// Returns INVALID_SOCKET on any failure (caller decides how to react).
SOCKET CreateListenSocket(uint16_t port)
{
	SOCKET fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd == INVALID_SOCKET)
		return INVALID_SOCKET;

	int reuseEnabled = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseEnabled, sizeof(reuseEnabled));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(port);

	if (bind(fd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR ||
		listen(fd, 1) == SOCKET_ERROR)
	{
		closesocket(fd);
		return INVALID_SOCKET;
	}
	return fd;
}

// Hand-built the same way SendVideoFrame() builds a type=1 message -- no
// unison_build_audio_frame() exists in core since, like video, the actual
// sample layout/rate is entirely up to each emulator's own audio pipeline
// (see unison/protocol.h's unison_audio_frame: type=3, sample_rate u32le,
// channels u8, then raw s16le samples, no further structure).
bool SendAudioFrame(SOCKET fd, const std::vector<int16_t>& samples, uint32_t sampleRate, uint8_t channels, const std::atomic_bool& stop)
{
	std::vector<uint8_t> message;
	message.reserve(6 + samples.size() * sizeof(int16_t));
	message.push_back((uint8_t)UNISON_MSG_AUDIO);
	AppendU32LE(message, sampleRate);
	message.push_back(channels);
	for (int16_t sample : samples)
		AppendS16LE(message, sample);

	return SendWebSocketBinaryFrame(fd, message, stop);
}

// Returns false only on a real socket error (caller should treat the
// session as dead).
//
// videoMode is always "h264" or "h265" by the time this is called --
// ServeConnection() normalizes anything else (an old/unaware client asking
// for the raw TILES/legacy modes this stream type used to also support,
// or nothing at all) to "h264" before RunSession() is ever entered. Both
// raw paths (a full non-tiled frame, and TILES delta-encoding + dedup
// against the previous frame) were removed entirely per explicit request:
// every client's own video-mode picker for WIIU_GAMEPAD no longer offers
// them either, so there is no longer a caller that could reach this
// function with anything else, and no reason to keep dead paths sending
// raw RGB565 that a real codec already does better.
//
// width/height are THIS frame's real captured DRC content size, not
// necessarily the fixed 854x480 every other stream type/mode here assumes
// -- LatteRenderTarget_itHLECopyColorBufferToScanBuffer() (LatteRenderTarget.cpp)
// builds the captured texture from whatever colorBufferWidth/Height the
// *game itself* used for that particular scan-buffer copy, which a title is
// free to vary per DRC content (confirmed for Wind Waker HD: its GamePad
// item-picker screen renders at a different size than the full
// TV-mirrored-to-GamePad view Select toggles to). The TILES/legacy paths
// below already use width/height correctly, dynamically, every call --
// SoftwareVideoEncoder used to be the one exception, built once in
// RunSession with the fixed 854x480 constants and never revisited:
// EncodeFrame() would then blindly read width*height*4 bytes assuming its
// own, possibly stale, construction-time stride, silently misinterpreting
// (at best) or reading past the end of (at worst) whatever the real,
// differently-sized rgba8 buffer for that frame actually was -- confirmed
// live as the cause of h264/h265 showing nothing at all while Wind Waker
// HD's GamePad was on the item-picker screen (only the TV-mirrored view
// happens to match 854x480). Fixed by (re)constructing videoEncoder
// in-place whenever this frame's width/height don't match its current
// Width()/Height(), same as a resolution change on a first connect.
bool SendVideoFrame(SOCKET fd, const std::vector<uint8_t>& rgba8, int width, int height,
                    const std::string& videoMode,
                    std::unique_ptr<SoftwareVideoEncoder>& videoEncoder, uint32_t encoderFps,
                    const std::atomic_bool& stop)
{
	// (Re)build whenever there's no encoder yet (first frame this session)
	// or this frame's real captured size no longer matches what the
	// current one was built for (a DRC content change, e.g. Wind Waker
	// HD's item-picker vs. its TV-mirrored view) -- see this function's own
	// top comment. A rebuild means a fresh encoder context (no reference-
	// frame state carried over, same as a new session), which
	// SendWebSocketBinaryFrame naturally surfaces as a forced keyframe on
	// this codec's very next EncodeFrame() call.
	if (!videoEncoder || videoEncoder->Width() != (uint32_t)width || videoEncoder->Height() != (uint32_t)height)
	{
		videoEncoder = std::make_unique<SoftwareVideoEncoder>(
			videoMode == "h264" ? VideoCodec::H264 : VideoCodec::H265, (uint32_t)width, (uint32_t)height, encoderFps);
	}

	if (!videoEncoder->IsValid())
		return true; // Real encoder-open failure -- skip this frame rather than kill the session over it.

	std::vector<uint8_t> nals;
	// Temporary diagnostic timing (see the "verzögert nach dem Intro"
	// investigation) -- logs only when either half takes long enough to
	// plausibly explain visible lag, so this doesn't spam the log on
	// the common fast case. Encode is CPU-bound (competes with Cemu's
	// own emulation for the same cores); send is bound by the actual
	// Wi-Fi link. Remove once the bottleneck is confirmed.
	const auto encodeStart = std::chrono::steady_clock::now();
	const bool encodeOk = videoEncoder->EncodeFrame(rgba8.data(), nals);
	const auto encodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - encodeStart).count();
	if (!encodeOk)
		return true; // Real encoder error -- skip this frame rather than kill the session over it.
	if (nals.empty())
	{
		if (encodeMs > 20)
			cemuLog_log(LogType::Force, fmt::format("Unison {} encode took {}ms (no output yet)", videoMode, encodeMs));
		return true; // Encoder produced no output yet (internal buffering) -- nothing to send.
	}

	// Coded (padded, macroblock/CTU-aligned) dimensions, not the raw
	// display width/height -- see SoftwareVideoEncoder::CodedWidth()'s
	// own comment: the bitstream is encoded at this size, and at least
	// one real hardware decoder has been observed to distort the
	// picture if told to crop a non-macroblock-aligned SPS conformance
	// window, so nothing ever asks a decoder to crop here at all.
	std::vector<uint8_t> message;
	message.reserve(10 + nals.size());
	message.push_back((uint8_t)UNISON_MSG_VIDEO);
	AppendU32LE(message, videoEncoder->CodedWidth());
	AppendU32LE(message, videoEncoder->CodedHeight());
	message.push_back(videoMode == "h264" ? UNISON_VIDEO_FORMAT_H264 : UNISON_VIDEO_FORMAT_H265);
	message.insert(message.end(), nals.begin(), nals.end());

	const auto sendStart = std::chrono::steady_clock::now();
	const bool sendOk = SendWebSocketBinaryFrame(fd, message, stop);
	const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - sendStart).count();
	if (encodeMs > 20 || sendMs > 20)
		cemuLog_log(LogType::Force, fmt::format("Unison {} frame: {} bytes, encode {}ms, send {}ms", videoMode, nals.size(), encodeMs, sendMs));
	return sendOk;
}

}

WiiuGamepadStream::WiiuGamepadStream(uint16_t port) : m_port(port)
{
#if BOOST_OS_WINDOWS
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	m_listenSocket = CreateListenSocket(m_port);
	if (m_listenSocket == INVALID_SOCKET)
		return;

	// Dedicated video connection (docs/protocol.md, protocol_version 3) --
	// bound here, alongside m_listenSocket, so it's already listening
	// before any client ever connects, same as m_listenSocket itself. Not
	// fatal if this fails (falls back to no video_port in session_ready,
	// i.e. the pre-protocol_version-3 single-connection behavior) -- only
	// m_listenSocket failing aborts construction entirely.
	m_videoListenSocket = CreateListenSocket((uint16_t)(m_port + kVideoPortOffset));
	// Non-blocking: unlike m_listenSocket (accepted from a plain blocking
	// AcceptLoop() below), this one is only ever accepted from inside
	// AcceptVideoConnection()'s own bounded-wait loop, which needs to be
	// able to time out.
	if (m_videoListenSocket != INVALID_SOCKET)
		SocketSetNonBlocking(m_videoListenSocket);

	m_acceptThread = std::thread(&WiiuGamepadStream::AcceptLoop, this);

	m_beacon = std::make_unique<Beacon>(m_port);
}

WiiuGamepadStream::~WiiuGamepadStream()
{
	m_stop = true;
	m_beacon.reset();
	if (m_listenSocket != INVALID_SOCKET)
		closesocket(m_listenSocket);
	if (m_videoListenSocket != INVALID_SOCKET)
		closesocket(m_videoListenSocket);
	if (m_acceptThread.joinable())
		m_acceptThread.join();
#if BOOST_OS_WINDOWS
	WSACleanup();
#endif
}

void WiiuGamepadStream::OnDrcFrame(LatteTextureView* texView)
{
	if (!m_active)
		return; // No client attached at all -- don't even rate-limit-check, this is the hot path.

	const auto now = std::chrono::steady_clock::now();
	if (now - m_lastCaptureTime < kMinCaptureInterval)
		return;

	std::vector<uint8_t> rgba;
	int width = 0, height = 0;
	if (!g_renderer->CaptureStreamFrame(texView, rgba, width, height))
		return;
	m_lastCaptureTime = now;

	std::lock_guard lock(m_frameMutex);
	m_latestFrameRgba = std::move(rgba);
	m_latestFrameWidth = width;
	m_latestFrameHeight = height;
	m_frameId++;
}

std::optional<unison_extended_input> WiiuGamepadStream::GetInputOverride() const
{
	if (!m_inputActive.load(std::memory_order_relaxed))
		return std::nullopt;
	std::lock_guard lock(m_inputMutex);
	return m_latestInput;
}

void WiiuGamepadStream::RequestTextInput(const std::string& initialText, uint32_t maxLength)
{
	std::lock_guard lock(m_textInputMutex);
	m_textInputRequestPending = true;
	m_textInputRequestInitialText = initialText;
	m_textInputRequestMaxLength = maxLength;
}

std::optional<WiiuGamepadStream::TextInputResult> WiiuGamepadStream::PollTextInputResponse()
{
	std::lock_guard lock(m_textInputMutex);
	auto result = std::move(m_textInputResponse);
	m_textInputResponse.reset();
	return result;
}

bool WiiuGamepadStream::SubmitGamepadAudio(const int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint8_t channels)
{
	if (!m_active.load(std::memory_order_relaxed))
		return false; // No client connected -- caller should play locally as usual.

	std::lock_guard lock(m_audioMutex);
	m_audioSampleRate = sampleRate;
	m_audioChannels = channels;

	// ~2s of backlog at 48kHz stereo -- if the network thread ever falls
	// behind that far (a stalled/slow client), drop the backlog rather than
	// let it grow unbounded or block the audio thread; a brief gap is far
	// less disruptive than unbounded latency growth.
	constexpr size_t kMaxPendingSamples = 48000 * 2 * 2;
	if (m_pendingAudioSamples.size() + sampleCount > kMaxPendingSamples)
		m_pendingAudioSamples.clear();
	m_pendingAudioSamples.insert(m_pendingAudioSamples.end(), samples, samples + sampleCount);
	return true; // Took ownership -- caller must not also play these locally.
}

void WiiuGamepadStream::SetMicWanted(bool wanted, uint32_t sampleRate)
{
	std::lock_guard lock(m_micMutex);
	m_micWanted = wanted;
	m_micWantedSampleRate = sampleRate;
}

std::vector<uint8_t> WiiuGamepadStream::PollMicAudio()
{
	std::lock_guard lock(m_micMutex);
	std::vector<uint8_t> result = std::move(m_pendingMicAudio);
	m_pendingMicAudio.clear();
	return result;
}

void WiiuGamepadStream::AcceptLoop()
{
	if (m_listenSocket == INVALID_SOCKET)
		return;
	while (!m_stop)
	{
		sockaddr_in clientAddr{};
		socklen_t clientAddrSize = sizeof(clientAddr);
		SOCKET fd = accept(m_listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);
		if (fd == INVALID_SOCKET)
			break; // Listening socket closed (destructor) or errored -- stop.
		ServeConnection(fd);
	}
}

void WiiuGamepadStream::ServeConnection(SOCKET fd)
{
	SocketSetNonBlocking(fd);
	SocketSetNoDelay(fd);

	const auto request = ReadHttpRequest(fd, m_stop);
	if (!request || !IsWebSocketUpgradeRequest(*request))
	{
		closesocket(fd);
		return;
	}
	if (!SendWebSocketUpgradeResponse(fd, *request, m_stop))
	{
		closesocket(fd);
		return;
	}
	if (!SendWebSocketTextFrame(fd, BuildHelloMessage(), m_stop))
	{
		closesocket(fd);
		return;
	}

	const auto frame = ReceiveOneWebSocketFrame(fd, m_stop, std::chrono::seconds(5));
	if (!frame || frame->opcode != UNISON_WS_OPCODE_TEXT)
	{
		closesocket(fd);
		return;
	}

	const auto ack = ParseHelloAck(frame->payload);
	if (!ack)
	{
		SendWebSocketTextFrame(fd, BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest, "Malformed hello_ack"), m_stop);
		closesocket(fd);
		return;
	}
	if (ack->protocolVersion != kProtocolVersion)
	{
		SendWebSocketTextFrame(fd, BuildHandshakeErrorMessage(HandshakeErrorCode::VersionMismatch, "Protocol version mismatch"), m_stop);
		closesocket(fd);
		return;
	}

	bool expected = false;
	if (!m_active.compare_exchange_strong(expected, true))
	{
		SendWebSocketTextFrame(fd, BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable, "WIIU_GAMEPAD stream already has an active client"), m_stop);
		closesocket(fd);
		return;
	}

	// No raw (TILES/legacy) fallback anymore for this stream type -- see
	// SendVideoFrame()'s own comment on why both were removed entirely.
	// Anything other than an explicit "h265" request gets h264, the same
	// "always a real codec" default every client's own video-mode picker
	// for WIIU_GAMEPAD now enforces (their raw options simply aren't
	// offered there any more) -- this normalizes the case of an old/
	// unaware client that still asks for "legacy"/"tiles"/nothing at all.
	const std::string videoMode = (ack->videoMode == "h265") ? "h265" : "h264";
	const uint16_t videoPort = (uint16_t)(m_port + kVideoPortOffset);

	if (!SendWebSocketTextFrame(fd, BuildSessionReadyMessage(videoMode, videoPort), m_stop))
	{
		m_active = false;
		closesocket(fd);
		return;
	}

	// Dedicated video connection (docs/protocol.md, protocol_version 3):
	// the client is expected to open a second connection to videoPort right
	// after receiving session_ready above -- wait for it here, bounded,
	// before ever entering RunSession(), so that function never has to
	// handle "video socket not there yet" itself. A client this version
	// always attempts this (exact-match protocol_version already ensures
	// it speaks 3, see docs/protocol.md's "Protocol Version") -- a timeout
	// here means a genuine connectivity problem, treated as a handshake
	// failure the same as any other.
	const SOCKET videoFd = AcceptVideoConnection(std::chrono::seconds(5));
	if (videoFd == INVALID_SOCKET)
	{
		m_active = false;
		closesocket(fd);
		return;
	}

	RunSession(fd, videoFd, videoMode);

	closesocket(videoFd);

	m_streaming = false;
	m_inputActive = false;
	m_active = false;
	// Drop any mic audio this client sent but nobody drained yet -- left
	// sitting here, it would otherwise get fed to UnisonInputAPI::
	// ConsumeBlock() as if it were fresh once a later session (or a
	// belated poll from this one) reads it, mislabeling stale audio as
	// current.
	{
		std::lock_guard lock(m_micMutex);
		m_pendingMicAudio.clear();
	}
	closesocket(fd);
}

SOCKET WiiuGamepadStream::AcceptVideoConnection(std::chrono::milliseconds timeout)
{
	if (m_videoListenSocket == INVALID_SOCKET)
		return INVALID_SOCKET;

	// Same bounded-wait idiom as ReadHttpRequest()/ReceiveOneWebSocketFrame()
	// in UnisonWebSocket.h: non-blocking socket, poll via accept() itself
	// (which returns immediately with WouldBlock when nothing's pending
	// rather than actually blocking), short sleep between attempts, given
	// up once the deadline passes.
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (m_stop)
			return INVALID_SOCKET;
		sockaddr_in clientAddr{};
		socklen_t clientAddrSize = sizeof(clientAddr);
		SOCKET fd = accept(m_videoListenSocket, (sockaddr*)&clientAddr, &clientAddrSize);
		if (fd != INVALID_SOCKET)
		{
			SocketSetNonBlocking(fd);
			SocketSetNoDelay(fd);
			return fd;
		}
		if (!SocketWouldBlock())
			return INVALID_SOCKET; // Listening socket closed (destructor) or errored.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return INVALID_SOCKET; // Timed out.
}

void WiiuGamepadStream::RunSession(SOCKET fd, SOCKET videoFd, const std::string& videoMode)
{
	m_streaming = true;
	m_inputActive = true;
	uint64_t lastSentFrameId = 0;
	// Session-local H.264/H265 encoder -- fresh per session, same reasoning
	// as lastSentMicWanted below: encoder/decoder reference-frame state
	// must never cross sessions.
	// Left null here (rather than eagerly constructed against the fixed
	// kStreamWidth/kStreamHeight, as this used to do) -- SendVideoFrame()
	// now (re)builds it lazily, against whichever real per-frame width/
	// height it's actually given, the first time it's needed and again on
	// any later resolution change; see that function's own comment on why
	// a fixed size here was wrong. Effective fps is this stream's real,
	// throttled capture rate (kMinCaptureInterval), not the console's
	// nominal output rate (kStreamFps) -- see SoftwareVideoEncoder's own
	// constructor comment.
	std::unique_ptr<SoftwareVideoEncoder> videoEncoder;
	const uint32_t encoderFps = (uint32_t)(1000 / kMinCaptureInterval.count());
	// Edge-detection for the mic-enable signal -- session-local (not a
	// member), same reasoning as lastSentFrameId above. Starts at "not
	// wanted" so a session that begins with a mic already wanted (the game
	// had it open before this client connected) still sends an initial
	// enable=1 on its first loop iteration.
	bool lastSentMicWanted = false;
	uint32_t lastSentMicSampleRate = 0;
	std::vector<uint8_t> recvBuffer;
	std::array<uint8_t, 4096> readBuf{};

	while (!m_stop)
	{
		std::vector<uint8_t> frameCopy;
		int width = 0, height = 0;
		uint64_t currentId = 0;
		{
			std::lock_guard lock(m_frameMutex);
			currentId = m_frameId;
			if (currentId != lastSentFrameId)
			{
				frameCopy = m_latestFrameRgba;
				width = m_latestFrameWidth;
				height = m_latestFrameHeight;
			}
		}
		if (!frameCopy.empty())
		{
			if (!SendVideoFrame(videoFd, frameCopy, width, height, videoMode, videoEncoder, encoderFps, m_stop))
				return;
			lastSentFrameId = currentId;
		}

		{
			std::vector<int16_t> audioSamples;
			uint32_t audioSampleRate = 48000;
			uint8_t audioChannels = 2;
			{
				std::lock_guard lock(m_audioMutex);
				if (!m_pendingAudioSamples.empty())
				{
					audioSamples = std::move(m_pendingAudioSamples);
					m_pendingAudioSamples.clear();
					audioSampleRate = m_audioSampleRate;
					audioChannels = m_audioChannels;
				}
			}
			if (!audioSamples.empty())
			{
				if (!SendAudioFrame(fd, audioSamples, audioSampleRate, audioChannels, m_stop))
					return;
			}
		}

		{
			std::string initialText;
			uint32_t maxLength = 0;
			bool pending = false;
			{
				std::lock_guard lock(m_textInputMutex);
				pending = m_textInputRequestPending;
				if (pending)
				{
					initialText = m_textInputRequestInitialText;
					maxLength = m_textInputRequestMaxLength;
					m_textInputRequestPending = false;
				}
			}
			if (pending)
			{
				unison_text_input_request req;
				req.max_length = maxLength;
				req.text = initialText.data();
				req.text_len = initialText.size();
				std::vector<uint8_t> payload(unison_text_input_request_max_size(initialText.size()));
				size_t payloadLen = unison_build_text_input_request(&req, payload.data(), payload.size());
				if (payloadLen > 0)
				{
					payload.resize(payloadLen);
					if (!SendWebSocketBinaryFrame(fd, payload, m_stop))
						return;
				}
			}
		}

		{
			bool wanted = false;
			uint32_t sampleRate = 0;
			{
				std::lock_guard lock(m_micMutex);
				wanted = m_micWanted;
				sampleRate = m_micWantedSampleRate;
			}
			if (wanted != lastSentMicWanted || (wanted && sampleRate != lastSentMicSampleRate))
			{
				unison_mic_enable enable;
				enable.enabled = wanted ? 1 : 0;
				enable.sample_rate = sampleRate;
				uint8_t payload[UNISON_MIC_ENABLE_FRAME_SIZE];
				unison_build_mic_enable_frame(&enable, payload);
				std::vector<uint8_t> message(payload, payload + UNISON_MIC_ENABLE_FRAME_SIZE);
				if (!SendWebSocketBinaryFrame(fd, message, m_stop))
					return;
				lastSentMicWanted = wanted;
				lastSentMicSampleRate = sampleRate;
			}
		}

		int received = recv(fd, (char*)readBuf.data(), (int)readBuf.size(), 0);
		if (received == 0)
			return; // Disconnected.
		if (received < 0 && !SocketWouldBlock())
			return; // Error.
		if (received > 0)
		{
			recvBuffer.insert(recvBuffer.end(), readBuf.begin(), readBuf.begin() + received);
			for (;;)
			{
				bool protocolError = false;
				auto parsed = TryParseOneFrame(recvBuffer, &protocolError);
				if (!parsed)
				{
					if (protocolError)
						return;
					break;
				}
				if (parsed->opcode == UNISON_WS_OPCODE_CLOSE)
					return;
				if (parsed->opcode != UNISON_WS_OPCODE_BINARY)
					continue;
				unison_msg_type type;
				if (unison_peek_type(parsed->payload.data(), parsed->payload.size(), &type) != UNISON_OK)
					continue;
				if (type == UNISON_MSG_INPUT)
				{
					unison_extended_input input{};
					if (unison_parse_extended_input_frame(parsed->payload.data(), parsed->payload.size(), &input) == UNISON_OK)
					{
						std::lock_guard lock(m_inputMutex);
						m_latestInput = input;
					}
				}
				else if (type == UNISON_MSG_TEXT_INPUT_RESPONSE)
				{
					unison_text_input_response resp{};
					if (unison_parse_text_input_response(parsed->payload.data(), parsed->payload.size(), &resp) == UNISON_OK)
					{
						std::lock_guard lock(m_textInputMutex);
						m_textInputResponse = TextInputResult{resp.confirmed != 0, std::string(resp.text, resp.text_len)};
					}
				}
				else if (type == UNISON_MSG_MIC_AUDIO)
				{
					unison_audio_frame audio{};
					if (unison_parse_mic_audio_frame(parsed->payload.data(), parsed->payload.size(), &audio) == UNISON_OK)
					{
						std::lock_guard lock(m_micMutex);
						// PollMicAudio()/UnisonInputAPI::ConsumeBlock() only
						// ever see raw sample bytes, not a rate -- they trust
						// the client to always send at whatever rate the
						// last MIC_ENABLE requested. Reject anything else
						// here instead, rather than silently mixing
						// differently-rated audio into one buffer that gets
						// played back as if it were all m_micWantedSampleRate.
						if (audio.sample_rate != m_micWantedSampleRate)
							continue;
						// ~2s cap at typical mic rates -- if UnisonInputAPI::
						// ConsumeBlock() ever falls behind that far, drop the
						// backlog rather than let it grow unboundedly (same
						// tradeoff SubmitGamepadAudio() makes for the reverse
						// direction).
						constexpr size_t kMaxPendingBytes = 48000 * sizeof(int16_t) * 2;
						const size_t byteLen = audio.sample_count * sizeof(int16_t);
						if (m_pendingMicAudio.size() + byteLen > kMaxPendingBytes)
							m_pendingMicAudio.clear();
						m_pendingMicAudio.insert(m_pendingMicAudio.end(), audio.samples, audio.samples + byteLen);
					}
				}
			}
		}

		// videoFd carries no client->server traffic at all (see
		// docs/protocol.md, "Dedicated video connection") -- this is purely
		// a liveness check, so a disconnect there ends the whole session
		// the same as fd disconnecting does, rather than silently
		// continuing to "stream" video into a dead socket. Any actual bytes
		// (shouldn't happen, but not a protocol violation worth tearing the
		// session down over on their own) are simply discarded.
		uint8_t videoLivenessBuf[64];
		const int videoReceived = recv(videoFd, (char*)videoLivenessBuf, (int)sizeof(videoLivenessBuf), 0);
		if (videoReceived == 0)
			return; // Disconnected.
		if (videoReceived < 0 && !SocketWouldBlock())
			return; // Error.

		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

}
