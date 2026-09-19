#ifndef METALIO_SYSTEM_INFO_H
#define METALIO_SYSTEM_INFO_H

#include <string>
#include <cstddef>

/**
 * NuttX-compatible SystemInfo — replaces ESP-IDF system APIs with
 * NuttX equivalents (/proc, mallinfo, boardctl).
 */
class SystemInfo
{
public:
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static void PrintTaskList();
    static void PrintHeapStats();
};

#endif // METALIO_SYSTEM_INFO_H
