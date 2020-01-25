#include "../../VoIPController.h"

using namespace tgvoip;
using namespace std;


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