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


#include <driver/rtc_io.h>
#include "CC1101Lib.h"
#include "CC1101Device.h"
#include "SpiMaster.h"

static const char *TAG = "CC110Device";

namespace TI_CC1101
{
    CC1101Device::CC1101Device()
    {
    }

    CC1101Device::~CC1101Device()
    {
    }

    void CC1101Device::Init(std::shared_ptr<SpiMaster> spiMaster,  float crystalFrequencyHz)
    {
        m_spiMaster = spiMaster;
        if(crystalFrequencyHz != 0)
        {
            m_oscillatorFrequencyHz = crystalFrequencyHz;
        }
        m_frequencyIncrement = m_oscillatorFrequencyHz/ kFrequencyDivisor;

        ESP_LOGI(TAG, "CC1101Device::Init");

        Reset();
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
        bool bRet = true;

        CERA(gpio_set_level(m_spiMaster->ClockPin(),1));
        CERA(gpio_set_level(m_spiMaster->MosiPin(),0));

        CBRA(lowerChipSelect());
        delayMicroseconds(5);
        CBRA(raiseChipSelect());
        delayMicroseconds(5);
        CBRA(lowerChipSelect());
        delayMicroseconds(40);
        CBRA(raiseChipSelect());

        // This is a command strobe so we only need the lower 6 bits, i.e, the address.
        // See page 32, Section 10.4
        CBRA(m_spiMaster->WriteByte(CC1101_CONFIG::SRES));

        waitForMisoLow();

    Error:
        if(!bRet)
        {
            ESP_LOGW(TAG, "CC1101 reset failed"); // reset failed
        }
    }
    // Page 75 of TI Datasheet
    // Frequency is a 24-bit word set via FREQ0,FREQ1 and FREQ2 registers
    //  (but upper bits 22 and 23 of FREQ2 are always 0)
    //
    // Frequency is calculated as:
    //
    //        (oscillator freq / divisor) * FREQ[23:0] 
    //
    // In the default case 26 MHz / 65536 = 396.728 Hz  is the multiplier, stored as frequencyIncrement variable
    //
    void CC1101Device::SetFrequency(float frequencyMHz)
    {
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
        byte freq2 = (byte)((frequencySteps & 0x001F0000) >> 16);

        ESP_LOGD(TAG,"SetFrequency() -> freq0=0x%X, freq1=0x%X, freq2 = 0x%X", freq0, freq1, freq2);

        writeRegister(CC1101_CONFIG::FREQ0, freq0);
        writeRegister(CC1101_CONFIG::FREQ1, freq1);
        writeRegister(CC1101_CONFIG::FREQ2, freq2);

        m_oscillatorFrequencyHz = frequencyMHz * 1'000'000;
        ESP_LOGI(TAG, "m_oscillatorFrequencyHz is now %g", m_oscillatorFrequencyHz);
    }
    // Page 76 of TI Datasheet
    //
    // Filter B/W is in  top 4 bits of MDMCFG4
    //  Calculated by the formula :  BW =  ( Crystal frequency / (8 * (4 + Mantissa)* 2^Exponent) )
    //  Where:
    //     Exponent is in bits 6 and 7 (Two MSBs) of MDMCFG4
    //     Mantissa is bits 4 and 5
    //
    // The bottom 4 bits of MDMCFG4 are used to configure the Data rate in the MDMCFG3 register
    //
    // Page 35 of the TI Datasheet gives a table of ranges for the 4x4 combinations of the 2-bit bitfields.
    // A dictionary would be nice but alas nanoFramework does not support Generics and DictionaryEntry is
    // unusable.
    //
    void CC1101Device::SetReceiveChannelFilterBandwidth(float bandwidthKHz)
    {
        byte modemCFG = readRegister(CC1101_CONFIG::MDMCFG4);
        byte DataRate = (byte)(modemCFG & 0xF);

        // Page 35 table in an array. The exponent is the horizontal stride (4 columns corresponding to the bit values 00,01,10,11) 
        // Mantissa is the vertical stride (4 rows corresponding to the bit values 00,01,10,11)
        float constantPart     = m_oscillatorFrequencyHz / 8;
        float allowableBWKHz[] = {
            constantPart / (4 * 1), constantPart / (4 * 2), constantPart / (4 * 4), constantPart / (4 * 8),
            constantPart / (5 * 1), constantPart / (5 * 2), constantPart / (5 * 4), constantPart / (5 * 8),
            constantPart / (6 * 1), constantPart / (6 * 2), constantPart / (6 * 4), constantPart / (6 * 8),
            constantPart / (7 * 1), constantPart / (7 * 2), constantPart / (7 * 4), constantPart / (7 * 8)};

        // default to lowest allowed
        byte Exponent = 3;
        byte Mantissa = 3;
        // scan the array, from index 1
        for (int i = 1; i < 16; i++)
        {
            // if the bandwidth setting is between two values, choose the closer one
            if (bandwidthKHz > allowableBWKHz[i])
            {
                int index = i - 1;

                float diffFromLarger  = allowableBWKHz[i - 1] - bandwidthKHz;
                float diffFromSmaller = bandwidthKHz - allowableBWKHz[i];

                if (diffFromLarger > diffFromSmaller)
                {
                    index = i;
                }
                Mantissa = (byte)(index / 4);            // row in array above
                Exponent = (byte)(index - Mantissa * 4); // mod
                break;
            }
        }
        byte result = (byte)(((Exponent << 2 | Mantissa) << 4) | DataRate);

        ESP_LOGI(TAG, "SetReceiveChannelFilterBandwidth: setting result=0x%X, Mantissa=0x%X,Exponent=0x%X",  result, Exponent, Mantissa);
        writeRegister(CC1101_CONFIG::MDMCFG4, result);
    }
    /// <summary>
    /// Sets modem deviation allowed, per page 78 of TI Datasheet
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
        float constantPart            = m_oscillatorFrequencyHz / 131072;
        float allowableDeviationKHz[] = {
            constantPart * (8 * 1),   constantPart / (8 * 2),   constantPart / (8 * 4),   constantPart / (8 * 8),
            constantPart / (8 * 16),  constantPart / (8 * 32),  constantPart / (8 * 64),  constantPart / (8 * 128),
            constantPart * (9 * 1),   constantPart / (9 * 2),   constantPart / (9 * 4),   constantPart / (9 * 8),
            constantPart / (9 * 16),  constantPart / (9 * 32),  constantPart / (9 * 64),  constantPart / (9 * 128),
            constantPart * (10 * 1),  constantPart / (10 * 2),  constantPart / (10 * 4),  constantPart / (10 * 8),
            constantPart / (10 * 16), constantPart / (10 * 32), constantPart / (10 * 64), constantPart / (10 * 128),
            constantPart * (11 * 1),  constantPart / (11 * 2),  constantPart / (11 * 4),  constantPart / (11 * 8),
            constantPart / (11 * 16), constantPart / (11 * 32), constantPart / (11 * 64), constantPart / (11 * 128),
            constantPart * (12 * 1),  constantPart / (12 * 2),  constantPart / (12 * 4),  constantPart / (12 * 8),
            constantPart / (12 * 16), constantPart / (12 * 32), constantPart / (12 * 64), constantPart / (12 * 128),
            constantPart * (13 * 1),  constantPart / (13 * 2),  constantPart / (13 * 4),  constantPart / (13 * 8),
            constantPart / (13 * 16), constantPart / (13 * 32), constantPart / (13 * 64), constantPart / (13 * 128),
            constantPart * (14 * 1),  constantPart / (14 * 2),  constantPart / (14 * 4),  constantPart / (14 * 8),
            constantPart / (14 * 16), constantPart / (14 * 32), constantPart / (14 * 64), constantPart / (14 * 128),
            constantPart * (15 * 1),  constantPart / (15 * 2),  constantPart / (15 * 4),  constantPart / (15 * 8),
            constantPart / (15 * 16), constantPart / (15 * 32), constantPart / (15 * 64), constantPart / (15 * 128),
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
        ESP_LOGI(TAG, "SetModemDeviation: setting result=0x%X, Mantissa=0x%X,Exponent=0x%X",  result, Exponent, Mantissa);
        writeRegister(CC1101_CONFIG::DEVIATN, result);
    }
    /// <summary>
    /// Set output power level
    /// </summary>
    /// <param name="outputPower"></param>
    //
    // Page 59 of TI Datasheet
    void CC1101Device::SetOutputPower(int outputPower)
    {
        float frequencyMHz = m_oscillatorFrequencyHz * 1'000'000;
        byte  paSetting;
        const byte  *currentTable = nullptr;

        // Page of datasheet indicates that the operational frequency bands are 300-348,387-464 and 779-928
        if (frequencyMHz <= 348)
        {
            currentTable   = &ConfigValues::PATABLE_315_SETTINGS[0];
            paSetting      = setMultiLayerInductorPower(outputPower, currentTable,ARRAYSIZE(ConfigValues::PATABLE_315_SETTINGS));
            m_currentPATable = ConfigValues::PATables::PA_315;
        }
        else if (frequencyMHz <= 464)
        {
            currentTable   = ConfigValues::PATABLE_433_SETTINGS;
            paSetting      = setMultiLayerInductorPower(outputPower, currentTable,ARRAYSIZE(ConfigValues::PATABLE_433_SETTINGS));
            m_currentPATable = ConfigValues::PATables::PA_433;
        }
        // I'm not sure what to do about 868, so this is all a bit adhoc over 464 MHz. I suppose it depends on your
        // chip.
        else if (frequencyMHz <= 880)
        {
            currentTable   = ConfigValues::PATABLE_868_SETTINGS;
            paSetting      = setWireWoundInductorPower(outputPower, currentTable,ARRAYSIZE(ConfigValues::PATABLE_868_SETTINGS));
            m_currentPATable = ConfigValues::PATables::PA_868;
        }
        else
        {
            currentTable   = ConfigValues::PATABLE_915_SETTINGS;
            paSetting      = setWireWoundInductorPower(outputPower, currentTable,ARRAYSIZE(ConfigValues::PATABLE_915_SETTINGS));
            m_currentPATable = ConfigValues::PATables::PA_915;
        }

        // ASK always uses index 0 PATABLE to transmit a 0;
        if (m_currentModulationType == ModulationType::ASK_OOK)
        {
            m_PATABLE[0] = 0;
            m_PATABLE[1] = paSetting;
        }
        else
        {
            m_PATABLE[0] = paSetting;
            m_PATABLE[1] = 0;
        }

        writeBurstRegister(CC1101_CONFIG::PATABLE, m_PATABLE, 8);
    }
    //
    //  Page 77,89 of TI Datasheet
    //
    // Set Modulation mode in MDMCFG2 and point to the PATABLE entry in register FREND0
    //
    void CC1101Device::SetModulation(ModulationType modulationType)
    {
        byte currentMDMCFG2 = readRegister(CC1101_CONFIG::MDMCFG2);
        byte currentFREND0  = readRegister(CC1101_CONFIG::FREND0);

        byte frend0, mdmcfg2;

        // default to non-ASK_OOK, i.e, PATABLE[0] contains our only power level
        frend0  = (byte)(currentFREND0 & 0b11110000);
        mdmcfg2 = currentMDMCFG2;
        switch (modulationType)
        {
            case ModulationType::FSK_2:
                mdmcfg2 = (byte)(currentMDMCFG2 & 0b10001111); // clear bits 4-6
                break;
            case ModulationType::GFSK:
                mdmcfg2 = (byte)((currentMDMCFG2 & 0b10001111) | 0b00010000); // 1 in bits 4-6
                break;
            // For ASK_OOK we point to PATABLE[1] and for the rest to PATABLE[0], because ASK always uses PATABLE[0] for
            // transmitting '0' This assumes we set PATABLE[0] to 0
            case ModulationType::ASK_OOK:
                frend0  = (byte)((currentFREND0 & 0b11110000) | 0b11110001);  // 1 in the low nibble
                mdmcfg2 = (byte)((currentMDMCFG2 & 0b10001111) | 0b10110001); // 011 in bits 4-6
                break;
            case ModulationType::FSK_4:
                mdmcfg2 = (byte)((currentMDMCFG2 & 0b10001111) | 0b01000000); // 100 in bits 4-6
                SetManchesterEncoding(false);                                 // Page 43
                break;
            case ModulationType::MSK:
                mdmcfg2 = (byte)((currentMDMCFG2 & 0b10001111) | 0b01110000); // 111 in bits 4-6
                break;
            case ModulationType::INVALID_2:
            case ModulationType::INVALID_5:
            case ModulationType::INVALID_6:
                assert(false);
                break;
        }
        m_currentModulationType = modulationType;
        writeRegister(CC1101_CONFIG::MDMCFG2, mdmcfg2);
        writeRegister(CC1101_CONFIG::FREND0, frend0);
    }
    /// <summary>
    /// Sets or unsets Manchester encoding (Pg 77) in register MDMCFG2
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetManchesterEncoding(bool shouldEnable)
    {
        byte currentMDMCFG2 = readRegister(CC1101_CONFIG::MDMCFG2);

        int currentManchester = ((currentMDMCFG2 & 0b00001000) >> 3);
        // If already the same
        if ((currentManchester == 1) == shouldEnable)
        {
            return;
        }
        byte result = (byte)(currentMDMCFG2 & 0b11110111);
        if (shouldEnable)
        {
            result |= 0b00001000;
        }
        else
        {
            result &= 0b11110111;
        }
        m_currentManchesterEnabled = shouldEnable;
        writeRegister(CC1101_CONFIG::MDMCFG2, result);
    }
    /// <summary>
    /// Set Packet Format (Pg 74) in register PKTCTRL0
    /// </summary>
    /// <param name="packetFormat"></param>
    void CC1101Device::SetPacketFormat(PacketFormat packetFormat)
    {
        byte currentPktCtrl0  = readRegister(CC1101_CONFIG::PKTCTRL0);
        int  currentPktFormat = ((currentPktCtrl0 & 0b00110000) >> 4);
        if (currentPktFormat == (int)packetFormat)
        {
            return;
        }
        byte result = (byte)(currentPktCtrl0 & 0b11001111);
        switch (packetFormat)
        {
            case PacketFormat::Normal:
                break;
            case PacketFormat::Synchronous:
                result |= 0b00010000;
                break;
            case PacketFormat::RandomTX:
                result |= 0b00100000;
                break;
            case PacketFormat::Asynchronous:
                result |= 0b00110000;
                break;
        }
        writeRegister(CC1101_CONFIG::PKTCTRL0, result);
    }
    /// <summary>
    /// Disable Digital DC blocking filter (Pg 77) in register MDMCFG2
    /// </summary>
    void CC1101Device::DisableDigitalDCFilter()
    {
        byte currentMdmcfg2 = readRegister(CC1101_CONFIG::MDMCFG2);
        int  currentSetting = (currentMdmcfg2 & 0b10000000) >> 7;
        if (currentSetting == 1)
        {
            return;
        }
        writeRegister(CC1101_CONFIG::MDMCFG2, (byte)(currentMdmcfg2 | 0b10000000));
    }
    /// <summary>
    /// Set CRC for data (Pg 74) in register PKTCTRL0
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetCRC(bool shouldEnable)
    {
        byte currentPktCtrl0 = readRegister(CC1101_CONFIG::PKTCTRL0);
        bool currentSetting  = ((currentPktCtrl0 & 0b00000100) >> 2 == 1);

        if ((currentSetting ==1) == shouldEnable)
        {
            return;
        }
        byte result = (byte)(currentPktCtrl0 & 0b11111011);
        if (shouldEnable)
        {
            result |= 0b00000100;
        }
        writeRegister(CC1101_CONFIG::PKTCTRL0, result);
    }
    /// <summary>
    /// Set CRC Autoflush (Pg 73) in register PKTCTRL1
    /// Enable automatic flush of RX FIFO when CRC is not OK. This requires that
    /// only one packet is in the RXIFIFO and that packet length is limited to the RX FIFO size.
    /// </summary>
    /// <param name="shouldEnable"></param>
    void CC1101Device::SetCRCAutoFlush(bool shouldEnable)
    {
        byte currentPktCtrl1 = readRegister(CC1101_CONFIG::PKTCTRL1);
        bool currentSetting  = ((currentPktCtrl1 & 0b00001000) >> 3 == 1);
        if ((currentSetting == 1) == shouldEnable)
        {
            return;
        }
        byte result = (byte)(currentPktCtrl1 & 0b11110111);
        if (shouldEnable)
        {
            result |= 0b00001000;
        }
        writeRegister(CC1101_CONFIG::PKTCTRL1, result);
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
        byte currentMdmcfg2 = readRegister(CC1101_CONFIG::MDMCFG2);
        int  currentSetting = currentMdmcfg2 & 0b00000011;

        if ((int)syncMode == currentSetting)
        {
            return;
        }
        byte result = (byte)((currentMdmcfg2 & 0b11111100) | (int)syncMode);

        writeRegister(CC1101_CONFIG::MDMCFG2, result);
    }
    /// <summary>
    /// Set Address Check (Pg 73) in register PKTCTRL1
    /// </summary>
    /// <param name="addressCheckConfig"></param>
    void CC1101Device::SetAddressCheck(AddressCheckConfiguration addressCheckConfig)
    {
        byte currentPktCtrl1 = readRegister(CC1101_CONFIG::PKTCTRL1);
        int  currentSetting  = currentPktCtrl1 & 0b00000011;

        if ((int)addressCheckConfig == currentSetting)
        {
            return;
        }
        byte result = (byte)((currentPktCtrl1 & 0b11111100) | (int)addressCheckConfig);

        writeRegister(CC1101_CONFIG::PKTCTRL1, result);
    }

