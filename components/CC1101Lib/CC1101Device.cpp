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

#include <cmath>
#include <driver/rtc_io.h>
#include <format>
#include "CC1101Device.h"
#include "CC1101Lib.h"
#include "SpiMaster.h"

static const char *TAG = "CC110Device";

namespace TI_CC1101
{
    void CC110DeviceConfig::DebugDump()
    {
#if _DEBUG
        ESP_LOGD(TAG, "Device Config");

        ESP_LOGD(TAG, "\tTXPin = %d", TxPin);
        ESP_LOGD(TAG, "\tRXPin = %d", RxPin);
        ESP_LOGD(TAG, "\tOscillatorFrequencyMHz = %g", OscillatorFrequencyMHz);
        ESP_LOGD(TAG, "\tCarrierFrequencyMHz = %g", CarrierFrequencyMHz);
        ESP_LOGD(TAG, "\tReceiveFilterBandwidthKHz = %g", ReceiveFilterBandwidthKHz);
        ESP_LOGD(TAG, "\tFrequencyDeviationKhz = %g", FrequencyDeviationKhz);
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
        bool bRet      = true;
        m_spiMaster    = spiMaster;
        m_deviceConfig = deviceConfig;

        m_deviceConfig.DebugDump();
        if (m_deviceConfig.OscillatorFrequencyMHz != 0)
        {
            m_oscillatorFrequencyHz = m_deviceConfig.OscillatorFrequencyMHz * 1'000'000;
        }
        m_frequencyIncrement = m_oscillatorFrequencyHz / kFrequencyDivisor;

        m_ISRQueueHandle = deviceConfig.InterruptQueue;

        Reset();
        regConfig();
        configure();

        delayMilliseconds(1);

        byte partNumber  = readRegister(CC1101_CONFIG::PARTNUM);
        byte chipVersion = readRegister(CC1101_CONFIG::VERSION);

        ESP_LOGI(TAG, "Part Number 0x%0X and chip version 0x%0X", partNumber, chipVersion);
        CBRA((partNumber == kPartNumber) && (chipVersion == kChipVersion));

    Error:
        return bRet;
    }

