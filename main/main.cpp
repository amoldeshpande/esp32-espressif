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
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <CC1101Lib/CC1101Device.h>
#include <CC1101Lib/SpiMaster.h>
#include "SomfyReceiver.h"
#include "SomfyController.h"
#include "SomfyHttp.h"

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

    static constexpr ReceiveProfile kReceiveProfiles[] = {
        // Carrier (MHz), filter bandwidth (kHz), frequency offset (synthesizer steps).
        {433.42f, 270.8333f,  0}, // Working profile from the HiLetGo bench test.
        {433.92f, 270.8333f,  0}, // Alternate ISM carrier.
        {433.42f, 101.5625f,  0}, // Narrow filter with no offset.
        {433.42f, 101.5625f, 35}, // Narrow filter with the SmartRC offset (+55.5 kHz at 26 MHz).
    };
    static constexpr char kFirstProfileKey = '1';
    static constexpr char kDefaultProfileKey = kFirstProfileKey; // Profile validated with the HiLetGo and RTS remote.

    static constexpr unsigned kRssiSampleIntervalMs = 10;    // Sample several times during a short remote burst.
    static constexpr int      kMinRssiDbm           = -140;  // Below the lowest representable CC1101 RSSI.
} // namespace

static void     processTransmitKey(SomfyController &controller, char key);
static uint64_t monotonicMs();
static void     logTransmitStatus(const SomfyController &controller);
static void     receiveSomfyFrames(CC1101Device &cc1101Device, QueueHandle_t edgeQueue, gpio_num_t rxPin, gpio_num_t txPin);

