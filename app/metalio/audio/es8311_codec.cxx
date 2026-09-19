/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ES8311 audio codec implementation for NuttX/openvela.
 *
 * Uses the NuttX I2C character driver (/dev/i2c0) with I2CIOC_TRANSFER
 * to directly access ES8311 codec registers for:
 *   - Volume control (DAC + ADC volume registers)
 *   - Mute control (DAC + ADC mute registers)
 *   - Input / output path configuration
 *   - Clock tree setup for the configured sample rate
 *
 * PCM data transport is handled by the AudioCodec base class via
 * /dev/audio/pcm0 (NuttX audio subsystem).
 */

#include "es8311_codec.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"

#include <nuttx/i2c/i2c_master.h>

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#define TAG "Es8311AudioCodec"

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

Es8311AudioCodec::Es8311AudioCodec(int input_sample_rate, int output_sample_rate,
                                   const char* i2c_dev_path, uint8_t i2c_addr)
{
    /* ES8311 is a full-duplex codec */
    duplex_ = true;
    input_reference_ = false;
    input_channels_  = 1;
    output_channels_ = 1;
    input_sample_rate_  = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_  = 30.0f;   /* dB — matches ESP-IDF default */
    output_volume_ = 70;

    if (i2c_dev_path)
        i2c_dev_path_ = i2c_dev_path;
    else
        i2c_dev_path_ = "/dev/i2c0";

    i2c_addr_ = i2c_addr;

    ESP_LOGI(TAG, "Es8311AudioCodec: in_rate=%d out_rate=%d i2c=%s addr=0x%02x",
             input_sample_rate, output_sample_rate,
             i2c_dev_path_.c_str(), i2c_addr_);
}

Es8311AudioCodec::~Es8311AudioCodec()
{
    CloseI2c();
}

/* ------------------------------------------------------------------ */
/* I2C device management                                              */
/* ------------------------------------------------------------------ */

bool Es8311AudioCodec::OpenI2c(void)
{
    if (i2c_fd_ >= 0)
        return true;

    i2c_fd_ = open(i2c_dev_path_.c_str(), O_RDWR);
    if (i2c_fd_ < 0)
    {
        ESP_LOGE(TAG, "Failed to open I2C device %s: %d",
                 i2c_dev_path_.c_str(), errno);
        return false;
    }

    ESP_LOGI(TAG, "Opened I2C device %s (fd=%d)", i2c_dev_path_.c_str(), i2c_fd_);
    return true;
}

void Es8311AudioCodec::CloseI2c(void)
{
    if (i2c_fd_ >= 0)
    {
        close(i2c_fd_);
        i2c_fd_ = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Low-level I2C register access                                      */
/* ------------------------------------------------------------------ */

/*
 * Perform an I2C write of one byte (register address + value) to the
 * ES8311 using the NuttX I2C character-driver I2CIOC_TRANSFER ioctl.
 */
bool Es8311AudioCodec::I2cWrite(uint8_t reg, uint8_t value)
{
    if (!OpenI2c())
        return false;

    uint8_t buf[2] = { reg, value };

    struct i2c_msg_s msg;
    memset(&msg, 0, sizeof(msg));
    msg.addr   = i2c_addr_;
    msg.flags  = 0;          /* write */
    msg.buffer = buf;
    msg.length = 2;

    struct i2c_transfer_s xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.msgv = &msg;
    xfer.msgc = 1;

    int ret = ioctl(i2c_fd_, I2CIOC_TRANSFER,
                    (unsigned long)(uintptr_t)&xfer);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "I2C write (reg=0x%02x val=0x%02x) failed: %d (errno=%d)",
                 reg, value, ret, errno);
        return false;
    }
    return true;
}

/*
 * Perform a register read: I2C write of the register address (no stop),
 * then a repeated-start read of one byte.
 */
