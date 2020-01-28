#pragma once

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif
#include <errno.h>
#include <string.h>
#include <wchar.h>
#include "../VoIPController.h"
#include "../tools/logging.h"
#include "../tools/threading.h"
#include "../tools/Buffers.h"
#include "../controller/audio/OpusEncoder.h"
#include "../controller/audio/OpusDecoder.h"
#include "../VoIPServerConfig.h"
#include "../controller/PrivateDefines.h"
#include "../controller/net/Endpoint.h"
#include "../tools/json11.hpp"
#include "../controller/net/PacketSender.h"
#include "../controller/audio/AudioPacketSender.h"
#include "../video/VideoPacketSender.h"
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


#ifdef __ANDROID__
#include "os/android/JNIUtilities.h"
#include "os/android/AudioInputAndroid.h"
#include "controller/net/NetworkSocket.h"

extern jclass jniUtilitiesClass;
#endif

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
#include "audio/AudioIOCallback.h"
#endif

