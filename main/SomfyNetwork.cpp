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

#include "SomfyNetwork.h"

#include <CC1101Lib/LocalTypes.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <esp_wifi.h>
#include <nvs_flash.h>

static const char *TAG = "SomfyNetwork";

bool               SomfyNetwork::Start(uint32_t remoteAddress, uint64_t nowMs)
{
    bool               bRet = true;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t      ap{};
    Credentials        saved{};
    size_t             size = sizeof(saved);
    esp_err_t          readResult;

    m_requests = xQueueCreate(1, sizeof(Request)); // One pending setup operation.
    m_status   = xQueueCreate(1, sizeof(Status));  // Latest state for the HTTP task.
    CBR(m_requests != nullptr);
    CBR(m_status != nullptr);
    CER(nvs_flash_init());
    CER(nvs_open("somfy_wifi", NVS_READWRITE, &m_storage));
    snprintf(m_setupSsid, sizeof(m_setupSsid), "Somfy-Setup-%06" PRIX32, remoteAddress);
    memcpy(ap.ap.ssid, m_setupSsid, sizeof(m_setupSsid));
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.channel        = 1; // Initial 2.4 GHz setup channel.
    ap.ap.max_connection = 2; // Permit two setup clients.

    CER(esp_netif_init());
    CER(esp_event_loop_create_default());
    m_eventLoopCreated = true;
    m_accessPoint      = esp_netif_create_default_wifi_ap();
    CBR(m_accessPoint != nullptr);
    m_station = esp_netif_create_default_wifi_sta();
    CBR(m_station != nullptr);
    CER(esp_wifi_init(&init));
    m_wifiInitialized = true;
    CER(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    CER(esp_wifi_set_mode(WIFI_MODE_APSTA));
    CER(esp_wifi_set_config(WIFI_IF_AP, &ap));
    CER(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onNetworkEvent, this));
    CER(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onNetworkEvent, this));

    readResult = nvs_get_blob(m_storage, "credentials", &saved, &size);
    if (readResult == ESP_OK && size == sizeof(saved) && saved.ssid[0] &&
        saved.ssid[sizeof(saved.ssid) - 1] == '\0' && saved.password[sizeof(saved.password) - 1] == '\0')
    {
        CER(esp_wifi_set_mode(WIFI_MODE_STA));
        CER(esp_wifi_start());
        m_wifiStarted = true;
        join(saved, false, nowMs);
    }
    else
    {
        CER(esp_wifi_set_mode(WIFI_MODE_AP));
        CER(esp_wifi_start());
        m_wifiStarted = true;
        openSetup(Failure::None);
    }

Error:
    if (!bRet)
    {
        Stop();
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
    }
    return bRet;
}

void SomfyNetwork::Stop()
{
    if (m_wifiStarted)
    {
        esp_wifi_stop();
        m_wifiStarted = false;
    }
    if (m_wifiInitialized)
    {
        esp_wifi_deinit();
        m_wifiInitialized = false;
    }
    if (m_station)
    {
        esp_netif_destroy_default_wifi(m_station);
        m_station = nullptr;
    }
    if (m_accessPoint)
    {
        esp_netif_destroy_default_wifi(m_accessPoint);
        m_accessPoint = nullptr;
    }
    if (m_eventLoopCreated)
    {
        esp_event_loop_delete_default();
        m_eventLoopCreated = false;
    }
    if (m_storage)
    {
        nvs_close(m_storage);
        m_storage = 0;
    }
    if (m_requests)
    {
        vQueueDelete(m_requests);
        m_requests = nullptr;
    }
    if (m_status)
    {
        vQueueDelete(m_status);
        m_status = nullptr;
    }
}

void SomfyNetwork::publish(Mode mode, Failure failure, esp_ip4_addr_t address)
{
    m_current = {mode, failure, address};
    xQueueOverwrite(m_status, &m_current);
}

void SomfyNetwork::openSetup(Failure failure)
{
    m_hasIp = false;
    m_retry = false;
    if (esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK)
    {
        publish(Mode::Setup, failure, {});
        ESP_LOGI(TAG, "Wi-Fi setup: join %s and open http://192.168.4.1/", m_setupSsid);
    }
    else
    {
        publish(Mode::Unavailable, Failure::SetupFailed, {});
        ESP_LOGE(TAG, "Could not open Wi-Fi setup");
    }
}

