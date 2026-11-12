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

#include "SomfyHttp.h"

#include <CC1101Lib/LocalTypes.h>
#include <cJSON.h>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <esp_log.h>

static const char *TAG = "SomfyHttp";

static esp_err_t   sendJson(httpd_req_t *request, const char *status, const char *body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

extern const char somfyPage[] asm("_binary_Somfy_html_start");

static esp_err_t  servePage(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, somfyPage);
}

bool SomfyHttp::Start(SomfyController &controller, uint64_t nowMs)
{
    bool           bRet   = true;
    httpd_config_t server = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t    route{};

    // One pending request and one replaceable status snapshot.
    m_requests  = xQueueCreate(1, sizeof(Request));
    m_snapshots = xQueueCreate(1, sizeof(Snapshot));
    CBR(m_requests != nullptr);
    CBR(m_snapshots != nullptr);

    CBR(m_network.Start(controller.GetStatus().address, nowMs));
    Publish(controller, nowMs);
    server.uri_match_fn      = httpd_uri_match_wildcard;
    // Bound how long an idle client can hold the single HTTP task.
    server.recv_wait_timeout = 2;
    server.send_wait_timeout = 2;
    CER(httpd_start(&m_server, &server));
    route.uri      = "/api/*";
    route.method   = HTTP_GET;
    route.handler  = handleRequest;
    route.user_ctx = this;
    CER(httpd_register_uri_handler(m_server, &route));
    route.method = HTTP_POST;
    CER(httpd_register_uri_handler(m_server, &route));

    route.uri      = "/";
    route.method   = HTTP_GET;
    route.handler  = servePage;
    route.user_ctx = nullptr;
    CER(httpd_register_uri_handler(m_server, &route));

Error:
    if (!bRet)
    {
        if (m_server)
        {
            httpd_stop(m_server);
            m_server = nullptr;
        }
        m_network.Stop();
        if (m_requests)
        {
            vQueueDelete(m_requests);
            m_requests = nullptr;
        }
        if (m_snapshots)
        {
            vQueueDelete(m_snapshots);
            m_snapshots = nullptr;
        }
        ESP_LOGE(TAG, "HTTP startup failed");
    }
    return bRet;
}

void SomfyHttp::Publish(SomfyController &controller, uint64_t nowMs)
{
    if (m_snapshots)
    {
        m_snapshot.blind     = controller.GetStatus();
        m_snapshot.discovery = controller.GetDiscovery(nowMs);
        xQueueOverwrite(m_snapshots, &m_snapshot);
    }
}

bool SomfyHttp::ProcessRequest(SomfyController &controller, uint64_t nowMs)
{
    bool       bRet = false;
    Request    request;
    Completion completion{SomfyController::Result::NotSent, false};

    if (m_server)
    {
        m_network.Poll(nowMs);
    }
    if (m_requests && xQueueReceive(m_requests, &request, 0) == pdTRUE)
    {
        if (m_network.GetStatus().mode == SomfyNetwork::Mode::Online)
        {
            if (request.operation == Operation::Discovery)
            {
                controller.StartDiscovery(nowMs);
                completion.discoveryStarted = true;
            }
            else
            {
                completion.result = controller.Send(request.command);
                bRet              = true; // Reset the pulse decoder after a possible TX pause.
            }
        }
        m_snapshot.completions[request.id % m_snapshot.completions.size()] = completion;
        m_snapshot.lastCompletedId                                         = request.id;
        Publish(controller, nowMs);
    }
    return bRet;
}

static bool sameOrigin(httpd_req_t *request)
{
    bool bRet = true;
    char origin[80]; // HTTP scheme and host for requests made by a browser.
    char host[64];
    char expected[sizeof(origin)];

    if (httpd_req_get_hdr_value_len(request, "Origin") != 0)
    {
        CER(httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)));
        CER(httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)));
        snprintf(expected, sizeof(expected), "http://%s", host);
        CBR(strcmp(origin, expected) == 0);
    }

Error:
    return bRet;
}

esp_err_t SomfyHttp::handleRequest(httpd_req_t *request)
{
    SomfyHttp &http = *static_cast<SomfyHttp *>(request->user_ctx);
    esp_err_t  result;

    if (request->method == HTTP_POST && !sameOrigin(request))
    {
        result = sendJson(request, "403 Forbidden", R"({"error":"cross_origin_request"})");
    }
    else if (strncmp(request->uri, "/api/network", sizeof("/api/network") - 1) == 0)
    {
        result = http.networkRequest(request);
    }
    else if (http.m_network.GetStatus().mode != SomfyNetwork::Mode::Online)
    {
        result = sendJson(request, "503 Service Unavailable", R"({"error":"wifi_setup_in_progress"})");
    }
    else if (request->method == HTTP_GET)
    {
        result = http.get(request);
    }
    else
    {
        result = http.post(request);
    }
    return result;
}

