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

#include "SomfyFirmware.h"

#include <CC1101Lib/LocalTypes.h>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "SomfyFirmware";

esp_err_t SomfyFirmware::Upload(httpd_req_t *request)
{
    bool bRet = true;
    bool reserved = false;
    bool otaOpen = false;
    State expected = State::Idle;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t handle{};
    esp_app_desc_t image{};
    const char *status = "409 Conflict";
    const char *body = R"({"error":"ota_partition_unavailable"})";
    unsigned char buffer[1024]; // Stream one KiB at a time on the HTTP task.
    size_t remaining = request->content_len;
    int received;
    int64_t deadline;
    esp_err_t result;

    CBR(slot != nullptr);
    status = "400 Bad Request";
    body = R"({"error":"invalid_image_size"})";
    CBR(remaining > 0 && remaining <= slot->size);
    status = "409 Conflict";
    body = R"({"error":"radio_or_update_busy"})";
    CBR(m_state.compare_exchange_strong(expected, State::Requested));
    reserved = true;
    // The radio task grants access between operations. A group command can take several seconds.
    deadline = esp_timer_get_time() + 5'000'000;
    while (m_state.load() == State::Requested && esp_timer_get_time() < deadline)
    {
        vTaskDelay(1);
    }
    CBR(m_state.load() == State::Granted);

    status = "500 Internal Server Error";
    body = R"({"error":"ota_write_failed"})";
    CER(esp_ota_begin(slot, remaining, &handle));
    otaOpen = true;
    // Bound the total upload time even when a client sends bytes slowly.
    deadline = esp_timer_get_time() + 120'000'000;
    while (remaining != 0)
    {
        status = "400 Bad Request";
        body = R"({"error":"incomplete_upload"})";
        CBR(esp_timer_get_time() < deadline);
        received = httpd_req_recv(request, reinterpret_cast<char *>(buffer),
                                  remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        CBR(received > 0);
        body = R"({"error":"invalid_or_unwritable_image"})";
        CER(esp_ota_write(handle, buffer, static_cast<size_t>(received)));
        remaining -= static_cast<size_t>(received);
    }
    status = "400 Bad Request";
    body = R"({"error":"invalid_firmware_image"})";
    otaOpen = false; // esp_ota_end releases the handle even on failure.
    CER(esp_ota_end(handle));
    CER(esp_ota_get_partition_description(slot, &image));
    CBR(strncmp(image.project_name, esp_app_get_description()->project_name, sizeof(image.project_name)) == 0);
    status = "500 Internal Server Error";
    body = R"({"error":"boot_slot_selection_failed"})";
    CER(esp_ota_set_boot_partition(slot));

Error:
    if (otaOpen)
    {
        esp_ota_abort(handle);
    }
    httpd_resp_set_status(request, bRet ? "202 Accepted" : status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    result = httpd_resp_sendstr(request, bRet ? R"({"result":"rebooting"})" : body);
    if (bRet)
    {
        m_restartAtUs = esp_timer_get_time() + 1'000'000; // Allow one second for the reply.
        m_state.store(State::Reboot);
    }
    else if (reserved)
    {
        m_state.store(State::Idle);
    }
    // Close the socket even on rejection so unread upload bytes are discarded.
    return result == ESP_OK ? ESP_FAIL : result;
}

bool SomfyFirmware::PauseRadio(bool timedMove)
{
    State expected = State::Requested;
    State state;

    if (!timedMove)
    {
        m_state.compare_exchange_strong(expected, State::Granted);
    }
    state = m_state.load();
    if (state == State::Reboot && static_cast<uint64_t>(esp_timer_get_time()) >= m_restartAtUs)
    {
        esp_restart();
    }
    return state == State::Granted || state == State::Reboot;
}

void SomfyFirmware::CheckBoot(bool healthy)
{
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        if (!healthy || esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        {
            ESP_LOGE(TAG, "Updated firmware failed startup checks");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        else
        {
            ESP_LOGI(TAG, "Updated firmware passed startup checks");
        }
    }
}
