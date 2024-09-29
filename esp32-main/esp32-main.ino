#include "src/CC1101Lib/SpiMaster.h"
#include "src/CC1101Lib/CC1101Lib.h"
#include "src/CC1101Lib/CC1101Device.h"

using namespace TI_CC1101;

CC1101Device cc1101Device;
void setup() {

  auto spiMaster = std::make_shared<SpiMaster>();

  //https://randomnerdtutorials.com/esp32-pinout-reference-gpios/, VSPI Pinout
  SpiConfig spiConfig = {
    .misoPin = GPIO_NUM_19,
    .mosiPin = GPIO_NUM_23,
    .clockPin = GPIO_NUM_18,
    .chipSelectPin = GPIO_NUM_5,
    .clockFrequencyHz = 4'000'000,
    .queueSize = 8,
    .spiMode = SpiMode::ModeZero,
    .spiHost = Esp32SPIHost::HOST_VSPI
  };
  CC110DeviceConfig somfyRadioConfig = {
    .TxPin = GPIO_NUM_13,
    .RxPin = GPIO_NUM_14,
    .InterruptQueue = nullptr
  };

  Serial.begin(9600);

  Log.begin(LOG_LEVEL_VERBOSE,&Serial);

  ESP_LOGD("main","Initializing SPI\n");
  spiMaster->Init(spiConfig);

  ESP_LOGD("main","Initializing CC1101\n");
  assert(cc1101Device.Init(spiMaster, somfyRadioConfig));

  cc1101Device.BeginReceive();
}
void loop() {
  cc1101Device.Update();
}