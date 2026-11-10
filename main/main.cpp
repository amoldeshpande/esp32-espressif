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

#include <algorithm>
#include <cinttypes>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <CC1101Lib/CC1101Device.h>
#include <CC1101Lib/SpiMaster.h>
#include "SomfyReceiver.h"

static const char *TAG = "main";
using namespace TI_CC1101;

namespace
{
    struct ReceiveProfile
    {
        float  frequencyMHz;
        float  bandwidthKHz;
        int8_t offsetSteps;
    };

    static constexpr float          kSomfyCarrierMHz             = 433.42f;
    static constexpr float          kAlternateCarrierMHz         = 433.92f; // Common 433 MHz ISM carrier used for comparison.
    // Available CC1101 filter settings with a 26 MHz crystal.
    static constexpr float          kWideBandwidthKHz            = 270.8333f;
    static constexpr float          kNarrowBandwidthKHz          = 101.5625f;
    static constexpr int8_t         kNoFrequencyOffset           = 0;
    static constexpr int8_t         kSmartRcFrequencyOffsetSteps = 35; // About +55.5 kHz at 26 MHz.

    static constexpr ReceiveProfile kReceiveProfiles[]           = {
        {kSomfyCarrierMHz, kWideBandwidthKHz, kNoFrequencyOffset},
        {kAlternateCarrierMHz, kWideBandwidthKHz, kNoFrequencyOffset},
        {kSomfyCarrierMHz, kNarrowBandwidthKHz, kNoFrequencyOffset},
        {kSomfyCarrierMHz, kNarrowBandwidthKHz, kSmartRcFrequencyOffsetSteps},
    };
    static constexpr char kFirstProfileKey = '1';
    static constexpr char kLastProfileKey  = kFirstProfileKey + ARRAYSIZE(kReceiveProfiles) - 1;
#ifndef SOMFY_LEGACY_RF
    static constexpr char           kDefaultProfileKey     = kFirstProfileKey; // Profile validated with the HiLetGo and RTS remote.
    static constexpr ReceiveProfile kDefaultReceiveProfile = kReceiveProfiles[kDefaultProfileKey - kFirstProfileKey];
#endif

    // An 80-bit Manchester frame can have two edges per bit, plus the preamble.
    static constexpr unsigned kEdgeQueueDepth       = 256;
    static constexpr unsigned kRssiSampleIntervalMs = 10;    // Sample several times during a short remote burst.
    static constexpr unsigned kReportIntervalMs     = 2'000; // Group samples into the console's two-second windows.
    static constexpr int      kMinRssiDbm           = -140;  // Below the lowest representable CC1101 RSSI.
} // namespace

static void     receiveSomfyFrames(CC1101Device &cc1101Device, QueueHandle_t edgeQueue, gpio_num_t rxPin);

extern "C" void app_main(void)
{
    static CC1101Device        cc1101Device;
    const QueueHandle_t        edgeQueue = xQueueCreate(kEdgeQueueDepth, sizeof(uint32_t));
    std::shared_ptr<SpiMaster> spiMaster;
    bool                       bRet = true;
    const SpiConfig            spiConfig = {
                   .misoPin          = GPIO_NUM_19,
                   .mosiPin          = GPIO_NUM_23,
                   .clockPin         = GPIO_NUM_18,
                   .chipSelectPin    = GPIO_NUM_5,
                   .clockFrequencyHz = 4'000'000, // Original SPI clock for this wiring.
                   .queueSize        = 8,         // Original ESP-IDF device queue depth. Register transfers poll synchronously.
                   .spiMode          = SpiMode::ModeZero,
                   .spiHost          = Esp32SPIHost::HOST_VSPI};
    CC110DeviceConfig somfyRadioConfig = {
        .TxPin = GPIO_NUM_13,
        .RxPin = GPIO_NUM_14,
#ifndef SOMFY_LEGACY_RF
        .CarrierFrequencyMHz       = kDefaultReceiveProfile.frequencyMHz,
        .ReceiveFilterBandwidthKHz = kDefaultReceiveProfile.bandwidthKHz,
        .FrequencyOffsetSteps      = kDefaultReceiveProfile.offsetSteps,
        .DisableDCFilter           = false,
        .UseSomfyRtsRxProfile      = true,
#endif
        .InterruptQueue = edgeQueue};

    if (!edgeQueue)
    {
        ESP_LOGE(TAG, "Could not allocate the radio edge queue");
    }
    CBR(edgeQueue != nullptr);

    spiMaster = std::make_shared<SpiMaster>();
    CBRA(spiMaster->Init(spiConfig));
    CBRA(cc1101Device.Init(spiMaster, somfyRadioConfig));
    CBRA(cc1101Device.BeginReceive());
    receiveSomfyFrames(cc1101Device, edgeQueue, somfyRadioConfig.RxPin);

Error:
    if (!bRet && edgeQueue)
    {
        ESP_LOGE(TAG, "Radio initialization failed. Check CC1101 power and SPI wiring.");
        vQueueDelete(edgeQueue);
    }
}

