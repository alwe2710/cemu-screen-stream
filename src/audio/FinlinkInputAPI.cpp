#include "FinlinkInputAPI.h"

#include <cstring>

#include "Cemu/finlinkStream/WiiuGamepadStream.h"

FinlinkInputAPI::FinlinkInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioInputAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
}

FinlinkInputAPI::~FinlinkInputAPI()
{
	Stop();
}

bool FinlinkInputAPI::ConsumeBlock(sint16* data)
{
	std::unique_lock lock(m_mutex);

	if (Cemu::FinlinkStream::g_wiiuGamepadStream)
	{
		auto newSamples = Cemu::FinlinkStream::g_wiiuGamepadStream->PollMicAudio();
		if (!newSamples.empty())
			m_buffer.insert(m_buffer.end(), newSamples.begin(), newSamples.end());
	}

	if (m_buffer.empty())
	{
		memset(data, 0x00, m_bytesPerBlock);
	}
	else
	{
		const auto copied = std::min(m_buffer.size(), (size_t)m_bytesPerBlock);
		memcpy(data, m_buffer.data(), copied);
		m_buffer.erase(m_buffer.begin(), std::next(m_buffer.begin(), copied));
		if (copied != m_bytesPerBlock)
			memset((uint8*)data + copied, 0x00, m_bytesPerBlock - copied);
	}

	return true;
}

bool FinlinkInputAPI::Play()
{
	if (m_is_playing)
		return true;

	m_is_playing = true;
	if (Cemu::FinlinkStream::g_wiiuGamepadStream)
		Cemu::FinlinkStream::g_wiiuGamepadStream->SetMicWanted(true, m_samplerate);
	return true;
}

bool FinlinkInputAPI::Stop()
{
	if (!m_is_playing)
		return true;

	m_is_playing = false;
	if (Cemu::FinlinkStream::g_wiiuGamepadStream)
		Cemu::FinlinkStream::g_wiiuGamepadStream->SetMicWanted(false, 0);
	return true;
}

std::vector<IAudioInputAPI::DeviceDescriptionPtr> FinlinkInputAPI::GetDevices()
{
	return {std::make_shared<FinlinkDeviceDescription>()};
}

bool FinlinkInputAPI::InitializeStatic()
{
	return true; // No host device/library dependency, unlike Cubeb.
}
