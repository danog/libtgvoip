#include "../../VoIPController.h"

using namespace tgvoip;
using namespace std;



#pragma mark - Bandwidth management

void VoIPController::UpdateAudioBitrateLimit()
{
    if (encoder)
    {
        if (dataSavingMode || dataSavingRequestedByPeer)
        {
            maxBitrate = maxAudioBitrateSaving;
            encoder->SetBitrate(initAudioBitrateSaving);
        }
        else if (networkType == NET_TYPE_GPRS)
        {
            maxBitrate = maxAudioBitrateGPRS;
            encoder->SetBitrate(initAudioBitrateGPRS);
        }
        else if (networkType == NET_TYPE_EDGE)
        {
            maxBitrate = maxAudioBitrateEDGE;
            encoder->SetBitrate(initAudioBitrateEDGE);
        }
        else
        {
            maxBitrate = maxAudioBitrate;
            encoder->SetBitrate(initAudioBitrate);
        }
        encoder->SetVadMode(dataSavingMode || dataSavingRequestedByPeer);
        if (echoCanceller)
            echoCanceller->SetVoiceDetectionEnabled(dataSavingMode || dataSavingRequestedByPeer);
    }
}

void VoIPController::UpdateDataSavingState()
{
    if (config.dataSaving == DATA_SAVING_ALWAYS)
    {
        dataSavingMode = true;
    }
    else if (config.dataSaving == DATA_SAVING_MOBILE)
    {
        dataSavingMode = networkType == NET_TYPE_GPRS || networkType == NET_TYPE_EDGE ||
                         networkType == NET_TYPE_3G || networkType == NET_TYPE_HSPA || networkType == NET_TYPE_LTE || networkType == NET_TYPE_OTHER_MOBILE;
    }
    else
    {
        dataSavingMode = false;
    }
    LOGI("update data saving mode, config %d, enabled %d, reqd by peer %d", config.dataSaving, dataSavingMode, dataSavingRequestedByPeer);
}


double VoIPController::GetAverageRTT()
{
    ENFORCE_MSG_THREAD;

    if (lastSentSeq >= lastRemoteAckSeq)
    {
        uint32_t diff = lastSentSeq - lastRemoteAckSeq;
        //LOGV("rtt diff=%u", diff);
        if (diff < 32)
        {
            double res = 0;
            int count = 0;
            for (const auto &packet : recentOutgoingPackets)
            {
                if (packet.ackTime > 0)
                {
                    res += (packet.ackTime - packet.sendTime);
                    count++;
                }
            }
            if (count > 0)
                res /= count;
            return res;
        }
    }
    return 999;
}