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
#include <driver/gpio.h>
#include <vector>
#include <memory>
#include "CC1101Lib.h"

namespace TI_CC1101
{
    class SpiMaster;
    
    struct CC110DeviceConfig
    {
        gpio_num_t                TxPin;
        gpio_num_t                RxPin;
        float                     OscillatorFrequencyMHz {26};
        float                     CarrierFrequencyMHz {433.92};
        float                     ReceiveFilterBandwidthKHz {812.5};
        float                     FrequencyDeviationKhz {47.6};
        int                       TxPower {-30}; // Also called Output Power in the datasheet
        ModulationType            Modulation {ModulationType::ASK_OOK};
        bool                      ManchesterEnabled {true};
        PacketFormat              PacketFmt {PacketFormat::AsyncSerialMode}; // this field and PacketlengthCfg go into the PKTCTRL0 register, pg 74
        PacketLengthConfig        PacketLengthCfg {PacketLengthConfig::Infinite};
        bool                      DisableDCFilter {true};
        bool                      EnableCRC {false};
        bool                      EnableCRCAutoflush {false};
        SyncWordQualifierMode     SyncMode {SyncWordQualifierMode::NoPreambleOrSync_CarrierSenseAboveThreshold};
        AddressCheckConfiguration AddressCheck {AddressCheckConfiguration::None};
        bool                      EnableAppendStatusBytes {false};

        void DebugDump();
    };
    class CC1101Device final
    {
      protected:
        // 26 MHz crystal by default. Apparently it can be 27 as well according to docs.
        const float                kDefaultOscillatorFrequencyMHz = 26;
        const float                kFrequencyDivisor              = 2 << 16;
        float                      m_oscillatorFrequencyHz        = kDefaultOscillatorFrequencyMHz * (1'000'000);
        // see SetFrequency() 
        float                      m_frequencyIncrement           = kDefaultOscillatorFrequencyMHz / kFrequencyDivisor;
        float                      m_carrierFrequencyMHz          = 433;
        PATables                   m_currentPATable               = PATables::PA_433;
        // PATABLE is 8 bytes
        byte                       m_PATABLE[8]                   = {0, 0, 0, 0, 0, 0, 0, 0};
        std::shared_ptr<SpiMaster> m_spiMaster;
        CC110DeviceConfig          m_deviceConfig;

        const byte kSpiHeaderByteWriteMask = 0b01111111; // Mask out high bit.
        const byte kSpiHeaderReadBit       = 0b10000000; // OR this in to set read bit in the header
        const byte kSpiNoBurstAccessMask   = 0b10111111; // No. 2 MSB is burst bit
        const byte kSpiBurstAccessBit      = 0b01000000; // OR this to set burst bit on
        const byte kRxFifoByteCountMask    = 0b01111111; // High bit is overflow, pg 94

        //Pg 92 of datasheet
        const byte kPartNumber = 0x0;
        const byte kChipVersion = 0x14;

        // State variables
        bool m_dataReceived = false;

      public:
        CC1101Device();
        ~CC1101Device();
        bool Init(std::shared_ptr<SpiMaster> spiMaster, CC110DeviceConfig &deviceConfig);
        void Reset();
        bool BeginReceive();
        void Update();
        void SetFrequencyMHz(float frequencyMHz);
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
        void SetAppendStatus(bool shouldEnable);

      protected:
        bool lowerChipSelect();
        bool raiseChipSelect();
        void waitForMisoLow();
        bool digitalWrite(int pin, uint32_t value);
        void delayMilliseconds(int millis);
        void delayMicroseconds(int micros);
        [[nodiscard]] byte readRegister(byte address);
        bool readBurstRegister(byte address, byte *buffer, int len);
        [[nodiscard]] byte writeRegister(byte address, byte value);
        void writeBurstRegister(byte address, byte *values, int valueLen);
        byte sendStrobe(byte strobeCmd);
        byte getMultiLayerInductorPower(int outPower, const byte *currentTable, int currentTableLen);
        byte getWireWoundInductorPower(int outPower, const byte *currentTable, int currentTableLen);
        void configure();

        void handleCommonStatusCodes(byte status);
        void readRXFIFO(byte* buffer, int expectedCount); // will reset FIFO if overflowed.

        static void gpioISR(void *);
    };
} // namespace TI_CC1101