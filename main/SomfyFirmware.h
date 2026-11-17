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

#include <atomic>
#include <cstdint>
#include <esp_http_server.h>

class SomfyFirmware final
{
  public:
    esp_err_t Upload(httpd_req_t *request);
    bool PauseRadio(bool timedMove);
    void CheckBoot(bool healthy);

  private:
    enum class State { Idle, Requested, Granted, Reboot };
    std::atomic<State> m_state{State::Idle};
    uint64_t m_restartAtUs = 0; // Published by the transition to Reboot.
};
