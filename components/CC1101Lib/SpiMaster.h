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
#include "LocalTypes.h"
#ifdef ARDUINO
#include <stddef.h>
#else
#include <memory.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#endif

namespace TI_CC1101
{

  // https://en.wikipedia.org/wiki/Serial_Peripheral_Interface, see table Mode numbers
  enum class SpiMode : uint8_t
  {
    ModeZero = 0, // Clock Polarity 0, Clock Phase 0
    ModeOne = 1,  // Clock Polarity 0, Clock Phase 1
    ModeTwo = 2,  // Clock Polarity 1, Clock Phase 0
    ModeThree = 3 // Clock Polarity 1, Clock Phase 1
  };
  enum class Esp32SPIHost
  {
    HOST_HSPI = 0,
    HOST_VSPI = 1
  };
  #ifdef ARDUINO
  typedef void* spi_device_handle_t;
  #endif
  struct SpiConfig
  {
    gpio_num_t misoPin;
    gpio_num_t mosiPin;
    gpio_num_t clockPin;
    gpio_num_t chipSelectPin;
    int        clockFrequencyHz;
    int        queueSize; // number of parallel transactions

    SpiMode spiMode;
    Esp32SPIHost spiHost;
  };

  class SpiMaster
  {
    protected:
      spi_device_handle_t m_DeviceHandle;

      const int kDmaChannelToUse = 0; // no DMA (for now ?)
      SpiConfig m_config;

    public:
      SpiMaster();
      ~SpiMaster();
      bool Init(const SpiConfig &cfg);

      gpio_num_t MisoPin() { return m_config.misoPin; }
      gpio_num_t MosiPin() { return m_config.mosiPin; }
      gpio_num_t ClockPin() { return m_config.clockPin; }
      gpio_num_t ChipSelectPin() { return m_config.chipSelectPin; }

      bool WriteByte(byte toWrite,byte& outData);
      bool WriteByteToAddress(byte address, byte value, byte&  outData);
      bool WriteBytesToAddress(byte address,byte *toWrite, size_t arrayLen, byte&  outData);
      bool ReadBurstRegister(byte address,byte *toRead, size_t arrayLen);
      bool ReadRegister(byte addr, byte& outData);
      void lowerChipSelect();
      void raiseChipSelect();
      void waitForMisoLow();

    protected:
#ifndef ARDUINO
      inline void intializeDefaultTransaction(spi_transaction_t &transToInitialize) { memset(&transToInitialize, 0, sizeof(transToInitialize)); }
      inline void startTransaction(){}
      inline void endTransaction(){}
#else  
      void startTransaction();
      void endTransaction();
    #endif
  };

}//namespace