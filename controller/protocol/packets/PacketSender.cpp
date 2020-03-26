#include "PacketSender.h"
#include "../Stream.h"

using namespace tgvoip;

PacketSender::PacketSender(VoIPController *_controller, std::shared_ptr<OutgoingStream> _stream) : controller(_controller), stream(_stream), packetManager(_stream->packetManager){};
