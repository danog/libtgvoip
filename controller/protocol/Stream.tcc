#pragma once
#include "../../VoIPController.h"
#include "../../video/VideoPacketSender.h"
#include "../audio/AudioPacketSender.h"
#include "Stream.h"
#include "protocol/Index.h"

namespace tgvoip
{

template <class T>
OutgoingStream<T>::OutgoingStream(uint8_t _id, StreamType _type) : StreamInfo(_id, _type), packetManager(_id){};

template <class T>
OutgoingStream<T>::~OutgoingStream(){};

template <class T>
ExtraStreamInfo OutgoingStream<T>::getStreamInfo() const
{
    ExtraStreamInfo a;
    a.streamId = id;
    a.enabled = enabled;
    a.type = type;
    return a;
}

template <class T>
OutgoingMediaStream<T>::OutgoingMediaStream(uint8_t id, StreamType type) : OutgoingStream<T>(id, type){};

template <class T>
OutgoingMediaStream<T>::~OutgoingMediaStream(){};

template <class T>
ExtraStreamInfo OutgoingMediaStream<T>::getStreamInfo() const
{
    ExtraStreamInfo a;
    a.streamId = OutgoingStream<T>::id;
    a.codec = codec;
    a.enabled = OutgoingStream<T>::enabled;
    a.type = OutgoingStream<T>::type;
    return a;
}
}; // namespace tgvoip