#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;


shared_ptr<VoIPController::Stream> VoIPController::GetStreamByType(StreamType type, bool outgoing)
{
    for (shared_ptr<Stream> &ss : (outgoing ? outgoingStreams : incomingStreams))
    {
        if (ss->type == type)
            return ss;
    }
    shared_ptr<Stream> s;
    return s;
}

shared_ptr<VoIPController::Stream> VoIPController::GetStreamByID(unsigned char id, bool outgoing)
{
    for (shared_ptr<Stream> &ss : (outgoing ? outgoingStreams : incomingStreams))
    {
        if (ss->id == id)
            return ss;
    }
    shared_ptr<Stream> s;
    return s;
}
