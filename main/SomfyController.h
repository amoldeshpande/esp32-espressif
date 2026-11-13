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
// with the radio initialized on the transmit profile before constructing this module.
class SomfyController final
{
  public:
    using Command                           = SomfyTransmitter::Command;
    using Result                            = SomfyTransmitter::Result;
    using Status                            = SomfyTransmitter::Status;
    using Blinds                            = SomfyTransmitter::Blinds;
    using Pairing                           = SomfyTransmitter::Pairing;
    static constexpr uint32_t kFirstBlindId = 1; // Legacy serial controls and Wi-Fi setup identity.

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
    bool                       Init(gpio_num_t txPin);
    bool                       RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps, bool transmitProfile);
    uint32_t                   AddBlind(const char *name);
    bool                       RenameBlind(uint32_t id, const char *name);
    bool                       RemoveBlind(uint32_t id);
    bool                       RenameGroup(uint32_t address, const char *name);
    bool                       DeleteGroup(uint32_t address);
    bool                       SetRemote(uint32_t id, uint32_t address);
    bool                       SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs);
    GroupResult                SendGroup(uint32_t address, Command command);
    int                        Position(uint32_t id) const; // 0 closed, 100 open, -1 unknown.
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
    bool                     m_transmitProfile = true;
    Discovery                m_discovery;
    uint64_t                 m_discoveryStartMs = 0;

    struct Motion
    {
        int      start     = -1;
        uint64_t since     = 0;
        Command  direction = Command::My;
    };

    std::array<Motion, Blinds{}.size()> m_motion{};
    void                                trackCommand(uint32_t id, Command command, Result result, uint64_t nowMs);
    int                                 position(uint32_t id, uint64_t nowMs) const;
    bool                                discoveryActive(uint64_t nowMs) const;
};
