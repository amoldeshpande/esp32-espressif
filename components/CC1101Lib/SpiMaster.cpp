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
#include <algorithm>
#include <climits>

static const char *TAG = "SpiMaster";

namespace TI_CC1101
{
    static constexpr int      kUnusedPin              = -1; // ESP-IDF sentinel for an unassigned SPI pin.
    static constexpr size_t   kMaxNonDmaTransferBytes = 64; // ESP32 SPI hardware buffer size.
    static constexpr unsigned kResetPulseUs           = 1;  // Short CSn pulse preceding the reset strobe.
    static constexpr unsigned kResetDelayUs           = 50; // CC1101 requires at least 40 us after the CSn pulse.
    static constexpr unsigned kChipReadyPollUs        = 1;
    static constexpr unsigned kChipReadyTimeoutUs     = 10'000; // Bound the wait if the radio never drives CHIP_RDYn low.
    static constexpr byte     kDummyReadByte          = 0;      // Supply clock cycles with SI low when reading.
    // The radio returns status during the address byte and data during the following byte.
    static constexpr size_t   kRegisterStatusByte     = 0;
    static constexpr size_t   kRegisterDataByte       = 1;

    SpiMaster::SpiMaster()
    {
    }

    SpiMaster::~SpiMaster()
    {
        if (m_DeviceHandle)
        {
            spi_bus_remove_device(m_DeviceHandle);
            spi_bus_free(m_config.spiHost == Esp32SPIHost::HOST_HSPI ? HSPI_HOST : VSPI_HOST);
        }
    }

    bool SpiMaster::Init(const SpiConfig &cfg)
    {
        bool                          bRet = true;
        esp_err_t                     ret;
        spi_bus_config_t              busConfig{};
        spi_device_interface_config_t deviceConfig{};
        const spi_host_device_t       host_id = (cfg.spiHost == Esp32SPIHost::HOST_HSPI ? HSPI_HOST : VSPI_HOST);

        m_config                              = cfg;
        busConfig.mosi_io_num                 = cfg.mosiPin;
        busConfig.miso_io_num                 = cfg.misoPin;
        busConfig.sclk_io_num                 = cfg.clockPin;
        busConfig.quadwp_io_num               = kUnusedPin;
        busConfig.quadhd_io_num               = kUnusedPin;
        busConfig.data4_io_num                = kUnusedPin;
        busConfig.data5_io_num                = kUnusedPin;
        busConfig.data6_io_num                = kUnusedPin;
        busConfig.data7_io_num                = kUnusedPin;
        busConfig.flags                       = SPICOMMON_BUSFLAG_MASTER;
        busConfig.isr_cpu_id                  = ESP_INTR_CPU_AFFINITY_AUTO;

        deviceConfig.mode                     = static_cast<uint8_t>(cfg.spiMode);
        deviceConfig.clock_source             = SPI_CLK_SRC_DEFAULT;
        deviceConfig.clock_speed_hz           = cfg.clockFrequencyHz;
        deviceConfig.spics_io_num             = kUnusedPin; // Chip select is managed by this driver.
        deviceConfig.queue_size               = cfg.queueSize;

        // Keep CSn and SCLK high and SI low for the CC1101 power-up reset sequence.
        gpio_reset_pin(cfg.chipSelectPin);
        gpio_set_level(cfg.chipSelectPin, 1);
        gpio_set_direction(cfg.chipSelectPin, GPIO_MODE_OUTPUT);
        gpio_reset_pin(cfg.clockPin);
        gpio_set_level(cfg.clockPin, 1);
        gpio_set_direction(cfg.clockPin, GPIO_MODE_OUTPUT);
        gpio_reset_pin(cfg.mosiPin);
        gpio_set_level(cfg.mosiPin, 0);
        gpio_set_direction(cfg.mosiPin, GPIO_MODE_OUTPUT);
        lowerChipSelect();
        delayMicroseconds(kResetPulseUs);
        raiseChipSelect();
        delayMicroseconds(kResetDelayUs);

        ret = spi_bus_initialize(host_id, &busConfig, kDmaChannelToUse);

        ESP_LOGI(TAG, "spi_bus_initialize() returned %d", ret);
        CERA(ret);

        ret = spi_bus_add_device(host_id, &deviceConfig, &m_DeviceHandle);
        ESP_LOGI(TAG, "spi_bus_add_device() returned %d", ret);
        if (ret != ESP_OK)
        {
            spi_bus_free(host_id);
        }
        CERA(ret);

    Error:
        if (!bRet)
        {
            ESP_LOGE(TAG, "%s failed", __PRETTY_FUNCTION__);
        }
        return bRet;
    }
    bool SpiMaster::transferSelected(const byte *tx, byte *rx, size_t byteCount)
    {
        spi_transaction_t transaction{};
        esp_err_t         result;
        bool              bRet;

        transaction.length    = byteCount * CHAR_BIT; // ESP-IDF takes the transfer length in bits.
        transaction.tx_buffer = tx;
        transaction.rx_buffer = rx;

        result                = spi_device_polling_transmit(m_DeviceHandle, &transaction);
        bRet                  = result == ESP_OK;
        if (!bRet)
        {
            ESP_LOGE(TAG, "SPI transfer failed: %s", esp_err_to_name(result));
        }
        return bRet;
    }

