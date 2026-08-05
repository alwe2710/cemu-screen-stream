#include "UnisonInputAPI.h"

#include <cstring>

#include "Cemu/unisonStream/WiiuGamepadStream.h"

UnisonInputAPI::UnisonInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioInputAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
}

UnisonInputAPI::~UnisonInputAPI()
{
	Stop();
}

bool UnisonInputAPI::ConsumeBlock(sint16* data)
{
	std::unique_lock lock(m_mutex);

	if (Cemu::UnisonStream::g_wiiuGamepadStream)
	{
		auto newSamples = Cemu::UnisonStream::g_wiiuGamepadStream->PollMicAudio();
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

bool UnisonInputAPI::Play()
{
	if (m_is_playing)
		return true;

	m_is_playing = true;
	if (Cemu::UnisonStream::g_wiiuGamepadStream)
		Cemu::UnisonStream::g_wiiuGamepadStream->SetMicWanted(true, m_samplerate);
	return true;
}

bool UnisonInputAPI::Stop()
{
	if (!m_is_playing)
		return true;

	m_is_playing = false;
	if (Cemu::UnisonStream::g_wiiuGamepadStream)
		Cemu::UnisonStream::g_wiiuGamepadStream->SetMicWanted(false, 0);
	return true;
}

std::vector<IAudioInputAPI::DeviceDescriptionPtr> UnisonInputAPI::GetDevices()
{
	return {std::make_shared<UnisonDeviceDescription>()};
}

bool UnisonInputAPI::InitializeStatic()
{
	return true; // No host device/library dependency, unlike Cubeb.
}