    bool CC1101Device::lowerChipSelect()
    {
        return digitalWrite(m_spiMaster->ChipSelectPin(),0);
    }

    bool CC1101Device::raiseChipSelect()
    {
        return digitalWrite(m_spiMaster->ChipSelectPin(),1);
    }

    void CC1101Device::waitForMisoLow()
    {
        while (gpio_get_level(m_spiMaster->MisoPin()) == 1)
            delayMicroseconds(2); // busy-ish wait
    }

    bool CC1101Device::digitalWrite(int pin, uint32_t value)
    {
        return (gpio_set_level(static_cast<gpio_num_t>(pin), value) == ESP_OK);
    }

    void CC1101Device::delayMilliseconds(int millis)
    {
        vTaskDelay(millis / portTICK_PERIOD_MS);
    }

    void CC1101Device::delayMicroseconds(int micros)
    {
        vTaskDelay(micros / 1000 / portTICK_PERIOD_MS);
    }

    byte CC1101Device::readRegister(byte address)
    {
        address &= 0b00111111; //clear R/W and burst bit
        if(address >= CC1101_CONFIG::PARTNUM && address <= CC1101_CONFIG::RCCTRL0_STATUS)
        {
            address |= kSpiBurstAccessBit;
        }
        address |= kSpiHeaderReadBit;

        lowerChipSelect();
        waitForMisoLow();
        byte value = m_spiMaster->WriteByte(address);

        return value;
    }

    void CC1101Device::writeRegister(byte address, byte value)
    {
        lowerChipSelect();
        waitForMisoLow();
        m_spiMaster->WriteByteToAddress(address,value);
        raiseChipSelect();
    }

    void CC1101Device::writeBurstRegister(byte address, byte *values, int valueLen)
    {
        lowerChipSelect();
        waitForMisoLow();
        m_spiMaster->WriteByte(address | kSpiBurstAccessBit);
        m_spiMaster->WriteBytes(values,valueLen);
        raiseChipSelect();
    }

    byte CC1101Device::setMultiLayerInductorPower(int outputPower, const byte *currentTable, int currentTableLen)
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

    byte CC1101Device::setWireWoundInductorPower(int outputPower, const byte *currentTable, int currentTableLen)
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

} // namespace TI_CC1101