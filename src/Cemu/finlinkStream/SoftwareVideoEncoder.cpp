#include "SoftwareVideoEncoder.h"

#include <x264.h>
#include <x265.h>

#include <algorithm>
#include <cstring>

namespace Cemu::FinlinkStream
{

namespace
{

// x264/x265 both take signed 8-bit RGB->YUV component math the same way --
// standard BT.601 full-swing formulas, clamped since the exact math can spill
// a few units outside [0, 255] at the extremes.
inline uint8_t ClampByte(int value)
{
	return (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

inline uint32_t RoundUpTo16(uint32_t value)
{
	return (value + 15u) & ~15u;
}

// Deliberately conservative for 854x480 @ ~20fps -- real streaming
// guidelines put good-quality 480p well under this, leaving margin under
// typical home Wi-Fi throughput so the encoder itself never demands more
// bandwidth than the link can sustain in real time (see this file's rate
// control comment). Both codecs share one value for now; H.265's better
// efficiency means it could go lower, but starting equal keeps this easy
// to reason about while tuning.
constexpr int kTargetBitrateKbps = 4000;

// VBV buffer size, deliberately much smaller than kTargetBitrateKbps (a
// full 1-second buffer): the periodic ~3s forced keyframes (see docs/
// protocol.md's "Keyframe discipline") are far bigger than an average
// frame, and a full-second VBV buffer legally lets the encoder dump an
// entire second's bitrate budget into one of them -- a burst the network/
// decoder still has to absorb all at once, which showed up as a
// backlog recurring in lockstep with the keyframe interval (see
// jni_bridge.c's "finlink video decode backlog" diagnostic). A ~125ms
// buffer (2-3 frame periods at 20fps) instead forces the rate controller
// to keep even keyframes close to the average frame size, trading a
// slightly softer keyframe for never spiking the instantaneous rate.
constexpr int kVbvBufferKbits = 500;

}

SoftwareVideoEncoder::SoftwareVideoEncoder(VideoCodec codec, uint32_t width, uint32_t height, uint32_t fps)
	: m_codec(codec), m_width(width), m_height(height), m_codedWidth(RoundUpTo16(width)),
	  m_codedHeight(RoundUpTo16(height)), m_fps(fps == 0 ? 20 : fps)
{
	// ~3 seconds between forced keyframes at the real capture rate -- see
	// docs/protocol.md's "Keyframe discipline".
	m_keyframeInterval = m_fps * 3;
	if (m_keyframeInterval == 0)
		m_keyframeInterval = 60;

	m_planeY.resize((size_t)m_codedWidth * m_codedHeight);
	m_planeU.resize((size_t)(m_codedWidth / 2) * (m_codedHeight / 2));
	m_planeV.resize((size_t)(m_codedWidth / 2) * (m_codedHeight / 2));

	if (codec == VideoCodec::H264)
	{
		x264_param_t param;
		if (x264_param_default_preset(&param, "ultrafast", "zerolatency") != 0)
			return;
		param.i_width = (int)m_codedWidth;
		param.i_height = (int)m_codedHeight;
		param.i_fps_num = m_fps;
		param.i_fps_den = 1;
		param.i_keyint_max = (int)m_keyframeInterval;
		// Repeats SPS/PPS before every keyframe (not just the first) --
		// lets a decoder that missed the session's very first frame (or
		// desynced) still recover cleanly from any later forced keyframe,
		// not only session start.
		param.b_repeat_headers = 1;
		param.b_annexb = 1;
		// Capped bitrate (ABR + a VBV ceiling at the same rate), not CRF:
		// CRF targets a *quality* level with no bound on the resulting
		// bitrate, so a busy/fast-changing scene (real gameplay, vs. a
		// mostly-static menu) can spike well past what a real Wi-Fi link
		// sustains in real time -- data then queues up and arrives in
		// bursts, which is exactly the decode-side backlog (see
		// jni_bridge.c's "finlink video decode backlog" diagnostic)
		// observed specifically once gameplay -- not simpler screens --
		// started rendering. kTargetBitrateKbps is deliberately
		// conservative for this resolution/frame rate, leaving real margin
		// under typical home Wi-Fi throughput.
		param.rc.i_rc_method = X264_RC_ABR;
		param.rc.i_bitrate = kTargetBitrateKbps;
		param.rc.i_vbv_max_bitrate = kTargetBitrateKbps;
		param.rc.i_vbv_buffer_size = kVbvBufferKbits;
		if (x264_param_apply_profile(&param, "main") != 0)
			return;

		x264_t* encoder = x264_encoder_open(&param);
		m_encoderHandle = encoder;
	}
	else
	{
		x265_param* param = x265_param_alloc();
		if (!param)
			return;
		x265_param_default_preset(param, "ultrafast", "zerolatency");
		param->sourceWidth = (int)m_codedWidth;
		param->sourceHeight = (int)m_codedHeight;
		param->fpsNum = m_fps;
		param->fpsDenom = 1;
		param->keyframeMax = (int)m_keyframeInterval;
		param->bRepeatHeaders = 1;
		param->internalCsp = X265_CSP_I420;
		// Same reasoning as the H.264 branch above: capped bitrate, not CRF.
		param->rc.rateControlMode = X265_RC_ABR;
		param->rc.bitrate = kTargetBitrateKbps;
		param->rc.vbvMaxBitrate = kTargetBitrateKbps;
		param->rc.vbvBufferSize = kVbvBufferKbits;

		x265_encoder* encoder = x265_encoder_open(param);
		x265_param_free(param);
		m_encoderHandle = encoder;
	}
}

SoftwareVideoEncoder::~SoftwareVideoEncoder()
{
	if (!m_encoderHandle)
		return;
	if (m_codec == VideoCodec::H264)
		x264_encoder_close((x264_t*)m_encoderHandle);
	else
		x265_encoder_close((x265_encoder*)m_encoderHandle);
}

void SoftwareVideoEncoder::ConvertRgba8ToI420(const uint8_t* rgba8)
{
	// Writes the full m_codedWidth x m_codedHeight plane (see m_codedWidth's
	// own comment on why the coded picture can be larger than the real
	// width/height) -- source coordinates are clamped to the last real row/
	// column for any padding region, i.e. simple edge replication, rather
	// than reading past the actual RGBA8 buffer (which is only
	// m_width*m_height*4 bytes, never padded itself).
	for (uint32_t y = 0; y < m_codedHeight; y++)
	{
		const uint32_t srcY = y < m_height ? y : m_height - 1;
		const uint8_t* srcRow = rgba8 + (size_t)srcY * m_width * 4;
		uint8_t* yRow = m_planeY.data() + (size_t)y * m_codedWidth;
		for (uint32_t x = 0; x < m_codedWidth; x++)
		{
			const uint32_t srcX = x < m_width ? x : m_width - 1;
			const uint8_t r = srcRow[srcX * 4 + 0];
			const uint8_t g = srcRow[srcX * 4 + 1];
			const uint8_t b = srcRow[srcX * 4 + 2];
			yRow[x] = ClampByte((77 * r + 150 * g + 29 * b + 128) >> 8);
		}
	}

	// 4:2:0 chroma: one U/V sample per 2x2 luma block, averaged from the
	// 4 source pixels it covers rather than just point-sampling one of
	// them, for a cleaner downscale. Same edge-replication as above for
	// any 2x2 block that falls partly or fully in the padding region.
	const uint32_t chromaWidth = m_codedWidth / 2;
	const uint32_t chromaHeight = m_codedHeight / 2;
	for (uint32_t cy = 0; cy < chromaHeight; cy++)
	{
		uint8_t* uRow = m_planeU.data() + (size_t)cy * chromaWidth;
		uint8_t* vRow = m_planeV.data() + (size_t)cy * chromaWidth;
		for (uint32_t cx = 0; cx < chromaWidth; cx++)
		{
			int r = 0, g = 0, b = 0;
			for (int dy = 0; dy < 2; dy++)
			{
				const uint32_t srcY = std::min(cy * 2 + dy, m_height - 1);
				const uint8_t* srcRow = rgba8 + (size_t)srcY * m_width * 4;
				for (int dx = 0; dx < 2; dx++)
				{
					const uint32_t srcX = std::min(cx * 2 + dx, m_width - 1);
					const uint8_t* px = srcRow + (size_t)srcX * 4;
					r += px[0];
					g += px[1];
					b += px[2];
				}
			}
			r /= 4;
			g /= 4;
			b /= 4;
			uRow[cx] = ClampByte(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
			vRow[cx] = ClampByte(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
		}
	}
}

bool SoftwareVideoEncoder::EncodeFrame(const uint8_t* rgba8, std::vector<uint8_t>& outNals)
{
	if (!m_encoderHandle)
		return false;

	ConvertRgba8ToI420(rgba8);
	outNals.clear();

	const bool forceKeyframe = (m_frameCounter % m_keyframeInterval) == 0;
	const int64_t pts = (int64_t)m_frameCounter;
	m_frameCounter++;

	if (m_codec == VideoCodec::H264)
	{
		x264_picture_t picIn;
		x264_picture_init(&picIn);
		picIn.img.i_csp = X264_CSP_I420;
		picIn.img.i_plane = 3;
		picIn.img.plane[0] = m_planeY.data();
		picIn.img.plane[1] = m_planeU.data();
		picIn.img.plane[2] = m_planeV.data();
		picIn.img.i_stride[0] = (int)m_codedWidth;
		picIn.img.i_stride[1] = (int)(m_codedWidth / 2);
		picIn.img.i_stride[2] = (int)(m_codedWidth / 2);
		picIn.i_pts = pts;
		picIn.i_type = forceKeyframe ? X264_TYPE_IDR : X264_TYPE_AUTO;

		x264_nal_t* nals = nullptr;
		int nalCount = 0;
		x264_picture_t picOut;
		const int frameSize =
			x264_encoder_encode((x264_t*)m_encoderHandle, &nals, &nalCount, &picIn, &picOut);
		if (frameSize < 0)
			return false;
		for (int i = 0; i < nalCount; i++)
			outNals.insert(outNals.end(), nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
		return true;
	}
	else
	{
		// Not x265_picture_init(): that takes the x265_param* the encoder
		// was opened with (to derive bitDepth/colorSpace defaults), which
		// isn't kept around past x265_encoder_open() above -- passing
		// nullptr crashes inside x265_picture_init itself. A plain
		// zero-init is equivalent here since every field x265_picture_init
		// would otherwise have derived from param (colorSpace, bitDepth)
		// is set explicitly below anyway.
		x265_picture picIn;
		memset(&picIn, 0, sizeof(picIn));
		picIn.planes[0] = m_planeY.data();
		picIn.planes[1] = m_planeU.data();
		picIn.planes[2] = m_planeV.data();
		picIn.stride[0] = (int)m_codedWidth;
		picIn.stride[1] = (int)(m_codedWidth / 2);
		picIn.stride[2] = (int)(m_codedWidth / 2);
		picIn.colorSpace = X265_CSP_I420;
		picIn.bitDepth = 8;
		picIn.pts = pts;
		if (forceKeyframe)
			picIn.sliceType = X265_TYPE_IDR;

		x265_nal* nals = nullptr;
		uint32_t nalCount = 0;
		x265_picture picOut;
		memset(&picOut, 0, sizeof(picOut));
		const int ret =
			x265_encoder_encode((x265_encoder*)m_encoderHandle, &nals, &nalCount, &picIn, &picOut);
		if (ret < 0)
			return false;
		for (uint32_t i = 0; i < nalCount; i++)
			outNals.insert(outNals.end(), nals[i].payload, nals[i].payload + nals[i].sizeBytes);
		return true;
	}
}

}
