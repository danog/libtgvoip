#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Internal intialization

void VoIPController::InitializeTimers()
{
    initTimeoutID = messageThread.Post(
        [this] {
            LOGW("Init timeout, disconnecting");
            lastError = ERROR_TIMEOUT;
            SetState(STATE_FAILED);
        },
        config.initTimeout);

    if (!config.statsDumpFilePath.empty())
    {
        messageThread.Post(
            [this] {
                if (statsDump && incomingStreams.size() == 1)
                {
                    shared_ptr<JitterBuffer> &jitterBuffer = incomingStreams[0]->jitterBuffer;
                    statsDump << std::setprecision(3)
                              << GetCurrentTime() - connectionInitTime
                              << endpoints.at(currentEndpoint).rtts[0]
                              << lastRemoteSeq
                              << (uint32_t)getLocalSeq()
                              << peerAcks[0]
                              << recvLossCount
                              << conctl.GetSendLossCount()
                              << (int)conctl.GetInflightDataSize()
                              << (encoder ? encoder->GetBitrate() : 0)
                              << (encoder ? encoder->GetPacketLoss() : 0)
                              << (jitterBuffer ? jitterBuffer->GetLastMeasuredJitter() : 0)
                              << (jitterBuffer ? jitterBuffer->GetLastMeasuredDelay() * 0.06 : 0)
                              << (jitterBuffer ? jitterBuffer->GetAverageDelay() * 0.06 : 0);
                }
            },
            0.1, 0.1);
    }

    messageThread.Post(std::bind(&VoIPController::SendRelayPings, this), 0.0, 2.0);
}
