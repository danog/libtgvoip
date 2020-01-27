#include "Ack.h"
#include "../PrivateDefines.h"

using namespace tgvoip;
using namespace std;

void Ack::ack(uint32_t ackId, uint32_t mask)
{
    peerAcks[0] = ackId;
    for (unsigned int i = 1; i <= 32; i++)
    {
        peerAcks[i] = (mask >> (32 - i)) & 1 ? ackId - i : 0;
    }
}
bool Ack::wasAcked(uint32_t seq)
{
    if (seqgt(seq, peerAcks[0]))
        return false;

    uint32_t distance = peerAcks[0] - seq;
    if (distance >= 0 && distance <= 32)
    {
        return peerAcks[distance];
    }

    return false;
}