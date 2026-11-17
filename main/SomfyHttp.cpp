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
#include <algorithm>
#include <cJSON.h>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <esp_app_desc.h>
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

static esp_err_t sendFirmwareInfo(httpd_req_t *request)
{
    bool                  bRet = true;
    const esp_app_desc_t *app  = esp_app_get_description();
    // Each SHA-256 byte takes two hexadecimal characters.
    char                  build[sizeof(app->app_elf_sha256) * 2 + 1]{};
    cJSON                *info = cJSON_CreateObject();
    char                 *json = nullptr;
    esp_err_t             result;

    CBR(info != nullptr);
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
    {
        snprintf(build + i * 2, 3, "%02x", static_cast<unsigned>(app->app_elf_sha256[i]));
    }
    CBR(cJSON_AddStringToObject(info, "version", app->version));
    CBR(cJSON_AddStringToObject(info, "build", build));
    CBR(cJSON_AddStringToObject(info, "project", app->project_name));
    json = cJSON_PrintUnformatted(info);
    CBR(json != nullptr);

Error:
    result = bRet ? sendJson(request, "200 OK", json)
                  : sendJson(request, "500 Internal Server Error", R"({"error":"firmware_info_unavailable"})");
    cJSON_free(json);
    cJSON_Delete(info);
    return result;
}

