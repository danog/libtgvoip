#pragma once
#include <string>

namespace tgvoip
{

class AudioDevice
{
public:
    std::string id;
    std::string displayName;
};

class AudioOutputDevice : public AudioDevice
{
};

class AudioInputDevice : public AudioDevice
{
};

} // namespace tgvoip