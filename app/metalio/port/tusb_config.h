/* HiOS USB SD reader: one worker owns both DWC2 polling and MSC callbacks. */
#pragma once
#define HIOS_NUTTX_USB 1
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 1280
#define CFG_TUD_VENDOR_TX_BUFSIZE 64
#define CFG_TUD_VENDOR_EPSIZE 64
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 4096
#define CFG_TUD_DWC2_DMA_ENABLE 0
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_TASK_QUEUE_SZ 16
#define CFG_TUSB_MEM_SECTION __attribute__((section(".ext_ram.bss")))
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
