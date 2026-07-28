#include "WiiuGamepadStream.h"

#include "Common/socket.h"

#include <array>
#include <cstring>

#include "finlink/deflate.h"
#include "finlink/protocol.h"
#include "FinlinkMessages.h"
#include "FinlinkWebSocket.h"

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

bool SendVideoFrame(SOCKET fd, const std::vector<uint8_t>& rgba8, int width, int height, const std::atomic_bool& stop)
{
	std::vector<uint8_t> rgb565;
	ConvertRgba8ToRgb565(rgba8.data(), width, height, rgb565);

	std::vector<uint8_t> compressed(finlink_deflate_max_size(rgb565.size()));
	size_t compressedSize = 0;
	if (finlink_deflate_raw(rgb565.data(), rgb565.size(), compressed.data(), compressed.size(), &compressedSize) != FINLINK_DEFLATE_OK)
		return false;
	compressed.resize(compressedSize);

	std::vector<uint8_t> message;
	message.reserve(10 + compressed.size());
	message.push_back((uint8_t)FINLINK_MSG_VIDEO);
	AppendU32LE(message, (uint32_t)width);
	AppendU32LE(message, (uint32_t)height);
	message.push_back(0); // format = 0: full frame, raw (non-indexed, non-tiled) RGB565.
	message.insert(message.end(), compressed.begin(), compressed.end());

	return SendWebSocketBinaryFrame(fd, message, stop);
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
}

WiiuGamepadStream::~WiiuGamepadStream()
{
	m_stop = true;
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

std::optional<TouchOverride> WiiuGamepadStream::GetTouchOverride() const noexcept
{
	if (!m_streaming.load(std::memory_order_relaxed))
		return std::nullopt;
	TouchOverride result;
	result.pressed = m_touchPressed.load(std::memory_order_relaxed);
	result.x = m_touchX.load(std::memory_order_relaxed);
	result.y = m_touchY.load(std::memory_order_relaxed);
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

	RunSession(fd);

	m_streaming = false;
	m_touchPressed = false;
	m_active = false;
	closesocket(fd);
}

void WiiuGamepadStream::RunSession(SOCKET fd)
{
	m_streaming = true;
	uint64_t lastSentFrameId = 0;
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
			if (!SendVideoFrame(fd, frameCopy, width, height, m_stop))
				return;
			lastSentFrameId = currentId;
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
				finlink_touch_state touch{};
				if (finlink_parse_touch_frame(parsed->payload.data(), parsed->payload.size(), &touch) == FINLINK_OK)
				{
					m_touchPressed = touch.pressed != 0;
					m_touchX = touch.x;
					m_touchY = touch.y;
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

}
