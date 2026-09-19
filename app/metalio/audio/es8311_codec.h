/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ES8311 audio codec hardware abstraction for NuttX/openvela.
 *
 * Replaces the ESP-IDF esp_codec_dev + i2c_master based ES8311 driver
 * with direct I2C register access via the NuttX I2C character driver
 * (/dev/i2c0) using the I2CIOC_TRANSFER ioctl.
 *
 * PCM data transport (I2S) is handled by the AudioCodec base class
 * through the NuttX audio subsystem (/dev/audio/pcm0).  This subclass
 * handles codec-chip configuration: volume, mute, input/output path
 * selection, and clock setup.
 */

#ifndef _ES8311_AUDIO_CODEC_H
#define _ES8311_AUDIO_CODEC_H

#include "audio_codec.h"

#include <nuttx/i2c/i2c_master.h>

#include <mutex>

/*
 * ES8311 I2C slave address (7-bit).
 * The ES8311 supports addresses 0x18 (ADDR pin low) or 0x19 (ADDR pin
 * high).  Most MetalioClaw boards use 0x18.
 */
#define ES8311_I2C_ADDR_DEFAULT  0x18

/* ES8311 register addresses (subset relevant to this driver) */
#define ES8311_RESET_REG         0x00
#define ES8311_CLK_MANAGER_REG0  0x01
#define ES8311_CLK_MANAGER_REG1  0x02
#define ES8311_CLK_MANAGER_REG2  0x03
#define ES8311_CLK_MANAGER_REG4  0x05
#define ES8311_CLK_MANAGER_REG5  0x06
#define ES8311_CLK_MANAGER_REG6  0x07
#define ES8311_CLK_MANAGER_REG7  0x08
#define ES8311_CLK_MANAGER_REG8  0x09
#define ES8311_SDPIN_REG         0x0A  /* DAC serial data port */
#define ES8311_SDPOUT_REG        0x0B  /* ADC serial data port */
#define ES8311_ADC_REG0          0x0C  /* ADC volume */
#define ES8311_ADC_REG1          0x0D  /* ADC mute / ramp */
#define ES8311_DAC_REG0          0x0E  /* DAC volume */
#define ES8311_DAC_REG1          0x0F  /* DAC mute / ramp */
#define ES8311_GPIO_REG0         0x10
#define ES8311_GPIO_REG1         0x12
#define ES8311_GPIO_REG2         0x14
#define ES8311_GPIO_REG3         0x16
#define ES8311_GPIO_REG4         0x17
#define ES8311_SYSTEM_REG0       0x00  /* Reset / system */
#define ES8311_SYSTEM_REG1       0x01
#define ES8311_CHIP_ID_REG       0x20  /* Chip ID */
#define ES8311_CHIP_VERSION_REG  0x21  /* Chip version */

/* Reset value */
#define ES8311_RESET_VAL         0x80

/* Default I2C bus speed */
#define ES8311_I2C_FREQ_DEFAULT  I2C_SPEED_FAST  /* 400 kHz */

class Es8311AudioCodec : public AudioCodec
{
public:
    /*
     * input_sample_rate  — ADC (microphone) sample rate in Hz
     * output_sample_rate — DAC (speaker) sample rate in Hz
     * i2c_dev_path       — NuttX I2C character device path (e.g. "/dev/i2c0")
     * i2c_addr           — 7-bit ES8311 I2C address
     */
    Es8311AudioCodec(int input_sample_rate, int output_sample_rate,
                     const char* i2c_dev_path = "/dev/i2c0",
                     uint8_t i2c_addr = ES8311_I2C_ADDR_DEFAULT);

    virtual ~Es8311AudioCodec();

    /* Override base class methods for codec-specific control */
    virtual void SetOutputVolume(int volume) override;
    virtual void SetInputGain(float gain) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual void SetMute(bool mute) override;

    /* ES8311-specific: configure input/output paths */
    void ConfigureInputPath(int mic_gain_db);
    void ConfigureOutputPath(void);
    void ResetCodec(void);

    /* Direct register access (exposed for debugging / advanced use) */
    bool WriteRegister(uint8_t reg, uint8_t value);
    bool ReadRegister(uint8_t reg, uint8_t* value);

    void ConfigureClocks(int sample_rate);  // moved to public for AudioService access
private:
    std::string i2c_dev_path_;
    uint8_t  i2c_addr_ = ES8311_I2C_ADDR_DEFAULT;
    int      i2c_fd_   = -1;
    std::mutex i2c_mutex_;

    bool OpenI2c(void);
    void CloseI2c(void);

    /* I2C transfer helper: write reg + value, or write reg then read */
    bool I2cWriteRead(uint8_t reg, uint8_t* read_val);
    bool I2cWrite(uint8_t reg, uint8_t value);

    /* Apply current volume / gain / mute to codec registers */
    void ApplyVolumeRegisters(void);
    void ApplyMuteRegisters(void);

    /* Set up clock tree for the configured sample rate */
};

#endif /* _ES8311_AUDIO_CODEC_H */
