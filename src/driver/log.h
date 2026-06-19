#pragma once

#include <openvr_driver.h>

void DriverLog_InitDriverLog();
void DriverLog_CleanupDriverLog();
void DriverLog(const char* fmt, ...);