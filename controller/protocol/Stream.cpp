#include "Stream.h"
#include "../../VoIPController.h"
#include "protocol/Index.h"

using namespace tgvoip;

OutgoingAudioStream::OutgoingAudioStream(uint8_t _id) : OutgoingMediaStream(_id, TYPE){};
OutgoingAudioStream::~OutgoingAudioStream(){};

OutgoingVideoStream::OutgoingVideoStream(uint8_t _id) : OutgoingMediaStream(_id, TYPE){};
OutgoingVideoStream::~OutgoingVideoStream(){};

ExtraStreamInfo OutgoingAudioStream::getStreamInfo() const
{
    ExtraStreamInfo a;
    a.streamId = id;
    a.codec = codec;
    a.enabled = enabled;
    a.type = type;
    a.frameDuration = frameDuration;
    return a;
}