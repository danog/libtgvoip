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

OutgoingStream::OutgoingStream(uint8_t _id, StreamType _type) : StreamInfo(_id, _type), packetManager(_id){};

OutgoingStream::~OutgoingStream(){};

ExtraStreamInfo OutgoingStream::getStreamInfo() const
{
    ExtraStreamInfo a;
    a.streamId = id;
    a.enabled = enabled;
    a.type = type;
    return a;
}

OutgoingMediaStream::OutgoingMediaStream(uint8_t id, StreamType type) : OutgoingStream(id, type){};

OutgoingMediaStream::~OutgoingMediaStream(){};

ExtraStreamInfo OutgoingMediaStream::getStreamInfo() const
{
    ExtraStreamInfo a;
    a.streamId = OutgoingStream::id;
    a.codec = codec;
    a.enabled = OutgoingStream::enabled;
    a.type = OutgoingStream::type;
    return a;
}