/* SPDX-License-Identifier: MIT
 * ESP32-P4 FS controller, serviced exclusively by the USB worker. The board
 * ISR only signals a semaphore; no TinyUSB API runs in interrupt context,
 * so OS_NONE queue critical sections remain no-ops.
 */
#pragma once
#include <unistd.h>
#define DWC2_EP_MAX 7
static const dwc2_controller_t _dwc2_controller[] = {
  {.reg_base = 0x50040000UL, .irqnum = 0, .ep_count = 7,
   .ep_in_count = 5, .otg_dfifo_depth = 256}
};
static inline void dwc2_clock_init(uint8_t p, tusb_role_t r) {(void)p; (void)r;}
static inline void dwc2_int_set(uint8_t p, tusb_role_t r, bool e)
{(void)p; (void)r; (void)e;}
static inline void dwc2_remote_wakeup_delay(void) {usleep(1000);}
static inline void dwc2_phy_init(dwc2_regs_t *d, uint8_t h) {(void)d; (void)h;}
static inline void dwc2_phy_deinit(dwc2_regs_t *d, uint8_t h) {(void)d; (void)h;}
static inline void dwc2_phy_update(dwc2_regs_t *d, uint8_t h) {(void)d; (void)h;}