extern "C" void app_main(void)
{
    static CC1101Device        cc1101Device;
    // 256 entries accommodate two edges per bit of an 80-bit frame, plus the preamble.
    const QueueHandle_t        edgeQueue = xQueueCreate(256, sizeof(uint32_t));
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
    const ReceiveProfile &defaultProfile = kReceiveProfiles[kDefaultProfileKey - kFirstProfileKey];
    CC110DeviceConfig somfyRadioConfig = {
        .TxPin = GPIO_NUM_13,
        .RxPin = GPIO_NUM_14,
        .CarrierFrequencyMHz       = defaultProfile.frequencyMHz,
        .ReceiveFilterBandwidthKHz = defaultProfile.bandwidthKHz,
        .FrequencyOffsetSteps      = defaultProfile.offsetSteps,
        .DisableDCFilter           = false,
        // SmartRC v2.5.7 asynchronous modem settings used by ESPSomfy-RTS.
        .ReceiveModem              = {
            .rateExponent = 7,
            .rateMantissa = 0x93, // About 5 kBaud at 26 MHz.
            .bitSync      = 0x1C, // Clock recovery starts at KI and 2KP, with data rate offset compensation disabled.
            .gainControl2 = 0xC7, // 42 dB target, all LNA settings, three highest digital gain settings disabled.
            .gainControl1 = 0x00, // Reduce LNA2 gain first. Carrier sense is at the gain target, with no relative threshold.
            .gainControl0 = 0xB2, // Medium hysteresis, 32 samples after a gain change, 12 dB OOK threshold, no gain freeze.
            .test2        = 0x81, // CC1101 analog settings for a receive bandwidth below 325 kHz.
            .test1        = 0x35,
        },
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
    receiveSomfyFrames(cc1101Device, edgeQueue, somfyRadioConfig.RxPin, somfyRadioConfig.TxPin);

Error:
    if (!bRet && edgeQueue)
    {
        ESP_LOGE(TAG, "Radio initialization failed. Check CC1101 power and SPI wiring.");
        vQueueDelete(edgeQueue);
    }
}

static void receiveSomfyFrames(CC1101Device &cc1101Device, QueueHandle_t edgeQueue, gpio_num_t rxPin, gpio_num_t txPin)
{
    bool consoleReady;
    char activeProfile = kDefaultProfileKey;
    SomfyController            controller(cc1101Device);
    static SomfyHttp           http(controller);
    SomfyReceiver::Frame       frame;
    SomfyReceiver::Result      decodeResult;
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

    if (!controller.Init(txPin))
    {
        ESP_LOGE(TAG, "Transmit setup failed; continuing with reception only");
    }
    logTransmitStatus(controller);
    if (!http.Start(monotonicMs()))
    {
        ESP_LOGE(TAG, "Continuing with serial control");
    }
    ESP_LOGI(TAG, "Listening for Somfy RTS on GPIO%d", rxPin);
    consoleReady = fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) != -1;
    if (consoleReady)
    {
        ESP_LOGI(TAG, "Keys: 1=433.42 wide, 2=433.92 wide, 3=433.42 narrow, 4=433.42 narrow +55.5 kHz");
        ESP_LOGI(TAG, "TX keys (profile 1): p=program/pair, y=confirm pairing, o=open, c=close, m=My/stop, i=virtual remote status");
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
            decodeResult = controller.OnEdge(timestamp, frame);
            if (decodeResult == SomfyReceiver::Result::Decoded)
            {
                ++frameCount;
                controller.ObserveRemote({frame.RemoteAddress(), frame.Command(), frame.RollingCode(), frame.bitCount},
                                         monotonicMs());
                ESP_LOGI(TAG, "RTS %u-bit frame, base checksum OK, remote=%06" PRIX32 " command=%u rolling=%u",
                         frame.bitCount, frame.RemoteAddress(), frame.Command(), frame.RollingCode());
            }
            else if (decodeResult == SomfyReceiver::Result::BadChecksum)
            {
                ++badChecksumCount;
            }
        }

        now = xTaskGetTickCount();
        if (now - lastRssiSample >= pdMS_TO_TICKS(kRssiSampleIntervalMs))
        {
            if (consoleReady && read(STDIN_FILENO, &key, sizeof(key)) == sizeof(key))
            {
                if (key >= kFirstProfileKey && key < kFirstProfileKey + ARRAYSIZE(kReceiveProfiles))
                {
                    profile = kReceiveProfiles[key - kFirstProfileKey];
                    if (controller.RetuneReceive(profile.frequencyMHz, profile.bandwidthKHz, profile.offsetSteps,
                                                 key == kDefaultProfileKey))
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
                }
                else
                {
                    processTransmitKey(controller, key);
                }
                edgeCount        = 0;
                frameCount       = 0;
                badChecksumCount = 0;
                peakRssi         = kMinRssiDbm;
                now              = xTaskGetTickCount();
                lastReport       = now;
            }
            http.Poll(monotonicMs());
            now = xTaskGetTickCount();
            peakRssi       = std::max(peakRssi, cc1101Device.ReadRssiDbm());
            lastRssiSample = now;
        }
        // Report the receiver statistics in two-second windows.
        if (now - lastReport >= pdMS_TO_TICKS(2'000))
        {
            sample = controller.TakeDiagnostics();
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

static void processTransmitKey(SomfyController &controller, char key)
{
    SomfyController::Command command = SomfyController::Command::My;
    bool                     selected = true;

    switch (key)
    {
        case 'p':
            command = SomfyController::Command::Program;
            break;
        case 'o':
            command = SomfyController::Command::Open;
            break;
        case 'c':
            command = SomfyController::Command::Close;
            break;
        case 'm':
            command = SomfyController::Command::My;
            break;
        case 'y':
            if (controller.ConfirmPairing(SomfyController::kFirstBlindId))
            {
                ESP_LOGI(TAG, "Blind 1 pairing confirmed");
            }
            else
            {
                ESP_LOGE(TAG, "No pending pairing could be confirmed");
            }
            selected = false;
            break;
        case 'i':
            logTransmitStatus(controller);
            selected = false;
            break;
        default:
            selected = false;
            break;
    }
    if (selected)
    {
        switch (controller.Send(SomfyController::kFirstBlindId, command))
        {
            case SomfyController::Result::NotSent:
                ESP_LOGE(TAG, "Command was not transmitted. Check TX status with i.");
                break;
            case SomfyController::Result::TransmissionUncertain:
                ESP_LOGE(TAG, "Transmission incomplete. Check the blind before retrying.");
                break;
            case SomfyController::Result::TransmittedWithCleanupError:
                ESP_LOGE(TAG, "Command transmitted, but radio cleanup failed");
                break;
            case SomfyController::Result::Transmitted:
                break;
        }
    }
}

static void logTransmitStatus(const SomfyController &controller)
{
    const SomfyController::Status status = controller.GetStatus(SomfyController::kFirstBlindId);

    ESP_LOGI(TAG, "Virtual remote=%06" PRIX32 ", next rolling=%" PRIu32 ", TX %s",
             status.address, status.nextCode, status.ready ? "ready" : "disabled");
}


static uint64_t monotonicMs()
{
    return static_cast<uint64_t>(esp_timer_get_time()) / 1'000; // Convert microseconds to milliseconds.
}
