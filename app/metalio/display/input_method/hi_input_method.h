// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"

enum class HiInputLayout { NineKey, Qwerty };
// One system surface. Initialize once after pointer/display setup.
void HiInputMethod_InitGlobal();
void HiInputMethod_Show(lv_obj_t *textarea,bool chinese_allowed=true);
// Bind each editable field once, including fields created after the keyboard.
void HiInputMethod_Bind(lv_obj_t *textarea,bool chinese_allowed=true);
void HiInputMethod_ObservePointer(lv_event_t *event);
void HiInputMethod_Hide();
bool HiInputMethod_IsVisible();
// Compatibility handle: no embedded keyboard pixels. READY is forwarded to
// this handle; the system surface owns layout and visibility for every app.
lv_obj_t *HiInputMethod_Create(lv_obj_t *parent);
void HiInputMethod_SetLayout(lv_obj_t *panel, HiInputLayout layout);
HiInputLayout HiInputMethod_GetLayout(lv_obj_t *panel);
// Switching fields discards uncommitted composition, never writes to the old field.
// Password and accepted-character fields automatically use direct input.
void HiInputMethod_SetTextArea(lv_obj_t *panel, lv_obj_t *textarea,
                               bool chinese_allowed = true);
// Commit first candidates. Unknown QWERTY tails are literal; unmatched nine-key
// codes return false. Also returns false if the target cannot accept more text.
// Call before reading text from a separate submit button. READY commits automatically.
bool HiInputMethod_Flush(lv_obj_t *panel);

// Hide the system surface immediately, preserving uncommitted composition.
void HiInputMethod_Suspend();

// Same clipboard used by the system keyboard copy/cut/paste tools. UI thread only.
void HiInputMethod_CopyText(const char *text);
