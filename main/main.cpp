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

#include <stdio.h>
#include <memory>
#include <driver/spi_master.h>
#include <esp_log.h>
#include<CC1101Lib/SpiMaster.h>
#include <CC1101Lib/CC1101Lib.h>
#include <CC1101Lib/CC1101Device.h>

static const char *TAG = "main";
using namespace TI_CC1101;

extern "C" void app_main(void)
{
    CC1101Device cc1101Device; 
    auto spiMaster = std::make_shared<SpiMaster>();
    SpiConfig spiConfig = {
        .misoPin = GPIO_NUM_19,
        .mosiPin = GPIO_NUM_23,
        .clockPin = GPIO_NUM_18,
        .chipSelectPin = GPIO_NUM_5,
        .clockFrequencyHz = 4'000'000,
        .queueSize = 1,
        .spiMode = SpiMode::ModeZero,
        .spiHost = Esp32SPIHost::VSPI
    };
    ESP_LOGI(TAG, "Initializing SPI");
    spiMaster->Init(spiConfig);

    ESP_LOGI(TAG, "Initializing CC1101");
    cc1101Device.Init(spiMaster);
}