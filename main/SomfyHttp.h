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

#include "SomfyController.h"
#include "SomfyNetwork.h"
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// HTTP exchanges requests and snapshots with the radio task through queues.
class SomfyHttp final
{
  public:
    explicit SomfyHttp(SomfyController &controller) : m_controller(controller) {}
    bool Start(uint64_t nowMs);
    // Called by the radio task to service queued work and refresh HTTP snapshots.
    void Poll(uint64_t nowMs);

  private:
    enum class Operation : uint8_t
    {
        Command,
        Discovery
    };
    struct Request
    {
        uint32_t                 id;
        Operation                operation;
        SomfyController::Command command;
    };

    struct Completion
    {
        SomfyController::Result result;
        bool                    discoveryStarted;
    };

    struct Snapshot
    {
        SomfyController::Status    blind;
        SomfyController::Discovery discovery;
        // Keep the latest 16 completed requests in RAM. Only the radio task writes these.
        std::array<Completion, 16> completions;
        uint32_t                   lastCompletedId;
    };

    static esp_err_t handleRequest(httpd_req_t *request);
    esp_err_t        get(httpd_req_t *request);
    esp_err_t        getRequest(httpd_req_t *request, const char *suffix, const Snapshot &snapshot);
    esp_err_t        post(httpd_req_t *request);
    esp_err_t        networkRequest(httpd_req_t *request);

    void publish(uint64_t nowMs);

    SomfyController &m_controller;
    QueueHandle_t    m_requests  = nullptr;
    QueueHandle_t    m_snapshots = nullptr;
    httpd_handle_t   m_server    = nullptr;
    SomfyNetwork     m_network;
    uint32_t         m_lastAcceptedId = 0; // HTTP task only.
    Snapshot         m_snapshot{};         // Radio task only.
};