bool Es8311AudioCodec::I2cWriteRead(uint8_t reg, uint8_t* read_val)
{
    if (!OpenI2c() || !read_val)
        return false;

    struct i2c_msg_s msgs[2];
    memset(msgs, 0, sizeof(msgs));

    /* Write: register address, no STOP (repeated start follows) */
    msgs[0].addr   = i2c_addr_;
    msgs[0].flags  = I2C_M_NOSTOP;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;

    /* Read: one data byte */
    msgs[1].addr   = i2c_addr_;
    msgs[1].flags  = I2C_M_READ;
    msgs[1].buffer = read_val;
    msgs[1].length = 1;

    struct i2c_transfer_s xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.msgv = msgs;
    xfer.msgc = 2;

    int ret = ioctl(i2c_fd_, I2CIOC_TRANSFER,
                    (unsigned long)(uintptr_t)&xfer);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "I2C read (reg=0x%02x) failed: %d (errno=%d)",
                 reg, ret, errno);
        return false;
    }
    return true;
}

bool Es8311AudioCodec::WriteRegister(uint8_t reg, uint8_t value)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    return I2cWrite(reg, value);
}

bool Es8311AudioCodec::ReadRegister(uint8_t reg, uint8_t* value)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    return I2cWriteRead(reg, value);
}

/* ------------------------------------------------------------------ */
/* Codec configuration                                                */
/* ------------------------------------------------------------------ */

void Es8311AudioCodec::ResetCodec(void)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    /* Write 0x80 to the reset register to perform a soft reset */
    I2cWrite(ES8311_RESET_REG, ES8311_RESET_VAL);
    /* Small delay to let the codec settle */
    usleep(10 * 1000);
    ESP_LOGI(TAG, "ES8311 soft reset complete");
}

void Es8311AudioCodec::ConfigureClocks(int sample_rate)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);

    /*
     * Minimal clock-tree setup for the ES8311.
     * The ES8311 uses an internal PLL to generate the master clock (MCLK)
     * from the BCLK or an external MCLK.  The exact register values depend
     * on the board's I2S clock configuration.
     *
     * For the MetalioClaw-4 board, the SoC provides MCLK at 256*Fs.
     * The registers below configure the codec to accept external MCLK
     * and set the correct divider ratios for 16 kHz / 48 kHz operation.
     *
     * NOTE: These values may need adjustment for specific board clocks.
     * The NuttX ES8311 lower-half driver (if registered) handles this
     * automatically; this direct-I2C path is a fallback / override.
     */

    /* Select external MCLK, set clock source */
    I2cWrite(ES8311_CLK_MANAGER_REG0, 0x3F);

    /* Set divider based on sample rate.
     * For 16 kHz: divide ratio configured for 256*Fs MCLK
     * For 48 kHz: divide ratio configured for 256*Fs MCLK
     * The ES8311 datasheet provides the divider table. */
    uint8_t div_val;
    if (sample_rate >= 48000)
        div_val = 0x08;  /* 48 kHz configuration */
    else if (sample_rate >= 32000)
        div_val = 0x0C;  /* 32 kHz configuration */
    else if (sample_rate >= 24000)
        div_val = 0x10;  /* 24 kHz configuration */
    else
        div_val = 0x18;  /* 16 kHz configuration */

    I2cWrite(ES8311_CLK_MANAGER_REG1, div_val);
    I2cWrite(ES8311_CLK_MANAGER_REG2, 0x44);

    /* Configure SDP (serial data port) for I2S format, 16-bit, 2-slot */
    I2cWrite(ES8311_SDPIN_REG,  0x00);   /* DAC: I2S, 16-bit */
    I2cWrite(ES8311_SDPOUT_REG, 0x00);   /* ADC: I2S, 16-bit */

    ESP_LOGI(TAG, "ES8311 clocks configured for %d Hz", sample_rate);
}

void Es8311AudioCodec::ConfigureInputPath(int mic_gain_db)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);

    /*
     * Select AMIC (analog microphone) input path.
     * ES8311 supports AMIC and DMIC; the MetalioClaw-4 uses AMIC.
     *
     * Register 0x14 (GPIO_REG2): input source selection
     *   bit 7: 0 = AMIC, 1 = DMIC
     */
    I2cWrite(ES8311_GPIO_REG2, 0x00);   /* AMIC path */

    /* Set microphone bias / PGA gain */
    /* The mic gain is controlled via the ADC PGA. Values 0..255 map
     * roughly to 0..42 dB. Scale the requested dB to the register. */
    uint8_t pga_val = (uint8_t)((mic_gain_db * 255) / 42);
    I2cWrite(ES8311_ADC_REG0, pga_val);

    ESP_LOGI(TAG, "ES8311 input path configured: AMIC, gain=%d dB (reg=0x%02x)",
             mic_gain_db, pga_val);
}

