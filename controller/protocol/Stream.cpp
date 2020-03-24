#include "Stream.h"
#include "../PrivateDefines.cpp"

OutgoingStream<>::OutgoingStream(uint8_t _id, StreamType _type) : StreamInfo(_id, _type), packetManager(_id){};
