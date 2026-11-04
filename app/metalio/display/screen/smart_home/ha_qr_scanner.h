#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

enum HaQrResult { kHaQrNone, kHaQrToken, kHaQrNotToken, kHaQrFailure };
bool HaQrScanner_Start();
void HaQrScanner_Stop();
/* Camera observer: copy grayscale only; QR decoding runs off the LVGL thread. */
void HaQrScanner_Frame(const uint8_t *bgr, int width, int height);
HaQrResult HaQrScanner_Poll(std::string *token);
bool HaQrScanner_HasFrame();
bool HaQrToken_Parse(const uint8_t *payload, size_t length, std::string *token);
