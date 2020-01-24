#include "AudioInputTester.h"
#include "../tools/logging.h"

using namespace tgvoip;

AudioInputTester::AudioInputTester(std::string deviceID) : deviceID(std::move(deviceID))
{
	io = audio::AudioIO::Create(deviceID, "default");
	if (io->Failed())
	{
		LOGE("Audio IO failed");
		return;
	}
	input = io->GetInput();
	input->SetCallback([](unsigned char *data, size_t size, void *ctx) -> size_t {
		reinterpret_cast<AudioInputTester *>(ctx)->Update(reinterpret_cast<int16_t *>(data), size / 2);
		return 0;
	},
					   this);
	input->Start();
}

AudioInputTester::~AudioInputTester()
{
	input->Stop();
}

void AudioInputTester::Update(int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		int16_t s = abs(samples[i]);
		if (s > maxSample)
			maxSample = s;
	}
}

float AudioInputTester::GetAndResetLevel()
{
	float s = maxSample;
	maxSample = 0;
	return s / (float)INT16_MAX;
}
