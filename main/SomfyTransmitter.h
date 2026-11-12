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
#include <cstdint>
#include <driver/rmt_tx.h>
#include <nvs.h>

namespace TI_CC1101
{
    class CC1101Device;
}

// One persistent virtual remote. Call from the radio task.
class SomfyTransmitter final
{
  public:
    enum class Command : uint8_t
    {
        My      = 0x1, // RTS My: stop while moving, favorite position while idle.
        Open    = 0x2,
        Close   = 0x4,
        Program = 0x8 // Short programming command to add/remove a remote in motor programming mode.
    };

    enum class Result
    {
        NotSent,
        TransmissionUncertain, // A frame was queued, but the full burst did not complete.
        Transmitted,
        TransmittedWithCleanupError
    };

    struct Status
    {
        uint32_t address;
        uint32_t nextCode;
        bool     ready;
    };

    bool   Init(gpio_num_t txPin);
    Result Send(TI_CC1101::CC1101Device &radio, Command command);
    Status GetStatus() const;

  private:
    bool                              loadRemote();
    void                              buildFrame(Command command, uint16_t rollingCode);
    void                              buildSymbols(bool firstFrame);

    nvs_handle_t                      m_storage  = 0;
    rmt_channel_handle_t              m_channel  = nullptr;
    rmt_encoder_handle_t              m_encoder  = nullptr;
    uint32_t                          m_address  = 0;
    uint32_t                          m_nextCode = 0; // Wider than the wire counter so exhaustion never wraps to zero.
    bool                              m_ready    = false;
    // Standard RTS payload: key, command/checksum, two-byte rolling code, three-byte address.
    std::array<uint8_t, 7>            m_frame{};
    // Wake-up, seven hardware syncs, software sync, 56 Manchester bits, and silence.
    std::array<rmt_symbol_word_t, 66> m_symbols{};
    size_t                            m_symbolCount = 0;
};
