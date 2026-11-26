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
#include "SomfyFirmware.h"
#include "SomfyNetwork.h"
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// HTTP exchanges requests and snapshots with the radio task through queues.
class SomfyHttp final
{
  public:
    explicit SomfyHttp(SomfyController &controller)
        : m_controller(controller) {}
    bool Start(uint64_t nowMs);
    // Called by the radio task to service queued work and refresh HTTP snapshots.
    void Poll(uint64_t nowMs);
    bool PauseRadioForUpdate();
    void CheckFirmware(bool healthy);

  private:
    enum class Operation : uint8_t
    {
        Command,
        Discovery,
        CreateBlind,
        ConfirmPairing,
        RenameBlind,
        RemoveBlind,
        SetRemote,
        SetIndividualRemote,
        LinkPhysicalRemote,
        UnlinkPhysicalRemote,
        SetTravelTimes,
        SetFavoritePosition,
        ProgramFavorite,
        ConfirmFavorite,
        DiscardFavorite,
        SetPosition,
        GroupCommand,
        SaveBlindGroup,
        DeleteBlindGroup,
        BlindGroupCommand,
        RenameGroup,
        DeleteGroup,
        LinkGroupChannel,
        UnlinkGroupChannel,
        RestoreBackup
    };
    struct Request
    {
        uint32_t                 id;
        Operation                operation;
        SomfyController::Backup  backup;
        SomfyController::Command command;
        uint32_t                 blindId;
        uint32_t                 remoteAddress;
        uint32_t                 groupChannel;
        uint32_t                 blindGroupId;
        SomfyController::BlindIds members;
        uint32_t                 openMs;
        uint32_t                 closeMs;
        int                      percent;
        char                     name[sizeof(SomfyController::Status::name)];
    };

    struct Completion
    {
        SomfyController::Result result;
        const char             *setupResult; // Static result text for discovery and registry requests.
        uint32_t                blindId;
        uint32_t                groupId;
    };

    struct Snapshot
    {
        SomfyController::Backup                           backup;
        SomfyController::Blinds                           blinds;
        SomfyController::BlindGroups                      blindGroups;
        std::array<int, SomfyController::Blinds{}.size()> positions;
        std::array<int, SomfyController::Blinds{}.size()> targets;
        SomfyController::Discovery                        discovery;
        bool                                              radioReady;
        // Keep the latest 16 completed requests in RAM. Only the radio task writes these.
        std::array<Completion, 16>                        completions;
        uint32_t                                          lastCompletedId;
    };

    static esp_err_t handleRequest(httpd_req_t *request);
    esp_err_t        get(httpd_req_t *request);
    esp_err_t        getRequest(httpd_req_t *request, const char *suffix, const Snapshot &snapshot);
    esp_err_t        post(httpd_req_t *request);
    esp_err_t        networkRequest(httpd_req_t *request);

    void             publish(uint64_t nowMs);
    static bool      needsRadio(Operation operation);

    SomfyController &m_controller;
    QueueHandle_t    m_requests  = nullptr;
    QueueHandle_t    m_snapshots = nullptr;
    httpd_handle_t   m_server    = nullptr;
    SomfyNetwork     m_network;
    SomfyFirmware    m_firmware;
    uint32_t         m_lastAcceptedId = 0; // HTTP task only.
    Request          m_radioRequest{};    // Radio task only. Keep backup uploads off its stack.
    Snapshot         m_snapshot{};         // Radio task only.
    Snapshot         m_httpSnapshot{};     // HTTP task only. Keep the queue copy off the task stack.
};
