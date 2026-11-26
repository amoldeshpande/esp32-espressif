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

#include "SomfyReceiver.h"
#include "SomfyTransmitter.h"

// Owns command execution for saved blinds. Call on the radio task,
// Radio availability is supplied at initialization; saved blind configuration remains accessible without it.
class SomfyController final
{
  public:
    using Command                           = SomfyTransmitter::Command;
    using Result                            = SomfyTransmitter::Result;
    using Status                            = SomfyTransmitter::Status;
    using Blinds                            = SomfyTransmitter::Blinds;
    using Pairing                           = SomfyTransmitter::Pairing;
    static constexpr uint32_t kFirstBlindId = 1; // Legacy serial controls and Wi-Fi setup identity.

    bool                      HasTimedMove() const { return m_targetBlind != 0; }

    using Backup = SomfyTransmitter::Backup;
    Backup      ExportBackup() const;
    const char *RestoreBackup(const Backup &backup);

    struct GroupResult
    {
        unsigned members;
        unsigned transmitted;
    };

    struct Remote
    {
        uint32_t address;
        unsigned command;
        unsigned rollingCode;
        unsigned bitCount;
    };

    struct Discovery
    {
        // Bound storage for nearby transmitters. Further distinct addresses set overflow.
        std::array<Remote, 16> remotes{};
        unsigned               count    = 0;
        bool                   active   = false;
        bool                   overflow = false;
    };

    explicit SomfyController(TI_CC1101::CC1101Device &radio)
        : m_radio(radio) {}
    bool                       Init(gpio_num_t txPin, bool radioReady);
    bool                       RadioReady() const { return m_radioReady; }
    bool                       RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps, bool transmitProfile);
    uint32_t                   AddBlind(const char *name);
    bool                       RenameBlind(uint32_t id, const char *name);
    bool                       RemoveBlind(uint32_t id);
    bool                       RenameGroup(uint32_t address, const char *name);
    bool                       DeleteGroup(uint32_t address);
    const char                *UpdateGroupChannel(uint32_t groupAddress, uint32_t address, bool linked);
    bool                       SetRemote(uint32_t id, uint32_t address);
    const char                *ProgramFavorite(uint32_t id);
    const char                *FinishFavoriteProgramming(uint32_t id, bool confirmed);
    bool                       SetFavoritePosition(uint32_t id, int percent);
    bool                       SetIndividualRemote(uint32_t id, uint32_t address);
    const char                *UpdatePhysicalRemote(uint32_t id, uint32_t address, bool linked);
    bool                       SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs);
    using BlindIds = SomfyTransmitter::BlindIds;
    using BlindGroups = SomfyTransmitter::BlindGroups;
    GroupResult                SendGroup(uint32_t address, Command command);
    GroupResult                SendBlindGroup(uint32_t id, Command command);
    uint32_t                   SaveBlindGroup(uint32_t id, const char *name, const BlindIds &members);
    bool                       DeleteBlindGroup(uint32_t id);
    const BlindGroups         &GetBlindGroups() const;
    int                        Position(uint32_t id) const; // 0 closed, 100 open, -1 unknown.
    const char                *SetPosition(uint32_t id, int percent);
    void                       PollPosition(uint64_t nowMs);
    int                        TargetPosition(uint32_t id) const;
    bool                       ConfirmPairing(uint32_t id);
    Result                     Send(uint32_t id, Command command);
    Status                     GetStatus(uint32_t id) const;
    Blinds                     GetBlinds() const;
    SomfyReceiver::Result      OnEdge(uint32_t timestamp, SomfyReceiver::Frame &frame);
    SomfyReceiver::Diagnostics TakeDiagnostics();
    void                       StartDiscovery(uint64_t nowMs);
    bool                       ObserveRemote(const Remote &remote, uint64_t nowMs);
    Discovery                  GetDiscovery(uint64_t nowMs) const;

  private:
    TI_CC1101::CC1101Device &m_radio;
    SomfyTransmitter         m_transmitter;
    SomfyReceiver            m_receiver;
    bool                     m_radioReady      = false;
    bool                     m_transmitProfile = true;
    Discovery                m_discovery;
    uint64_t                 m_discoveryStartMs = 0;

    // One timed move at a time on the shared radio.
    uint32_t                 m_targetBlind      = 0;
    int                      m_targetPercent    = -1;
    uint64_t                 m_stopAtMs         = 0;

    struct Motion
    {
        int      start        = -1;
        uint64_t since        = 0;
        uint32_t duration     = 0;
        int      target       = -1;
        uint32_t fullTravelMs = 0;
        bool     towardFavorite = false;
    };

    std::array<Motion, Blinds{}.size()> m_motion{};
    std::array<uint8_t, Blinds{}.size()> m_pendingFavorites{}; // Percent open plus one. Zero means no pending jog.
    std::array<Remote, Blinds{}.size()> m_lastRemote{};
    std::array<std::array<Remote, SomfyTransmitter::IndividualAddresses{}.size()>, Blinds{}.size()> m_lastIndividual{};
    decltype(m_lastIndividual) m_lastGroupChannels{};
    void                                trackRemote(const Remote &remote, uint64_t nowMs);
    void                                trackCommand(uint32_t id, Command command, Result result, uint64_t nowMs);
    int                                 position(uint32_t id, uint64_t nowMs) const;
    GroupResult                         sendGroup(uint32_t members, Command command);
    bool                                discoveryActive(uint64_t nowMs) const;
};
