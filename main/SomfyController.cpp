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

#include "SomfyController.h"

#include <CC1101Lib/CC1101Device.h>
#include <CC1101Lib/LocalTypes.h>
#include <algorithm>
#include <esp_timer.h>

bool SomfyController::Init(gpio_num_t txPin)
{
    return m_transmitter.Init(txPin);
}

bool SomfyController::RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps, bool transmitProfile)
{
    bool bRet         = true;

    m_receiver        = {};
    m_transmitProfile = false;
    CBR(m_radio.RetuneReceive(frequencyMHz, bandwidthKHz, offsetSteps));
    m_transmitProfile = transmitProfile;

Error:
    return bRet;
}

uint32_t SomfyController::AddBlind(const char *name)
{
    return m_transmitter.AddBlind(name);
}

bool SomfyController::RenameBlind(uint32_t id, const char *name)
{
    return m_transmitter.RenameBlind(id, name);
}

bool SomfyController::RemoveBlind(uint32_t id)
{
    bool bRet = true;

    CBR(m_transmitter.RemoveBlind(id));
    m_motion[id - 1] = {};

Error:
    return bRet;
}

bool SomfyController::RenameGroup(uint32_t address, const char *name)
{
    return m_transmitter.RenameGroup(address, name);
}

bool SomfyController::DeleteGroup(uint32_t address)
{
    const Blinds blinds = m_transmitter.GetBlinds();
    const bool   bRet   = m_transmitter.DeleteGroup(address);

    for (uint32_t id = 1; id <= blinds.size(); ++id)
    {
        if (blinds[id - 1].remoteAddress == address)
        {
            m_motion[id - 1] = {};
        }
    }
    return bRet;
}

bool SomfyController::SetRemote(uint32_t id, uint32_t address)
{
    bool bRet = true;

    CBR(m_transmitter.SetRemote(id, address));
    m_motion[id - 1] = {};

Error:
    return bRet;
}

bool SomfyController::SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs)
{
    bool bRet = true;

    CBR(m_transmitter.SetTravelTimes(id, openMs, closeMs));
    m_motion[id - 1] = {};

Error:
    return bRet;
}

int SomfyController::Position(uint32_t id) const
{
    return position(id, esp_timer_get_time() / 1000); // Convert microseconds to milliseconds.
}

int SomfyController::position(uint32_t id, uint64_t nowMs) const
{
    int           estimate = -1;
    const Status  blind    = m_transmitter.GetStatus(id);
    const Motion *motion;
    uint32_t      duration;
    uint64_t      elapsed;
    int           travel;

    if (blind.address && blind.openMs && blind.closeMs)
    {
        motion   = &m_motion[id - 1];
        estimate = motion->start;
        if (motion->direction != Command::My)
        {
            duration = motion->direction == Command::Open ? blind.openMs : blind.closeMs;
            elapsed  = nowMs - motion->since;
            if (elapsed >= duration)
            {
                estimate = motion->direction == Command::Open ? 100 : 0; // Fully open or closed.
            }
            else if (estimate >= 0)
            {
                travel   = static_cast<int>(elapsed * 100 / duration); // Percent of full travel.
                estimate = motion->direction == Command::Open ? std::min(100, estimate + travel)
                                                              : std::max(0, estimate - travel);
            }
        }
    }
    return estimate;
}

void SomfyController::trackCommand(uint32_t id, Command command, Result result, uint64_t nowMs)
{
    const int estimate = position(id, nowMs);
    Motion   *motion;

    if (id && id <= m_motion.size() && result != Result::NotSent && command != Command::Program)
    {
        motion = &m_motion[id - 1];
        if (result == Result::TransmissionUncertain)
        {
            *motion = {};
        }
        else if (command == Command::My)
        {
            // My while stopped can move the blind to an unknown favorite position.
            motion->start     = motion->direction != Command::My && estimate > 0 && estimate < 100 ? estimate : -1;
            motion->direction = Command::My;
        }
        else
        {
            *motion = {estimate, nowMs, command};
        }
    }
}

