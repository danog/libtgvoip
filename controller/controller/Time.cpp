#include "../../VoIPController.h"

using namespace tgvoip;

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#endif
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

#if defined(__APPLE__)
static void initMachTimestart()
{
    mach_timebase_info_data_t tb = {0, 0};
    mach_timebase_info(&tb);
    VoIPController::machTimebase = tb.numer;
    VoIPController::machTimebase /= tb.denom;
    VoIPController::machTimestart = mach_absolute_time();
}
#endif

double VoIPController::GetCurrentTime()
{
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#elif defined(__APPLE__)
    static pthread_once_t token = PTHREAD_ONCE_INIT;
    pthread_once(&token, &initMachTimestart);
    return (mach_absolute_time() - machTimestart) * machTimebase / 1000000000.0f;
#elif defined(_WIN32)
    if (!didInitWin32TimeScale)
    {
        LARGE_INTEGER scale;
        QueryPerformanceFrequency(&scale);
        win32TimeScale = scale.QuadPart;
        didInitWin32TimeScale = true;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)win32TimeScale;
#endif
}