static const char *networkMode(SomfyNetwork::Mode mode)
{
    const char *name = "unavailable";

    switch (mode)
    {
        case SomfyNetwork::Mode::Setup:
            name = "setup";
            break;
        case SomfyNetwork::Mode::Joining:
            name = "joining";
            break;
        case SomfyNetwork::Mode::Joined:
            name = "joined";
            break;
        case SomfyNetwork::Mode::Online:
            name = "online";
            break;
        case SomfyNetwork::Mode::Reopening:
            name = "reopening";
            break;
        case SomfyNetwork::Mode::Unavailable:
            break;
    }
    return name;
}

static const char *networkFailure(SomfyNetwork::Failure failure)
{
    const char *name = "";

    switch (failure)
    {
        case SomfyNetwork::Failure::JoinFailed:
            name = "join_failed";
            break;
        case SomfyNetwork::Failure::SaveFailed:
            name = "save_failed";
            break;
        case SomfyNetwork::Failure::SetupFailed:
            name = "setup_failed";
            break;
        case SomfyNetwork::Failure::None:
            break;
    }
    return name;
}

static cJSON *readJson(httpd_req_t *request, char *body, size_t capacity)
{
    bool   bRet     = true;
    size_t received = 0;
    int    length;
    cJSON *json = nullptr;

    CBR(request->content_len > 0 && request->content_len < capacity);
    while (received < request->content_len)
    {
        length = httpd_req_recv(request, body + received, request->content_len - received);
        CBR(length > 0);
        received += static_cast<size_t>(length);
    }
    CBR(memchr(body, '\0', received) == nullptr);
    body[received] = '\0';
    json           = cJSON_ParseWithLengthOpts(body, received + 1, nullptr, true);

Error:
    if (!bRet)
    {
        json = nullptr;
    }
    return json;
}

esp_err_t SomfyHttp::networkRequest(httpd_req_t *request)
{
    const SomfyNetwork::Status network = m_network.GetStatus();
    bool                       bRet    = true;
    char                       body[640]; // Fits a 32-byte SSID and 64-byte key even when JSON escapes each byte.
    const char                *status   = "404 Not Found";
    const char                *response = R"({"error":"unknown_path"})";
    cJSON                     *json     = nullptr;
    const cJSON               *ssid;
    const cJSON               *password;

    if (request->method == HTTP_GET && strcmp(request->uri, "/api/network") == 0)
    {
        snprintf(body, sizeof(body), R"({"mode":"%s","error":"%s","setupSsid":"%s","address":")" IPSTR R"("})", networkMode(network.mode), networkFailure(network.failure),
                 m_network.SetupSsid(), IP2STR(&network.address));
        status   = "200 OK";
        response = body;
    }
    else if (request->method == HTTP_POST && strcmp(request->uri, "/api/network/setup") == 0)
    {
        status   = "409 Conflict";
        response = R"({"error":"setup_unavailable"})";
        CBR(request->content_len == 0);
        CBR(m_network.RequestSetup());
        status   = "202 Accepted";
        response = R"({"accepted":true})";
    }
    else if (request->method == HTTP_POST && strcmp(request->uri, "/api/network/join") == 0)
    {
        status   = "409 Conflict";
        response = R"({"error":"setup_unavailable"})";
        CBR(network.mode == SomfyNetwork::Mode::Setup);
        status   = "400 Bad Request";
        response = R"({"error":"invalid_credentials_or_setup_busy"})";
        json     = readJson(request, body, sizeof(body));
        ssid     = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        password = cJSON_GetObjectItemCaseSensitive(json, "password");
        CBR(cJSON_IsString(ssid));
        CBR(cJSON_IsString(password));
        CBR(m_network.RequestJoin(ssid->valuestring, password->valuestring));
        status   = "202 Accepted";
        response = R"({"accepted":true})";
    }

Error:
    cJSON_Delete(json);
    if (!bRet)
    {
        ESP_LOGW(TAG, "Wi-Fi setup request rejected");
    }
    return sendJson(request, status, response);
}

static esp_err_t sendRequestResult(httpd_req_t *request, uint32_t id, const char *result)
{
    char body[96]; // Request ID and result name as JSON.

    snprintf(body, sizeof(body), R"({"requestId":%)" PRIu32 R"(,"result":"%s"})", id, result);
    return sendJson(request, "200 OK", body);
}

esp_err_t SomfyHttp::getRequest(httpd_req_t *request, const char *suffix, const Snapshot &snapshot)
{
    uint32_t               id     = snapshot.lastCompletedId;
    bool                   latest = strcmp(suffix, "latest") == 0;
    const char            *end    = suffix + strlen(suffix);
    std::from_chars_result parsed{};
    const Completion      *completion;
    const char            *outcome = "not_sent";
    esp_err_t              result;

    if (!latest)
    {
        end    = suffix + strlen(suffix);
        parsed = std::from_chars(suffix, end, id);
    }

    if (latest && id == 0)
    {
        result = sendJson(request, "200 OK", "null");
    }
    else if (!latest && (parsed.ec != std::errc{} || parsed.ptr != end || id == 0))
    {
        result = sendJson(request, "400 Bad Request", R"({"error":"invalid_request_id"})");
    }
    else if (id > m_lastAcceptedId)
    {
        result = sendJson(request, "404 Not Found", R"({"error":"unknown_request"})");
    }
    else if (id > snapshot.lastCompletedId)
    {
        result = sendRequestResult(request, id, "pending");
    }
    else if (snapshot.lastCompletedId - id >= snapshot.completions.size())
    {
        result = sendJson(request, "410 Gone", R"({"error":"result_expired"})");
    }
    else
    {
        completion = &snapshot.completions[id % snapshot.completions.size()];
        if (completion->discoveryStarted)
        {
            outcome = "discovery_started";
        }
        else
        {
            switch (completion->result)
            {
                case SomfyController::Result::NotSent:
                    break;
                case SomfyController::Result::TransmissionUncertain:
                    outcome = "transmission_uncertain";
                    break;
                case SomfyController::Result::Transmitted:
                    outcome = "transmitted";
                    break;
                case SomfyController::Result::TransmittedWithCleanupError:
                    outcome = "transmitted_with_cleanup_error";
                    break;
            }
        }
        result = sendRequestResult(request, id, outcome);
    }
    return result;
}

