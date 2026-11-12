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

bool SomfyController::Init(gpio_num_t txPin)
{
    return m_transmitter.Init(txPin);
}

bool SomfyController::RetuneReceive(float frequencyMHz, float bandwidthKHz, int8_t offsetSteps, bool transmitProfile)
{
    bool bRet         = true;

    m_transmitProfile = false;
    CBR(m_radio.RetuneReceive(frequencyMHz, bandwidthKHz, offsetSteps));
    m_transmitProfile = transmitProfile;

Error:
    return bRet;
}

SomfyController::Result SomfyController::Send(Command command)
{
    Result result = Result::NotSent;

    if (m_transmitProfile)
    {
        result = m_transmitter.Send(m_radio, command);
    }
    return result;
}

SomfyController::Status SomfyController::GetStatus() const
{
    Status status = m_transmitter.GetStatus();

    status.ready  = status.ready && m_transmitProfile;
    return status;
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
