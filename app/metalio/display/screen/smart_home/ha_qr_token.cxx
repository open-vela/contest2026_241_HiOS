#include "ha_qr_scanner.h"
#include <cstring>

bool HaQrToken_Parse(const uint8_t *payload, size_t length, std::string *token)
{
    if (!payload || !token || length > 1100) return false;
    size_t first = 0, last = length;
    auto space = [](uint8_t c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; };
    while (first < last && space(payload[first])) ++first;
    while (last > first && space(payload[last - 1])) --last;
    if (last - first >= 7 && !std::memcmp(payload + first, "Bearer ", 7)) first += 7;
    const size_t size = last - first;
    if (size < 64 || size > 1024 || std::memcmp(payload + first, "eyJ", 3)) return false;
    int dots = 0;
    size_t part = 0;
    for (size_t i = first; i < last; ++i) {
        const uint8_t c = payload[i];
        if (c == '.') {
            if (!part || ++dots > 2) return false;
            part = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
            ++part;
        }
    }
    if (dots != 2 || !part) return false;
    token->assign(reinterpret_cast<const char *>(payload + first), size);
    return true;
}
