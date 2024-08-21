#include "CC1101Lib.h"
#include "SpiDevice.h"

bool TI_CC1101::SpiDevice::Init(const SpiConfig &cfg)
{
    bool bRet = true;
    esp_err_t ret;

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
        .mode = static_cast<uint8_t>(cfg.spiMode),
        .clock_speed_hz = cfg.clockFrequency,
        .spics_io_num = cfg.chipSelectPin,
        .queue_size = cfg.queueSize
    };

    spi_host_device_t host_id = (cfg.spiHost == Esp32SPIHost::HSPI ? HSPI_HOST : VSPI_HOST);
    ret = spi_bus_initialize(host_id,  &busConfig,kDmaChannelToUse);
    CERA(ret == ESP_OK);

    ret = spi_bus_add_device(host_id,&deviceConfig,&m_DeviceHandle);
    CERA(ret == ESP_OK);

Error:
    return bRet;
}