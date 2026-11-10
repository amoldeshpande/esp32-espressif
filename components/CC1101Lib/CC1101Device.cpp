// Copyright (C) 2024 Amol Deshpande
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <climits>
#include <cmath>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <format>
#include "CC1101Device.h"
#include "CC1101Lib.h"
#include "SpiMaster.h"

static const char *TAG = "CC1101Device";


namespace TI_CC1101
{
    // MDMCFG4 has two-bit bandwidth fields above the four-bit data rate exponent.
    static constexpr byte     kBandwidthFieldMask     = 0x03;
    static constexpr unsigned kBandwidthMantissaShift = 4;
    static constexpr byte     kDataRateExponentMask   = (1U << kBandwidthMantissaShift) - 1;
    static constexpr unsigned kHertzPerKHz            = 1'000;

    struct RxModemPreset
    {
        byte rateExponent;
        byte rateMantissa;
        byte bitSync;
        byte gainControl2;
        byte gainControl1;
        byte gainControl0;
        byte test2;
        byte test1;
    };

    // The original asynchronous modem settings from SmartRF Studio.
    static constexpr RxModemPreset kSmartRfRxModem = {
        .rateExponent = 5,
        .rateMantissa = 0x83, // About 1.2 kBaud at 26 MHz.
        .bitSync      = 0x6C, // Clock recovery starts at 2KI and 3KP, with data rate offset compensation disabled.
        .gainControl2 = 0x03, // 33 dB target, with all analog and digital gain settings available.
        .gainControl1 = 0x40, // Reduce LNA gain first. Carrier sense is at the gain target, with no relative threshold.
        .gainControl0 = 0x91, // Medium hysteresis, 16 samples after a gain change, 8 dB OOK threshold, no gain freeze.
        .test2        = 0x88, // Original CC1101 analog settings for the wide filter.
        .test1        = 0x31,
    };

    // SmartRC v2.5.7 asynchronous modem settings used by ESPSomfy-RTS.
    static constexpr RxModemPreset kSomfyRxModem = {
        .rateExponent = 7,
        .rateMantissa = 0x93, // About 5 kBaud at 26 MHz.
        .bitSync      = 0x1C, // Clock recovery starts at KI and 2KP, with data rate offset compensation disabled.
        .gainControl2 = 0xC7, // 42 dB target, all LNA settings, three highest digital gain settings disabled.
        .gainControl1 = 0x00, // Reduce LNA2 gain first. Carrier sense is at the gain target, with no relative threshold.
        .gainControl0 = 0xB2, // Medium hysteresis, 32 samples after a gain change, 12 dB OOK threshold, no gain freeze.
        .test2        = 0x81, // CC1101 analog settings for a receive bandwidth below 325 kHz.
        .test1        = 0x35,
    };

    static constexpr byte     kMarcStateMask       = 0x1F; // MARCSTATE[4:0] is the radio state.
    static constexpr byte     kMarcStateIdle       = 0x01;
    static constexpr byte     kMarcStateRx         = 0x0D;
    // Allow up to 3 ms for the radio to enter IDLE or to calibrate and enter RX.
    static constexpr unsigned kStateTimeoutUs      = 3'000;
    static constexpr unsigned kStatePollIntervalUs = 100;

    static double channelFilterBandwidthKHz(float oscillatorFrequencyHz, byte exponent, byte mantissa)
    {
        // The CC1101 formula divides the crystal rate by 8 * (4 + mantissa) * 2^exponent.
        return double(oscillatorFrequencyHz) / (kHertzPerKHz * 8 * (4 + mantissa) * (1U << exponent));
    }

    void CC110DeviceConfig::DebugDump()
    {
#if _DEBUG
        ESP_LOGD(TAG, "Device Config");

        ESP_LOGD(TAG, "\tTXPin = %d", TxPin);
        ESP_LOGD(TAG, "\tRXPin = %d", RxPin);
        ESP_LOGD(TAG, "\tOscillatorFrequencyMHz = " FLOAT_FMT , OscillatorFrequencyMHz);
        ESP_LOGD(TAG, "\tCarrierFrequencyMHz = " FLOAT_FMT, CarrierFrequencyMHz);
        ESP_LOGD(TAG, "\tReceiveFilterBandwidthKHz = " FLOAT_FMT, ReceiveFilterBandwidthKHz);
        ESP_LOGD(TAG, "\tFrequencyDeviationKhz = " FLOAT_FMT, FrequencyDeviationKhz);
        ESP_LOGD(TAG, "\tTxPower = %d ", TxPower);
        ESP_LOGD(TAG, "\tModulationType = %d", (int)Modulation);
        ESP_LOGD(TAG, "\tManchesterEnabled = %s", ManchesterEnabled ? "true" : "false");
        ESP_LOGD(TAG, "\tPacketFormat = %d", (int)PacketFmt);
        ESP_LOGD(TAG, "\tPacketLengthConfig = %d", (int)PacketLengthCfg);
        ESP_LOGD(TAG, "\tDisableDCFilter = %s", DisableDCFilter ? "true" : "false");
        ESP_LOGD(TAG, "\tEnableCRC = %s", EnableCRC ? "true" : "false");
        ESP_LOGD(TAG, "\tEnableCRCAutoflush = %s", EnableCRCAutoflush ? "true" : "false");
        ESP_LOGD(TAG, "\tSyncWordQualifierMode = %d", (int)SyncMode);
        ESP_LOGD(TAG, "\tAddressCheckConfiguration = %d", (int)AddressCheck);
        ESP_LOGD(TAG, "\tEnableAppendStatusBytes = %s", EnableAppendStatusBytes ? "true" : "false");
#endif
    }
    CC1101Device::CC1101Device()
    {
    }

    CC1101Device::~CC1101Device()
    {
    }

    bool CC1101Device::Init(std::shared_ptr<SpiMaster> &spiMaster, CC110DeviceConfig &deviceConfig)
    {
        bool bRet = true;
        byte partNumber;
        byte chipVersion;

        m_spiMaster    = spiMaster;
        m_deviceConfig = deviceConfig;

        m_deviceConfig.DebugDump();
        if (m_deviceConfig.OscillatorFrequencyMHz != 0)
        {
            m_oscillatorFrequencyHz = m_deviceConfig.OscillatorFrequencyMHz * 1'000'000;
        }
        m_ISRQueueHandle = deviceConfig.InterruptQueue;

        CBRA(m_spiMaster->ResetDevice());
        regConfig();
        configure();

        delayMilliseconds(1);

        DumpRegisters();

        partNumber  = readRegister(CC1101_CONFIG::PARTNUM);
        chipVersion = readRegister(CC1101_CONFIG::VERSION);

        ESP_LOGI(TAG, "Part Number " HEX_FMT " and chip version " HEX_FMT, partNumber, chipVersion);
        if (partNumber != kPartNumber || chipVersion != kChipVersion)
        {
            ESP_LOGE(TAG, "Unexpected CC1101 part number or version");
        }
        CBRA(partNumber == kPartNumber && chipVersion == kChipVersion);

        if (m_deviceConfig.PacketFmt == PacketFormat::AsyncSerialMode)
        {
            CBRA(verifyAsyncConfiguration());
        }

    Error:
        return bRet;
    }