    bool SpiMaster::transfer(const byte *tx, byte *rx, size_t byteCount)
    {
        bool bRet;

        lowerChipSelect();
        bRet = waitForMisoLow();
        if (bRet)
        {
            bRet = transferSelected(tx, rx, byteCount);
        }
        raiseChipSelect();
        return bRet;
    }

    bool SpiMaster::WriteByte(byte toWrite, byte &outData)
    {
        return transfer(&toWrite, &outData, sizeof(toWrite));
    }

    bool SpiMaster::WriteByteToAddress(byte address, byte value, byte &outData)
    {
        const byte tx[] = {address, value};
        byte       rx[sizeof(tx)]{};
        const bool bRet = transfer(tx, rx, sizeof(tx));
        if (bRet)
        {
            outData = rx[kRegisterStatusByte];
        }
        return bRet;
    }

    bool SpiMaster::transferBurst(byte address, const byte *tx, byte *rx, size_t byteCount, byte &status)
    {
        // The address and all data must be clocked under the same CS assertion.
        const byte zeros[kMaxNonDmaTransferBytes]{};
        bool       bRet;
        size_t     count;

        lowerChipSelect();
        bRet = waitForMisoLow();
        if (bRet)
        {
            bRet = transferSelected(&address, &status, sizeof(address));
        }
        for (size_t offset = 0; bRet && offset < byteCount;)
        {
            count = std::min(byteCount - offset, sizeof(zeros));
            bRet  = transferSelected(tx ? tx + offset : zeros, rx ? rx + offset : nullptr, count);
            offset += count;
        }
        raiseChipSelect();
        return bRet;
    }

    bool SpiMaster::WriteBytesToAddress(byte address, byte *toWrite, size_t arrayLen, byte &outData)
    {
        return transferBurst(address, toWrite, nullptr, arrayLen, outData);
    }

    bool SpiMaster::ReadBurstRegister(byte address, byte *toRead, size_t arrayLen)
    {
        byte status = 0;
        return transferBurst(address, nullptr, toRead, arrayLen, status);
    }

    bool SpiMaster::ReadRegister(byte address, byte &outData)
    {
        const byte tx[] = {address, kDummyReadByte};
        byte       rx[sizeof(tx)]{};
        const bool bRet = transfer(tx, rx, sizeof(tx));
        if (bRet)
        {
            outData = rx[kRegisterDataByte];
        }
        return bRet;
    }

    bool SpiMaster::ResetDevice()
    {
        const byte strobe = CC1101_CONFIG::SRES;
        byte       status = 0;
        bool       bRet;

        // Init performed the initial CS pulse. Further resets need only SRES.
        lowerChipSelect();
        bRet = waitForMisoLow();
        if (bRet)
        {
            bRet = transferSelected(&strobe, &status, sizeof(strobe));
        }
        if (bRet)
        {
            bRet = waitForMisoLow();
        }
        raiseChipSelect();
        return bRet;
    }

    void SpiMaster::lowerChipSelect()
    {
        gpio_set_level(m_config.chipSelectPin, 0);
    }

    void SpiMaster::raiseChipSelect()
    {
        gpio_set_level(m_config.chipSelectPin, 1);
    }

    bool SpiMaster::waitForMisoLow()
    {
        bool bRet = false;
        for (unsigned elapsedUs = 0; elapsedUs < kChipReadyTimeoutUs; elapsedUs += kChipReadyPollUs)
        {
            if (gpio_get_level(m_config.misoPin) == 0)
            {
                bRet = true;
                break;
            }
            delayMicroseconds(kChipReadyPollUs);
        }
        if (!bRet)
        {
            ESP_LOGE(TAG, "CC1101 did not pull MISO low after chip select");
        }
        return bRet;
    }

} // namespace TI_CC1101
