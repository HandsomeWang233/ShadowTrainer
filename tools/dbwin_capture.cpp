// Minimal DBWIN capture: reads OutputDebugStringW messages and prints them.
// build.cmd already passes both of these on the command line; the guards keep
// the file compilable on its own too, and keep /W4 quiet about redefinition.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
int main() {
    HANDLE ack = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    HANDLE data = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
    HANDLE mapping = CreateFileMappingW(nullptr, nullptr, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
    if (!ack || !data || !mapping) { std::printf("dbwin setup failed\n"); return 1; }
    auto* buffer = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096));
    if (!buffer) return 1;
    SetEvent(ack);
    DWORD deadline = GetTickCount() + 15000;
    while (GetTickCount() < deadline) {
        const DWORD wait = WaitForSingleObject(data, 500);
        if (wait == WAIT_OBJECT_0) {
            DWORD pid = 0;
            memcpy(&pid, buffer, 4);
            std::printf("[%lu] %s", pid, buffer + 4);
            SetEvent(ack);
        }
    }
    return 0;
}
