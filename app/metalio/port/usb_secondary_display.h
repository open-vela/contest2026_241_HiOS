/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
struct _lv_obj_t;
namespace UsbSecondary {
void Attach(_lv_obj_t *root);
void Configure(int quality,int fps,int bytes);
size_t FrameLimit();
const char *Description();
void Receive(const uint8_t *data,size_t bytes);
void Reset();
void ServiceTouch();
bool HasPicture();
}
