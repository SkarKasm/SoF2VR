#ifndef SOF2_VR_LOG_H
#define SOF2_VR_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void VR_Log(const char* level, const char* format, ...) {
    char message[1024];
    char output[1100];
    char exePath[MAX_PATH];
    char* lastSlash;
    HANDLE logFile;
    va_list args;

    va_start(args, format);
    _vsnprintf(message, sizeof(message) - 1, format, args);
    va_end(args);
    message[sizeof(message) - 1] = '\0';
    _snprintf(output, sizeof(output) - 1, "[SoF2 VR] [%s] %s\r\n", level, message);
    output[sizeof(output) - 1] = '\0';
    OutputDebugStringA(output);

    if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            lastSlash[1] = '\0';
            strncat(exePath, "sof2_vr_proxy.log", MAX_PATH - strlen(exePath) - 1);
            logFile = CreateFileA(exePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (logFile != INVALID_HANDLE_VALUE) {
                DWORD bytesWritten;
                WriteFile(logFile, output, (DWORD)strlen(output), &bytesWritten, NULL);
                CloseHandle(logFile);
            }
        }
    }
}

#define LOG_INFO(...)  VR_Log("INFO", __VA_ARGS__)
#define LOG_WARN(...)  VR_Log("WARN", __VA_ARGS__)
#define LOG_ERROR(...) VR_Log("ERROR", __VA_ARGS__)

#endif
