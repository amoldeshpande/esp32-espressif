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

SomfyController::Backup SomfyController::ExportBackup() const
{
    return m_transmitter.ExportBackup();
}

const char *SomfyController::RestoreBackup(const Backup &backup)
{
    const char *result = "position_busy";

    if (!HasTimedMove())
    {
        result       = m_transmitter.RestoreBackup(backup);
        m_motion     = {};
        m_lastRemote = {};
        m_lastIndividual = {};
        m_lastGroupChannels = {};
        m_pendingFavorites = {};
    }
    return result;
}

bool SomfyController::Init(gpio_num_t txPin, bool radioReady)
{
    const bool initialized = m_transmitter.Init(txPin);

    m_radioReady = radioReady && initialized;
    return initialized;
}

bool SomfyController::RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps, bool transmitProfile)
{
    bool bRet = true;

    CBR(m_radioReady && m_targetBlind == 0);
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

    CBR(m_targetBlind != id);
    CBR(m_transmitter.RemoveBlind(id));
    m_lastGroupChannels[id - 1] = {};
    m_pendingFavorites[id - 1] = 0;
    m_lastIndividual[id - 1] = {};
    m_lastRemote[id - 1] = {};
    m_motion[id - 1]     = {};

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
    const bool   bRet   = m_targetBlind == 0 && m_transmitter.DeleteGroup(address);

    for (uint32_t id = 1; id <= blinds.size(); ++id)
    {
        if (bRet && blinds[id - 1].remoteAddress == address)
        {
            m_motion[id - 1] = {};
            m_lastGroupChannels[id - 1] = {};
        }
    }
    return bRet;
}

const char *SomfyController::UpdateGroupChannel(uint32_t groupAddress, uint32_t address, bool linked)
{
    return m_transmitter.UpdateGroupChannel(groupAddress, address, linked);
}

bool SomfyController::SetRemote(uint32_t id, uint32_t address)
{
    bool bRet = true;

    CBR(m_targetBlind != id);
    CBR(m_transmitter.SetRemote(id, address));
    m_lastGroupChannels[id - 1] = {};
    m_lastRemote[id - 1] = {};
    m_motion[id - 1]     = {};

Error:
    return bRet;
}

bool SomfyController::SetIndividualRemote(uint32_t id, uint32_t address)
{
    bool bRet = true;

    CBR(m_targetBlind != id);
    CBR(m_transmitter.SetIndividualRemote(id, address));
    m_lastIndividual[id - 1] = {};

Error:
    return bRet;
}

const char *SomfyController::UpdatePhysicalRemote(uint32_t id, uint32_t address, bool linked)
{
    const char *outcome = "physical_remote_save_failed";

    if (id != m_targetBlind)
    {
        outcome = m_transmitter.UpdatePhysicalRemote(id, address, linked);
        if (!linked && id != 0 && id <= m_lastIndividual.size())
        {
            for (Remote &last : m_lastIndividual[id - 1])
            {
                if (last.address == address)
                {
                    last = {};
                }
            }
        }
    }
    return outcome;
}

bool SomfyController::SetFavoritePosition(uint32_t id, int percent)
{
    bool bRet = true;

    CBR(id != 0 && id <= m_pendingFavorites.size());
    CBR(m_pendingFavorites[id - 1] == 0);
    CBR(m_transmitter.SetFavoritePosition(id, percent));

Error:
    return bRet;
}

const char *SomfyController::ProgramFavorite(uint32_t id)
{
    bool           bRet = true;
    const uint64_t nowMs = esp_timer_get_time() / 1000; // Convert microseconds to milliseconds.
    const Status   blind = GetStatus(id);
    const char    *outcome = "favorite_program_unavailable";
    const Motion  *motion;
    int            estimate;
    Result         result;

    CBR(blind.ready);
    outcome = "favorite_program_busy";
    CBR(m_targetBlind == 0);
    if (!m_pendingFavorites[id - 1])
    {
        motion = &m_motion[id - 1];
        CBR(nowMs - motion->since >= motion->duration);
        estimate = position(id, nowMs);
        outcome  = "favorite_program_unavailable";
        CBR(estimate >= 0);
        outcome = "favorite_already_recorded";
        CBR(estimate != blind.favoritePosition);
        outcome = "favorite_program_not_sent";
        result  = m_transmitter.ProgramFavorite(m_radio, id);
        m_receiver = {};
        CBR(result != Result::NotSent);
        // An uncertain transmission can reach the motor. Let the operator verify the jog.
        m_pendingFavorites[id - 1] = static_cast<uint8_t>(estimate + 1);
    }

Error:
    return bRet ? "favorite_jog_pending" : outcome;
}