void Es8311AudioCodec::ConfigureOutputPath(void)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);

    /*
     * Configure DAC output path to the on-board speaker amplifier.
     * The ES8311 DAC output goes to the lineout / speaker amp.
     *
     * Register 0x10 (GPIO_REG0): output source / power
     * Register 0x0F (DAC_REG1): DAC mute / ramp rate
     */
    I2cWrite(ES8311_GPIO_REG0, 0x00);   /* DAC to lineout */
    I2cWrite(ES8311_DAC_REG1,  0x00);   /* Unmute, normal ramp */

    ESP_LOGI(TAG, "ES8311 output path configured");
}

void Es8311AudioCodec::ApplyVolumeRegisters(void)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);

    /* DAC volume: 0..255 maps to -95.5..+32 dB.
     * Map our 0..100 volume scale to 0..255.
     * 0 = mute, 100 = max gain. */
    uint8_t dac_vol = (uint8_t)(output_volume_ * 255 / 100);
    I2cWrite(ES8311_DAC_REG0, dac_vol);

    /* ADC volume (input gain): map 0..42 dB to 0..255 */
    uint8_t adc_vol = (uint8_t)((input_gain_ * 255) / 42);
    I2cWrite(ES8311_ADC_REG0, adc_vol);

    ESP_LOGI(TAG, "Volume registers applied: DAC=0x%02x ADC=0x%02x",
             dac_vol, adc_vol);
}

void Es8311AudioCodec::ApplyMuteRegisters(void)
{
    std::lock_guard<std::mutex> lock(i2c_mutex_);

    /* DAC mute: bit 2 of register 0x0F */
    uint8_t dac_reg1;
    if (I2cWriteRead(ES8311_DAC_REG1, &dac_reg1))
    {
        if (muted_)
            dac_reg1 |= 0x20;   /* Set mute bit */
        else
            dac_reg1 &= ~0x20;  /* Clear mute bit */
        I2cWrite(ES8311_DAC_REG1, dac_reg1);
    }

    /* ADC mute: bit 2 of register 0x0D */
    uint8_t adc_reg1;
    if (I2cWriteRead(ES8311_ADC_REG1, &adc_reg1))
    {
        if (muted_)
            adc_reg1 |= 0x20;
        else
            adc_reg1 &= ~0x20;
        I2cWrite(ES8311_ADC_REG1, adc_reg1);
    }

    ESP_LOGI(TAG, "Mute registers applied: muted=%s", muted_ ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* Override base class methods                                        */
/* ------------------------------------------------------------------ */

void Es8311AudioCodec::SetOutputVolume(int volume)
{
    AudioCodec::SetOutputVolume(volume);  /* Updates output_volume_ + NuttX ioctl */
    ApplyVolumeRegisters();               /* Also write ES8311 DAC register */
}

void Es8311AudioCodec::SetInputGain(float gain)
{
    AudioCodec::SetInputGain(gain);
    ApplyVolumeRegisters();
}

void Es8311AudioCodec::SetMute(bool mute)
{
    AudioCodec::SetMute(mute);
    ApplyMuteRegisters();
}

void Es8311AudioCodec::EnableInput(bool enable)
{
    if (enable == input_enabled_)
        return;

    if (enable)
    {
        /* Power up ADC path */
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        I2cWrite(ES8311_GPIO_REG2, 0x10);   /* Power up ADC */
        ESP_LOGI(TAG, "ES8311 ADC powered up");
    }
    else
    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        I2cWrite(ES8311_GPIO_REG2, 0x00);   /* Power down ADC */
        ESP_LOGI(TAG, "ES8311 ADC powered down");
    }

    AudioCodec::EnableInput(enable);
}

void Es8311AudioCodec::EnableOutput(bool enable)
{
    if (enable == output_enabled_)
        return;

    if (enable)
    {
        /* Power up DAC path */
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        I2cWrite(ES8311_GPIO_REG0, 0x10);   /* Power up DAC */
        ESP_LOGI(TAG, "ES8311 DAC powered up");
    }
    else
    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        I2cWrite(ES8311_GPIO_REG0, 0x00);   /* Power down DAC */
        ESP_LOGI(TAG, "ES8311 DAC powered down");
    }

    AudioCodec::EnableOutput(enable);
}
