#include "CC1101Lib.h"
#include "SpiDevice.h"

namespace TI_CC1101
{

SpiDevice::SpiDevice()
{
}

SpiDevice::~SpiDevice()
{
    spi_bus_remove_device(m_DeviceHandle);
    spi_bus_free(m_config.spiHost == Esp32SPIHost::HSPI ? HSPI_HOST : VSPI_HOST);
}

bool SpiDevice::Init(const SpiConfig &cfg)
{
    bool bRet = true;
    esp_err_t ret;

    m_config = cfg;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
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
        // No command bits
        .command_bits = 0,
        .address_bits = 0,
        .mode = static_cast<uint8_t>(cfg.spiMode),
        .clock_speed_hz = cfg.clockFrequencyMHz*(1'000'000),
        .spics_io_num = cfg.chipSelectPin,
        .queue_size = cfg.queueSize,
        .pre_cb = nullptr,
        .post_cb = nullptr
    };
#pragma GCC diagnostic pop

    spi_host_device_t host_id = (cfg.spiHost == Esp32SPIHost::HSPI ? HSPI_HOST : VSPI_HOST);
    ret = spi_bus_initialize(host_id,  &busConfig,kDmaChannelToUse);
    CERA(ret);

    ret = spi_bus_add_device(host_id,&deviceConfig,&m_DeviceHandle);
    CERA(ret);

Error:
    return bRet;
}
bool SpiDevice::WriteByte(byte toWrite)
{
    byte statusByte;
    spi_transaction_t transaction = {
        .flags = 0,
        .length = 8,
        .rxlength = 0,
        .tx_buffer = &toWrite,
    };

    return (spi_device_transmit(m_DeviceHandle,&transaction) == ESP_OK);
}

bool SpiDevice::WriteByteToAddress(byte address, byte value)
{
    return false;
}

}//namespace