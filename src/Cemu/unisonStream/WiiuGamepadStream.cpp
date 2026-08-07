#include "WiiuGamepadStream.h"

#include "Common/socket.h"

#include <array>
#include <cstring>

#include "unison/deflate.h"
#include "unison/protocol.h"
#include "unison/video_encode.h"
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

// Converts VulkanRenderer::CaptureStreamFrame()'s R8G8B8A8 output into
// row-major u16le RGB565. No vertical flip: the DRC texture is already
// top-down row-major from Cemu's own perspective (glReadPixels-style
// bottom-up conventions only apply to OpenGL's default framebuffer, not to
// an explicit vkCmdCopyImageToBuffer from a 2D image).
void ConvertRgba8ToRgb565(const uint8_t* rgba8, int width, int height, std::vector<uint8_t>& outRgb565)
{
	outRgb565.resize((size_t)width * height * 2);
	const size_t pixelCount = (size_t)width * height;
	for (size_t i = 0; i < pixelCount; i++)
	{
		const uint8_t r = rgba8[i * 4 + 0];
		const uint8_t g = rgba8[i * 4 + 1];
		const uint8_t b = rgba8[i * 4 + 2];
		const uint16_t pixel = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		outRgb565[i * 2 + 0] = (uint8_t)(pixel & 0xFF);
		outRgb565[i * 2 + 1] = (uint8_t)((pixel >> 8) & 0xFF);
	}
}

// lastSentRgb565 is RunSession()'s own session-local "previous frame" state
// (in/out) -- empty means no previous frame yet (this session's first
// frame, or a resolution change), matching unison_encode_video_frame()'s
// previous_rgb565=NULL contract. Encodes via TILES delta + dedup against it
// instead of always sending a full frame (see docs/protocol.md, "Frame
// semantics (video dedup)" -- this is that behavior, actually implemented).
// Returns false only on a real socket error (caller should treat the
// session as dead); a deduped ("nothing changed") frame still returns true
// having sent nothing.
// videoMode comes from the client's hello_ack.video_mode (UnisonMessages.h's
// HandshakeAck): "h264"/"h265" use videoEncoder (RunSession's session-local
// SoftwareVideoEncoder, own by reference here -- see this function's own
// resolution-change handling below for why it's (re)built in here rather
// than once up front in RunSession); "legacy" always sends a full,
// non-tiled, non-deduped frame (the original, pre-TILES behavior, kept as a
// user-selectable fallback); anything else uses the TILES delta-encoding +
// dedup path.
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
                    std::vector<uint8_t>& lastSentRgb565, const std::string& videoMode,
                    std::unique_ptr<SoftwareVideoEncoder>& videoEncoder, uint32_t encoderFps,
                    const std::atomic_bool& stop)
{
	if (videoMode == "h264" || videoMode == "h265")
	{
		// (Re)build whenever there's no encoder yet (first frame this
		// session) or this frame's real captured size no longer matches
		// what the current one was built for (a DRC content change, e.g.
		// Wind Waker HD's item-picker vs. its TV-mirrored view) -- see this
		// function's own top comment. A rebuild means a fresh encoder
		// context (no reference-frame state carried over, same as a new
		// session), which SendWebSocketBinaryFrame naturally surfaces as a
		// forced keyframe on this codec's very next EncodeFrame() call.
		if (!videoEncoder || videoEncoder->Width() != (uint32_t)width || videoEncoder->Height() != (uint32_t)height)
		{
			videoEncoder = std::make_unique<SoftwareVideoEncoder>(
				videoMode == "h264" ? VideoCodec::H264 : VideoCodec::H265, (uint32_t)width, (uint32_t)height, encoderFps);
		}
	}

	if ((videoMode == "h264" || videoMode == "h265") && videoEncoder && videoEncoder->IsValid())
	{
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

	std::vector<uint8_t> rgb565;
	ConvertRgba8ToRgb565(rgba8.data(), width, height, rgb565);

	if (videoMode == "legacy")
	{
		std::vector<uint8_t> compressed(unison_deflate_max_size(rgb565.size()));
		size_t compressedSize = 0;
		if (unison_deflate_raw(rgb565.data(), rgb565.size(), compressed.data(), compressed.size(), &compressedSize) != UNISON_DEFLATE_OK)
			return true; // compressed is sized correctly above, so this shouldn't happen -- skip this frame rather than kill the session over it.

		std::vector<uint8_t> message;
		message.reserve(10 + compressedSize);
		message.push_back((uint8_t)UNISON_MSG_VIDEO);
		AppendU32LE(message, (uint32_t)width);
		AppendU32LE(message, (uint32_t)height);
		message.push_back(0); // format=0: full frame, no INDEXED/TILES bits set.
		message.insert(message.end(), compressed.begin(), compressed.begin() + compressedSize);
		return SendWebSocketBinaryFrame(fd, message, stop);
	}

	// Guards against a stale previous-frame buffer from a different
	// resolution (not expected for this fixed-854x480 stream type, but
	// mismatched sizes would otherwise be undefined behavior for the tile
	// diff below) -- treat it the same as "no previous frame yet".
	if (lastSentRgb565.size() != rgb565.size())
		lastSentRgb565.clear();

	std::vector<uint8_t> scratch(unison_video_encode_scratch_size((uint32_t)width, (uint32_t)height));
	std::vector<uint8_t> compressed(unison_video_encode_max_size((uint32_t)width, (uint32_t)height));
	size_t compressedSize = 0;
	uint8_t format = 0;

	const uint8_t* previous = lastSentRgb565.empty() ? nullptr : lastSentRgb565.data();
	unison_encode_status status = unison_encode_video_frame(
		rgb565.data(), previous, (uint32_t)width, (uint32_t)height, scratch.data(), scratch.size(),
		compressed.data(), compressed.size(), &compressedSize, &format);

	if (status == UNISON_ENCODE_UNCHANGED)
		return true; // Pixel-identical to the last frame actually sent -- nothing to do.
	if (status != UNISON_ENCODE_OK)
		return true; // scratch/compressed are sized correctly above, so this shouldn't happen -- skip this frame rather than kill the session over it.

	std::vector<uint8_t> message;
	message.reserve(10 + compressedSize);
	message.push_back((uint8_t)UNISON_MSG_VIDEO);
	AppendU32LE(message, (uint32_t)width);
	AppendU32LE(message, (uint32_t)height);
	message.push_back(format);
	message.insert(message.end(), compressed.begin(), compressed.begin() + compressedSize);

	if (!SendWebSocketBinaryFrame(fd, message, stop))
		return false;

	lastSentRgb565 = std::move(rgb565);
	return true;
}

}

