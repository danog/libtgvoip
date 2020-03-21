#include "Stream.h"
#include "../PrivateDefines.cpp"

Stream::Stream(uint8_t _id, StreamInfo::Type _type) : id(_id), packetManager(_id), type(_type){};
MediaStream::MediaStream(uint8_t _id, StreamInfo::Type _type) : Stream(_id, _type){};
AudioStream::AudioStream(uint8_t _id = Packet::StreamId::Audio) : MediaStream(_id, StreamInfo::Type::Audio){};
VideoStream::VideoStream(uint8_t _id = Packet::StreamId::Video) : MediaStream(_id, StreamInfo::Type::Video){};
