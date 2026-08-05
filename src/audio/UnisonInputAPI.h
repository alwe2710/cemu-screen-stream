#pragma once

#include "IAudioInputAPI.h"

// Sources the Wii U GamePad microphone from a connected Unison client's own
// real microphone (src/Cemu/unisonStream/WiiuGamepadStream.h) instead of a
// host device -- selectable in General Settings exactly like a Cubeb device,
// see GeneralSettings2.cpp's UpdateAudioDeviceList(). ConsumeBlock() returns
// silence whenever no client is connected or none has sent anything yet,
// mirroring CubebInputAPI's own "no data buffered yet" behavior -- there's
// no error state here, just "nothing available right now".
class UnisonInputAPI : public IAudioInputAPI
{
public:
	class UnisonDeviceDescription : public DeviceDescription
	{
	public:
		UnisonDeviceDescription() : DeviceDescription(L"Unison Remote Microphone") {}
		std::wstring GetIdentifier() const override { return L"Unison"; }
	};

	UnisonInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~UnisonInputAPI() override;

	AudioInputAPI GetType() const override { return Unison; }

	bool ConsumeBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	bool IsPlaying() const override { return m_is_playing; }

	static std::vector<DeviceDescriptionPtr> GetDevices();
	static bool InitializeStatic();

private:
	bool m_is_playing = false;

	// Local hand-off buffer between WiiuGamepadStream::PollMicAudio()'s
	// per-call drain and ConsumeBlock()'s fixed-size-block contract -- same
	// role as CubebInputAPI::m_buffer, just fed by the network thread's
	// queue instead of a cubeb data callback.
	mutable std::shared_mutex m_mutex;
	std::vector<uint8> m_buffer;
};