    bool CC1101Device::verifyAsyncConfiguration()
    {
        bool               bRet                           = true;
        // FREQ2:FREQ0 is a three-byte word. PKT_FORMAT is PKTCTRL0[5:4].

        const byte         gdo2                           = readRegister(CC1101_CONFIG::IOCFG2);
        const byte         packet                         = readRegister(CC1101_CONFIG::PKTCTRL0);
        const byte         frequencyHigh                  = readRegister(CC1101_CONFIG::FREQ2);
        const byte         frequencyMid                   = readRegister(CC1101_CONFIG::FREQ1);
        const byte         frequencyLow                   = readRegister(CC1101_CONFIG::FREQ0);
        const uint32_t     frequencyWord                  = (uint32_t(frequencyHigh) << (2 * CHAR_BIT)) |
                                       (uint32_t(frequencyMid) << CHAR_BIT) | frequencyLow;
        const double actualMHz       = frequencyWord * (m_oscillatorFrequencyHz / kFrequencyDivisor) / 1'000'000;
        const byte   modem           = readRegister(CC1101_CONFIG::MDMCFG4);
        const byte   rateMantissa    = readRegister(CC1101_CONFIG::MDMCFG3);
        const int8_t offset          = static_cast<int8_t>(readRegister(CC1101_CONFIG::FSCTRL0));
        // MDMCFG4[7:6] is the bandwidth exponent.
        const double actualBandwidth = channelFilterBandwidthKHz(m_oscillatorFrequencyHz,
                                                                 modem >> 6,
                                                                 (modem >> kBandwidthMantissaShift) & kBandwidthFieldMask);
        // MDMCFG3 omits the mantissa high bit; the rate divisor is 2^28.
        const double rateKBaud       = double(m_oscillatorFrequencyHz) * ((1U << CHAR_BIT) + rateMantissa) *
                                 (1U << (modem & kDataRateExponentMask)) / (double(kHertzPerKHz) * (1U << 28));
        ESP_LOGI(TAG, "Readback: %.3f MHz, %.2f kHz BW, IOCFG2=0x%02X, PKTCTRL0=0x%02X",
                 actualMHz, actualBandwidth, gdo2, packet);
        // FSCTRL0 uses synthesizer steps of f_XOSC / 2^14.
        ESP_LOGI(TAG, "RX modem: offset=%+.1f kHz, rate=%.2f kBaud, AGC=%02X/%02X/%02X",
                 double(offset) * m_oscillatorFrequencyHz / (double((1U << 14)) * kHertzPerKHz), rateKBaud,
                 readRegister(CC1101_CONFIG::AGCCTRL2), readRegister(CC1101_CONFIG::AGCCTRL1),
                 readRegister(CC1101_CONFIG::AGCCTRL0));
        // Allow 1 kHz of rounding in the frequency readback.
        if (gdo2 != byte(ConfigValues::GDx_CFG_LowerSixBits::SERIAL_DATA_OUTPUT) || ((packet >> 4) & 0x03) != byte(PacketFormat::AsyncSerialMode) || offset != m_deviceConfig.FrequencyOffsetSteps || std::abs(actualMHz - m_carrierFrequencyMHz) > 0.001)
        {
            ESP_LOGE(TAG, "CC1101 configuration readback failed");
            bRet = false;
        }
        return bRet;
    }

