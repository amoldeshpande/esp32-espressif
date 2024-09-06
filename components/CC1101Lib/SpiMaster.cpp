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
#include "CC1101Lib.h"
#include "SpiMaster.h"

static const char *TAG = "SpiMaster";

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
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };
    spi_device_interface_config_t deviceConfig = {
        .command_bits = 0,// No command bits
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = static_cast<uint8_t>(cfg.spiMode),
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .duty_cycle_pos = 0,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = cfg.clockFrequencyHz,
        .input_delay_ns = 0,
        .spics_io_num = -1,// Required because we'll be managing the CS high/low ourselves.
        .flags = 0,
        .queue_size = cfg.queueSize,
        .pre_cb = nullptr,
        .post_cb = nullptr
    };

    gpio_reset_pin(cfg.chipSelectPin);
    gpio_set_direction(cfg.chipSelectPin, GPIO_MODE_OUTPUT);
    gpio_set_level(cfg.chipSelectPin, 1);

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
        ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
    }
    return bRet;
}
bool SpiMaster::WriteByte(byte toWrite, byte& outData)
{
    bool              bRet = true;
    esp_err_t         retCode;
    spi_transaction_t transaction;

    intializeDefaultTransaction(transaction);
    transaction.length    = 8;
    transaction.tx_buffer = &toWrite;
    transaction.rx_buffer = &outData;

    retCode = spi_device_transmit(m_DeviceHandle, &transaction);
    CERA(retCode);

Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, spi_device_transmit returned -> 0x%X", __PRETTY_FUNCTION__,retCode);
    }
    return bRet;
}

bool SpiMaster::WriteByteToAddress(byte address, byte value, byte&  outData)
{
    bool              bRet = true;
    esp_err_t         retCode;
    spi_transaction_t transaction;

    intializeDefaultTransaction(transaction);
    transaction.flags      = SPI_TRANS_USE_TXDATA;
    transaction.length  = 16;
    transaction.tx_data[0] = address;
    transaction.tx_data[1] = value;
    transaction.rx_buffer = &outData;
    retCode = spi_device_transmit(m_DeviceHandle, &transaction);
    CERA(retCode);
Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, spi_device_transmit returned ->  0x%X", __PRETTY_FUNCTION__,retCode);
    }
    return bRet;
}

bool SpiMaster::WriteBytes(byte *toWrite, size_t arrayLen, byte& outData)
{
    bool              bRet = true;
    esp_err_t         retCode;
    spi_transaction_t transaction;

    intializeDefaultTransaction(transaction);
    transaction.length    = arrayLen * 8;
    transaction.tx_buffer = toWrite;
    transaction.rx_buffer = &outData;
    retCode = spi_device_transmit(m_DeviceHandle, &transaction);
    CERA(retCode);
Error:
    if (!bRet)
    {
        ESP_LOGE(TAG, "%s failed, spi_device_transmit returned -> 0x%X", __PRETTY_FUNCTION__,retCode);
    }
    return bRet;
}

}//namespace