bool SomfyHttp::Start(uint64_t nowMs)
{
    bool           bRet   = true;
    httpd_config_t server = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t    route{};

    // One pending request and one replaceable status snapshot.
    m_requests  = xQueueCreate(1, sizeof(Request));
    m_snapshots = xQueueCreate(1, sizeof(Snapshot));
    CBR(m_requests != nullptr);
    CBR(m_snapshots != nullptr);

    CBR(m_network.Start(m_controller.GetStatus(SomfyController::kFirstBlindId).address, nowMs));
    publish(nowMs);
    // Leave room for HTTP parsing, JSON encoding, and ESP-IDF logging.
    server.stack_size        = 8 * 1024; // 8 KiB, above the ESP-IDF default of 4 KiB.
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

void SomfyHttp::publish(uint64_t nowMs)
{
    if (m_snapshots)
    {
        m_snapshot.blinds = m_controller.GetBlinds();
        m_snapshot.blindGroups = m_controller.GetBlindGroups();
        for (uint32_t id = 1; id <= m_snapshot.blinds.size(); ++id)
        {
            m_snapshot.positions[id - 1] = m_controller.Position(id);
            m_snapshot.targets[id - 1]   = m_controller.TargetPosition(id);
        }
        m_snapshot.backup    = m_controller.ExportBackup();
        m_snapshot.discovery = m_controller.GetDiscovery(nowMs);
        xQueueOverwrite(m_snapshots, &m_snapshot);
    }
}

void SomfyHttp::Poll(uint64_t nowMs)
{
    const Request &request = m_radioRequest;
    Completion     completion{SomfyController::Result::NotSent, nullptr, 0, 0};

    if (m_server)
    {
        m_network.Poll(nowMs);
    }
    if (m_requests && xQueueReceive(m_requests, &m_radioRequest, 0) == pdTRUE)
    {
        if (m_network.GetStatus().mode == SomfyNetwork::Mode::Online)
        {
            switch (request.operation)
            {
                case Operation::RestoreBackup:
                    completion.setupResult = m_controller.RestoreBackup(request.backup);
                    break;
                case Operation::Discovery:
                    m_controller.StartDiscovery(nowMs);
                    completion.setupResult = "discovery_started";
                    break;
                case Operation::CreateBlind:
                    completion.blindId     = m_controller.AddBlind(request.name);
                    completion.setupResult = completion.blindId ? "blind_created" : "blind_create_failed";
                    break;
                case Operation::ConfirmPairing:
                    completion.setupResult = m_controller.ConfirmPairing(request.blindId)
                                                 ? "pairing_confirmed"
                                                 : "confirmation_rejected";
                    break;
                case Operation::RenameBlind:
                    completion.setupResult = m_controller.RenameBlind(request.blindId, request.name)
                                                 ? "blind_renamed"
                                                 : "rename_failed";
                    break;
                case Operation::RemoveBlind:
                    completion.setupResult = m_controller.RemoveBlind(request.blindId)
                                                 ? "blind_removed"
                                                 : "remove_failed";
                    break;
                case Operation::SetRemote:
                    completion.setupResult = m_controller.SetRemote(request.blindId, request.remoteAddress)
                                                 ? "remote_saved"
                                                 : "remote_save_failed";
                    break;
                case Operation::SetIndividualRemote:
                    completion.setupResult = m_controller.SetIndividualRemote(request.blindId, request.remoteAddress)
                                                 ? "individual_remote_saved"
                                                 : "individual_remote_save_failed";
                    break;
                case Operation::LinkPhysicalRemote:
                case Operation::UnlinkPhysicalRemote:
                    completion.setupResult = m_controller.UpdatePhysicalRemote(
                        request.blindId, request.remoteAddress, request.operation == Operation::LinkPhysicalRemote);
                    break;
                case Operation::SetPosition:
                    completion.setupResult = m_controller.SetPosition(request.blindId, request.percent);
                    break;
                case Operation::SetTravelTimes:
                    completion.setupResult = m_controller.SetTravelTimes(request.blindId, request.openMs, request.closeMs)
                                                 ? "travel_times_saved"
                                                 : "travel_times_save_failed";
                    break;
                case Operation::SetFavoritePosition:
                    completion.setupResult = m_controller.SetFavoritePosition(request.blindId, request.percent)
                                                 ? "favorite_saved"
                                                 : "favorite_save_failed";
                    break;
                case Operation::ProgramFavorite:
                    completion.setupResult = m_controller.ProgramFavorite(request.blindId);
                    break;
                case Operation::ConfirmFavorite:
                case Operation::DiscardFavorite:
                    completion.setupResult = m_controller.FinishFavoriteProgramming(
                        request.blindId, request.operation == Operation::ConfirmFavorite);
                    break;
                case Operation::RenameGroup:
                    completion.setupResult = m_controller.RenameGroup(request.remoteAddress, request.name)
                                                 ? "group_renamed"
                                                 : "group_rename_failed";
                    break;
                case Operation::DeleteGroup:
                    completion.setupResult = m_controller.DeleteGroup(request.remoteAddress)
                                                 ? "group_deleted"
                                                 : "group_delete_failed";
                    break;
                case Operation::LinkGroupChannel:
                case Operation::UnlinkGroupChannel:
                    completion.setupResult = m_controller.UpdateGroupChannel(
                        request.remoteAddress, request.groupChannel, request.operation == Operation::LinkGroupChannel);
                    break;
                case Operation::SaveBlindGroup:
                    completion.groupId = m_controller.SaveBlindGroup(request.blindGroupId, request.name, request.members);
                    completion.setupResult = completion.groupId ? "blind_group_saved" : "blind_group_save_failed";
                    break;
                case Operation::DeleteBlindGroup:
                    completion.setupResult = m_controller.DeleteBlindGroup(request.blindGroupId)
                                                 ? "blind_group_deleted"
                                                 : "blind_group_delete_failed";
                    break;
                case Operation::GroupCommand:
                case Operation::BlindGroupCommand:
                    {
                        const auto group = request.operation == Operation::GroupCommand
                                               ? m_controller.SendGroup(request.remoteAddress, request.command)
                                               : m_controller.SendBlindGroup(request.blindGroupId, request.command);
                        if (group.transmitted == 0)
                        {
                            completion.setupResult = "group_not_transmitted";
                        }
                        else
                        {
                            completion.setupResult = group.transmitted == group.members
                                                         ? "group_transmitted"
                                                         : "group_partially_transmitted";
                        }
                    }
                    break;
                case Operation::Command:
                    completion.result = m_controller.Send(request.blindId, request.command);
                    break;
            }
        }
        m_snapshot.completions[request.id % m_snapshot.completions.size()] = completion;
        m_snapshot.lastCompletedId                                         = request.id;
    }
    publish(nowMs);
}

bool SomfyHttp::PauseRadioForUpdate()
{
    return m_firmware.PauseRadio(m_controller.HasTimedMove());
}

void SomfyHttp::CheckFirmware(bool healthy)
{
    m_firmware.CheckBoot(healthy);
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
    else if (request->method == HTTP_GET && strcmp(request->uri, "/api/firmware") == 0)
    {
        result = sendFirmwareInfo(request);
    }
    else if (request->method == HTTP_POST && strcmp(request->uri, "/api/firmware") == 0)
    {
        result = http.m_firmware.Upload(request);
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

static esp_err_t sendRequestResult(httpd_req_t *request, uint32_t id, const char *result,
                                   uint32_t blindId, uint32_t groupId)
{
    char body[96]; // Request ID and result name as JSON.

    if (blindId)
    {
        snprintf(body, sizeof(body), R"({"requestId":%)" PRIu32 R"(,"result":"%s","blindId":%)" PRIu32 "}",
                 id, result, blindId);
    }
    else if (groupId)
    {
        snprintf(body, sizeof(body), R"({"requestId":%)" PRIu32 R"(,"result":"%s","groupId":%)" PRIu32 "}",
                 id, result, groupId);
    }
    else
    {
        snprintf(body, sizeof(body), R"({"requestId":%)" PRIu32 R"(,"result":"%s"})", id, result);
    }
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
        result = sendRequestResult(request, id, "pending", 0, 0);
    }
    else if (snapshot.lastCompletedId - id >= snapshot.completions.size())
    {
        result = sendJson(request, "410 Gone", R"({"error":"result_expired"})");
    }
    else
    {
        completion = &snapshot.completions[id % snapshot.completions.size()];
        if (completion->setupResult)
        {
            outcome = completion->setupResult;
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
        result = sendRequestResult(request, id, outcome, completion->blindId, completion->groupId);
    }
    return result;
}

static esp_err_t sendBlinds(httpd_req_t *request, const SomfyController::Blinds &blinds, const int *positions, const int *targets)
{
    bool                           bRet = true;
    cJSON                         *list = cJSON_CreateArray();
    cJSON                         *item;
    cJSON                         *individual;
    cJSON                         *groupChannels;
    char                          *json = nullptr;
    char                           address[sizeof("FFFFFF")];
    const char                    *pairing;
    const SomfyController::Status *blind;
    esp_err_t                      result;

    CBR(list != nullptr);
    for (uint32_t id = 1; id <= blinds.size(); ++id)
    {
        blind = &blinds[id - 1];
        if (blind->address == 0)
        {
            continue;
        }
        item = cJSON_CreateObject();
        CBR(item != nullptr);
        if (!cJSON_AddItemToArray(list, item))
        {
            cJSON_Delete(item);
            CBR(false);
        }
        snprintf(address, sizeof(address), "%06" PRIX32, blind->address);
        pairing = "unpaired";
        if (blind->pairing == SomfyController::Pairing::AwaitingConfirmation)
        {
            pairing = "awaiting_confirmation";
        }
        else if (blind->pairing == SomfyController::Pairing::Paired)
        {
            pairing = "paired";
        }
        CBR(cJSON_AddNumberToObject(item, "id", id));
        CBR(cJSON_AddStringToObject(item, "name", blind->name));
        CBR(cJSON_AddStringToObject(item, "address", address));
        CBR(cJSON_AddStringToObject(item, "pairing", pairing));
        snprintf(address, sizeof(address), "%06" PRIX32, blind->remoteAddress);
        CBR(cJSON_AddStringToObject(item, "remoteAddress", blind->remoteAddress ? address : ""));
        snprintf(address, sizeof(address), "%06" PRIX32, blind->individualAddresses.front());
        CBR(cJSON_AddStringToObject(item, "individualAddress", blind->individualAddresses.front() ? address : ""));
        individual = cJSON_AddArrayToObject(item, "individualAddresses");
        CBR(individual);
        for (uint32_t channel : blind->individualAddresses)
        {
            if (channel)
            {
                snprintf(address, sizeof(address), "%06" PRIX32, channel);
                CBR(cJSON_AddItemToArray(individual, cJSON_CreateString(address)));
            }
        }
        CBR(cJSON_AddStringToObject(item, "groupName", blind->groupName));
        groupChannels = cJSON_AddArrayToObject(item, "groupChannels");
        CBR(groupChannels);
        if (blind->remoteAddress)
        {
            snprintf(address, sizeof(address), "%06" PRIX32, blind->remoteAddress);
            CBR(cJSON_AddItemToArray(groupChannels, cJSON_CreateString(address)));
        }
        for (uint32_t channel : blind->additionalGroupAddresses)
        {
            if (channel)
            {
                snprintf(address, sizeof(address), "%06" PRIX32, channel);
                CBR(cJSON_AddItemToArray(groupChannels, cJSON_CreateString(address)));
            }
        }
        CBR(cJSON_AddNumberToObject(item, "openMs", blind->openMs));
        CBR(cJSON_AddNumberToObject(item, "closeMs", blind->closeMs));
        if (blind->favoritePosition < 0)
        {
            CBR(cJSON_AddNullToObject(item, "favoritePosition"));
        }
        else
        {
            CBR(cJSON_AddNumberToObject(item, "favoritePosition", blind->favoritePosition));
        }
        if (positions[id - 1] < 0)
        {
            CBR(cJSON_AddNullToObject(item, "position"));
        }
        else
        {
            CBR(cJSON_AddNumberToObject(item, "position", positions[id - 1]));
        }
        if (targets[id - 1] < 0)
        {
            CBR(cJSON_AddNullToObject(item, "targetPosition"));
        }
        else
        {
            CBR(cJSON_AddNumberToObject(item, "targetPosition", targets[id - 1]));
        }
        CBR(cJSON_AddBoolToObject(item, "ready", blind->ready));
    }
    json = cJSON_PrintUnformatted(list);
    CBR(json != nullptr);

Error:
    if (bRet)
    {
        result = sendJson(request, "200 OK", json);
    }
    else
    {
        result = sendJson(request, "500 Internal Server Error", R"({"error":"blind_list_unavailable"})");
    }
    cJSON_free(json);
    cJSON_Delete(list);
    return result;
}

static esp_err_t sendBlindGroups(httpd_req_t *request, const SomfyController::BlindGroups &groups,
                                 const SomfyController::Blinds &blinds)
{
    bool      bRet = true;
    cJSON    *list = cJSON_CreateArray();
    cJSON    *item;
    cJSON    *members;
    const SomfyController::BlindGroups::value_type *group;
    uint32_t  address;
    char     *json = nullptr;
    esp_err_t result;

    CBR(list);
    for (uint32_t id = 1; id <= groups.size(); ++id)
    {
        group = &groups[id - 1];
        if (!group->name[0])
        {
            continue;
        }
        item = cJSON_CreateObject();
        CBR(cJSON_AddItemToArray(list, item));
        CBR(cJSON_AddNumberToObject(item, "id", id));
        CBR(cJSON_AddStringToObject(item, "name", group->name));
        members = cJSON_AddArrayToObject(item, "blindIds");
        CBR(members);
        for (uint32_t blindId = 1; blindId <= blinds.size(); ++blindId)
        {
            address = blinds[blindId - 1].address;
            if (address && std::find(group->members.begin(), group->members.end(), address) != group->members.end())
            {
                CBR(cJSON_AddItemToArray(members, cJSON_CreateNumber(blindId)));
            }
        }
    }
    json = cJSON_PrintUnformatted(list);
    CBR(json);

Error:
    result = bRet ? sendJson(request, "200 OK", json)
                  : sendJson(request, "500 Internal Server Error", R"({"error":"blind_groups_unavailable"})");
    cJSON_free(json);
    cJSON_Delete(list);
    return result;
}

esp_err_t SomfyHttp::get(httpd_req_t *request)
{
    esp_err_t                      result = ESP_OK;
    char                           body[160]; // A single blind, request, or remote record.
    const char                     requestPrefix[] = "/api/requests/";
    const SomfyController::Remote *remote;

    xQueuePeek(m_snapshots, &m_httpSnapshot, 0);
    if (strcmp(request->uri, "/api/backup") == 0)
    {
        httpd_resp_set_type(request, "application/octet-stream");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=somfy-backup.esb");
        result = httpd_resp_send(request, reinterpret_cast<const char *>(&m_httpSnapshot.backup),
                                 sizeof(m_httpSnapshot.backup));
    }
    else if (strcmp(request->uri, "/api/blinds") == 0)
    {
        result = sendBlinds(request, m_httpSnapshot.blinds, m_httpSnapshot.positions.data(), m_httpSnapshot.targets.data());
    }
    else if (strcmp(request->uri, "/api/blind-groups") == 0)
    {
        result = sendBlindGroups(request, m_httpSnapshot.blindGroups, m_httpSnapshot.blinds);
    }
    else if (strncmp(request->uri, requestPrefix, sizeof(requestPrefix) - 1) == 0)
    {
        result = getRequest(request, request->uri + sizeof(requestPrefix) - 1, m_httpSnapshot);
    }
    else if (strcmp(request->uri, "/api/discovery") == 0)
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        snprintf(body, sizeof(body), R"({"active":%s,"overflow":%s,"remotes":[)",
                 m_httpSnapshot.discovery.active ? "true" : "false", m_httpSnapshot.discovery.overflow ? "true" : "false");
        result = httpd_resp_sendstr_chunk(request, body);
        for (unsigned i = 0; i < m_httpSnapshot.discovery.count && result == ESP_OK; ++i)
        {
            remote = &m_httpSnapshot.discovery.remotes[i];
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

static bool parseActionPath(const char *path, const char *prefix, int base, uint32_t &id, const char *&action)
{
    bool                   bRet         = true;
    const size_t           prefixLength = strlen(prefix);
    const char            *start;
    const char            *slash;
    std::from_chars_result parsed{};

    CBR(strncmp(path, prefix, prefixLength) == 0);
    start = path + prefixLength;
    slash = strchr(start, '/');
    CBR(slash != nullptr);
    parsed = std::from_chars(start, slash, id, base);
    CBR(parsed.ec == std::errc{} && parsed.ptr == slash && id != 0);
    action = slash + 1;

Error:
    return bRet;
}

static bool readCommand(const cJSON *json, SomfyController::Command &command)
{
    bool         bRet  = true;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(json, "command");

    CBR(cJSON_IsString(field));
    if (strcmp(field->valuestring, "open") == 0)
    {
        command = SomfyController::Command::Open;
    }
    else if (strcmp(field->valuestring, "close") == 0)
    {
        command = SomfyController::Command::Close;
    }
    else
    {
        CBR(strcmp(field->valuestring, "my") == 0 || strcmp(field->valuestring, "stop") == 0);
        command = SomfyController::Command::My;
    }

Error:
    return bRet;
}

static bool readTravelTime(const cJSON *field, uint32_t &ms)
{
    bool bRet = true;

    CBR(cJSON_IsNumber(field));
    // A full travel time must be an integer from one millisecond to ten minutes.
    CBR(field->valuedouble >= 1 && field->valuedouble <= 600'000);
    ms = static_cast<uint32_t>(field->valuedouble);
    CBR(ms == field->valuedouble);

Error:
    return bRet;
}

static bool readBlindGroup(const cJSON *json, char *name, size_t nameSize, SomfyController::BlindIds &members)
{
    bool         bRet = true;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON *entry;
    size_t       count = 0;
    uint32_t     id;

    CBR(cJSON_IsString(field));
    CBR(field->valuestring[0] && strlen(field->valuestring) < nameSize);
    memcpy(name, field->valuestring, strlen(field->valuestring));
    field = cJSON_GetObjectItemCaseSensitive(json, "blindIds");
    CBR(cJSON_IsArray(field));
    cJSON_ArrayForEach(entry, field)
    {
        CBR(count < members.size() && cJSON_IsNumber(entry));
        CBR(entry->valuedouble >= 1 && entry->valuedouble <= members.size());
        id = static_cast<uint32_t>(entry->valuedouble);
        CBR(id == entry->valuedouble);
        CBR(std::find(members.begin(), members.begin() + count, id) == members.begin() + count);
        members[count++] = id;
    }
    CBR(count != 0);

Error:
    return bRet;
}

esp_err_t SomfyHttp::post(httpd_req_t *request)
{
    bool                   bRet = true;
    Request                pending{};
    const char            *status   = "400 Bad Request";
    const char            *response = R"({"error":"invalid_command"})";
    char                   body[256]; // A 32-byte name can expand to six JSON characters per byte.
    cJSON                 *json = nullptr;
    const cJSON           *field;
    const char            *action;
    const char            *end;
    std::from_chars_result parsed{};

    size_t                 received = 0;
    int                    length;
    char                  *backupBytes;

    if (strcmp(request->uri, "/api/backup") == 0)
    {
        pending.operation = Operation::RestoreBackup;
        response          = R"({"error":"invalid_backup_size_or_body"})";
        CBR(request->content_len == sizeof(pending.backup) ||
            request->content_len == SomfyController::Backup::LegacySize() ||
            request->content_len == SomfyController::Backup::IndividualSize() ||
            request->content_len == SomfyController::Backup::PhysicalSize() ||
            request->content_len == SomfyController::Backup::GroupChannelSize());
        backupBytes = reinterpret_cast<char *>(&pending.backup);
        while (received < request->content_len)
        {
            length = httpd_req_recv(request, backupBytes + received, request->content_len - received);
            CBR(length > 0);
            received += static_cast<size_t>(length);
        }
        CBR(pending.backup.MatchesSize(received));
    }
    else if (strcmp(request->uri, "/api/discovery") == 0)
    {
        pending.operation = Operation::Discovery;
        CBR(request->content_len == 0);
    }
    else if (strcmp(request->uri, "/api/blinds") == 0)
    {
        pending.operation = Operation::CreateBlind;
        response          = R"({"error":"invalid_blind_name"})";
        json              = readJson(request, body, sizeof(body));
        field             = cJSON_GetObjectItemCaseSensitive(json, "name");
        CBR(cJSON_IsString(field));
        CBR(field->valuestring[0] && strlen(field->valuestring) < sizeof(pending.name));
        memcpy(pending.name, field->valuestring, strlen(field->valuestring));
    }
    else if (strcmp(request->uri, "/api/blind-groups") == 0 ||
             parseActionPath(request->uri, "/api/blind-groups/", 10, pending.blindGroupId, action))
    {
        status = "400 Bad Request";
        if (pending.blindGroupId == 0 || strcmp(action, "save") == 0)
        {
            pending.operation = Operation::SaveBlindGroup;
            response          = R"({"error":"invalid_blind_group"})";
            json              = readJson(request, body, sizeof(body));
            CBR(readBlindGroup(json, pending.name, sizeof(pending.name), pending.members));
        }
        else if (strcmp(action, "delete") == 0)
        {
            pending.operation = Operation::DeleteBlindGroup;
            response          = R"({"error":"unexpected_body"})";
            CBR(request->content_len == 0);
        }
        else if (strcmp(action, "commands") == 0)
        {
            pending.operation = Operation::BlindGroupCommand;
            json              = readJson(request, body, sizeof(body));
            CBR(readCommand(json, pending.command));
        }
        else
        {
            status   = "404 Not Found";
            response = R"({"error":"unknown_path"})";
            CBR(false);
        }
    }
    else if (parseActionPath(request->uri, "/api/groups/", 16, pending.remoteAddress, action))
    {
        CBR(pending.remoteAddress < 0xFFFFFF); // 24-bit RTS address.
        if (strcmp(action, "commands") == 0)
        {
            pending.operation = Operation::GroupCommand;
            json              = readJson(request, body, sizeof(body));
            CBR(readCommand(json, pending.command));
        }
        else if (strcmp(action, "rename") == 0)
        {
            pending.operation = Operation::RenameGroup;
            response          = R"({"error":"invalid_group_name"})";
            json              = readJson(request, body, sizeof(body));
            field             = cJSON_GetObjectItemCaseSensitive(json, "name");
            CBR(cJSON_IsString(field));
            CBR(field->valuestring[0] && strlen(field->valuestring) < sizeof(pending.name));
            memcpy(pending.name, field->valuestring, strlen(field->valuestring));
        }
        else if (strcmp(action, "link-channel") == 0 || strcmp(action, "unlink-channel") == 0)
        {
            pending.operation = strcmp(action, "link-channel") == 0 ? Operation::LinkGroupChannel
                                                                       : Operation::UnlinkGroupChannel;
            response = R"({"error":"invalid_remote_address"})";
            json     = readJson(request, body, sizeof(body));
            field    = cJSON_GetObjectItemCaseSensitive(json, "address");
            CBR(cJSON_IsString(field));
            CBR(strlen(field->valuestring) == 6); // A 24-bit RTS address uses six hexadecimal digits.
            end    = field->valuestring + strlen(field->valuestring);
            parsed = std::from_chars(field->valuestring, end, pending.groupChannel, 16);
            CBR(parsed.ec == std::errc{} && parsed.ptr == end && pending.groupChannel != 0 &&
                pending.groupChannel < 0xFFFFFF);
        }
        else if (strcmp(action, "delete") == 0)
        {
            pending.operation = Operation::DeleteGroup;
            response          = R"({"error":"unexpected_body"})";
            CBR(request->content_len == 0);
        }
        else
        {
            status   = "404 Not Found";
            response = R"({"error":"unknown_path"})";
            CBR(false);
        }
    }
    else
    {
        status   = "404 Not Found";
        response = R"({"error":"unknown_blind_or_path"})";
        CBR(parseActionPath(request->uri, "/api/blinds/", 10, pending.blindId, action));
        xQueuePeek(m_snapshots, &m_httpSnapshot, 0);
        CBR(pending.blindId <= m_httpSnapshot.blinds.size());
        CBR(m_httpSnapshot.blinds[pending.blindId - 1].address != 0);
        if (strcmp(action, "commands") == 0)
        {
            pending.operation = Operation::Command;
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_command"})";
            json              = readJson(request, body, sizeof(body));
            CBR(readCommand(json, pending.command));
        }
        else if (strcmp(action, "rename") == 0)
        {
            pending.operation = Operation::RenameBlind;
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_blind_name"})";
            json              = readJson(request, body, sizeof(body));
            field             = cJSON_GetObjectItemCaseSensitive(json, "name");
            CBR(cJSON_IsString(field));
            CBR(field->valuestring[0] && strlen(field->valuestring) < sizeof(pending.name));
            memcpy(pending.name, field->valuestring, strlen(field->valuestring));
        }
        else if (strcmp(action, "remove") == 0)
        {
            pending.operation = Operation::RemoveBlind;
            status            = "400 Bad Request";
            response          = R"({"error":"unexpected_body"})";
            CBR(request->content_len == 0);
        }
        else if (strcmp(action, "remote") == 0 || strcmp(action, "individual-remote") == 0 ||
                 strcmp(action, "link-remote") == 0 || strcmp(action, "unlink-remote") == 0)
        {
            if (strcmp(action, "remote") == 0)
            {
                pending.operation = Operation::SetRemote;
            }
            else if (strcmp(action, "individual-remote") == 0)
            {
                pending.operation = Operation::SetIndividualRemote;
            }
            else
            {
                pending.operation = strcmp(action, "link-remote") == 0 ? Operation::LinkPhysicalRemote
                                                                        : Operation::UnlinkPhysicalRemote;
            }
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_remote_address"})";
            json              = readJson(request, body, sizeof(body));
            field             = cJSON_GetObjectItemCaseSensitive(json, "address");
            CBR(cJSON_IsString(field));
            CBR(field->valuestring[0] || pending.operation == Operation::SetRemote ||
                pending.operation == Operation::SetIndividualRemote);
            if (field->valuestring[0])
            {
                end    = field->valuestring + strlen(field->valuestring);
                parsed = std::from_chars(field->valuestring, end, pending.remoteAddress, 16);
                // RTS uses six hexadecimal digits per remote address.
                CBR(end - field->valuestring == 6 && parsed.ec == std::errc{} && parsed.ptr == end &&
                    pending.remoteAddress != 0 && pending.remoteAddress < 0xFFFFFF);
            }
        }
        else if (strcmp(action, "position") == 0)
        {
            pending.operation = Operation::SetPosition;
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_position"})";
            json              = readJson(request, body, sizeof(body));
            field             = cJSON_GetObjectItemCaseSensitive(json, "position");
            CBR(cJSON_IsNumber(field));
            // Accept whole percentages from fully closed to fully open.
            CBR(field->valuedouble >= 0 && field->valuedouble <= 100);
            pending.percent = static_cast<int>(field->valuedouble);
            CBR(pending.percent == field->valuedouble);
        }
        else if (strcmp(action, "favorite-program") == 0 || strcmp(action, "favorite-confirm") == 0 ||
                 strcmp(action, "favorite-discard") == 0)
        {
            response = R"({"error":"unexpected_body"})";
            CBR(request->content_len == 0);
            if (strcmp(action, "favorite-program") == 0)
            {
                pending.operation = Operation::ProgramFavorite;
            }
            else
            {
                pending.operation = strcmp(action, "favorite-confirm") == 0 ? Operation::ConfirmFavorite
                                                                             : Operation::DiscardFavorite;
            }
        }
        else if (strcmp(action, "favorite") == 0)
        {
            pending.operation = Operation::SetFavoritePosition;
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_favorite"})";
            json              = readJson(request, body, sizeof(body));
            field             = cJSON_GetObjectItemCaseSensitive(json, "position");
            if (cJSON_IsNull(field))
            {
                pending.percent = -1; // Clear the saved favorite.
            }
            else
            {
                CBR(cJSON_IsNumber(field));
                // Accept whole percentages from fully closed to fully open.
                CBR(field->valuedouble >= 0 && field->valuedouble <= 100);
                pending.percent = static_cast<int>(field->valuedouble);
                CBR(pending.percent == field->valuedouble);
            }
        }
        else if (strcmp(action, "calibrate") == 0)
        {
            pending.operation = Operation::SetTravelTimes;
            status            = "400 Bad Request";
            response          = R"({"error":"invalid_travel_times"})";
            json              = readJson(request, body, sizeof(body));
            CBR(readTravelTime(cJSON_GetObjectItemCaseSensitive(json, "openMs"), pending.openMs));
            CBR(readTravelTime(cJSON_GetObjectItemCaseSensitive(json, "closeMs"), pending.closeMs));
        }
        else if (strcmp(action, "pair") == 0 || strcmp(action, "confirm") == 0)
        {
            status   = "400 Bad Request";
            response = R"({"error":"unexpected_body"})";
            CBR(request->content_len == 0);
            pending.operation = strcmp(action, "pair") == 0 ? Operation::Command : Operation::ConfirmPairing;
            pending.command   = SomfyController::Command::Program;
        }
        else
        {
            CBR(false);
        }
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