WiiuGamepadStream::WiiuGamepadStream(uint16_t port) : m_port(port)
{
#if BOOST_OS_WINDOWS
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

	m_listenSocket = socket(PF_INET, SOCK_STREAM, 0);
	if (m_listenSocket == INVALID_SOCKET)
		return;

	int reuseEnabled = 1;
	setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseEnabled, sizeof(reuseEnabled));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(m_port);

	if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR ||
		listen(m_listenSocket, 1) == SOCKET_ERROR)
	{
		closesocket(m_listenSocket);
		m_listenSocket = INVALID_SOCKET;
		return;
	}

	m_acceptThread = std::thread(&WiiuGamepadStream::AcceptLoop, this);

	m_beacon = std::make_unique<Beacon>(m_port);
}

WiiuGamepadStream::~WiiuGamepadStream()
{
	m_stop = true;
	m_beacon.reset();
	if (m_listenSocket != INVALID_SOCKET)
		closesocket(m_listenSocket);
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

	if (!SendWebSocketTextFrame(fd, BuildSessionReadyMessage(ack->videoMode), m_stop))
	{
		m_active = false;
		closesocket(fd);
		return;
	}

	RunSession(fd, ack->videoMode);

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

void WiiuGamepadStream::RunSession(SOCKET fd, const std::string& videoMode)
{
	m_streaming = true;
	m_inputActive = true;
	uint64_t lastSentFrameId = 0;
	// TILES-diff/dedup state for SendVideoFrame() -- session-local (not a
	// member), same reasoning as the mic-enable edge-detection below: reset
	// to empty (meaning "no previous frame", forcing a fresh keyframe) at
	// the start of every new session rather than persisting across
	// reconnects.
	std::vector<uint8_t> lastSentFrameRgb565;
	// Session-local H.264/H265 encoder (only ever used for those two modes)
	// -- fresh per session, same reasoning as lastSentFrameRgb565 above:
	// encoder/decoder reference-frame state must never cross sessions.
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
			if (!SendVideoFrame(fd, frameCopy, width, height, lastSentFrameRgb565, videoMode, videoEncoder, encoderFps, m_stop))
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

		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

}
