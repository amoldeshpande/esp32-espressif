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
#pragma once
#include <driver/spi_master.h>
#include <vector>
#include <memory>
#include "CC1101Lib.h"

namespace TI_CC1101
{
    class SpiMaster;
    
    struct CC110DeviceConfig
    {
        byte TxPin;
        byte RxPin;
    };
    class CC1101Device final
    {
      protected:
        // 26 MHz crystal by default. Apparently it can be 27 as well according to docs.
        const float                kDefaultOscillatorFrequencyMhz = 26;
        const float                kFrequencyDivisor              = 2 << 16;
        float                      m_oscillatorFrequencyHz        = kDefaultOscillatorFrequencyMhz * (1'000'000);
        // see SetFrequency() below
        float                      m_frequencyIncrement           = kDefaultOscillatorFrequencyMhz / kFrequencyDivisor;
        ConfigValues::PATables     m_currentPATable               = ConfigValues::PATables::PA_433;
        ModulationType             m_currentModulationType        = ModulationType::FSK_2;
        bool                       m_currentManchesterEnabled     = false;
        // PATABLE is 8 bytes
        byte                       m_PATABLE[8]                   = {0, 0, 0, 0, 0, 0, 0, 0};
        std::shared_ptr<SpiMaster> m_spiMaster;

        const byte kSpiHeaderByteWriteMask = 0b01111111; // Mask out high bit.
        const byte kSpiHeaderReadBit       = 0b10000000; // OR this in to set read bit in the header
        const byte kSpiNoBurstAccessMask   = 0b10111111; // No. 2 MSB is burst bit
        const byte kSpiBurstAccessBit      = 0b01000000; // OR this to set burst bit on

      public:
        CC1101Device();
        ~CC1101Device();
        void Init(std::shared_ptr<SpiMaster> spiMaster, float crystalFrequencyHz = -1);
        void Reset();
        void SetFrequency(float frequencyMHz);
        void SetReceiveChannelFilterBandwidth(float bandwidthKHz);
        void SetModemDeviation(float deviationKHz);
        void SetOutputPower(int outputPower);
        void SetModulation(ModulationType modulationType);
        void SetManchesterEncoding(bool shouldEnable);
        void SetPacketFormat(PacketFormat packetFormat);
        void DisableDigitalDCFilter();
        void SetCRC(bool shouldEnable);
        void SetCRCAutoFlush(bool shouldEnable);
        void SetSyncMode(SyncWordQualifierMode syncMode);
        void SetAddressCheck(AddressCheckConfiguration addressCheckConfig);

      protected:
        bool lowerChipSelect();
        bool raiseChipSelect();
        void waitForMisoLow();
        bool digitalWrite(int pin, uint32_t value);
        void delayMilliseconds(int millis);
        void delayMicroseconds(int micros);
        byte readRegister(byte address);
        void writeRegister(byte address, byte value);
        void writeBurstRegister(byte address, byte *values, int valueLen);
        byte setMultiLayerInductorPower(int outPower, const byte *currentTable, int currentTableLen);
        byte setWireWoundInductorPower(int outPower, const byte *currentTable, int currentTableLen);
    };
} // namespace TI_CC1101