static void receiveSomfyFrames(CC1101Device &cc1101Device, QueueHandle_t edgeQueue, gpio_num_t rxPin)
{
    bool consoleReady;
#ifdef SOMFY_LEGACY_RF
    char activeProfile = 'L';
#else
    char activeProfile = kDefaultProfileKey;
#endif
    SomfyReceiver              receiver;
    SomfyReceiver::Frame       frame;
    SomfyReceiver::Diagnostics sample;
    ReceiveProfile             profile;
    uint32_t                   timestamp;
    uint32_t                   edgeCount        = 0;
    uint32_t                   frameCount       = 0;
    uint32_t                   badChecksumCount = 0;
    TickType_t                 lastReport;
    TickType_t                 lastRssiSample;
    TickType_t                 now;
    int                        peakRssi = kMinRssiDbm;
    char                       key;

    ESP_LOGI(TAG, "Listening for Somfy RTS on GPIO%d", rxPin);
    consoleReady = fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) != -1;
    if (consoleReady)
    {
        ESP_LOGI(TAG, "Keys: 1=433.42 wide, 2=433.92 wide, 3=433.42 narrow, 4=433.42 narrow +55.5 kHz");
    }
    else
    {
        ESP_LOGW(TAG, "Serial input unavailable; the RX profile is fixed");
    }
    lastReport     = xTaskGetTickCount();
    lastRssiSample = lastReport;
    while (true)
    {
        if (xQueueReceive(edgeQueue, &timestamp, pdMS_TO_TICKS(kRssiSampleIntervalMs)) == pdTRUE)
        {
            ++edgeCount;
            if (receiver.OnEdge(timestamp, frame))
            {
                if (SomfyReceiver::DecodeBase(frame))
                {
                    ++frameCount;
                    ESP_LOGI(TAG, "RTS %u-bit frame, base checksum OK, remote=%06" PRIX32 " command=%u rolling=%u",
                             frame.bitCount, frame.RemoteAddress(), frame.Command(), frame.RollingCode());
                }
                else
                {
                    ++badChecksumCount;
                }
            }
        }

        now = xTaskGetTickCount();
        if (now - lastRssiSample >= pdMS_TO_TICKS(kRssiSampleIntervalMs))
        {
            if (consoleReady && read(STDIN_FILENO, &key, sizeof(key)) == sizeof(key) &&
                key >= kFirstProfileKey && key <= kLastProfileKey)
            {
                profile = kReceiveProfiles[key - kFirstProfileKey];
                if (cc1101Device.RetuneReceive(profile.frequencyMHz, profile.bandwidthKHz, profile.offsetSteps))
                {
                    activeProfile = key;
                    ESP_LOGI(TAG, "RX profile %c: %.3f MHz, %.2f kHz BW, offset steps=%d",
                             key, profile.frequencyMHz, profile.bandwidthKHz, profile.offsetSteps);
                }
                else
                {
                    activeProfile = '?';
                    ESP_LOGE(TAG, "Failed to set RX profile %c", key);
                }
                receiver         = {};
                edgeCount        = 0;
                frameCount       = 0;
                badChecksumCount = 0;
                peakRssi         = kMinRssiDbm;
                now              = xTaskGetTickCount();
                lastReport       = now;
            }
            peakRssi       = std::max(peakRssi, cc1101Device.ReadRssiDbm());
            lastRssiSample = now;
        }
        if (now - lastReport >= pdMS_TO_TICKS(kReportIntervalMs))
        {
            sample = receiver.TakeDiagnostics();
            ESP_LOGI(TAG, "RX 2s: profile=%c, edges=%" PRIu32 ", peak RSSI=%d dBm, syncs=%u, timing aborts=%u, max bits=%u,"
                          " checksum OK=%" PRIu32 ", checksum failed=%" PRIu32,
                     activeProfile, edgeCount, peakRssi, sample.syncs, sample.timingAborts, sample.mostBits,
                     frameCount, badChecksumCount);
            edgeCount        = 0;
            peakRssi         = kMinRssiDbm;
            frameCount       = 0;
            badChecksumCount = 0;
            lastReport       = now;
        }
    }
}
