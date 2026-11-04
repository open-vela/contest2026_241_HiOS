#pragma once

namespace HiNavigation {
// Logical pages/recents form a ring; sentinel clone widgets are not entries.
inline int Adjacent(int current,int direction,int count) {
    if(count<=0 || current<0 || current>=count || !direction) return -1;
    return (current+(direction>0?1:-1)+count)%count;
}
}
