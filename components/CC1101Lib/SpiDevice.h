#pragma once
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include "LocalTypes.h"

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
    HSPI = 0,
    VSPI = 1
  };
  struct SpiConfig
  {
    gpio_num_t misoPin;
    gpio_num_t mosiPin;
    gpio_num_t clockPin;
    gpio_num_t chipSelectPin;
    gpio_num_t clockFrequencyMHz;
    int queueSize; // number of parallel transactions 
    
    SpiMode spiMode;
    Esp32SPIHost spiHost;
  };

  class SpiDevice
  {
    protected:
       spi_device_handle_t m_DeviceHandle;

       const int kDmaChannelToUse = 0; //no DMA (for now ?)
       SpiConfig m_config;

     public:
       SpiDevice();
       ~SpiDevice();
       bool Init(const SpiConfig &cfg);

       gpio_num_t MisoPin() { return m_config.misoPin;}
       gpio_num_t MosiPin() { return m_config.mosiPin;}
       gpio_num_t ClockPin() { return m_config.clockPin;}
       gpio_num_t ChipSelectPin() { return m_config.chipSelectPin;}

       bool WriteByte(byte toWrite);
       bool WriteByteToAddress(byte address, byte value);

  };

}//namespace