#include "log.h"

#include <cstdarg>
#include <cstdio>

static vr::IVRDriverLog* g_pDriverLog = nullptr;

void DriverLog_InitDriverLog() {
    g_pDriverLog = vr::VRDriverLog();
}

void DriverLog_CleanupDriverLog() {
    g_pDriverLog = nullptr;
}

void DriverLog(const char* fmt, ...) {
    if (!g_pDriverLog) {
        return;
    }

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_pDriverLog->Log(buf);
}