void SomfyNetwork::join(const Credentials &credentials, bool trial, uint64_t nowMs)
{
    bool          bRet = true;
    wifi_config_t station{};

    m_hasIp       = false;
    m_retry       = false;
    m_trial       = trial;
    m_credentials = credentials;
    memcpy(station.sta.ssid, credentials.ssid, strlen(credentials.ssid));
    memcpy(station.sta.password, credentials.password, strlen(credentials.password));

    if (trial)
    {
        CER(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
    CER(esp_wifi_set_config(WIFI_IF_STA, &station));
    CER(esp_wifi_connect());
    m_deadline  = nowMs + 25'000; // Allow 25 seconds for association and DHCP.
    m_nextRetry = nowMs;
    publish(Mode::Joining, Failure::None, {});

Error:
    if (!bRet)
    {
        openSetup(Failure::JoinFailed);
    }
}

void SomfyNetwork::onNetworkEvent(void *arg, esp_event_base_t base, int32_t id, void *)
{
    SomfyNetwork &network = *static_cast<SomfyNetwork *>(arg);

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        network.m_hasIp = true;
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        network.m_hasIp = false;
        network.m_retry = true;
    }
}

void SomfyNetwork::pollJoin(uint64_t nowMs)
{
    esp_netif_ip_info_t ip{};
    esp_err_t           saved;

    if (m_hasIp && esp_netif_get_ip_info(m_station, &ip) == ESP_OK && ip.ip.addr)
    {
        if (m_trial)
        {
            saved = nvs_set_blob(m_storage, "credentials", &m_credentials, sizeof(m_credentials));
            if (saved == ESP_OK)
            {
                saved = nvs_commit(m_storage);
            }
            if (saved == ESP_OK)
            {
                // Leave the setup AP up long enough to display the LAN address.
                m_deadline = nowMs + 10'000;
                publish(Mode::Joined, Failure::None, ip.ip);
            }
            else
            {
                openSetup(Failure::SaveFailed);
            }
        }
        else
        {
            publish(Mode::Online, Failure::None, ip.ip);
        }
        if (m_current.mode == Mode::Joined || m_current.mode == Mode::Online)
        {
            ESP_LOGI(TAG, "LAN HTTP=http://" IPSTR "/", IP2STR(&ip.ip));
        }
    }
    else if (nowMs >= m_deadline)
    {
        openSetup(Failure::JoinFailed);
    }
    else if (nowMs >= m_nextRetry && m_retry.exchange(false))
    {
        m_nextRetry = nowMs + 3'000; // Space out retries after disconnect events.
        esp_wifi_connect();
    }
}

void SomfyNetwork::Poll(uint64_t nowMs)
{
    Request request;

    if (xQueueReceive(m_requests, &request, 0) == pdTRUE)
    {
        if (request.reopen && m_current.mode == Mode::Online)
        {
            m_deadline = nowMs + 1'000; // Give the LAN response time to reach the browser.
            publish(Mode::Reopening, Failure::None, {});
        }
        else if (!request.reopen && m_current.mode == Mode::Setup)
        {
            join(request.credentials, true, nowMs);
        }
    }

    switch (m_current.mode)
    {
        case Mode::Joining:
            pollJoin(nowMs);
            break;
        case Mode::Joined:
            if (!m_hasIp)
            {
                openSetup(Failure::JoinFailed);
            }
            else if (nowMs >= m_deadline)
            {
                if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK)
                {
                    publish(Mode::Online, Failure::None, m_current.address);
                }
                else
                {
                    openSetup(Failure::SetupFailed);
                }
            }
            break;
        case Mode::Online:
            if (!m_hasIp)
            {
                join(m_credentials, false, nowMs);
            }
            break;
        case Mode::Reopening:
            if (nowMs >= m_deadline)
            {
                openSetup(Failure::None);
            }
            break;
        case Mode::Setup:
        case Mode::Unavailable:
            break;
    }
}

SomfyNetwork::Status SomfyNetwork::GetStatus() const
{
    Status status{};

    if (m_status)
    {
        xQueuePeek(m_status, &status, 0);
    }
    return status;
}

const char *SomfyNetwork::SetupSsid() const
{
    return m_setupSsid;
}

bool SomfyNetwork::RequestSetup()
{
    const Request request{true, {}};
    bool          bRet = true;

    CBR(GetStatus().mode == Mode::Online);
    CBR(xQueueSend(m_requests, &request, 0) == pdTRUE);

Error:
    return bRet;
}

bool SomfyNetwork::RequestJoin(const char *ssid, const char *password)
{
    Request      request{};
    bool         bRet           = true;
    const size_t ssidLength     = strlen(ssid);
    const size_t passwordLength = strlen(password);

    CBR(ssidLength > 0 && ssidLength < sizeof(request.credentials.ssid));
    CBR(passwordLength < sizeof(request.credentials.password));
    CBR(passwordLength == 0 || passwordLength >= 8); // WPA passphrases start at eight bytes.
    CBR(GetStatus().mode == Mode::Setup);
    memcpy(request.credentials.ssid, ssid, ssidLength);
    memcpy(request.credentials.password, password, passwordLength);
    CBR(xQueueSend(m_requests, &request, 0) == pdTRUE);

Error:
    return bRet;
}
