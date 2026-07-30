#include "WiiuGamepadStream.h"

#include "Common/socket.h"

#include <array>
#include <cstring>

#include "finlink/deflate.h"
#include "finlink/protocol.h"
#include "finlink/video_encode.h"
#include "Beacon.h"
#include "FinlinkMessages.h"
#include "FinlinkWebSocket.h"
#include "SoftwareVideoEncoder.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

namespace Cemu::FinlinkStream
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
// finlink_build_audio_frame() exists in core since, like video, the actual
// sample layout/rate is entirely up to each emulator's own audio pipeline
// (see finlink/protocol.h's finlink_audio_frame: type=3, sample_rate u32le,
// channels u8, then raw s16le samples, no further structure).
bool SendAudioFrame(SOCKET fd, const std::vector<int16_t>& samples, uint32_t sampleRate, uint8_t channels, const std::atomic_bool& stop)
{
	std::vector<uint8_t> message;
	message.reserve(6 + samples.size() * sizeof(int16_t));
	message.push_back((uint8_t)FINLINK_MSG_AUDIO);
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
// frame, or a resolution change), matching finlink_encode_video_frame()'s
// previous_rgb565=NULL contract. Encodes via TILES delta + dedup against it
// instead of always sending a full frame (see docs/protocol.md, "Frame
// semantics (video dedup)" -- this is that behavior, actually implemented).
// Returns false only on a real socket error (caller should treat the
// session as dead); a deduped ("nothing changed") frame still returns true
// having sent nothing.
// videoMode comes from the client's hello_ack.video_mode (FinlinkMessages.h's
// HandshakeAck): "h264"/"h265" use videoEncoder (RunSession's session-local
// SoftwareVideoEncoder, non-null and IsValid() only when that mode was
// actually negotiated and construction succeeded -- falls through to the
// TILES path below otherwise, a safe default rather than sending nothing);
// "legacy" always sends a full, non-tiled, non-deduped frame (the original,
// pre-TILES behavior, kept as a user-selectable fallback); anything else
// uses the TILES delta-encoding + dedup path.
bool SendVideoFrame(SOCKET fd, const std::vector<uint8_t>& rgba8, int width, int height,
                    std::vector<uint8_t>& lastSentRgb565, const std::string& videoMode,
                    SoftwareVideoEncoder* videoEncoder, const std::atomic_bool& stop)
{
	if ((videoMode == "h264" || videoMode == "h265") && videoEncoder && videoEncoder->IsValid())
	{
		std::vector<uint8_t> nals;
		if (!videoEncoder->EncodeFrame(rgba8.data(), nals))
			return true; // Real encoder error -- skip this frame rather than kill the session over it.
		if (nals.empty())
			return true; // Encoder produced no output yet (internal buffering) -- nothing to send.

		std::vector<uint8_t> message;
		message.reserve(10 + nals.size());
		message.push_back((uint8_t)FINLINK_MSG_VIDEO);
		AppendU32LE(message, (uint32_t)width);
		AppendU32LE(message, (uint32_t)height);
		message.push_back(videoMode == "h264" ? FINLINK_VIDEO_FORMAT_H264 : FINLINK_VIDEO_FORMAT_H265);
		message.insert(message.end(), nals.begin(), nals.end());
		return SendWebSocketBinaryFrame(fd, message, stop);
	}

	std::vector<uint8_t> rgb565;
	ConvertRgba8ToRgb565(rgba8.data(), width, height, rgb565);

	if (videoMode == "legacy")
	{
		std::vector<uint8_t> compressed(finlink_deflate_max_size(rgb565.size()));
		size_t compressedSize = 0;
		if (finlink_deflate_raw(rgb565.data(), rgb565.size(), compressed.data(), compressed.size(), &compressedSize) != FINLINK_DEFLATE_OK)
			return true; // compressed is sized correctly above, so this shouldn't happen -- skip this frame rather than kill the session over it.

		std::vector<uint8_t> message;
		message.reserve(10 + compressedSize);
		message.push_back((uint8_t)FINLINK_MSG_VIDEO);
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

	std::vector<uint8_t> scratch(finlink_video_encode_scratch_size((uint32_t)width, (uint32_t)height));
	std::vector<uint8_t> compressed(finlink_video_encode_max_size((uint32_t)width, (uint32_t)height));
	size_t compressedSize = 0;
	uint8_t format = 0;

	const uint8_t* previous = lastSentRgb565.empty() ? nullptr : lastSentRgb565.data();
	finlink_encode_status status = finlink_encode_video_frame(
		rgb565.data(), previous, (uint32_t)width, (uint32_t)height, scratch.data(), scratch.size(),
		compressed.data(), compressed.size(), &compressedSize, &format);

	if (status == FINLINK_ENCODE_UNCHANGED)
		return true; // Pixel-identical to the last frame actually sent -- nothing to do.
	if (status != FINLINK_ENCODE_OK)
		return true; // scratch/compressed are sized correctly above, so this shouldn't happen -- skip this frame rather than kill the session over it.

	std::vector<uint8_t> message;
	message.reserve(10 + compressedSize);
	message.push_back((uint8_t)FINLINK_MSG_VIDEO);
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

std::optional<finlink_extended_input> WiiuGamepadStream::GetInputOverride() const
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
	if (!frame || frame->opcode != FINLINK_WS_OPCODE_TEXT)
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

	if (!SendWebSocketTextFrame(fd, BuildSessionReadyMessage(), m_stop))
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
	// sitting here, it would otherwise get fed to FinlinkInputAPI::
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
	// Session-local H.264/H265 encoder (only constructed for those two
	// modes) -- fresh per session, same reasoning as lastSentFrameRgb565
	// above: encoder/decoder reference-frame state must never cross
	// sessions. Effective fps is this stream's real, throttled capture
	// rate (kMinCaptureInterval), not the console's nominal output rate
	// (kStreamFps) -- see SoftwareVideoEncoder's own constructor comment.
	std::unique_ptr<SoftwareVideoEncoder> videoEncoder;
	if (videoMode == "h264" || videoMode == "h265")
	{
		const uint32_t fps = (uint32_t)(1000 / kMinCaptureInterval.count());
		videoEncoder = std::make_unique<SoftwareVideoEncoder>(
			videoMode == "h264" ? VideoCodec::H264 : VideoCodec::H265, kStreamWidth, kStreamHeight, fps);
	}
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
			if (!SendVideoFrame(fd, frameCopy, width, height, lastSentFrameRgb565, videoMode, videoEncoder.get(), m_stop))
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
				finlink_text_input_request req;
				req.max_length = maxLength;
				req.text = initialText.data();
				req.text_len = initialText.size();
				std::vector<uint8_t> payload(finlink_text_input_request_max_size(initialText.size()));
				size_t payloadLen = finlink_build_text_input_request(&req, payload.data(), payload.size());
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
				finlink_mic_enable enable;
				enable.enabled = wanted ? 1 : 0;
				enable.sample_rate = sampleRate;
				uint8_t payload[FINLINK_MIC_ENABLE_FRAME_SIZE];
				finlink_build_mic_enable_frame(&enable, payload);
				std::vector<uint8_t> message(payload, payload + FINLINK_MIC_ENABLE_FRAME_SIZE);
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
				if (parsed->opcode == FINLINK_WS_OPCODE_CLOSE)
					return;
				if (parsed->opcode != FINLINK_WS_OPCODE_BINARY)
					continue;
				finlink_msg_type type;
				if (finlink_peek_type(parsed->payload.data(), parsed->payload.size(), &type) != FINLINK_OK)
					continue;
				if (type == FINLINK_MSG_INPUT)
				{
					finlink_extended_input input{};
					if (finlink_parse_extended_input_frame(parsed->payload.data(), parsed->payload.size(), &input) == FINLINK_OK)
					{
						std::lock_guard lock(m_inputMutex);
						m_latestInput = input;
					}
				}
				else if (type == FINLINK_MSG_TEXT_INPUT_RESPONSE)
				{
					finlink_text_input_response resp{};
					if (finlink_parse_text_input_response(parsed->payload.data(), parsed->payload.size(), &resp) == FINLINK_OK)
					{
						std::lock_guard lock(m_textInputMutex);
						m_textInputResponse = TextInputResult{resp.confirmed != 0, std::string(resp.text, resp.text_len)};
					}
				}
				else if (type == FINLINK_MSG_MIC_AUDIO)
				{
					finlink_audio_frame audio{};
					if (finlink_parse_mic_audio_frame(parsed->payload.data(), parsed->payload.size(), &audio) == FINLINK_OK)
					{
						std::lock_guard lock(m_micMutex);
						// PollMicAudio()/FinlinkInputAPI::ConsumeBlock() only
						// ever see raw sample bytes, not a rate -- they trust
						// the client to always send at whatever rate the
						// last MIC_ENABLE requested. Reject anything else
						// here instead, rather than silently mixing
						// differently-rated audio into one buffer that gets
						// played back as if it were all m_micWantedSampleRate.
						if (audio.sample_rate != m_micWantedSampleRate)
							continue;
						// ~2s cap at typical mic rates -- if FinlinkInputAPI::
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
