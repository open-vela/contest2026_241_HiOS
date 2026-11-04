#pragma once
#include <cstdint>
#include <cstdlib>

namespace DesktopDrag {
constexpr uint32_t kMergeHoldMs = 350;
constexpr int kMergeEnter = 58;
constexpr int kMergeLeave = 78;
constexpr uint32_t kFolderMergeHoldMs = 180;
constexpr int kFolderPageSize = 9;
inline int PageCount(int members) { return (members + kFolderPageSize - 1) / kFolderPageSize; }
inline int FolderSlot(int page, int x, int y, int members) {
    if(members <= 0) return -1;
    const int col = x < 0 ? 0 : x / 206 > 2 ? 2 : x / 206;
    const int row = y < 0 ? 0 : y / 150 > 2 ? 2 : y / 150;
    const int slot = page * kFolderPageSize + row * 3 + col;
    return slot < members ? slot : members - 1;
}
inline bool Inside(int dx, int dy, bool retained, bool folder = false) {
    const int limit = folder ? (retained ? 86 : 68) : (retained ? kMergeLeave : kMergeEnter);
    return std::abs(dx) <= limit && std::abs(dy) <= limit;
}
inline bool Ready(uint32_t now, uint32_t entered, bool folder = false) {
    return uint32_t(now - entered) >= (folder ? kFolderMergeHoldMs : kMergeHoldMs);
}
}
