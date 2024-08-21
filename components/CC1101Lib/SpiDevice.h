#pragma once
#include <driver/spi_master.h>
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
    int misoPin;
    int mosiPin;
    int clockPin;
    int chipSelectPin;
    int clockFrequency;
    int queueSize; // number of parallel transactions 
    
    SpiMode spiMode;
    Esp32SPIHost spiHost;
  };

  class SpiDevice
  {
    protected:
       spi_device_handle_t m_DeviceHandle; 

       const int kDmaChannelToUse = 1; // 
    public:
        SpiDevice();
        ~SpiDevice();
        bool Init(const SpiConfig& cfg);

  };

}