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

#include <array>
#include <climits>
#include <cstdint>

class SomfyReceiver final
{
    // The extended frame carries three extra bytes after the standard seven.
    static constexpr unsigned kStandardFrameBits  = 56;
    static constexpr unsigned kExtendedFrameBits  = 80;
    static constexpr unsigned kExtendedFrameBytes = kExtendedFrameBits / CHAR_BIT;

  public:
    struct Frame
    {
        std::array<uint8_t, kExtendedFrameBytes> bytes{};
        unsigned                                 bitCount = 0;

        uint32_t                                 RemoteAddress() const;
        unsigned                                 Command() const;
        unsigned                                 RollingCode() const;
    };

    struct Diagnostics
    {
        unsigned syncs        = 0;
        unsigned timingAborts = 0;
        unsigned mostBits     = 0;
    };

    Diagnostics TakeDiagnostics();
    bool        OnEdge(uint32_t timestamp, Frame &frame);
    static bool DecodeBase(Frame &frame);

  private:
    static bool withinTolerance(uint32_t duration, uint32_t nominal);
    void        processSyncPulse(uint32_t duration);
    bool        processDataPulse(uint32_t duration, Frame &frame);

    Frame       m_buffer{};
    Diagnostics m_diagnostics{};
    uint32_t    m_lastEdge     = 0;
    unsigned    m_syncEdges    = 0;
    unsigned    m_expectedBits = 0;
    bool        m_haveEdge     = false;
    bool        m_previousBit  = false;
    bool        m_halfSymbol   = false;
};