const char *SomfyController::FinishFavoriteProgramming(uint32_t id, bool confirmed)
{
    bool        bRet = true;
    const char *outcome = "favorite_no_pending_jog";
    int         estimate;

    CBR(id != 0 && id <= m_pendingFavorites.size());
    CBR(m_pendingFavorites[id - 1] != 0);
    estimate = static_cast<int>(m_pendingFavorites[id - 1]) - 1;
    if (confirmed)
    {
        outcome = "favorite_save_failed";
        CBR(m_transmitter.SetFavoritePosition(id, estimate));
    }
    m_pendingFavorites[id - 1] = 0;

Error:
    return bRet ? (confirmed ? "favorite_saved" : "favorite_program_discarded") : outcome;
}

bool SomfyController::SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs)
{
    bool bRet = true;

    CBR(m_targetBlind != id);
    CBR(m_transmitter.SetTravelTimes(id, openMs, closeMs));
    m_motion[id - 1] = {};

Error:
    return bRet;
}

const char *SomfyController::SetPosition(uint32_t id, int percent)
{
    const Status blind   = GetStatus(id);
    const int    current = Position(id);
    const char  *outcome = "position_unavailable";
    Command      direction;
    Result       result;
    uint32_t     travelMs;
    uint64_t     nowMs;

    if (m_targetBlind != 0)
    {
        outcome = "position_busy";
    }
    else if (blind.ready && percent >= 0 && percent <= 100)
    {
        if (percent == 0 || percent == 100)
        {
            direction = percent == 100 ? Command::Open : Command::Close;
            result    = Send(id, direction);
            outcome   = result == Result::Transmitted ? "position_started" : "position_not_started";
        }
        else if (current >= 0)
        {
            direction = percent > current ? Command::Open : Command::Close;
            travelMs  = static_cast<uint64_t>(direction == Command::Open ? blind.openMs : blind.closeMs) *
                       std::abs(percent - current) / 100; // Fraction of full travel.
            nowMs = esp_timer_get_time() / 1000; // Convert microseconds to milliseconds.
            if (percent == current && nowMs - m_motion[id - 1].since >= m_motion[id - 1].duration)
            {
                outcome = "position_unchanged";
            }
            // Allow at least 500 ms for the movement burst and radio turnaround.
            else if (travelMs < 500)
            {
                outcome = "position_too_close";
            }
            else
            {
                result  = Send(id, direction);
                outcome = "position_not_started";
                if (result == Result::Transmitted)
                {
                    m_targetPercent = percent;
                    m_stopAtMs      = m_motion[id - 1].since + travelMs;
                    m_targetBlind   = id;
                    outcome         = "position_started";
                }
            }
        }
    }
    return outcome;
}

void SomfyController::PollPosition(uint64_t nowMs)
{
    const uint32_t id = m_targetBlind;
    Result         result;

    if (id != 0 && nowMs >= m_stopAtMs)
    {
        result        = Send(id, Command::My);
        m_targetBlind = 0;
        if (result != Result::Transmitted && result != Result::TransmittedWithCleanupError)
        {
            m_motion[id - 1] = {};
        }
    }
}