    void CC1101Device::Reset()
    {
        m_spiMaster->ResetDevice();
    }
    bool CC1101Device::BeginReceive()
    {
        bool          bRet    = true;
        gpio_config_t gpioConfig{};
        esp_err_t     result;

        gpioConfig.intr_type    = m_deviceConfig.PacketFmt == PacketFormat::AsyncSerialMode
                                      ? GPIO_INTR_ANYEDGE
                                      : GPIO_INTR_POSEDGE;
        gpioConfig.pin_bit_mask = 1ULL << m_deviceConfig.RxPin;
        gpioConfig.mode         = GPIO_MODE_INPUT;
        gpioConfig.pull_up_en   = GPIO_PULLUP_DISABLE;
        gpioConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;

        ESP_LOGD(TAG, "%s gpioconfig pin mask is " HEX_FMT, __FUNCTION__, (int)gpioConfig.pin_bit_mask);
        CERA(gpio_config(&gpioConfig));

        result = gpio_install_isr_service(0);
        if (result != ESP_ERR_INVALID_STATE)
        {
            CERA(result);
        }

        CERA(gpio_isr_handler_add(m_deviceConfig.RxPin, gpioISR, this));

        // Turn on the radio for receive
        if (!enableReceiveMode())
        {
            gpio_isr_handler_remove(m_deviceConfig.RxPin);
            bRet = false;
        }

        //esp_intr_dump(NULL);
    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return bRet;
    }
    void CC1101Device::Update()
    {
        uint32_t ignore = 0;
        // Bound each Update() call to 100 ms when the radio is idle.
        if (xQueueReceive(m_ISRQueueHandle, &ignore, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            ESP_LOGD(TAG, "interrupt received ");
            //gpio level % d ", 0);//gpio_get_level(m_deviceConfig.RxPin));
        }
    }

    int CC1101Device::ReadRssiDbm()
    {
        const int8_t  raw              = static_cast<int8_t>(readRegister(CC1101_CONFIG::RSSI));
        // The signed RSSI register uses half-dB steps with a typical 74 dB offset.
        const int     rssiDbm          = raw / 2 - 74;
        return rssiDbm;
    }

    bool CC1101Device::RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps)
    {
        bool       bRet   = true;
        byte       status = 0;
        bool       ready  = false;
        const byte offset = static_cast<byte>(offsetSteps);

        CERA(gpio_intr_disable(m_deviceConfig.RxPin));

        if (m_spiMaster->WriteByte(CC1101_CONFIG::SIDLE, status))
        {
            for (unsigned elapsedUs = 0; elapsedUs < kStateTimeoutUs; elapsedUs += kStatePollIntervalUs)
            {
                if ((readRegister(CC1101_CONFIG::MARCSTATE) & kMarcStateMask) == kMarcStateIdle)
                {
                    SetFrequencyMHz(frequencyMHz);
                    SetReceiveChannelFilterBandwidth(bandwidthKHz);
                    ready = m_spiMaster->WriteByteToAddress(CC1101_CONFIG::FSCTRL0, offset, status);
                    if (ready)
                    {
                        ready = readRegister(CC1101_CONFIG::FSCTRL0) == offset;
                    }
                    if (ready)
                    {
                        ready = enableReceiveMode();
                    }
                    break;
                }
                delayMicroseconds(kStatePollIntervalUs);
            }
        }

        xQueueReset(m_ISRQueueHandle);
        bRet = gpio_intr_enable(m_deviceConfig.RxPin) == ESP_OK && ready;

    Error:
        return bRet;
    }
    // Page 75 of TI Datasheet
    // Frequency is a 24-bit word set via FREQ0,FREQ1 and FREQ2 registers
    //  (but upper bits 22 and 23 of FREQ2 are always 0)
    //
    // Frequency is calculated as:
    //
    //        (oscillator freq / divisor) * FREQ[23:0]
    //
    // In the default case 26 MHz / 131072 = 198.36 Hz  is the multiplier, stored as frequencyIncrement variable
    //
    void CC1101Device::SetFrequencyMHz(float frequencyMHz)
    {
        byte statusCode = 0;
        float frequencyIncrement = m_oscillatorFrequencyHz / (float)kFrequencyDivisor;

        if (frequencyMHz < 300)
        {
            frequencyMHz = 300;
        }
        if (frequencyMHz > 928)
        {
            frequencyMHz = 928;
        }
        uint frequencySteps = (uint)(frequencyMHz * 1'000'000 / frequencyIncrement);

        byte freq0 = (byte)(frequencySteps & 0x000000FF);
        byte freq1 = (byte)((frequencySteps & 0x0000FF00) >> 8);
        byte freq2 = (byte)((frequencySteps & 0x003F0000) >> 16);

        ESP_LOGD(TAG, "SetFrequency() -> freq0=" HEX_FMT ", freq1=" HEX_FMT ", freq2 = " HEX_FMT ", increment= " FLOAT_FMT ", result = " FLOAT_FMT ", expected" FLOAT_FMT " MHz", freq0, freq1, freq2, frequencyIncrement,
                 frequencyIncrement * (float)(((int)freq2 << 16) | ((int)freq1 << 8) | (int)freq0), frequencyMHz);

        statusCode = writeRegister(CC1101_CONFIG::FREQ0, freq0);
        handleCommonStatusCodes(statusCode, false);
        statusCode = writeRegister(CC1101_CONFIG::FREQ1, freq1);
        handleCommonStatusCodes(statusCode, false);
        statusCode = writeRegister(CC1101_CONFIG::FREQ2, freq2);
        handleCommonStatusCodes(statusCode, false);

        m_carrierFrequencyMHz = frequencyMHz;
        ESP_LOGI(TAG, "m_carrierFrequencyMHz is now " FLOAT_FMT, m_carrierFrequencyMHz);
    }
    // Choose the nearest channel bandwidth while preserving MDMCFG4's data-rate exponent.
    void CC1101Device::SetReceiveChannelFilterBandwidth(float bandwidthKHz)
    {
        byte statusCode = 0;
        byte modemCFG   = readRegister(CC1101_CONFIG::MDMCFG4);
        byte  bestSetting  = 0;
        float bestError    = INFINITY;
        float candidate;
        float error;
        byte  result;

        ESP_LOGD(TAG, "%s: mdmcfg4 = " HEX_FMT, __FUNCTION__, modemCFG);
        // Try all 16 combinations: each setting packs a two-bit exponent above a two-bit mantissa.
        for (byte setting = 0; setting < 16; ++setting)
        {
            candidate = static_cast<float>(channelFilterBandwidthKHz(m_oscillatorFrequencyHz, setting >> 2, setting & kBandwidthFieldMask));
            error     = std::abs(candidate - bandwidthKHz);
            if (error < bestError)
            {
                bestError   = error;
                bestSetting = setting;
            }
        }
        result = static_cast<byte>((bestSetting << kBandwidthMantissaShift) | (modemCFG & kDataRateExponentMask));

        ESP_LOGI(TAG, "%s: requested bandwidth " FLOAT_FMT " kHz, MDMCFG4=" HEX_FMT, __FUNCTION__, bandwidthKHz, result);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG4, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// @brief Set the DataRate Exponent in MDMCFG4 and Mantissa in MDMCFG3
    /// @param Exponent
    /// @param Mantissa
    void CC1101Device::SetDataRate(byte Exponent, byte Mantissa)
    {
        byte statusCode = 0;
        byte modem4CFG  = readRegister(CC1101_CONFIG::MDMCFG4);

        byte result = ((modem4CFG & ~0x0F) | Exponent);

        ESP_LOGD(TAG, "%s: DataRate expected -> " FLOAT_FMT, __FUNCTION__, (float)(256 + Mantissa) * (1 << Exponent) / (float)(1 << 28) * m_oscillatorFrequencyHz);

        ESP_LOGD(TAG, "%s: Writing to MDMCFG4 -> " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG4, result);
        handleCommonStatusCodes(statusCode, false);
        ESP_LOGD(TAG, "%s: Writing to MDMCFG3 -> " HEX_FMT, __FUNCTION__, Mantissa);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG3, Mantissa);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Sets modem deviation allowed, per page 79 of TI Datasheet
    /// </summary>
    /// <param name="deviationKhz"></param>
    //
    // For 2-MSK,GFSK or 4-FSK
    // Formula to compute the deviation from the byte value of DEVIATN register:
    //
    //            deviation =  (Crystal Frequency/2^17)*(8 + Mantissa)*2^Exponent
    //
    // High bit is not used
    // Bits 4-6 are Exponent
    // Bit 3 is unused
    // Bit 0-2 are Mantissa
    void CC1101Device::SetModemDeviation(float deviationKHz)
    {
        byte statusCode = 0;
        // TODO this function seems broken. debug it

        float constantPart            = m_oscillatorFrequencyHz / (1 << 17);
        float allowableDeviationKHz[] = {
            constantPart * (8 * 1),
            constantPart / (8 * 2),
            constantPart / (8 * 4),
            constantPart / (8 * 8),
            constantPart / (8 * 16),
            constantPart / (8 * 32),
            constantPart / (8 * 64),
            constantPart / (8 * 128),
            constantPart * (9 * 1),
            constantPart / (9 * 2),
            constantPart / (9 * 4),
            constantPart / (9 * 8),
            constantPart / (9 * 16),
            constantPart / (9 * 32),
            constantPart / (9 * 64),
            constantPart / (9 * 128),
            constantPart * (10 * 1),
            constantPart / (10 * 2),
            constantPart / (10 * 4),
            constantPart / (10 * 8),
            constantPart / (10 * 16),
            constantPart / (10 * 32),
            constantPart / (10 * 64),
            constantPart / (10 * 128),
            constantPart * (11 * 1),
            constantPart / (11 * 2),
            constantPart / (11 * 4),
            constantPart / (11 * 8),
            constantPart / (11 * 16),
            constantPart / (11 * 32),
            constantPart / (11 * 64),
            constantPart / (11 * 128),
            constantPart * (12 * 1),
            constantPart / (12 * 2),
            constantPart / (12 * 4),
            constantPart / (12 * 8),
            constantPart / (12 * 16),
            constantPart / (12 * 32),
            constantPart / (12 * 64),
            constantPart / (12 * 128),
            constantPart * (13 * 1),
            constantPart / (13 * 2),
            constantPart / (13 * 4),
            constantPart / (13 * 8),
            constantPart / (13 * 16),
            constantPart / (13 * 32),
            constantPart / (13 * 64),
            constantPart / (13 * 128),
            constantPart * (14 * 1),
            constantPart / (14 * 2),
            constantPart / (14 * 4),
            constantPart / (14 * 8),
            constantPart / (14 * 16),
            constantPart / (14 * 32),
            constantPart / (14 * 64),
            constantPart / (14 * 128),
            constantPart * (15 * 1),
            constantPart / (15 * 2),
            constantPart / (15 * 4),
            constantPart / (15 * 8),
            constantPart / (15 * 16),
            constantPart / (15 * 32),
            constantPart / (15 * 64),
            constantPart / (15 * 128),
        };
        // default to lowest allowed
        byte Exponent = 0;
        byte Mantissa = 0;
        // scan the array, from index 1
        for (int i = 1; i < 49; i++)
        {
            // if the bandwidth setting is between two values, choose the closer one
            if (deviationKHz > allowableDeviationKHz[i])
            {
                int index = i - 1;

                float diffFromLarger  = allowableDeviationKHz[i - 1] - deviationKHz;
                float diffFromSmaller = deviationKHz - allowableDeviationKHz[i];

                if (diffFromLarger > diffFromSmaller)
                {
                    index = i;
                }
                Mantissa = (byte)(index / 7);            // row in array above
                Exponent = (byte)(index - Mantissa * 7); // mod
                break;
            }
        }
        byte result = (byte)(Exponent << 4 | Mantissa);
        ESP_LOGD(TAG, "%s: setting result=" HEX_FMT ", Mantissa=" HEX_FMT ",Exponent=" HEX_FMT , __FUNCTION__, result, Mantissa, Exponent);
        statusCode = writeRegister(CC1101_CONFIG::DEVIATN, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Set output power level
    /// </summary>
    /// <param name="outputPower"></param>
    //
    // Page 59 of TI Datasheet
    void CC1101Device::SetOutputPower(int outputPower)
    {
        byte        paSetting;
        const byte *currentTable = nullptr;

        // Page of datasheet indicates that the operational frequency bands are 300-348,387-464 and 779-928
        if (m_carrierFrequencyMHz <= 348)
        {
            currentTable     = ConfigValues::PATABLE_315_SETTINGS;
            paSetting        = getMultiLayerInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_315_SETTINGS));
            m_currentPATable = PATables::PA_315;
            ESP_LOGD(TAG, "%s: patable is 315 for freq " FLOAT_FMT "; setting is " HEX_FMT, __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        else if (m_carrierFrequencyMHz <= 464)
        {
            currentTable     = ConfigValues::PATABLE_433_SETTINGS;
            paSetting        = getMultiLayerInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_433_SETTINGS));
            m_currentPATable = PATables::PA_433;
            ESP_LOGD(TAG, "%s: patable is 433 for freq " FLOAT_FMT "; setting is " HEX_FMT, __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        // I'm not sure what to do about 868, so this is all a bit adhoc over 464 MHz. I suppose it depends on your
        // chip.
        else if (m_carrierFrequencyMHz <= 880)
        {
            currentTable     = ConfigValues::PATABLE_868_SETTINGS;
            paSetting        = getWireWoundInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_868_SETTINGS));
            m_currentPATable = PATables::PA_868;
            ESP_LOGD(TAG, "%s: patable is 868 for freq " FLOAT_FMT "; setting is " HEX_FMT, __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        else
        {
            currentTable     = ConfigValues::PATABLE_915_SETTINGS;
            paSetting        = getWireWoundInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_915_SETTINGS));
            m_currentPATable = PATables::PA_915;
            ESP_LOGD(TAG, "%s: patable is 915 for freq " FLOAT_FMT "; setting is " HEX_FMT, __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }

        // ASK always uses index 0 PATABLE to transmit a 0;
        if (m_deviceConfig.Modulation == ModulationType::ASK_OOK)
        {
            m_PATABLE[0] = 0;
            m_PATABLE[1] = paSetting;
            ESP_LOGD(TAG, "Modulation is ASK_OOK, patable should have " HEX_FMT " in index 1", paSetting);
        }
        else
        {
            m_PATABLE[0] = paSetting;
            m_PATABLE[1] = 0;
            ESP_LOGD(TAG, "Modulation is *not* ASK_OOK, patable should have " HEX_FMT " in index 0", paSetting);
        }
#if _DEBUG
        {
            byte patables[8];
            readBurstRegister(CC1101_CONFIG::PATABLE, patables, 8);
            std::string patableFmtStr;
            for (int i = 0; i < 8; i++)
            {
                patableFmtStr.append(std::format(" {:x} ", patables[i]));
            }
            ESP_LOGD(TAG, "PATABLE before: %s", patableFmtStr.c_str());
        }
#endif

        writeBurstRegister(CC1101_CONFIG::PATABLE, m_PATABLE, 8);

#if _DEBUG
        {
            byte patables[8];
            readBurstRegister(CC1101_CONFIG::PATABLE, patables, 8);
            std::string patableFmtStr;
            for (int i = 0; i < 8; i++)
            {
                patableFmtStr.append(std::format(" {:x} ", patables[i]));
            }
            ESP_LOGD(TAG, "PATABLE after: %s", patableFmtStr.c_str());
        }
#endif
    }
    //
    //  Page 77,89 of TI Datasheet
    //
    // Set Modulation mode in MDMCFG2 and point to the PATABLE entry in register FREND0
    //
    void CC1101Device::SetModulation(ModulationType modulationType)
    {
        byte statusCode     = 0;
        byte currentMDMCFG2 = readRegister(CC1101_CONFIG::MDMCFG2);
        byte currentFREND0  = readRegister(CC1101_CONFIG::FREND0);

        byte frend0, mdmcfg2 = currentMDMCFG2;

        // default to non-ASK_OOK, i.e, PATABLE[0] contains our only power level
        // For ASK_OOK we point to PATABLE[1], because ASK always uses PATABLE[0] for transmitting '0'
        frend0 = (byte)(currentFREND0 & 0b11110000); // clear the PA_POWER bits (pg 89)

        currentMDMCFG2 = (currentMDMCFG2 & ~0b01110000); // clear modulation bits
        switch (modulationType)
        {
            case ModulationType::FSK_2:
                mdmcfg2 = (byte)(currentMDMCFG2);
                break;
            case ModulationType::GFSK:
                mdmcfg2 = (byte)(currentMDMCFG2 | 0b00010000); // 1 in bits 4-6
                break;
            case ModulationType::ASK_OOK:
                frend0  = (byte)(frend0 | 0b00000001);         // 1 in the low nibble
                mdmcfg2 = (byte)(currentMDMCFG2 | 0b00110000); // 011 in bits 4-6
                break;
            case ModulationType::FSK_4:
                mdmcfg2 = (byte)(currentMDMCFG2 | 0b01000000); // 100 in bits 4-6
                SetManchesterEncoding(false);                  // Page 43
                break;
            case ModulationType::MSK:
                mdmcfg2 = (byte)(currentMDMCFG2 | 0b01110000); // 111 in bits 4-6
                break;
            case ModulationType::INVALID_2:
            case ModulationType::INVALID_5:
            case ModulationType::INVALID_6:
                assert(false);
                break;
        }

        ESP_LOGD(TAG, "%s Setting MDMCFG2 " HEX_FMT, __FUNCTION__, mdmcfg2);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG2, mdmcfg2);
        handleCommonStatusCodes(statusCode, false);

        ESP_LOGD(TAG, "%s Setting FREND0 " HEX_FMT, __FUNCTION__, frend0);
        statusCode = writeRegister(CC1101_CONFIG::FREND0, frend0);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Sets or unsets Manchester encoding (Pg 77) in register MDMCFG2
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetManchesterEncoding(bool shouldEnable)
    {
        byte statusCode     = 0;
        byte currentMDMCFG2 = readRegister(CC1101_CONFIG::MDMCFG2);
        byte result         = (byte)(currentMDMCFG2 & 0b11110111);

        if (shouldEnable)
        {
            result |= 0b00001000;
        }
        else
        {
            result &= 0b11110111;
        }

        ESP_LOGD(TAG, "%s Setting MDMCFG2 " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG2, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Disable Digital DC blocking filter (Pg 77) in register MDMCFG2
    /// </summary>
    void CC1101Device::SetDigitalDCFilter(bool shouldDisable)
    {
        byte statusCode     = 0;
        byte currentMdmcfg2 = readRegister(CC1101_CONFIG::MDMCFG2);

        currentMdmcfg2 = (currentMdmcfg2 & 0b01111111);

        byte setting = (shouldDisable ? 0b10000000 : 0b00000000);

        ESP_LOGD(TAG, "%s Setting MDMCFG2 " HEX_FMT, __FUNCTION__, (byte)(currentMdmcfg2 | setting));
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG2, (byte)(currentMdmcfg2 | setting));
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Sets Sync Mode according to Page 77 in register MDMCFG2
    ///    Combined sync-word qualifier mode.
    ///       The values 0 (000) and 4 (100) disables preamble and sync word
    ///       transmission in TX and preamble and sync word detection in RX.
    ///       The values 1 (001), 2 (010), 5 (101) and 6 (110) enables 16-bit sync word
    ///       transmission in TX and 16-bits sync word detection in RX.Only 15 of 16 bits
    ///       need to match in RX when using setting 1 (001) or 5 (101). The values 3 (011)
    ///       and 7 (111) enables repeated sync word transmission in TX and 32-bits sync
    ///       word detection in RX(only 30 of 32 bits need to match).
    /// </summary>
    void CC1101Device::SetSyncMode(SyncWordQualifierMode syncMode)
    {
        byte statusCode     = 0;
        byte currentMdmcfg2 = readRegister(CC1101_CONFIG::MDMCFG2);
        byte result         = (byte)((currentMdmcfg2 & 0b11111000) | (int)syncMode);

        ESP_LOGD(TAG, "%s Setting MDMCFG2 " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG2, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Set Packet Format (Pg 74) in register PKTCTRL0
    /// </summary>
    /// <param name="packetFormat"></param>
    void CC1101Device::SetPacketFormat(PacketFormat packetFormat)
    {
        byte statusCode      = 0;
        byte currentPktCtrl0 = readRegister(CC1101_CONFIG::PKTCTRL0);

        byte result = (byte)(currentPktCtrl0 & 0b11001111);
        switch (packetFormat)
        {
            case PacketFormat::Normal:
                break;
            case PacketFormat::SynchronousSerialMode:
                result |= 0b00010000;
                break;
            case PacketFormat::RandomTxMode:
                result |= 0b00100000;
                break;
            case PacketFormat::AsyncSerialMode:
                result |= 0b00110000;
                break;
        }
        ESP_LOGD(TAG, "%s Setting PKTCTRL0 " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL0, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Set CRC for data (Pg 74) in register PKTCTRL0
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetCRC(bool shouldEnable)
    {
        byte statusCode      = 0;
        byte currentPktCtrl0 = readRegister(CC1101_CONFIG::PKTCTRL0);

        byte result = (byte)(currentPktCtrl0 & 0b11111011);
        if (shouldEnable)
        {
            result |= 0b00000100;
        }
        ESP_LOGD(TAG, "%s Setting PKTCTRL0 " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL0, result);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Set CRC Autoflush (Pg 73) in register PKTCTRL1
    /// Enable automatic flush of RX FIFO when CRC is not OK. This requires that
    /// only one packet is in the RXIFIFO and that packet length is limited to the RX FIFO size.
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetCRCAutoFlush(bool shouldEnable)
    {
        byte statusCode      = 0;
        byte currentPktCtrl1 = readRegister(CC1101_CONFIG::PKTCTRL1);
        currentPktCtrl1      = (byte)(currentPktCtrl1 & 0b11110111);
        if (shouldEnable)
        {
            currentPktCtrl1 |= 0b00001000;
        }
        ESP_LOGD(TAG, "%s Setting PKTCTRL1" HEX_FMT, __FUNCTION__, currentPktCtrl1);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL1, currentPktCtrl1);
        handleCommonStatusCodes(statusCode, false);
    }
    /// <summary>
    /// Set Address Check (Pg 73) in register PKTCTRL1
    /// </summary>
    /// <param name="addressCheckConfig"></param>
    void CC1101Device::SetAddressCheck(AddressCheckConfiguration addressCheckConfig)
    {
        byte statusCode      = 0;
        byte currentPktCtrl1 = readRegister(CC1101_CONFIG::PKTCTRL1);
        currentPktCtrl1      = (byte)((currentPktCtrl1 & 0b11111100) | (int)addressCheckConfig);

        ESP_LOGD(TAG, "%s Setting PKTCTRL1 " HEX_FMT, __FUNCTION__, currentPktCtrl1);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL1, currentPktCtrl1);
        handleCommonStatusCodes(statusCode, false);
    }

    /// @brief When enabled, two status bytes will be appended to the payload of the packet. The status bytes contain RSSI and LQI values, as well as CRC OK.
    /// @param shouldEnable
    void CC1101Device::SetAppendStatus(bool shouldEnable)
    {
        byte statusCode      = 0;
        byte currentPktCtrl1 = readRegister(CC1101_CONFIG::PKTCTRL1);
        byte result          = (byte)((currentPktCtrl1 & 0b11111011) | (shouldEnable ? 0b100 : 0b000));

        ESP_LOGD(TAG, "%s Setting PKTCTRL1 " HEX_FMT, __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL1, result);
        handleCommonStatusCodes(statusCode, false);
    }

    // Dumps in SmartRF Studio order so we can compare
    void CC1101Device::DumpRegisters()
    {
#if _DEBUG
        ESP_LOGD(TAG, "IOCFG2:              " HEX_FMT, readRegister(CC1101_CONFIG::IOCFG2));
        ESP_LOGD(TAG, "IOCFG1:              " HEX_FMT, readRegister(CC1101_CONFIG::IOCFG1));
        ESP_LOGD(TAG, "IOCFG0:              " HEX_FMT, readRegister(CC1101_CONFIG::IOCFG0));
        ESP_LOGD(TAG, "FIFOTHR:             " HEX_FMT, readRegister(CC1101_CONFIG::FIFOTHR));
        ESP_LOGD(TAG, "SYNC1:               " HEX_FMT, readRegister(CC1101_CONFIG::SYNC1));
        ESP_LOGD(TAG, "SYNC0:               " HEX_FMT, readRegister(CC1101_CONFIG::SYNC0));
        ESP_LOGD(TAG, "PKTLEN:              " HEX_FMT, readRegister(CC1101_CONFIG::PKTLEN));
        ESP_LOGD(TAG, "PKTCTRL1:            " HEX_FMT, readRegister(CC1101_CONFIG::PKTCTRL1));
        ESP_LOGD(TAG, "PKTCTRL0:            " HEX_FMT, readRegister(CC1101_CONFIG::PKTCTRL0));
        ESP_LOGD(TAG, "ADDR:                " HEX_FMT, readRegister(CC1101_CONFIG::ADDR));
        ESP_LOGD(TAG, "CHANNR:              " HEX_FMT, readRegister(CC1101_CONFIG::CHANNR));
        ESP_LOGD(TAG, "FSCTRL1              " HEX_FMT, readRegister(CC1101_CONFIG::FSCTRL1));
        ESP_LOGD(TAG, "FSCTRL0:             " HEX_FMT, readRegister(CC1101_CONFIG::FSCTRL0));
        ESP_LOGD(TAG, "FREQ2:               " HEX_FMT, readRegister(CC1101_CONFIG::FREQ2));
        ESP_LOGD(TAG, "FREQ1:               " HEX_FMT, readRegister(CC1101_CONFIG::FREQ1));
        ESP_LOGD(TAG, "FREQ0:               " HEX_FMT, readRegister(CC1101_CONFIG::FREQ0));
        ESP_LOGD(TAG, "MDMCFG4:             " HEX_FMT, readRegister(CC1101_CONFIG::MDMCFG4));
        ESP_LOGD(TAG, "MDMCFG3:             " HEX_FMT, readRegister(CC1101_CONFIG::MDMCFG3));
        ESP_LOGD(TAG, "MDMCFG2:             " HEX_FMT, readRegister(CC1101_CONFIG::MDMCFG2));
        ESP_LOGD(TAG, "MDMCFG1:             " HEX_FMT, readRegister(CC1101_CONFIG::MDMCFG1));
        ESP_LOGD(TAG, "MDMCFG0:             " HEX_FMT, readRegister(CC1101_CONFIG::MDMCFG0));
        ESP_LOGD(TAG, "DEVIATN:             " HEX_FMT, readRegister(CC1101_CONFIG::DEVIATN));
        ESP_LOGD(TAG, "MCSM2:               " HEX_FMT, readRegister(CC1101_CONFIG::MCSM2));
        ESP_LOGD(TAG, "MCSM1:               " HEX_FMT, readRegister(CC1101_CONFIG::MCSM1));
        ESP_LOGD(TAG, "MCSM0:               " HEX_FMT, readRegister(CC1101_CONFIG::MCSM0));
        ESP_LOGD(TAG, "FOCCFG:              " HEX_FMT, readRegister(CC1101_CONFIG::FOCCFG));
        ESP_LOGD(TAG, "BSCFG:               " HEX_FMT, readRegister(CC1101_CONFIG::BSCFG));
        ESP_LOGD(TAG, "AGCCTRL2:            " HEX_FMT, readRegister(CC1101_CONFIG::AGCCTRL2));
        ESP_LOGD(TAG, "AGCCTRL1:            " HEX_FMT, readRegister(CC1101_CONFIG::AGCCTRL1));
        ESP_LOGD(TAG, "AGCCTRL0:            " HEX_FMT, readRegister(CC1101_CONFIG::AGCCTRL0));
        ESP_LOGD(TAG, "WOREVT1:             " HEX_FMT, readRegister(CC1101_CONFIG::WOREVT1));
        ESP_LOGD(TAG, "WOREVT0:             " HEX_FMT, readRegister(CC1101_CONFIG::WOREVT0));
        ESP_LOGD(TAG, "WORCTRL:             " HEX_FMT, readRegister(CC1101_CONFIG::WORCTRL));
        ESP_LOGD(TAG, "FREND1:              " HEX_FMT, readRegister(CC1101_CONFIG::FREND1));
        ESP_LOGD(TAG, "FREND0:              " HEX_FMT, readRegister(CC1101_CONFIG::FREND0));
        ESP_LOGD(TAG, "FSCAL3:              " HEX_FMT, readRegister(CC1101_CONFIG::FSCAL3));
        ESP_LOGD(TAG, "FSCAL2:              " HEX_FMT, readRegister(CC1101_CONFIG::FSCAL2));
        ESP_LOGD(TAG, "FSCAL1:              " HEX_FMT, readRegister(CC1101_CONFIG::FSCAL1));
        ESP_LOGD(TAG, "FSCAL0:              " HEX_FMT, readRegister(CC1101_CONFIG::FSCAL0));
        ESP_LOGD(TAG, "RCCTRL1:             " HEX_FMT, readRegister(CC1101_CONFIG::RCCTRL1));
        ESP_LOGD(TAG, "RCCTRL0:             " HEX_FMT, readRegister(CC1101_CONFIG::RCCTRL0));
        ESP_LOGD(TAG, "FSTEST:              " HEX_FMT, readRegister(CC1101_CONFIG::FSTEST));
        ESP_LOGD(TAG, "PTEST:               " HEX_FMT, readRegister(CC1101_CONFIG::PTEST));
        ESP_LOGD(TAG, "AGCTEST:             " HEX_FMT, readRegister(CC1101_CONFIG::AGCTEST));
        ESP_LOGD(TAG, "TEST2:               " HEX_FMT, readRegister(CC1101_CONFIG::TEST2));
        ESP_LOGD(TAG, "TEST1:               " HEX_FMT, readRegister(CC1101_CONFIG::TEST1));
        ESP_LOGD(TAG, "TEST0:               " HEX_FMT, readRegister(CC1101_CONFIG::TEST0));
        ESP_LOGD(TAG, "PARTNUM:             " HEX_FMT, readRegister(CC1101_CONFIG::PARTNUM));
        ESP_LOGD(TAG, "VERSION:             " HEX_FMT, readRegister(CC1101_CONFIG::VERSION));
        ESP_LOGD(TAG, "FREQEST:             " HEX_FMT, readRegister(CC1101_CONFIG::FREQEST));
        ESP_LOGD(TAG, "LQI:                 " HEX_FMT, readRegister(CC1101_CONFIG::LQI));
        ESP_LOGD(TAG, "RSSI:                " HEX_FMT, readRegister(CC1101_CONFIG::RSSI));
        ESP_LOGD(TAG, "MARCSTATE:           " HEX_FMT, readRegister(CC1101_CONFIG::MARCSTATE));
        ESP_LOGD(TAG, "WORTIME1:            " HEX_FMT, readRegister(CC1101_CONFIG::WORTIME1));
        ESP_LOGD(TAG, "WORTIME0:            " HEX_FMT, readRegister(CC1101_CONFIG::WORTIME0));
        ESP_LOGD(TAG, "PKTSTATUS:           " HEX_FMT, readRegister(CC1101_CONFIG::PKTSTATUS));
        ESP_LOGD(TAG, "VCO_VC_DAC:          " HEX_FMT, readRegister(CC1101_CONFIG::VCO_VC_DAC));
        ESP_LOGD(TAG, "TXBYTES:             " HEX_FMT, readRegister(CC1101_CONFIG::TXBYTES));
        ESP_LOGD(TAG, "RXBYTES:             " HEX_FMT, readRegister(CC1101_CONFIG::RXBYTES));
        byte patables[8];
        readBurstRegister(CC1101_CONFIG::PATABLE, patables, 8);
        ESP_LOGD(TAG, "PA_TABLE0:           " HEX_FMT, patables[0]);
        ESP_LOGD(TAG, "PA_TABLE1:           " HEX_FMT, patables[1]);
        ESP_LOGD(TAG, "PA_TABLE2:           " HEX_FMT, patables[2]);
        ESP_LOGD(TAG, "PA_TABLE3:           " HEX_FMT, patables[3]);
        ESP_LOGD(TAG, "PA_TABLE4:           " HEX_FMT, patables[4]);
        ESP_LOGD(TAG, "PA_TABLE5:           " HEX_FMT, patables[5]);
        ESP_LOGD(TAG, "PA_TABLE6:           " HEX_FMT, patables[6]);
        ESP_LOGD(TAG, "PA_TABLE7:           " HEX_FMT, patables[7]);
#endif
    }
    bool CC1101Device::enableReceiveMode()
    {
        bool bRet = false;
        // The status returned during SRX can still report the previous state.
        byte status = 0;
        byte state;
        if (m_spiMaster->WriteByte(CC1101_CONFIG::SRX, status))
        {
            for (unsigned elapsedUs = 0; elapsedUs < kStateTimeoutUs; elapsedUs += kStatePollIntervalUs)
            {
                state = readRegister(CC1101_CONFIG::MARCSTATE) & kMarcStateMask;
                if (state == kMarcStateRx)
                {
                    ESP_LOGI(TAG, "CC1101 entered RX (MARCSTATE=0x%02X)", state);
                    bRet = true;
                    break;
                }
                delayMicroseconds(kStatePollIntervalUs);
            }
            if (!bRet)
            {
                ESP_LOGE(TAG, "CC1101 did not enter RX (MARCSTATE=0x%02X)",
                         readRegister(CC1101_CONFIG::MARCSTATE) & kMarcStateMask);
            }
        }
        return bRet;
    }
    void CC1101Device::delayMilliseconds(int millis)
    {
        vTaskDelay(pdMS_TO_TICKS(millis));
    }
    byte CC1101Device::readRegister(byte address)
    {
        bool bRet  = true;
        byte value = 0;
        address &= 0b00111111; // clear R/W and burst bit
        if (address >= CC1101_CONFIG::PARTNUM && address <= CC1101_CONFIG::RCCTRL0_STATUS)
        {
            address |= kSpiBurstAccessBit;
        }
        address |= kSpiHeaderReadBit;

        CBRA(m_spiMaster->ReadRegister(address,value));

    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return value;
    }

    bool CC1101Device::readBurstRegister(byte address, byte *buffer, int len)
    {
        bool bRet = true;
        if (address >= CC1101_CONFIG::PARTNUM && address <= CC1101_CONFIG::RCCTRL0_STATUS)
        {
            ESP_LOGE(TAG, "Control registers cannot be read with burst access");
            return false;
        }
        address |= (kSpiBurstAccessBit | kSpiHeaderReadBit);

        CBRA(m_spiMaster->ReadBurstRegister(address,buffer,len));
    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return bRet;
    }
    void CC1101Device::readRXFIFO(int expectedCount)
    {
        byte resetStatus;
        byte status[2];
        byte scratch[255]; // absolute max of RXFIFO
        byte regVal = readRegister(CC1101_CONFIG::RXBYTES);

        byte avail = (regVal & kRxFifoByteCountMask);

        ESP_LOGD(TAG, "%s, expected %d, avail %d", __FUNCTION__, expectedCount, avail);
        readBurstRegister(CC1101_CONFIG::RXFIFO, scratch, avail);
        for (int i = 0; i < avail; i++)
        {
            ESP_LOGD(TAG, "%s buffer[%d]= " HEX_FMT, __FUNCTION__, i, scratch[i]);
        }
        if (m_deviceConfig.EnableAppendStatusBytes)
        {
            readBurstRegister(CC1101_CONFIG::RXFIFO, status, 2);
            ESP_LOGD(TAG, "%s status= {" HEX_FMT "," HEX_FMT "}", __FUNCTION__, status[0], status[1]);
        }
        if ((regVal & ~kRxFifoByteCountMask) != 0)
        {
            ESP_LOGW(TAG, "RX_FIFO overflow, sending reset");
            m_spiMaster->WriteByte(CC1101_CONFIG::SFRX, resetStatus);
            ESP_LOGW(TAG, "RX_FIFO overflow, new status " HEX_FMT, resetStatus);
        }
    }

    void CC1101Device::setMDMCFG2()
    {
        SetModulation(m_deviceConfig.Modulation);
        SetDigitalDCFilter(m_deviceConfig.DisableDCFilter);
        SetManchesterEncoding(m_deviceConfig.ManchesterEnabled);
        SetSyncMode(m_deviceConfig.SyncMode);
    }

    byte CC1101Device::writeRegister(byte address, byte value)
    {
        byte statusCode = 0;

        m_spiMaster->WriteByteToAddress(address, value, statusCode);
        return statusCode;
    }

    void CC1101Device::writeBurstRegister(byte address, byte *values, int valueLen)
    {
        byte statusCode = 0;
        m_spiMaster->WriteBytesToAddress(address | kSpiBurstAccessBit,values, valueLen, statusCode);
        ESP_LOGD(TAG, "Write values to address " HEX_FMT " statusCode " HEX_FMT, address, statusCode);
    }

    byte CC1101Device::sendStrobe(byte strobeCmd)
    {
        byte outStatus = 0;
        m_spiMaster->WriteByte(strobeCmd, outStatus);

        return outStatus;
    }

    byte CC1101Device::getMultiLayerInductorPower(int outputPower, const byte *currentTable, int currentTableLen)
    {
        byte paSetting;

        assert(currentTableLen == 8);
        if (outputPower <= -30)
        {
            paSetting = currentTable[0];
        }
        else if (outputPower <= -20)
        {
            paSetting = currentTable[1];
        }
        else if (outputPower <= -15)
        {
            paSetting = currentTable[2];
        }
        else if (outputPower <= -10)
        {
            paSetting = currentTable[3];
        }
        else if (outputPower <= 0)
        {
            paSetting = currentTable[4];
        }
        else if (outputPower <= 5)
        {
            paSetting = currentTable[5];
        }
        else if (outputPower <= 7)
        {
            paSetting = currentTable[6];
        }
        else
        {
            paSetting = currentTable[7];
        }
        return paSetting;
    }

    byte CC1101Device::getWireWoundInductorPower(int outputPower, const byte *currentTable, int currentTableLen)
    {
        byte paSetting;
        assert(currentTableLen == 10);
        if (outputPower <= -30)
        {
            paSetting = currentTable[0];
        }
        else if (outputPower <= -20)
        {
            paSetting = currentTable[1];
        }
        else if (outputPower <= -15)
        {
            paSetting = currentTable[2];
        }
        else if (outputPower <= -10)
        {
            paSetting = currentTable[3];
        }
        else if (outputPower <= -6)
        {
            paSetting = currentTable[4];
        }
        else if (outputPower <= 0)
        {
            paSetting = currentTable[5];
        }
        else if (outputPower <= 5)
        {
            paSetting = currentTable[6];
        }
        else if (outputPower <= 7)
        {
            paSetting = currentTable[7];
        }
        else if (outputPower <= 10)
        {
            paSetting = currentTable[8];
        }
        else
        {
            paSetting = currentTable[9];
        }
        return paSetting;
    }

    void CC1101Device::configure()
    {
        const RxModemPreset &preset     = m_deviceConfig.UseSomfyRtsRxProfile ? kSomfyRxModem : kSmartRfRxModem;
        byte statusCode = 0;
        byte pktctrlVal = (byte)(((int)m_deviceConfig.PacketFmt << 4 | (int)m_deviceConfig.PacketLengthCfg));

        // Set PKTCTRL0
        switch (m_deviceConfig.PacketFmt)
        {
            case PacketFormat::Normal:
                handleCommonStatusCodes(statusCode, false);
                statusCode = writeRegister(CC1101_CONFIG::IOCFG0, (byte)ConfigValues::GDx_CFG_LowerSixBits::RX_FIFO_ABOVE_THRESHOLD);
                handleCommonStatusCodes(statusCode, false);
                statusCode = writeRegister(CC1101_CONFIG::IOCFG2, (byte)ConfigValues::GDx_CFG_LowerSixBits::CHIP_RDY_N);
                handleCommonStatusCodes(statusCode, false);
                ESP_LOGD(TAG, "%s: Writing PKTCTRL0 " HEX_FMT " for Packet format %d", __FUNCTION__, pktctrlVal, (int)m_deviceConfig.PacketFmt);
                statusCode = writeRegister(CC1101_CONFIG::PKTCTRL0, pktctrlVal);

                // why?
                SetDataRate(11, 0xF8);
                break;
            case PacketFormat::AsyncSerialMode:
                {
                    statusCode = writeRegister(CC1101_CONFIG::IOCFG0, (byte)ConfigValues::GDx_CFG_LowerSixBits::SERIAL_DATA_OUTPUT);
                    handleCommonStatusCodes(statusCode, false);
                    statusCode = writeRegister(CC1101_CONFIG::IOCFG2, (byte)ConfigValues::GDx_CFG_LowerSixBits::SERIAL_DATA_OUTPUT);
                    handleCommonStatusCodes(statusCode, false);
                    ESP_LOGD(TAG, "%s: Writing PKTCTRL0 " HEX_FMT " for Packet format " HEX_FMT " length cfg " HEX_FMT, __FUNCTION__, pktctrlVal, (int)m_deviceConfig.PacketFmt, (int)m_deviceConfig.PacketLengthCfg);
                    statusCode = writeRegister(CC1101_CONFIG::PKTCTRL0, pktctrlVal);
                    handleCommonStatusCodes(statusCode, false);

                    SetDataRate(preset.rateExponent, preset.rateMantissa);
                }
                break;
            default:
                ESP_LOGW(TAG, "Unhandled Rx/Tx Packet format");
                break;
        }

        SetFrequencyMHz(m_deviceConfig.CarrierFrequencyMHz);
        SetReceiveChannelFilterBandwidth(m_deviceConfig.ReceiveFilterBandwidthKHz);

        // For this setting the DEVIATN register has no effect.
        if (m_deviceConfig.Modulation != ModulationType::ASK_OOK)
        {
            SetModemDeviation(m_deviceConfig.FrequencyDeviationKhz);
        }

        SetOutputPower(m_deviceConfig.TxPower);

        // Set MDMCFG2,FREND0
        setMDMCFG2();

        SetCRCAutoFlush(m_deviceConfig.EnableCRCAutoflush);
        SetAddressCheck(m_deviceConfig.AddressCheck);
        SetAppendStatus(m_deviceConfig.EnableAppendStatusBytes);

    }
    // Below are from SmartRF Studio
    void CC1101Device::regConfig()
    {
        byte returnValue = 0;
        const bool somfyRts    = m_deviceConfig.UseSomfyRtsRxProfile &&
                              m_deviceConfig.PacketFmt == PacketFormat::AsyncSerialMode;
        const RxModemPreset &preset = somfyRts ? kSomfyRxModem : kSmartRfRxModem;

        returnValue = writeRegister(CC1101_CONFIG::FSCTRL1, 0x06);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSCTRL0, static_cast<byte>(m_deviceConfig.FrequencyOffsetSteps));
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MDMCFG0, 0xF8);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MDMCFG1, 0x0);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::CHANNR, 0);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::DEVIATN, 0x15);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FREND1, 0x56);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MCSM0, 0x18);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FOCCFG, 0x16);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::WORCTRL, 0xFB);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::BSCFG, preset.bitSync);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL2, preset.gainControl2);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL1, preset.gainControl1);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL0, preset.gainControl0);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSCAL3, 0xE9);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSCAL2, 0x2A);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSCAL1, 0x00);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSCAL0, 0x1F);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FSTEST, 0x59);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::TEST2, preset.test2);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::TEST1, preset.test1);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FIFOTHR, 0x07);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::TEST0, 0x09);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::PKTCTRL1, 0x04);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::ADDR, 0x00);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::PKTLEN, 0xFF);
        handleCommonStatusCodes(returnValue, false);
    }
    void CC1101Device::handleCommonStatusCodes(byte status, bool wasReadOperation)
    {
        byte                       fifoBytesAvail = status & 0x0F;
        byte                       statusBits     = status & 0b01110000;
        StatusByteStateMachineMode machineState   = static_cast<StatusByteStateMachineMode>(statusBits >> 4);

        if ((machineState == StatusByteStateMachineMode::FIFOOverflowRX) && (fifoBytesAvail > 0))
        {
            ESP_LOGD(TAG, "%s statusCode " HEX_FMT ", fifo bytes available " HEX_FMT , __FUNCTION__, status, fifoBytesAvail);
        }
        switch (machineState)
        {
            // RX_FIFO overflow
            case StatusByteStateMachineMode::FIFOOverflowRX:
                {
                    readRXFIFO(fifoBytesAvail);
                }
                break;
            case StatusByteStateMachineMode::FIFOOverflowTX:
                break;
            case StatusByteStateMachineMode::ReceiveMode:
                {
                    if ((fifoBytesAvail > 0) && wasReadOperation)
                    {
                        readRXFIFO(fifoBytesAvail);
                    }
                }
                break;
            case StatusByteStateMachineMode::IDLE:
                break;
            default:
                ESP_LOGD(TAG, "%s status code not handled was " HEX_FMT, __FUNCTION__, status);
                break;
        }
    }
    void IRAM_ATTR CC1101Device::gpioISR(void *thisPtr)
    {
        CC1101Device  *device    = static_cast<CC1101Device *>(thisPtr);
        const uint32_t timestamp = static_cast<uint32_t>(esp_timer_get_time());
        BaseType_t     taskWoken = pdFALSE;
        xQueueSendFromISR(device->m_ISRQueueHandle, &timestamp, &taskWoken);
        if (taskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }

} // namespace TI_CC1101
