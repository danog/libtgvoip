#pragma once
#include <string>
#include "../controller/PrivateDefines.h"
#include "AudioIO.h"
#include "AudioInput.h"

namespace tgvoip
{

class AudioInputTester
{
public:
    AudioInputTester(const std::string deviceID);
    ~AudioInputTester();
    TGVOIP_DISALLOW_COPY_AND_ASSIGN(AudioInputTester);
    float GetAndResetLevel();
    bool Failed()
    {
        return io && io->Failed();
    }

private:
    void Update(int16_t *samples, size_t count);
    audio::AudioIO *io = NULL;
    audio::AudioInput *input = NULL;
    int16_t maxSample = 0;
    std::string deviceID;
};

} // namespace tgvoip