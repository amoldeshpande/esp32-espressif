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

#pragma once

#include <atomic>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <nvs.h>

class SomfyNetwork final
{
  public:
    enum class Mode
    {
        Unavailable,
        Setup,
        Joining,
        Joined,
        Online,
        Reopening
    };
    enum class Failure
    {
        None,
        JoinFailed,
        SaveFailed,
        SetupFailed
    };

    struct Status
    {
        Mode           mode;
        Failure        failure;
        esp_ip4_addr_t address;
    };

    bool        Start(uint32_t remoteAddress, uint64_t nowMs);
    void        Stop();
    void        Poll(uint64_t nowMs);
    Status      GetStatus() const;
    const char *SetupSsid() const;
    bool        RequestSetup();
    bool        RequestJoin(const char *ssid, const char *password);

  private:
    struct Credentials
    {
        char ssid[33];     // 32-byte SSID plus terminator.
        char password[65]; // 64-byte WPA key plus terminator.
    };
    struct Request
    {
        bool        reopen;
        Credentials credentials;
    };

    static void       onNetworkEvent(void *arg, esp_event_base_t base, int32_t id, void *data);
    void              publish(Mode mode, Failure failure, esp_ip4_addr_t address);
    void              openSetup(Failure failure);
    void              join(const Credentials &credentials, bool trial, uint64_t nowMs);
    void              pollJoin(uint64_t nowMs);

    QueueHandle_t     m_requests         = nullptr;
    QueueHandle_t     m_status           = nullptr;
    esp_netif_t      *m_accessPoint      = nullptr;
    bool              m_wifiInitialized  = false;
    bool              m_eventLoopCreated = false;
    bool              m_wifiStarted      = false;
    esp_netif_t      *m_station          = nullptr;
    nvs_handle_t      m_storage          = 0;
    char              m_setupSsid[sizeof("Somfy-Setup-FFFFFF")]{};
    Status            m_current{};
    Credentials       m_credentials{};
    std::atomic<bool> m_hasIp{false};
    std::atomic<bool> m_retry{false};
    bool              m_trial     = false;
    uint64_t          m_deadline  = 0;
    uint64_t          m_nextRetry = 0;
};