SomfyController::GroupResult SomfyController::SendGroup(uint32_t address, Command command)
{
    GroupResult  result{};
    const Blinds blinds = GetBlinds();
    Result       outcome;

    if (address && command != Command::Program)
    {
        for (uint32_t id = 1; id <= blinds.size(); ++id)
        {
            if (blinds[id - 1].address && blinds[id - 1].remoteAddress == address)
            {
                ++result.members;
                outcome = Send(id, command);
                if (outcome == Result::Transmitted || outcome == Result::TransmittedWithCleanupError)
                {
                    ++result.transmitted;
                }
            }
        }
    }
    return result;
}

bool SomfyController::ConfirmPairing(uint32_t id)
{
    return m_transmitter.ConfirmPairing(id);
}

SomfyController::Result SomfyController::Send(uint32_t id, Command command)
{
    Result result = Result::NotSent;

    if (m_transmitProfile)
    {
        result     = m_transmitter.Send(m_radio, id, command);
        m_receiver = {};
        trackCommand(id, command, result, esp_timer_get_time() / 1000);
    }
    return result;
}

SomfyController::Status SomfyController::GetStatus(uint32_t id) const
{
    Status status = m_transmitter.GetStatus(id);

    status.ready  = status.ready && m_transmitProfile;
    return status;
}

SomfyController::Blinds SomfyController::GetBlinds() const
{
    Blinds blinds = m_transmitter.GetBlinds();

    if (!m_transmitProfile)
    {
        for (Status &blind : blinds)
        {
            blind.ready = false;
        }
    }
    return blinds;
}

void SomfyController::StartDiscovery(uint64_t nowMs)
{
    m_discovery        = {};
    m_discovery.active = true;
    m_discoveryStartMs = nowMs;
}

bool SomfyController::discoveryActive(uint64_t nowMs) const
{
    return m_discovery.active && nowMs - m_discoveryStartMs < 30'000; // 30-second scan.
}

bool SomfyController::ObserveRemote(const Remote &remote, uint64_t nowMs)
{
    bool bRet = false;

    // Physical remote presses can change position outside bridge control.
    for (uint32_t id = 1; id <= m_motion.size(); ++id)
    {
        if (remote.address && m_transmitter.GetStatus(id).remoteAddress == remote.address)
        {
            m_motion[id - 1] = {};
        }
    }

    if (discoveryActive(nowMs))
    {
        for (unsigned i = 0; i < m_discovery.count; ++i)
        {
            if (m_discovery.remotes[i].address == remote.address)
            {
                if (m_discovery.remotes[i].rollingCode != remote.rollingCode ||
                    m_discovery.remotes[i].command != remote.command ||
                    m_discovery.remotes[i].bitCount != remote.bitCount)
                {
                    m_discovery.remotes[i] = remote;
                    bRet                   = true;
                }
                goto Done;
            }
        }
        if (m_discovery.count < m_discovery.remotes.size())
        {
            m_discovery.remotes[m_discovery.count++] = remote;
            bRet                                     = true;
        }
        else if (!m_discovery.overflow)
        {
            m_discovery.overflow = true;
            bRet                 = true;
        }
    }

Done:
    return bRet;
}

SomfyController::Discovery SomfyController::GetDiscovery(uint64_t nowMs) const
{
    Discovery discovery = m_discovery;

    discovery.active    = discoveryActive(nowMs);
    return discovery;
}

SomfyReceiver::Result SomfyController::OnEdge(uint32_t timestamp, SomfyReceiver::Frame &frame)
{
    return m_receiver.OnEdge(timestamp, frame);
}

SomfyReceiver::Diagnostics SomfyController::TakeDiagnostics()
{
    return m_receiver.TakeDiagnostics();
}
