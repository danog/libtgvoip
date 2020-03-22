#pragma once

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#endif
#include "../VoIPController.h"
#include "../VoIPServerConfig.h"
#include "../controller/PrivateDefines.h"
#include "../controller/audio/AudioPacketSender.h"
#include "../controller/audio/OpusDecoder.h"
#include "../controller/audio/OpusEncoder.h"
#include "../controller/net/Endpoint.h"
#include "../controller/protocol/packets/PacketSender.h"
#include "../controller/protocol/protocol/Index.h"
#include "../tools/Buffers.h"
#include "../tools/json11.hpp"
#include "../tools/logging.h"
#include "../tools/threading.h"
#include "../video/VideoPacketSender.h"
#include <algorithm>
#include <assert.h>
#include <errno.h>
#include <exception>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <sstream>
#include <stdexcept>
#include <string.h>
#include <time.h>
#include <wchar.h>

inline int pad4(int x)
{
    int r = PAD4(x);
    if (r == 4)
        return 0;
    return r;
}

using namespace tgvoip;
using namespace std;

#ifdef __ANDROID__
#include "controller/net/NetworkSocket.h"
#include "os/android/AudioInputAndroid.h"
#include "os/android/JNIUtilities.h"

extern jclass jniUtilitiesClass;
#endif

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
#include "audio/AudioIOCallback.h"
#endif
