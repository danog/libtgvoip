#include "PacketSender.h"
#include "Stream.h"

using namespace tgvoip;

OutgoingStream::~OutgoingStream(){};
PacketSender::PacketSender(VoIPController *_controller, const std::shared_ptr<Stream> &_stream) : controller(_controller), stream(_stream), packetManager(_stream->packetManager){};