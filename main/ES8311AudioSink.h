#pragma once

#include <cstdint>
#include <cstdlib>
#include "AudioSink.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

// GPIO assignments for Waveshare ESP32-S3-LCD-1.54
#define I2S_MCLK_PIN    8
#define I2S_BCK_PIN     9
#define I2S_WS_PIN      10
#define I2S_DOUT_PIN    12  // IO12 = DSDIN = DAC data input TO ES8311 (IO11 = ASDOUT = ADC output FROM ES8311)
#define I2C_SCL_PIN     41
#define I2C_SDA_PIN     42
#define PA_CTRL_PIN     7   // NS4150B power amplifier enable

// ES8311 7-bit I2C address (ADDR pin = GND → 0x18)
#define ES8311_ADDR     0x18

class ES8311AudioSink : public AudioSink {
public:
    ES8311AudioSink();
    ~ES8311AudioSink();

    void feedPCMFrames(const uint8_t* buffer, size_t bytes) override;
    void volumeChanged(uint16_t volume) override;
    bool setParams(uint32_t sampleRate, uint8_t channelCount, uint8_t bitDepth) override;

private:
    i2s_chan_handle_t    tx_chan  = nullptr;
    i2c_master_bus_handle_t i2c_bus  = nullptr;
    i2c_master_dev_handle_t codec    = nullptr;

    void i2cWrite(uint8_t reg, uint8_t val);
    void codecInit();
    void i2sInit(uint32_t sampleRate);
};
