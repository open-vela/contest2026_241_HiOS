/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen base class — default Destroy() / OnButtonClick() implementations.
 */

#include "screen_base.h"

void Screen::Destroy()
{
    if (root_ != nullptr)
    {
        // Async-delete so any in-flight LVGL animations referencing the
        // tree get a chance to be cancelled first.
        lv_obj_delete_async(root_);
        root_ = nullptr;
    }
}

void Screen::OnButtonClick(lv_event_t * /*e*/)
{
    // Default no-op. Concrete screens override or attach per-widget
    // callbacks directly during Create().
}
