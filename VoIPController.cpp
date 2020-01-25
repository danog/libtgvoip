//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif
#include <errno.h>
#include <string.h>
#include <wchar.h>
#include "VoIPController.h"
#include "tools/logging.h"
#include "tools/threading.h"
#include "tools/Buffers.h"
#include "controller/audio/OpusEncoder.h"
#include "controller/audio/OpusDecoder.h"
#include "VoIPServerConfig.h"
#include "controller/PrivateDefines.h"
#include "controller/net/Endpoint.h"
#include "tools/json11.hpp"
#include "controller/PacketSender.h"
#include "video/VideoPacketSender.h"
#include <assert.h>
#include <time.h>
#include <math.h>
#include <exception>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <inttypes.h>
#include <float.h>

inline int pad4(int x)
{
    int r = PAD4(x);
    if (r == 4)
        return 0;
    return r;
}

using namespace tgvoip;
using namespace std;

#ifdef __APPLE__
#include "os/darwin/AudioUnitIO.h"
#include <mach/mach_time.h>
double VoIPController::machTimebase = 0;
uint64_t VoIPController::machTimestart = 0;
#endif

#ifdef _WIN32
int64_t VoIPController::win32TimeScale = 0;
bool VoIPController::didInitWin32TimeScale = false;
#endif

#ifdef __ANDROID__
#include "os/android/JNIUtilities.h"
#include "os/android/AudioInputAndroid.h"
#include "controller/net/NetworkSocket.h"

extern jclass jniUtilitiesClass;
#endif

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
#include "audio/AudioIOCallback.h"
#endif


#include "controller/voip/PublicAPI.cpp"
#include "controller/voip/Init.cpp"


#pragma mark - Miscellaneous

void VoIPController::SetState(int state)
{
    this->state = state;
    LOGV("Call state changed to %d", state);
    stateChangeTime = GetCurrentTime();
    messageThread.Post([this, state] {
        if (callbacks.connectionStateChanged)
            callbacks.connectionStateChanged(this, state);
    });
    if (state == STATE_ESTABLISHED)
    {
        SetMicMute(micMuted);
        if (!wasEstablished)
        {
            wasEstablished = true;
            messageThread.Post(std::bind(&VoIPController::UpdateRTT, this), 0.1, 0.5);
            messageThread.Post(std::bind(&VoIPController::UpdateAudioBitrate, this), 0.0, 0.3);
            messageThread.Post(std::bind(&VoIPController::UpdateCongestion, this), 0.0, 1.0);
            messageThread.Post(std::bind(&VoIPController::UpdateSignalBars, this), 1.0, 1.0);
            messageThread.Post(std::bind(&VoIPController::TickJitterBufferAndCongestionControl, this), 0.0, 0.1);
        }
    }
}
