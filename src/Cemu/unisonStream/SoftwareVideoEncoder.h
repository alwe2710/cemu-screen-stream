#pragma once

#include <cstdint>
#include <vector>

namespace Cemu::UnisonStream
{

enum class VideoCodec
{
	H264,
	H265,
};

// Software H.264/H265 encoder (libx264/libx265) for the Unison WIIU_GAMEPAD
// stream -- session-local, like WiiuGamepadStream::RunSession()'s other
// per-session state (lastSentFrameRgb565 etc.): constructed fresh per
// session and destroyed at session end, so encoder/decoder reference-frame
// state never crosses sessions. Wraps whichever of x264/x265's C API the
// chosen VideoCodec needs behind one shared interface, since both are
// near-identical here (RGBA8->I420 conversion, per-session encode, periodic
// forced keyframe) -- see docs/protocol.md's "Keyframe discipline" section
// for why the keyframe cadence exists (a continuous bitstream, unlike
// TILES/legacy, so a dropped frame needs a bounded self-heal instead of
// just resending state next frame).
class SoftwareVideoEncoder
{
public:
	// fps is the *effective* capture rate Unison actually sends at
	// (WiiuGamepadStream's kMinCaptureInterval-throttled rate, not the
	// console's own nominal output rate) -- used for encoder rate-control
	// pacing and to derive the forced-keyframe interval, not treated as a
	// hard per-frame clock (real capture is variable-rate/dedup-skipped).
	SoftwareVideoEncoder(VideoCodec codec, uint32_t width, uint32_t height, uint32_t fps);
	~SoftwareVideoEncoder();

	SoftwareVideoEncoder(const SoftwareVideoEncoder&) = delete;
	SoftwareVideoEncoder& operator=(const SoftwareVideoEncoder&) = delete;

	// True if the encoder opened successfully -- check before calling
	// EncodeFrame(); a construction failure (e.g. codec init rejected the
	// resolution) should make the caller fall back to a different video
	// mode for this session rather than crash.
	bool IsValid() const { return m_encoderHandle != nullptr; }

	// The actual coded picture size the bitstream is encoded at -- may
	// exceed the constructor's width/height (854x480 rounds up to
	// 864x480), see m_codedWidth's own comment. SendVideoFrame() must send
	// THIS, not the display width/height, in the video message header
	// that accompanies an H264/H265 frame, since it's what the bitstream
	// actually describes and what the client's decoder needs to configure
	// against.
	uint32_t CodedWidth() const { return m_codedWidth; }
	uint32_t CodedHeight() const { return m_codedHeight; }

	// Encodes one RGBA8 frame (width*height*4 bytes, same layout
	// WiiuGamepadStream's m_latestFrameRgba already uses) into outNals --
	// an Annex-B byte stream (one or more NAL units, start-code prefixed),
	// ready to drop straight into a UNISON_MSG_VIDEO frame's
	// compressed_data with UNISON_VIDEO_FORMAT_H264/_H265 set. Returns
	// false only on a real encoder error (caller should treat this the
	// same as SendVideoFrame()'s other "skip this frame" cases) -- an
	// encode call that legitimately produces no output yet (encoder
	// look-ahead buffering) still returns true with outNals left empty.
	bool EncodeFrame(const uint8_t* rgba8, std::vector<uint8_t>& outNals);

private:
	void ConvertRgba8ToI420(const uint8_t* rgba8);

	VideoCodec m_codec;
	uint32_t m_width;
	uint32_t m_height;
	// Both H.264 and H.265 code pictures in fixed macroblock/CTU blocks
	// (16 pixels for H.264; also 16 here for simplicity on the H.265 side,
	// a safe common denominator even though HEVC's CTUs can be larger) --
	// a coded dimension that isn't a multiple of that has to be padded
	// internally and cropped back out via the bitstream's SPS conformance
	// window before display. That crop step is exactly where at least one
	// real hardware decoder has been observed to go wrong for 854 (not a
	// multiple of 16): the padding columns leak into the visible picture
	// as a distorted/torn-looking right edge. Padding up to a 16-aligned
	// size ourselves and never asking any decoder to crop anything (the
	// client just displays the full padded picture, a ~1% edge-replicated
	// sliver at most) sidesteps that class of decoder bug entirely, at
	// the cost of never being able to rely on the codec's own cropping.
	uint32_t m_codedWidth;
	uint32_t m_codedHeight;
	uint32_t m_fps;
	// Every Nth frame (see docs/protocol.md's "Keyframe discipline") is
	// forced as a keyframe regardless of what the encoder's own rate
	// control would otherwise pick -- self-healing bound on how long a
	// dropped/corrupted frame can affect the picture for.
	uint32_t m_keyframeInterval;
	uint64_t m_frameCounter = 0;

	// I420 planes, reused across calls (sized once in the constructor) --
	// both x264 and x265 require planar 4:2:0 input, never RGB.
	std::vector<uint8_t> m_planeY;
	std::vector<uint8_t> m_planeU;
	std::vector<uint8_t> m_planeV;

	// Exactly one real encoder handle type is ever behind this, depending
	// on m_codec -- opaque void* here so this header doesn't need to
	// expose x264.h/x265.h (and their near-identical but distinct types)
	// to every includer; SoftwareVideoEncoder.cpp is the only translation
	// unit that needs the real encoder types.
	void* m_encoderHandle = nullptr;
};

}
