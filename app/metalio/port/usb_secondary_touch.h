/* SPDX-License-Identifier: Apache-2.0
 * Touch report descriptor from MetalioClaw4 USB extend screen. */
#pragma once
#define TUD_HID_REPORT_DESC_TOUCH_SCREEN(report_id, width, height)             \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER), HID_USAGE(0x04),                 \
        HID_COLLECTION(HID_COLLECTION_APPLICATION), HID_REPORT_ID(report_id)   \
            FINGER_USAGE(width, height) FINGER_USAGE(width, height)            \
                FINGER_USAGE(width, height) FINGER_USAGE(width, height)        \
                    FINGER_USAGE(width, height) HID_USAGE(0x54),               \
        HID_LOGICAL_MAX(127), HID_REPORT_COUNT(1), HID_REPORT_SIZE(8),         \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                     \
        HID_REPORT_ID(report_id + 1) HID_USAGE(0x55), HID_REPORT_COUNT(1),     \
        HID_LOGICAL_MAX(0x10),                                                 \
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                   \
        HID_COLLECTION_END

#define FINGER_USAGE(width, height)                                            \
    HID_USAGE(0x42), HID_COLLECTION(HID_COLLECTION_LOGICAL), HID_USAGE(0x42),  \
        HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x01), HID_REPORT_SIZE(1),      \
        HID_REPORT_COUNT(1), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_REPORT_COUNT(7),                                                   \
        HID_INPUT(HID_CONSTANT | HID_ARRAY | HID_ABSOLUTE),                    \
        HID_REPORT_SIZE(8), HID_USAGE(0x51), HID_REPORT_COUNT(1),              \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                     \
        HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),                                \
        HID_LOGICAL_MAX_N(width, 2), HID_REPORT_SIZE(16),                      \
        HID_UNIT_EXPONENT(0x0e), HID_UNIT(0x13), HID_USAGE(0x30),              \
        HID_PHYSICAL_MIN(0), HID_PHYSICAL_MAX_N(width, 2),                     \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                     \
        HID_LOGICAL_MAX_N(height, 2), HID_PHYSICAL_MAX_N(height, 2),           \
        HID_USAGE(0x31), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),    \
        HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER), HID_USAGE(0x48),             \
        HID_USAGE(0x49), HID_REPORT_COUNT(2),                                  \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_COLLECTION_END,
