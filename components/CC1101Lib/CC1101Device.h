#pragma once
#include <driver/spi_master.h>
#include <vector>
#include <memory>
#include "CC1101Lib.h"

namespace TI_CC1101
{
    class SpiDevice;
    
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
        std::shared_ptr<SpiDevice> m_spiDevice;

      public:
        CC1101Device();
        ~CC1101Device();
        void Init(std::shared_ptr<SpiDevice> spiDevice,float crystalFrequencyHz = 0);
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
        void waitForMisoLow();
        void digitalWrite();
        void delayMilliseconds(int millis);
        void delayMicroseconds(int micros);
        byte readRegister(byte address);
        void writeRegister(byte address, byte value);
        void writeBurstRegister(byte address, byte* values,int valueLen);
        byte setMultiLayerInductorPower(int outPower, const byte* currentTable, int currentTableLen);
        byte setWireWoundInductorPower(int outPower, const byte* currentTable, int currentTableLen);
    };
} // namespace TI_CC1101