esp_err_t SomfyHttp::get(httpd_req_t *request)
{
    Snapshot                       snapshot{};
    esp_err_t                      result = ESP_OK;
    char                           body[160]; // A single blind, request, or remote record.
    const char                     requestPrefix[] = "/api/requests/";
    const SomfyController::Remote *remote;

    xQueuePeek(m_snapshots, &snapshot, 0);
    if (strcmp(request->uri, "/api/blinds") == 0)
    {
        snprintf(body, sizeof(body), R"([{"id":1,"address":"%06)" PRIX32 R"(","ready":%s}])",
                 snapshot.blind.address, snapshot.blind.ready ? "true" : "false");
        result = sendJson(request, "200 OK", body);
    }
    else if (strncmp(request->uri, requestPrefix, sizeof(requestPrefix) - 1) == 0)
    {
        result = getRequest(request, request->uri + sizeof(requestPrefix) - 1, snapshot);
    }
    else if (strcmp(request->uri, "/api/discovery") == 0)
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        snprintf(body, sizeof(body), R"({"active":%s,"overflow":%s,"remotes":[)",
                 snapshot.discovery.active ? "true" : "false", snapshot.discovery.overflow ? "true" : "false");
        result = httpd_resp_sendstr_chunk(request, body);
        for (unsigned i = 0; i < snapshot.discovery.count && result == ESP_OK; ++i)
        {
            remote = &snapshot.discovery.remotes[i];
            snprintf(body, sizeof(body), R"(%s{"address":"%06)" PRIX32 R"(","command":%u,"rollingCode":%u,"bits":%u})",
                     i == 0 ? "" : ",", remote->address, remote->command, remote->rollingCode, remote->bitCount);
            result = httpd_resp_sendstr_chunk(request, body);
        }
        if (result == ESP_OK)
        {
            result = httpd_resp_sendstr_chunk(request, "]}");
        }
        if (result == ESP_OK)
        {
            result = httpd_resp_send_chunk(request, nullptr, 0);
        }
    }
    else
    {
        result = sendJson(request, "404 Not Found", R"({"error":"unknown_path"})");
    }
    return result;
}

esp_err_t SomfyHttp::post(httpd_req_t *request)
{
    bool         bRet = true;
    Request      pending{};
    const char  *status   = "400 Bad Request";
    const char  *response = R"({"error":"invalid_command"})";
    char         body[81]{}; // A short JSON command object, plus its terminator.
    cJSON       *json = nullptr;
    const cJSON *command;

    if (strcmp(request->uri, "/api/discovery") == 0)
    {
        pending.operation = Operation::Discovery;
        CBR(request->content_len == 0);
    }
    else if (strcmp(request->uri, "/api/blinds/1/commands") == 0)
    {
        pending.operation = Operation::Command;
        json              = readJson(request, body, sizeof(body));
        command           = cJSON_GetObjectItemCaseSensitive(json, "command");
        CBR(cJSON_IsString(command));
        if (strcmp(command->valuestring, "open") == 0)
        {
            pending.command = SomfyController::Command::Open;
        }
        else if (strcmp(command->valuestring, "close") == 0)
        {
            pending.command = SomfyController::Command::Close;
        }
        else
        {
            CBR(strcmp(command->valuestring, "my") == 0 || strcmp(command->valuestring, "stop") == 0);
            pending.command = SomfyController::Command::My;
        }
    }
    else
    {
        status   = "404 Not Found";
        response = R"({"error":"unknown_path"})";
        CBR(false);
    }

    status   = "503 Service Unavailable";
    response = R"({"error":"request_ids_exhausted"})";
    // IDs are consecutive for accepted requests and do not wrap within a boot.
    CBR(m_lastAcceptedId < UINT32_MAX);
    pending.id = m_lastAcceptedId + 1;
    response   = R"({"error":"radio_queue_full"})";
    CBR(xQueueSend(m_requests, &pending, 0) == pdTRUE);
    m_lastAcceptedId = pending.id;
    snprintf(body, sizeof(body), R"({"requestId":%)" PRIu32 "}", pending.id);

Error:
    cJSON_Delete(json);
    return sendJson(request, bRet ? "202 Accepted" : status, bRet ? body : response);
}
