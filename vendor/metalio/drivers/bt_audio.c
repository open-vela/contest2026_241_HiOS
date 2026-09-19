#include <nuttx/config.h>
#include <syslog.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

int metalio_bt_audio_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, true);
  syslog(LOG_INFO, "BT audio UART TX=%d RX=%d 115200 (CX25601N)\n",
         BOARD_BT_AUDIO_TX_GPIO, BOARD_BT_AUDIO_RX_GPIO);
  return OK;
}

int metalio_i2s_audio_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);
  syslog(LOG_INFO, "I2S audio MIC WS=%d DIN=%d SPK DOUT=%d BCLK=%d\n",
         BOARD_I2S_MIC_WS_GPIO, BOARD_I2S_MIC_DIN_GPIO,
         BOARD_I2S_SPK_DOUT_GPIO, BOARD_I2S_SPK_BCLK_GPIO);
  return OK;
}
