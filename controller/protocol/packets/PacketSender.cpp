#include "PacketSender.h"
#include "../Stream.h"
#include "../Stream.tcc"

using namespace tgvoip;

PacketSender::PacketSender(VoIPController *_controller, const std::shared_ptr<OutgoingStream<>> &_stream) : controller(_controller), stream(_stream), packetManager(_stream->packetManager){};
