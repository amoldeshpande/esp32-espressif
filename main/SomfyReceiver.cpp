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

#include "SomfyReceiver.h"

#include <cstddef>

namespace
{
    // Pulse widths and the +/-30% receive tolerance used by ESPSomfy-RTS.
    static constexpr uint32_t    kHalfSymbolUs              = 640;
    static constexpr uint32_t    kFullSymbolUs              = 2 * kHalfSymbolUs;
    static constexpr uint32_t    kHardwareSyncUs            = 4 * kHalfSymbolUs;
    static constexpr uint32_t    kSoftwareSyncUs            = 4'850;
    static constexpr uint32_t    kPercent                   = 100;
    static constexpr uint32_t    kTimingTolerancePercent    = 30;
    static constexpr uint32_t    kShortestPulseUs           = kHalfSymbolUs * (kPercent - kTimingTolerancePercent) / kPercent;

    // Four hardware sync intervals are required before a software sync pulse.
    static constexpr unsigned    kMinSyncEdges              = 4;
    // Cap the count just above the longest observed preamble (24 intervals).
    static constexpr unsigned    kMaxSyncEdges              = 25;
    // The extended format has been observed with 12 or 13 sync intervals, or a longer preamble.
    static constexpr unsigned    kExtendedSyncEdges         = 12;
    static constexpr unsigned    kExtendedSyncEdgesRs485    = 13;
    static constexpr unsigned    kExtendedLongSyncThreshold = 17; // The reference decoder treats longer preambles as extended.

    // The base frame is key, command/checksum, rolling code (two bytes), address (three bytes).
    static constexpr std::size_t kCommandByte               = 1;
    static constexpr std::size_t kRollingCodeHighByte       = 2;
    static constexpr std::size_t kRollingCodeLowByte        = 3;
    static constexpr std::size_t kAddressHighByte           = 4;
    static constexpr std::size_t kAddressMidByte            = 5;
    static constexpr std::size_t kAddressLowByte            = 6;
    static constexpr unsigned    kNibbleBits                = CHAR_BIT / 2;
    static constexpr uint8_t     kChecksumMask              = (1 << kNibbleBits) - 1;
} // namespace

uint32_t SomfyReceiver::Frame::RemoteAddress() const
{
    return (uint32_t(bytes[kAddressHighByte]) << (2 * CHAR_BIT)) |
           (uint32_t(bytes[kAddressMidByte]) << CHAR_BIT) | bytes[kAddressLowByte];
}

unsigned SomfyReceiver::Frame::Command() const
{
    return bytes[kCommandByte] >> kNibbleBits;
}

unsigned SomfyReceiver::Frame::RollingCode() const
{
    return (unsigned(bytes[kRollingCodeHighByte]) << CHAR_BIT) | bytes[kRollingCodeLowByte];
}

SomfyReceiver::Diagnostics SomfyReceiver::TakeDiagnostics()
{
    const Diagnostics sample = m_diagnostics;
    m_diagnostics            = {};
    return sample;
}

bool SomfyReceiver::OnEdge(uint32_t timestamp, Frame &frame)
{
    bool           bRet     = false;

    // Unsigned subtraction also handles wraparound of the microsecond timestamp.
    const uint32_t duration = timestamp - m_lastEdge;
    if (!m_haveEdge)
    {
        m_lastEdge = timestamp;
        m_haveEdge = true;
    }
    else if (duration >= kShortestPulseUs)
    {
        m_lastEdge = timestamp;
        if (m_expectedBits == 0)
        {
            processSyncPulse(duration);
        }
        else
        {
            bRet = processDataPulse(duration, frame);
        }
    }
    return bRet;
}

void SomfyReceiver::processSyncPulse(uint32_t duration)
{
    bool extended;

    if (withinTolerance(duration, kHardwareSyncUs))
    {
        if (m_syncEdges < kMaxSyncEdges)
        {
            ++m_syncEdges;
        }
    }
    else if (m_syncEdges >= kMinSyncEdges && withinTolerance(duration, kSoftwareSyncUs))
    {
        ++m_diagnostics.syncs;
        extended = m_syncEdges == kExtendedSyncEdges ||
                   m_syncEdges == kExtendedSyncEdgesRs485 ||
                   m_syncEdges > kExtendedLongSyncThreshold;
        m_expectedBits = extended ? kExtendedFrameBits : kStandardFrameBits;
        m_buffer       = {};
        m_previousBit  = false; // The software sync is followed by a zero start symbol.
        m_halfSymbol   = false;
    }
    else
    {
        m_syncEdges = 0;
    }
}

bool SomfyReceiver::processDataPulse(uint32_t duration, Frame &frame)
{
    bool     bRet    = false;
    bool     haveBit = false;
    unsigned index;

    if (withinTolerance(duration, kHalfSymbolUs))
    {
        m_halfSymbol = !m_halfSymbol;
        haveBit      = !m_halfSymbol;
    }
    else if (!m_halfSymbol && withinTolerance(duration, kFullSymbolUs))
    {
        m_previousBit = !m_previousBit;
        haveBit       = true;
    }
    else
    {
        ++m_diagnostics.timingAborts;
        m_expectedBits = 0;
        m_syncEdges    = withinTolerance(duration, kHardwareSyncUs) ? 1 : 0;
    }

    if (haveBit)
    {
        index = m_buffer.bitCount++;
        // RTS sends the most significant bit of each byte first.
        m_buffer.bytes[index / CHAR_BIT] |= uint8_t(m_previousBit) << (CHAR_BIT - 1 - index % CHAR_BIT);
        if (m_buffer.bitCount > m_diagnostics.mostBits)
        {
            m_diagnostics.mostBits = m_buffer.bitCount;
        }
        if (m_buffer.bitCount == m_expectedBits)
        {
            frame          = m_buffer;
            m_expectedBits = 0;
            m_syncEdges    = 0;
            bRet           = true;
        }
    }
    return bRet;
}

bool SomfyReceiver::DecodeBase(Frame &frame)
{
    constexpr std::size_t kBaseFrameBytes = kStandardFrameBits / CHAR_BIT;
    bool                  bRet            = false;
    uint8_t               checksum        = 0;

    if (frame.bitCount == kStandardFrameBits || frame.bitCount == kExtendedFrameBits)
    {
        // Both variants obfuscate the base frame by XORing each byte with its predecessor.
        for (std::size_t index = kBaseFrameBytes - 1; index > 0; --index)
        {
            frame.bytes[index] ^= frame.bytes[index - 1];
        }

        // The base checksum is valid when all 4-bit nibbles, including the checksum, XOR to zero.
        for (std::size_t index = 0; index < kBaseFrameBytes; ++index)
        {
            checksum ^= frame.bytes[index] ^ (frame.bytes[index] >> kNibbleBits);
        }
        bRet = (checksum & kChecksumMask) == 0;
    }
    return bRet;
}

bool SomfyReceiver::withinTolerance(uint32_t duration, uint32_t nominal)
{
    return duration >= nominal * (kPercent - kTimingTolerancePercent) / kPercent &&
           duration <= nominal * (kPercent + kTimingTolerancePercent) / kPercent;
}