int SomfyController::TargetPosition(uint32_t id) const
{
    int           target = -1;
    const Motion *motion;
    const uint64_t nowMs = esp_timer_get_time() / 1000; // Convert microseconds to milliseconds.

    if (id != 0 && id <= m_motion.size())
    {
        motion = &m_motion[id - 1];
        if (id == m_targetBlind)
        {
            target = m_targetPercent;
        }
        else if (motion->towardFavorite && nowMs - motion->since < motion->duration)
        {
            target = motion->target;
        }
    }
    return target;
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
    uint64_t      elapsed;
    int           travel;

    if (blind.address && blind.openMs && blind.closeMs)
    {
        motion   = &m_motion[id - 1];
        elapsed  = nowMs - motion->since;
        estimate = motion->start;
        if (motion->target >= 0 && elapsed >= motion->duration)
        {
            estimate = motion->target;
        }
        else if (motion->target >= 0 && estimate >= 0)
        {
            travel = static_cast<int>(elapsed * 100 / motion->fullTravelMs); // Percent of full travel.
            estimate = motion->target > estimate ? estimate + travel : estimate - travel;
        }
    }
    return estimate;
}

void SomfyController::trackCommand(uint32_t id, Command command, Result result, uint64_t nowMs)
{
    const Status blind = m_transmitter.GetStatus(id);
    const int    estimate = position(id, nowMs);
    Motion      *motion;
    int          target;
    uint32_t     fullTravelMs;
    uint32_t     duration;

    if (blind.address && result != Result::NotSent && command != Command::Program)
    {
        motion = &m_motion[id - 1];
        if (result == Result::TransmissionUncertain || !blind.openMs || !blind.closeMs)
        {
            *motion = {};
        }
        else if (command == Command::My && nowMs - motion->since < motion->duration)
        {
            // My while moving stops the blind at its current estimated position.
            *motion       = {};
            motion->start = estimate;
        }
        else
        {
            // Position is expressed as percent open.
            target = command == Command::My ? blind.favoritePosition : (command == Command::Open ? 100 : 0);
            if (target < 0)
            {
                *motion = {};
            }
            else
            {
                if (estimate < 0)
                {
                    fullTravelMs = command == Command::Open ? blind.openMs : blind.closeMs;
                    duration     = fullTravelMs;
                    if (command == Command::My)
                    {
                        // With an unknown start, allow the longer trip from either endpoint.
                        // Round up the percentage of full travel to whole milliseconds.
                        duration = static_cast<uint32_t>((
                            std::max(static_cast<uint64_t>(blind.openMs) * target,
                                     static_cast<uint64_t>(blind.closeMs) * (100 - target)) + 99) / 100);
                    }
                }
                else
                {
                    fullTravelMs = target > estimate ? blind.openMs : blind.closeMs;
                    // Round up the fractional travel time to the next millisecond.
                    duration = static_cast<uint32_t>(
                        (static_cast<uint64_t>(fullTravelMs) * std::abs(target - estimate) + 99) / 100);
                }
                *motion = {estimate, nowMs, duration, target, fullTravelMs, command == Command::My};
            }
        }
    }
}

SomfyController::GroupResult SomfyController::SendGroup(uint32_t address, Command command)
{
    uint32_t members = 0;
    Status   blind;

    if (address)
    {
        for (uint32_t id = 1; id <= m_motion.size(); ++id)
        {
            blind = m_transmitter.GetStatus(id);
            if (blind.address && blind.remoteAddress == address)
            {
                members |= 1U << (id - 1); // The first blind occupies the low bit.
            }
        }
    }
    return sendGroup(members, command);
}

SomfyController::GroupResult SomfyController::SendBlindGroup(uint32_t id, Command command)
{
    const BlindGroups &groups = GetBlindGroups();
    uint32_t           members = 0;
    uint32_t           address;

    if (id && id <= groups.size() && groups[id - 1].name[0])
    {
        const BlindIds &saved = groups[id - 1].members;
        for (uint32_t member = 1; member <= m_motion.size(); ++member)
        {
            address = m_transmitter.GetStatus(member).address;
            if (address && std::find(saved.begin(), saved.end(), address) != saved.end())
            {
                members |= 1U << (member - 1);
            }
        }
    }
    return sendGroup(members, command);
}

