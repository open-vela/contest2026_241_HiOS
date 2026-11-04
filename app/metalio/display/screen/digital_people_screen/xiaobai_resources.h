#pragma once
namespace XiaobaiResources {
void EnsureAsync();
// 0 idle, 1 installing, 2 ready, negative on a real SD/write/verification error.
int Status();
}
