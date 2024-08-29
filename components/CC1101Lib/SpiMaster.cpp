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
#include <esp_log.h>
#include "CC1101Lib.h"
#include "SpiMaster.h"

static const char *TAG = "SpiMaster";

// TODO do something better about this stupid requirement
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
namespace TI_CC1101
{

SpiMaster::SpiMaster()
{
}

SpiMaster::~SpiMaster()
{
    spi_bus_remove_device(m_DeviceHandle);
    spi_bus_free(m_config.spiHost == Esp32SPIHost::HSPI ? HSPI_HOST : VSPI_HOST);
}

bool SpiMaster::Init(const SpiConfig &cfg)
{
    bool bRet = true;
    esp_err_t ret;

    m_config = cfg;
    spi_bus_config_t busConfig = {
        .mosi_io_num = cfg.mosiPin,
        .miso_io_num = cfg.misoPin,
        .sclk_io_num = cfg.clockPin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = 0,
        .flags = 0,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };
    spi_device_interface_config_t deviceConfig = {
        .command_bits = 0,// No command bits
        .address_bits = 0,
        .mode = static_cast<uint8_t>(cfg.spiMode),
        .clock_speed_hz = cfg.clockFrequencyHz,
        .spics_io_num = cfg.chipSelectPin,
        .queue_size = cfg.queueSize,
        .pre_cb = nullptr,
        .post_cb = nullptr
    };

    spi_host_device_t host_id = (cfg.spiHost == Esp32SPIHost::HSPI ? HSPI_HOST : VSPI_HOST);
    ret = spi_bus_initialize(host_id,  &busConfig,kDmaChannelToUse);

    ESP_LOGI(TAG, "spi_bus_initialize() returned %d", ret);
    CERA(ret);

    ret = spi_bus_add_device(host_id,&deviceConfig,&m_DeviceHandle);
    ESP_LOGI(TAG, "spi_bus_add_device() returned %d", ret);
    CERA(ret);

Error:
    if(!bRet)
    {
        ESP_LOGE(TAG, "%s failed", __FUNCTION__);
    }
    return bRet;
}
byte SpiMaster::WriteByte(byte toWrite)
{
    bool              bRet;
    byte              statusByte = 0;
    spi_transaction_t transaction = {.flags     = SPI_TRANS_USE_RXDATA,
                                     .length    = 8,
                                     .rxlength  = 1,
                                     .tx_buffer = &toWrite};

    CERA(spi_device_transmit(m_DeviceHandle, &transaction));
    statusByte = transaction.rx_data[0];
Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, statusByte = 0x%X", __FUNCTION__,statusByte);
        statusByte = 0;
    }
    return statusByte;
}

byte SpiMaster::WriteByteToAddress(byte address, byte value)
{
    bool              bRet;
    byte              statusByte = 0;
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
        .rxlength = 1,
        .tx_data = {address, value, 0, 0}
    };
    CERA(spi_device_transmit(m_DeviceHandle,&transaction));
    statusByte = transaction.rx_data[0];
Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, statusByte = 0x%X", __FUNCTION__,statusByte);
        statusByte = 0;
    }
    return statusByte;
}

byte SpiMaster::WriteBytes(byte *toWrite, size_t arrayLen)
{
    bool              bRet;
    byte              statusByte = 0;
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = arrayLen*8,
        .rxlength = 1,
        .tx_buffer = toWrite
    };
    CERA(spi_device_transmit(m_DeviceHandle,&transaction));
    statusByte = transaction.rx_data[0];
Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, statusByte = 0x%X", __FUNCTION__,statusByte);
        statusByte = 0;
    }
    return statusByte;
}

}//namespace
#pragma GCC diagnostic pop