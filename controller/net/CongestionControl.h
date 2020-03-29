//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_CONGESTIONCONTROL_H
#define LIBTGVOIP_CONGESTIONCONTROL_H

#include "tools/Buffers.h"
#include "tools/threading.h"
#include <cstdint>
#include <map>
#include <stdlib.h>

#define TGVOIP_CONCTL_ACT_INCREASE 1
#define TGVOIP_CONCTL_ACT_DECREASE 2
#define TGVOIP_CONCTL_ACT_NONE 0

#define TGVOIP_CONCTL_LOST_AFTER 2

namespace tgvoip
{

struct tgvoip_congestionctl_packet_t
{
    uint32_t seq;
    uint8_t streamId;
    double sendTime;
    bool ack = false;
    size_t size;
};
typedef struct tgvoip_congestionctl_packet_t tgvoip_congestionctl_packet_t;

struct Packet;
struct CongestionControlPacket
{
    CongestionControlPacket(uint32_t _seq, uint8_t _streamId);
    CongestionControlPacket(const Packet &pkt);
    uint32_t seq;
    uint8_t streamId;
};

class CongestionControl
{
public:
    CongestionControl();
    ~CongestionControl();

    void PacketSent(const CongestionControlPacket &pkt, size_t size);
    void PacketLost(const CongestionControlPacket &pkt);
    void PacketAcknowledged(const CongestionControlPacket &pkt);

    double GetAverageRTT();
    double GetMinimumRTT();
    size_t GetInflightDataSize();
    size_t GetCongestionWindow();
    size_t GetAcknowledgedDataSize();
    void Tick();
    int GetBandwidthControlAction();
    uint32_t GetSendLossCount();

private:
    HistoricBuffer<double, 100> rttHistory;
    HistoricBuffer<size_t, 30> inflightHistory;
    std::array<tgvoip_congestionctl_packet_t, 100> inflightPackets{};
    uint32_t lossCount = 0;
    double tmpRtt = 0.0;
    double lastActionTime = 0;
    double lastActionRtt = 0;
    double stateTransitionTime = 0;
    uint32_t tmpRttCount = 0;
    std::map<uint8_t, uint32_t> lastSentSeq;
    uint32_t tickCount = 0;
    size_t inflightDataSize = 0;

    size_t cwnd;
    size_t max;
    size_t min;
};
} // namespace tgvoip

#endif //LIBTGVOIP_CONGESTIONCONTROL_H