    void CC1101Device::Reset()
    {
        // See page 51, automatic POR (power-on reset)
        //
        /* * Set SCLK = 1 and SI = 0, to avoid potential problems with pin control mode (see Section 11.3).
         * Strobe CSn low / high.
         * Hold CSn low and then high for at least 40µs relative to pulling CSn low
         * Pull CSn low and wait for SO to go low (CHIP_RDYn).
         * Issue the SRES strobe on the SI line.
         * When SO goes low again, reset is complete and the chip is in the IDLE state.
         */
        bool bRet       = true;
        byte statusCode = 0;

        CERA(gpio_set_level(m_spiMaster->ClockPin(), 1));
        CERA(gpio_set_level(m_spiMaster->MosiPin(), 0));

        CBRA(lowerChipSelect());
        delayMilliseconds(1);
        CBRA(raiseChipSelect());
        delayMilliseconds(1);
        CBRA(lowerChipSelect());
        delayMilliseconds(1);
        CBRA(raiseChipSelect());

        waitForMisoLow();

        // This is a command strobe so we only need the lower 6 bits, i.e, the address.
        // See page 32, Section 10.4
        ESP_LOGI(TAG, "Sending reset");
        CBRA(m_spiMaster->WriteByte(CC1101_CONFIG::SRES, statusCode));

        waitForMisoLow();

        CBRA(lowerChipSelect());

    Error:
        if (!bRet)
        {
            ESP_LOGW(TAG, "CC1101 reset failed"); // reset failed
        }
    }
    bool CC1101Device::BeginReceive()
    {
        bool          bRet    = true;
        gpio_config_t gpioConfig;

        gpioConfig.intr_type    = GPIO_INTR_ANYEDGE;
        gpioConfig.pin_bit_mask = 1 << m_deviceConfig.RxPin;
        gpioConfig.mode         = GPIO_MODE_INPUT;
        gpioConfig.pull_up_en   = GPIO_PULLUP_DISABLE;
        gpioConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;

        ESP_LOGD(TAG, "%s gpioconfig pin mask is 0x%0X", __FUNCTION__, (int)gpioConfig.pin_bit_mask);
        CERA(gpio_config(&gpioConfig));

        CERA(gpio_install_isr_service(0));

        CERA(gpio_isr_handler_add(m_deviceConfig.RxPin, gpioISR, this));

        // Turn on the radio for receive
        ensureChipIdle();
        enableReceiveMode();

    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return false;
    }
    void CC1101Device::Update()
    {
        uint32_t ignore = 0;
        if (xQueueReceive(m_ISRQueueHandle, &ignore, 0) == pdTRUE)
        {
            ESP_LOGD(TAG, "interrupt received");
        }
        delayMilliseconds(100);
//        esp_intr_dump(NULL);
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

        if (frequencyMHz < 300)
        {
            frequencyMHz = 300;
        }
        if (frequencyMHz > 928)
        {
            frequencyMHz = 928;
        }
        uint frequencySteps = (uint)(frequencyMHz * 1'000'000 / m_frequencyIncrement);

        byte freq0 = (byte)(frequencySteps & 0x000000FF);
        byte freq1 = (byte)((frequencySteps & 0x0000FF00) >> 8);
        byte freq2 = (byte)((frequencySteps & 0x003F0000) >> 16);

        ESP_LOGD(TAG, "SetFrequency() -> freq0=0x%0X, freq1=0x%0X, freq2 = 0x%0X, result = %g, expected %g MHz", freq0, freq1, freq2,
                 m_frequencyIncrement * (float)(((int)freq2 << 16) | ((int)freq1 << 8) | (int)freq0), frequencyMHz);

        statusCode = writeRegister(CC1101_CONFIG::FREQ0, freq0);
        handleCommonStatusCodes(statusCode, false);
        statusCode = writeRegister(CC1101_CONFIG::FREQ1, freq1);
        handleCommonStatusCodes(statusCode, false);
        statusCode = writeRegister(CC1101_CONFIG::FREQ2, freq2);
        handleCommonStatusCodes(statusCode, false);

        m_carrierFrequencyMHz = frequencyMHz;
        ESP_LOGI(TAG, "m_carrierFrequencyMHz is now %g", m_carrierFrequencyMHz);
    }
    // Page 76 of TI Datasheet
    //
    // Filter B/W is in  top 4 bits of MDMCFG4
    //  Calculated by the formula :  BW =  ( Crystal frequency / (8 * (4 + Mantissa)* 2^Exponent) )
    //  Where:
    //     Exponent is in bits 6 and 7 (Two MSBs) of MDMCFG4
    //     Mantissa is bits 4 and 5
    //
    // The bottom 4 bits of MDMCFG4 are used to configure the Data rate exponent with the mantissa in the MDMCFG3 register
    //
    // Page 35 of the TI Datasheet gives a table of ranges for the 4x4 combinations of the 2-bit bitfields.
    //
    void CC1101Device::SetReceiveChannelFilterBandwidth(float bandwidthKHz)
    {
        byte statusCode = 0;
        byte modemCFG   = readRegister(CC1101_CONFIG::MDMCFG4);
        byte DataRate   = (byte)(modemCFG & 0x0F);

        ESP_LOGD(TAG, "%s: mdmcfg4 = 0x%0X", __FUNCTION__, modemCFG);

        // Page 35 table in an array. The exponent is the horizontal stride (4 columns corresponding to the bit values 00,01,10,11)
        // Mantissa is the vertical stride (4 rows corresponding to the bit values 00,01,10,11)
        float constantPart     = m_oscillatorFrequencyHz / 8 / 1000;
        float allowableBWKHz[] = {
            constantPart / (4 * 1), constantPart / (4 * 2), constantPart / (4 * 4), constantPart / (4 * 8),
            constantPart / (5 * 1), constantPart / (5 * 2), constantPart / (5 * 4), constantPart / (5 * 8),
            constantPart / (6 * 1), constantPart / (6 * 2), constantPart / (6 * 4), constantPart / (6 * 8),
            constantPart / (7 * 1), constantPart / (7 * 2), constantPart / (7 * 4), constantPart / (7 * 8)};

        // default to lowest allowed
        byte Exponent     = 3;
        byte Mantissa     = 3;
        // scan the array.The order of the entries is decreasing but columnwise, so it's a bit awkward to scan
        int  scannedCount = 0;
        int  currentIndex = 4;
        while (scannedCount < 16)
        {
            // if the bandwidth setting is between two values, choose the closer one
            if (bandwidthKHz > allowableBWKHz[currentIndex])
            {
                int previousIndex = currentIndex - 4;
                int index         = previousIndex;

                float diffFromLarger  = allowableBWKHz[previousIndex] - bandwidthKHz;
                float diffFromSmaller = bandwidthKHz - allowableBWKHz[currentIndex];

                if (diffFromLarger > diffFromSmaller)
                {
                    index = currentIndex;
                }
                Mantissa = (byte)(index / 4);            // row in array above
                Exponent = (byte)(index - Mantissa * 4); // mod
                break;
            }
            scannedCount++;
            currentIndex = currentIndex + 4;
            if (currentIndex > 15)
            {
                currentIndex = currentIndex - 15;
            }
        }
        byte result = (byte)(((Exponent << 2 | Mantissa) << 4) | DataRate);

        ensureChipIdle();

        ESP_LOGI(TAG, "%s:  input bw %g, datarate 0x%0X setting result=0x%0X, Mantissa=0x%0X,Exponent=0x%0X", __FUNCTION__, bandwidthKHz, DataRate, result, Mantissa, Exponent);
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

        ESP_LOGD(TAG, "%s: DataRate expected -> %g", __FUNCTION__, (256 + Mantissa) * (2 << Exponent) / (2 << 28) * m_oscillatorFrequencyHz);

        ensureChipIdle();

        ESP_LOGD(TAG, "%s: Writing to MDMCFG4 -> 0x%0X", __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG4, result);
        handleCommonStatusCodes(statusCode, false);
        ESP_LOGD(TAG, "%s: Writing to MDMCFG3 -> 0x%0X", __FUNCTION__, Mantissa);
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

        float constantPart            = m_oscillatorFrequencyHz / (2 << 17);
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
        ESP_LOGD(TAG, "%s: setting result=0x%0X, Mantissa=0x%0X,Exponent=0x%0X", __FUNCTION__, result, Mantissa, Exponent);
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
            ESP_LOGD(TAG, "%s: patable is 315 for freq %g, setting is 0x%X", __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        else if (m_carrierFrequencyMHz <= 464)
        {
            currentTable     = ConfigValues::PATABLE_433_SETTINGS;
            paSetting        = getMultiLayerInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_433_SETTINGS));
            m_currentPATable = PATables::PA_433;
            ESP_LOGD(TAG, "%s: patable is 433 for freq %g, setting is 0x%X", __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        // I'm not sure what to do about 868, so this is all a bit adhoc over 464 MHz. I suppose it depends on your
        // chip.
        else if (m_carrierFrequencyMHz <= 880)
        {
            currentTable     = ConfigValues::PATABLE_868_SETTINGS;
            paSetting        = getWireWoundInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_868_SETTINGS));
            m_currentPATable = PATables::PA_868;
            ESP_LOGD(TAG, "%s: patable is 868 for freq %g, setting is 0x%X", __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }
        else
        {
            currentTable     = ConfigValues::PATABLE_915_SETTINGS;
            paSetting        = getWireWoundInductorPower(outputPower, currentTable, ARRAYSIZE(ConfigValues::PATABLE_915_SETTINGS));
            m_currentPATable = PATables::PA_915;
            ESP_LOGD(TAG, "%s: patable is 915 for freq %g, setting is 0x%X", __FUNCTION__, m_carrierFrequencyMHz, paSetting);
        }

        // ASK always uses index 0 PATABLE to transmit a 0;
        if (m_deviceConfig.Modulation == ModulationType::ASK_OOK)
        {
            m_PATABLE[0] = 0;
            m_PATABLE[1] = paSetting;
            ESP_LOGD(TAG, "Modulation is ASK_OOK, patable should have 0x%0X in index 1", paSetting);
        }
        else
        {
            m_PATABLE[0] = paSetting;
            m_PATABLE[1] = 0;
            ESP_LOGD(TAG, "Modulation is *not* ASK_OOK, patable should have 0x%0X in index 0", paSetting);
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

        ensureChipIdle();
        ESP_LOGD(TAG, "%s Setting MDMCFG2 0x%0X", __FUNCTION__, mdmcfg2);
        statusCode = writeRegister(CC1101_CONFIG::MDMCFG2, mdmcfg2);
        handleCommonStatusCodes(statusCode, false);

        ESP_LOGD(TAG, "%s Setting FREND0 0x%0X", __FUNCTION__, frend0);
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

        ensureChipIdle();
        ESP_LOGD(TAG, "%s Setting MDMCFG2 0x%0X", __FUNCTION__, result);
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

        ensureChipIdle();
        ESP_LOGD(TAG, "%s Setting MDMCFG2 0x%0X", __FUNCTION__, (byte)(currentMdmcfg2 | setting));
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

        ensureChipIdle();
        ESP_LOGD(TAG, "%s Setting MDMCFG2 0x%0X", __FUNCTION__, result);
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
        ESP_LOGD(TAG, "%s Setting PKTCTRL0 0x%0X", __FUNCTION__, result);
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
        ESP_LOGD(TAG, "%s Setting PKTCTRL0 0x%0X", __FUNCTION__, result);
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
        ESP_LOGD(TAG, "%s Setting PKTCTRL1 0x%0X", __FUNCTION__, currentPktCtrl1);
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

        ESP_LOGD(TAG, "%s Setting PKTCTRL1 0x%0X", __FUNCTION__, currentPktCtrl1);
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

        ESP_LOGD(TAG, "%s Setting PKTCTRL1 0x%0X", __FUNCTION__, result);
        statusCode = writeRegister(CC1101_CONFIG::PKTCTRL1, result);
        handleCommonStatusCodes(statusCode, false);
    }

    bool CC1101Device::lowerChipSelect()
    {
        return digitalWrite(m_spiMaster->ChipSelectPin(), 0);
    }

    bool CC1101Device::raiseChipSelect()
    {
        return digitalWrite(m_spiMaster->ChipSelectPin(), 1);
    }

    void CC1101Device::waitForMisoLow()
    {
        while (gpio_get_level(m_spiMaster->MisoPin()) == 1)
            delayMilliseconds(1);
    }

    void CC1101Device::ensureChipIdle()
    {
        /*
        byte outData = 0;
        do
        {
            outData = sendStrobe(CC1101_CONFIG::SIDLE);
            ESP_LOGD(TAG, "SIDLE strobe returned status 0x%0X", outData);
        } while ((outData & 0b01110000) != 0);
        */
    }

    void CC1101Device::enableReceiveMode()
    {
        byte outData;
        for (int tries = 0; tries < 3; tries++) // Sometimes SRX fails to put chip in receive mode.
        {
            byte chipState = 0;

            outData = sendStrobe(CC1101_CONFIG::SRX);
            ESP_LOGD(TAG, "SRX strobe returned status 0x%0X", outData);

            chipState = outData & 0b01110000; // check chip state (pg 31)
            if (chipState != 0)
            {
                break;
            }
            delayMilliseconds(1);
        };
    }

    bool CC1101Device::digitalWrite(int pin, uint32_t value)
    {
        return (gpio_set_level(static_cast<gpio_num_t>(pin), value) == ESP_OK);
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

        lowerChipSelect();
        waitForMisoLow();
        CBRA(m_spiMaster->WriteByte(address, value));
        CBRA(m_spiMaster->WriteByte(0, value));
        raiseChipSelect();

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
        byte statusCode;
        if (address >= CC1101_CONFIG::PARTNUM && address <= CC1101_CONFIG::RCCTRL0_STATUS)
        {
            ESP_LOGE(TAG, "Control registers cannot be read with burst access");
            return false;
        }
        address |= (kSpiBurstAccessBit | kSpiHeaderReadBit);
        lowerChipSelect();
        waitForMisoLow();
        CBRA(m_spiMaster->WriteByte(address, statusCode));
        for (int i = 0; i < len; i++)
        {
            CBRA(m_spiMaster->WriteByte(0, statusCode));
            buffer[i] = statusCode;
        }
        raiseChipSelect();
    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return bRet;
    }
    void CC1101Device::readRXFIFO(byte *buffer, int expectedCount)
    {
        byte status[2];
        byte scratch[255]; // absolute max of RXFIFO
        byte regVal = readRegister(CC1101_CONFIG::RXBYTES);

        byte avail = (regVal & kRxFifoByteCountMask);

        ESP_LOGD(TAG, "%s, expected %d, avail %d", __FUNCTION__, expectedCount, avail);
        readBurstRegister(CC1101_CONFIG::RXFIFO, scratch, avail);
        for (int i = 0; i < avail; i++)
        {
            ESP_LOGD(TAG, "%s buffer[%d]= 0x%0X", __FUNCTION__, i, scratch[i]);
        }
        if (m_deviceConfig.EnableAppendStatusBytes)
        {
            readBurstRegister(CC1101_CONFIG::RXFIFO, status, 2);
            ESP_LOGD(TAG, "%s status= {0x%0X , 0x%0X}", __FUNCTION__, status[0], status[1]);
        }
        if ((regVal & ~kRxFifoByteCountMask) != 0)
        {
            byte resetStatus;
            ESP_LOGW(TAG, "RX_FIFO overflow, sending reset");
            m_spiMaster->WriteByte(CC1101_CONFIG::SFRX, resetStatus);
            ESP_LOGW(TAG, "RX_FIFO overflow, new status 0x%0X", resetStatus);
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
        lowerChipSelect();
        waitForMisoLow();
        m_spiMaster->WriteByteToAddress(address, value, statusCode);
        raiseChipSelect();

        return statusCode;
    }

    void CC1101Device::writeBurstRegister(byte address, byte *values, int valueLen)
    {
        byte statusCode = 0;
        lowerChipSelect();
        waitForMisoLow();
        m_spiMaster->WriteByte(address | kSpiBurstAccessBit, statusCode);
        ESP_LOGD(TAG, "Write burst to address 0x%0X statusCode 0x%0X", address, statusCode);
        m_spiMaster->WriteBytes(values, valueLen, statusCode);
        ESP_LOGD(TAG, "Write values to address 0x%0X statusCode 0x%0X", address, statusCode);
        raiseChipSelect();
    }

    byte CC1101Device::sendStrobe(byte strobeCmd)
    {
        byte outStatus = 0;
        lowerChipSelect();
        waitForMisoLow();
        m_spiMaster->WriteByte(strobeCmd, outStatus);
        raiseChipSelect();

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
        byte statusCode = 0;
        byte pktctrlVal = (byte)(((int)m_deviceConfig.PacketFmt << 4 | (int)m_deviceConfig.PacketLengthCfg));

        // Set PKTCTRL0
        switch (m_deviceConfig.PacketFmt)
        {
            case PacketFormat::Normal:
                handleCommonStatusCodes(statusCode, false);
                statusCode = writeRegister(CC1101_CONFIG::IOCFG0, (byte)ConfigValues::GDx_CFG_LowerSixBits::SYNC_WORD_OR_RX_PKT_DISCARDED_OR_TX_UNDERFLOW);
                handleCommonStatusCodes(statusCode, false);
                statusCode = writeRegister(CC1101_CONFIG::IOCFG2, (byte)ConfigValues::GDx_CFG_LowerSixBits::HIGH_IMPEDANCE_3_STATE);
                handleCommonStatusCodes(statusCode, false);
                ESP_LOGD(TAG, "%s: Writing PKTCTRL0 0x%X for Packet format %d", __FUNCTION__, pktctrlVal, (int)m_deviceConfig.PacketFmt);
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
                    ESP_LOGD(TAG, "%s: Writing PKTCTRL0 0x%X for Packet format 0x%X, length cfg 0x%X", __FUNCTION__, pktctrlVal, (int)m_deviceConfig.PacketFmt, (int)m_deviceConfig.PacketLengthCfg);
                    statusCode = writeRegister(CC1101_CONFIG::PKTCTRL0, pktctrlVal);
                    handleCommonStatusCodes(statusCode, false);

                    // why?
                    SetDataRate(7, 0x93);
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

        SetCRC(m_deviceConfig.EnableCRC);
        SetCRCAutoFlush(m_deviceConfig.EnableCRCAutoflush);
        SetAddressCheck(m_deviceConfig.AddressCheck);
        SetAppendStatus(m_deviceConfig.EnableAppendStatusBytes);

    }
    void CC1101Device::regConfig()
    {
        byte returnValue = 0;

        ensureChipIdle();

        returnValue = writeRegister(CC1101_CONFIG::FSCTRL1, 0x06);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MDMCFG0, 0xF8);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MDMCFG1, 0x02);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::CHANNR, 0);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::DEVIATN, 0x47);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FREND1, 0x56);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::MCSM0, 0x18);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::FOCCFG, 0x16);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::BSCFG, 0x1C);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL2, 0xC7);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL1, 0x00);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::AGCCTRL0, 0xB2);
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
        returnValue = writeRegister(CC1101_CONFIG::TEST2, 0x81);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::TEST1, 0x35);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::TEST0, 0x09);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::PKTCTRL1, 0x04);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::ADDR, 0x00);
        handleCommonStatusCodes(returnValue, false);
        returnValue = writeRegister(CC1101_CONFIG::PKTLEN, 0x00);
        handleCommonStatusCodes(returnValue, false);
    }
    void CC1101Device::handleCommonStatusCodes(byte status, bool wasReadOperation)
    {
        byte                       fifoBytesAvail = status & 0x0F;
        byte                       statusBits     = status & 0b01110000;
        StatusByteStateMachineMode machineState   = static_cast<StatusByteStateMachineMode>(statusBits >> 4);

        if ((machineState == StatusByteStateMachineMode::FIFOOverflowRX) && (fifoBytesAvail > 0))
        {
            ESP_LOGD(TAG, "%s statusCode 0x%02X, fifo bytes available 0x%02X", __FUNCTION__, status, fifoBytesAvail);
        }

        switch (machineState)
        {
            // RX_FIFO overflow
            case StatusByteStateMachineMode::FIFOOverflowRX:
                {
                    byte fifoBytes[fifoBytesAvail];
                    readRXFIFO(fifoBytes, fifoBytesAvail);
                }
                break;
            case StatusByteStateMachineMode::FIFOOverflowTX:
                break;
            case StatusByteStateMachineMode::ReceiveMode:
                {
                    if ((fifoBytesAvail > 0) && wasReadOperation)
                    {
                        byte fifoBytes[fifoBytesAvail];
                        readRXFIFO(fifoBytes, fifoBytesAvail);
                    }
                }
                break;
            case StatusByteStateMachineMode::IDLE:
                break;
            default:
                ESP_LOGD(TAG, "%s status code not handle was 0x%X", __FUNCTION__, status);
                break;
        }
    }
    void IRAM_ATTR CC1101Device::gpioISR(void *thisPtr)
    {
        CC1101Device *That   = static_cast<CC1101Device *>(thisPtr);
        uint32_t      ignore = 0;
        xQueueSendFromISR(That->m_ISRQueueHandle, (void *)&ignore, NULL);
    }

} // namespace TI_CC1101