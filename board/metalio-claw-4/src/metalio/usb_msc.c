/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <errno.h>
#include <unistd.h>
#include <metalio/metalio.h>
#include "hal/usb_wrap_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_pins.h"
#include "espressif/esp_irq.h"
#include "soc/interrupts.h"

static sem_t g_usb_event = SEM_INITIALIZER(0);
static int g_usb_cpuint = -1;
#define METALIO_USB_IRQ ESP_SOURCE2IRQ(ETS_USB_OTG11_CH0_INTR_SOURCE)

/* The ISR only wakes the transport owner. It never touches TinyUSB's
 * OS_NONE queue, SD, JPEG decoder or LVGL. Mask until the owner drains the
 * level interrupt to prevent an interrupt storm. */
static int metalio_usb_event(int irq, void *context, void *arg)
{
  up_disable_irq(METALIO_USB_IRQ);
  nxsem_post(&g_usb_event);
  return OK;
}

void metalio_usb_msc_wait_event(void)
{
  /* A one-millisecond usleep rounds up to this build's 10-ms tick. USB
   * packets must wake us immediately, with a bounded idle timeout for UI
   * stop requests and enumeration recovery. */
  up_enable_irq(METALIO_USB_IRQ);
  (void)nxsem_tickwait_uninterruptible(&g_usb_event, MSEC2TICK(20));
  up_disable_irq(METALIO_USB_IRQ);
}

void metalio_usb_msc_connect(bool connected)
{
  usb_wrap_otg_conf_reg_t conf = USB_WRAP.otg_conf;
  conf.pad_pull_override = !connected;
  conf.dp_pullup = 0;
  conf.dm_pullup = 0;
  conf.dp_pulldown = !connected;
  conf.dm_pulldown = !connected;
  USB_WRAP.otg_conf = conf;
}

int metalio_usb_msc_phy(bool enable)
{
  if (enable && g_usb_cpuint < 0)
    {
      nxsem_set_protocol(&g_usb_event, SEM_PRIO_NONE);
      g_usb_cpuint = esp_setup_irq(ETS_USB_OTG11_CH0_INTR_SOURCE,
                                  ESP_IRQ_PRIORITY_DEFAULT,
                                  ESP_IRQ_TRIGGER_LEVEL,
                                  metalio_usb_event, NULL);
      if (g_usb_cpuint < 0) return g_usb_cpuint;
    }
  if (g_usb_cpuint >= 0) up_disable_irq(METALIO_USB_IRQ);
  while (nxsem_trywait(&g_usb_event) == OK) { }
  irqstate_t flags = enter_critical_section();
  if (enable)
    {
      _usb_wrap_ll_enable_bus_clock(true);
      _usb_wrap_ll_reset_register();
      usb_serial_jtag_ll_phy_enable_pad(false);
      usb_wrap_ll_phy_select(&USB_WRAP, 0); /* GPIO24/25, leave BT26/27 alone */
      usb_wrap_ll_phy_set_defaults(&USB_WRAP);
      gpio_ll_set_drive_capability(GPIO_LL_GET_HW(0), 24, GPIO_DRIVE_CAP_3);
      gpio_ll_set_drive_capability(GPIO_LL_GET_HW(0), 25, GPIO_DRIVE_CAP_3);
      gpio_ll_set_input_signal_matrix_source(GPIO_LL_GET_HW(0),
          USB_OTG11_IDDIG_PAD_IN_IDX, GPIO_MATRIX_CONST_ONE_INPUT, false);
      gpio_ll_set_input_signal_matrix_source(GPIO_LL_GET_HW(0),
          USB_SRP_BVALID_PAD_IN_IDX, GPIO_MATRIX_CONST_ONE_INPUT, false);
      gpio_ll_set_input_signal_matrix_source(GPIO_LL_GET_HW(0),
          USB_OTG11_VBUSVALID_PAD_IN_IDX, GPIO_MATRIX_CONST_ONE_INPUT, false);
      gpio_ll_set_input_signal_matrix_source(GPIO_LL_GET_HW(0),
          USB_OTG11_AVALID_PAD_IN_IDX, GPIO_MATRIX_CONST_ZERO_INPUT, false);
    }
  else
    {
      usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
      usb_wrap_ll_phy_select(&USB_WRAP, 1);
      usb_serial_jtag_ll_phy_set_defaults();
      usb_serial_jtag_ll_phy_enable_pad(true);
      _usb_wrap_ll_enable_bus_clock(false);
    }
  leave_critical_section(flags);
  if (enable)
    {
      // Force an actual disconnect while the shared serial/JTAG PHY is
      // handed to OTG. Windows must retire the old device before reconnect.
      metalio_usb_msc_connect(false);
      usleep(300000);
      /* Do not enter TinyUSB's reset wait with a missing peripheral clock. */
      volatile uint32_t *regs = (volatile uint32_t *)0x50040000;
      if ((regs[0x40 / 4] & 0xffff0000) != 0x4f540000 ||
          !(regs[0x10 / 4] & (1u << 31)))
        {
          metalio_usb_msc_phy(false);
          return -ENODEV;
        }
    }
  return 0;
}