SomfyController::GroupResult SomfyController::sendGroup(uint32_t members, Command command)
{
    GroupResult result{};
    Result      outcome;

    if (command != Command::Program)
    {
        for (uint32_t id = 1; id <= m_motion.size(); ++id)
        {
            if (members & (1U << (id - 1)))
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

uint32_t SomfyController::SaveBlindGroup(uint32_t id, const char *name, const BlindIds &members)
{
    return m_transmitter.SaveBlindGroup(id, name, members);
}

bool SomfyController::DeleteBlindGroup(uint32_t id)
{
    return m_transmitter.DeleteBlindGroup(id);
}

const SomfyController::BlindGroups &SomfyController::GetBlindGroups() const
{
    return m_transmitter.GetBlindGroups();
}

bool SomfyController::ConfirmPairing(uint32_t id)
{
    return m_transmitter.ConfirmPairing(id);
}

SomfyController::Result SomfyController::Send(uint32_t id, Command command)
{
    Result   result = Result::NotSent;
    uint64_t startedMs;

    if (m_radioReady && m_transmitProfile && (m_targetBlind == 0 || (id == m_targetBlind && command == Command::My)))
    {
        startedMs  = esp_timer_get_time() / 1000; // Approximate when the first frame starts.
        result     = m_transmitter.Send(m_radio, id, command);
        m_receiver = {};
        trackCommand(id, command, result, startedMs);
        if (id == m_targetBlind && result != Result::NotSent)
        {
            m_targetBlind = 0;
        }
    }
    return result;
}

SomfyController::Status SomfyController::GetStatus(uint32_t id) const
{
    Status status = m_transmitter.GetStatus(id);

    status.ready  = status.ready && m_radioReady && m_transmitProfile;
    return status;
}

SomfyController::Blinds SomfyController::GetBlinds() const
{
    Blinds blinds = m_transmitter.GetBlinds();

    if (!m_radioReady || !m_transmitProfile)
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
    m_discovery.active = m_radioReady;
    m_discoveryStartMs = nowMs;
}

bool SomfyController::discoveryActive(uint64_t nowMs) const
{
    return m_discovery.active && nowMs - m_discoveryStartMs < 30'000; // 30-second scan.
}

void SomfyController::trackRemote(const Remote &remote, uint64_t nowMs)
{
    const Command command = static_cast<Command>(remote.command);
    Status        blind;
    SomfyTransmitter::IndividualAddresses::const_iterator slot;
    SomfyTransmitter::IndividualAddresses::const_iterator groupSlot;
    Remote       *last;

    for (uint32_t id = 1; id <= m_motion.size(); ++id)
    {
        blind = m_transmitter.GetStatus(id);
        slot  = std::find(blind.individualAddresses.cbegin(), blind.individualAddresses.cend(), remote.address);
        groupSlot = std::find(blind.additionalGroupAddresses.cbegin(), blind.additionalGroupAddresses.cend(),
                              remote.address);
        last = nullptr;
        if (remote.address)
        {
            if (blind.remoteAddress == remote.address)
            {
                last = &m_lastRemote[id - 1];
            }
            else if (slot != blind.individualAddresses.cend())
            {
                last = &m_lastIndividual[id - 1][slot - blind.individualAddresses.cbegin()];
            }
            else if (groupSlot != blind.additionalGroupAddresses.cend())
            {
                last = &m_lastGroupChannels[id - 1][groupSlot - blind.additionalGroupAddresses.cbegin()];
            }
        }
        if (last)
        {
            // One button press can send several frames with the same rolling code.
            if (last->address == remote.address && last->rollingCode == remote.rollingCode &&
                last->command == remote.command)
            {
                continue;
            }
            *last = remote;
            if (command == Command::Open || command == Command::Close || command == Command::My)
            {
                // A decoded command is evidence of intent. RTS provides no motor acknowledgement.
                trackCommand(id, command, Result::Transmitted, nowMs);
            }
            else
            {
                m_motion[id - 1] = {};
            }
            if (id == m_targetBlind)
            {
                m_targetBlind = 0;
            }
        }
    }
}

bool SomfyController::ObserveRemote(const Remote &remote, uint64_t nowMs)
{
    bool bRet = false;

    trackRemote(remote, nowMs);

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
