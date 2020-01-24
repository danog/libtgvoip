//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_AUDIOIO_H
#define LIBTGVOIP_AUDIOIO_H

#include "AudioInput.h"
#include "AudioOutput.h"
#include "../tools/utils.h"
#include <memory>
#include <string>

namespace tgvoip
{
namespace audio
{
class AudioIO
{
public:
	AudioIO(){};
	virtual ~AudioIO(){};
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(AudioIO);
	static std::unique_ptr<AudioIO> Create(std::string inputDevice, std::string outputDevice);
	virtual std::shared_ptr<AudioInput> GetInput() = 0;
	virtual std::shared_ptr<AudioOutput> GetOutput() = 0;
	bool Failed();
	std::string GetErrorDescription();

protected:
	bool failed = false;
	std::string error;
};

template <class I, class O>
class ContextlessAudioIO : public AudioIO
{
public:
	ContextlessAudioIO()
	{
		input = std::make_shared<I>();
		output = std::make_shared<O>();
	}

	ContextlessAudioIO(std::string inputDeviceID, std::string outputDeviceID)
	{
		input = std::make_shared<I>(inputDeviceID);
		output = std::make_shared<O>(outputDeviceID);
	}

	virtual std::shared_ptr<AudioInput> GetInput()
	{
		return input;
	}

	virtual std::shared_ptr<AudioOutput> *GetOutput()
	{
		return output;
	}

private:
	std::shared_ptr<I> input;
	std::shared_ptr<O> output;
};
} // namespace audio
} // namespace tgvoip

#endif //LIBTGVOIP_AUDIOIO_H
