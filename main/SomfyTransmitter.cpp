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

#include "SomfyTransmitter.h"

#include <CC1101Lib/CC1101Device.h>
#include <CC1101Lib/LocalTypes.h>
#include <cinttypes>
#include <climits>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs_flash.h>

static const char *TAG = "SomfyTransmitter";

namespace
{
    static constexpr uint32_t kAddressMask     = 0xFFFFFF;   // RTS transmitter addresses are 24 bits.
    static constexpr uint32_t kLastRollingCode = UINT16_MAX; // RTS carries an unsigned 16-bit counter.
    static constexpr unsigned kNibbleBits      = CHAR_BIT / 2;
    static constexpr uint8_t  kNibbleMask      = (1 << kNibbleBits) - 1;
    static constexpr uint16_t kHalfSymbolUs    = 640; // RTS Manchester half-bit duration.
    static constexpr uint16_t kHardwareSyncUs  = 4 * kHalfSymbolUs;
} // namespace

bool SomfyTransmitter::loadRemote()
{
    bool      bRet = true;
    esp_err_t addressResult;
    esp_err_t codeResult;

    // Storage and peripheral failures are recoverable; keep reception running in debug builds too.
    CER(nvs_flash_init());
    CER(nvs_open("somfy_bench", NVS_READWRITE, &m_storage));
    addressResult = nvs_get_u32(m_storage, "address", &m_address);
    codeResult    = nvs_get_u32(m_storage, "next_code", &m_nextCode);
    if (addressResult == ESP_ERR_NVS_NOT_FOUND && codeResult == ESP_ERR_NVS_NOT_FOUND)
    {
        do
        {
            m_address = esp_random() & kAddressMask;
        } while (m_address == 0 || m_address == kAddressMask);
        m_nextCode = 1; // Start a new virtual remote at the first nonzero rolling code.
        CER(nvs_set_u32(m_storage, "address", m_address));
        CER(nvs_set_u32(m_storage, "next_code", m_nextCode));
        CER(nvs_commit(m_storage));
    }
    else
    {
        // Missing or invalid state must not silently create a replacement paired identity.
        CER(addressResult);
        CER(codeResult);
    }
    CBR(m_address != 0 && m_address < kAddressMask);
    CBR(m_nextCode != 0 && m_nextCode <= kLastRollingCode + 1);

Error:
    return bRet;
}

bool SomfyTransmitter::Init(gpio_num_t txPin)
{
    bool                      bRet = true;
    rmt_tx_channel_config_t   channelConfig{};
    rmt_copy_encoder_config_t encoderConfig{};

    CBRA(!m_ready && !m_channel && !m_encoder && !m_storage);
    CBR(loadRemote());
    channelConfig.gpio_num          = txPin;
    channelConfig.clk_src           = RMT_CLK_SRC_DEFAULT;
    channelConfig.resolution_hz     = 1'000'000; // One RMT tick per microsecond.
    channelConfig.mem_block_symbols = 128;       // Two ESP32 blocks hold the whole longest frame without ISR refills.
    channelConfig.trans_queue_depth = 1;         // The radio task sends one frame at a time.
    CER(rmt_new_tx_channel(&channelConfig, &m_channel));
    CER(rmt_new_copy_encoder(&encoderConfig, &m_encoder));
    m_ready = true;
    ESP_LOGI(TAG, "TX on GPIO%d: connect to CC1101 GDO0", txPin);

Error:
    if (!bRet)
    {
        if (m_encoder)
        {
            rmt_del_encoder(m_encoder);
            m_encoder = nullptr;
        }
        if (m_channel)
        {
            rmt_del_channel(m_channel);
            m_channel = nullptr;
        }
        if (m_storage)
        {
            nvs_close(m_storage);
            m_storage = 0;
        }
        ESP_LOGE(TAG, "TX initialization failed; NVS has not been erased");
    }
    return bRet;
}

void SomfyTransmitter::buildFrame(Command command, uint16_t rollingCode)
{
    uint8_t checksum = 0;

    // Match the reference RTS key range A0-AF to the low nibble of the rolling code.
    m_frame[0]       = 0xA0 | (rollingCode & kNibbleMask);
    m_frame[1]       = static_cast<uint8_t>(command) << kNibbleBits;
    // Rolling code and 24-bit remote address are transmitted most significant byte first.
    m_frame[2]       = static_cast<uint8_t>(rollingCode >> CHAR_BIT);
    m_frame[3]       = static_cast<uint8_t>(rollingCode);
    m_frame[4]       = static_cast<uint8_t>(m_address >> (2 * CHAR_BIT));
    m_frame[5]       = static_cast<uint8_t>(m_address >> CHAR_BIT);
    m_frame[6]       = static_cast<uint8_t>(m_address);
    for (uint8_t value : m_frame)
    {
        checksum ^= value ^ (value >> kNibbleBits);
    }
    m_frame[1] |= checksum & kNibbleMask;
    for (size_t i = 1; i < m_frame.size(); ++i)
    {
        m_frame[i] ^= m_frame[i - 1]; // RTS obfuscation uses the preceding encoded byte.
    }
}

void SomfyTransmitter::buildSymbols(bool firstFrame)
{
    rmt_symbol_word_t symbol{};
    bool              bit;

    m_symbolCount = 0;
    if (firstFrame)
    {
        // Wake-up timings measured by ESPSomfy-RTS on Telis remotes, in microseconds.
        symbol                     = {};
        symbol.level0              = 1;
        symbol.duration0           = 10'920;
        symbol.level1              = 0;
        symbol.duration1           = 7'357;
        m_symbols[m_symbolCount++] = symbol;
    }
    // A standard frame has two hardware sync pulses initially, seven on each repeat.
    for (unsigned i = 0; i < (firstFrame ? 2U : 7U); ++i)
    {
        symbol.level0              = 1;
        symbol.duration0           = kHardwareSyncUs;
        symbol.level1              = 0;
        symbol.duration1           = kHardwareSyncUs;
        m_symbols[m_symbolCount++] = symbol;
    }
    symbol.level0              = 1;
    symbol.duration0           = 4'850; // RTS software sync high pulse, in microseconds.
    symbol.level1              = 0;
    symbol.duration1           = kHalfSymbolUs;
    m_symbols[m_symbolCount++] = symbol;
    for (uint8_t value : m_frame)
    {
        for (unsigned i = 0; i < CHAR_BIT; ++i)
        {
            bit                        = (value & (1U << (CHAR_BIT - 1 - i))) != 0;
            // Manchester one is low/high; zero is high/low, most significant bit first.
            symbol.level0              = !bit;
            symbol.duration0           = kHalfSymbolUs;
            symbol.level1              = bit;
            symbol.duration1           = kHalfSymbolUs;
            m_symbols[m_symbolCount++] = symbol;
        }
    }
    // The reference's 27,434 us inter-frame silence, split into equal low halves.
    symbol.level0              = 0;
    symbol.duration0           = 13'717;
    symbol.level1              = 0;
    symbol.duration1           = symbol.duration0;
    m_symbols[m_symbolCount++] = symbol;
}

SomfyTransmitter::Result SomfyTransmitter::Send(TI_CC1101::CC1101Device &radio, Command command)
{
    bool                  bRet           = true;
    bool                  radioAttempted = false;
    bool                  channelEnabled = false;
    rmt_transmit_config_t transmitConfig{}; // No looping; idle output is low (carrier off).
    uint16_t              rollingCode;
    Result                result = Result::NotSent;

    CBR(m_ready);
    CBR(command == Command::My || command == Command::Open ||
        command == Command::Close || command == Command::Program);
    if (m_nextCode > kLastRollingCode)
    {
        ESP_LOGE(TAG, "Rolling codes exhausted; this virtual remote needs replacement and pairing");
    }
    CBR(m_nextCode <= kLastRollingCode);
    rollingCode = static_cast<uint16_t>(m_nextCode);
    ++m_nextCode;
    // Fail closed on a write error. Never transmit until the next unused code is durable.
    m_ready = false;
    CER(nvs_set_u32(m_storage, "next_code", m_nextCode));
    CER(nvs_commit(m_storage));
    m_ready = true;
    buildFrame(command, rollingCode);
    radioAttempted = true;
    CBR(radio.BeginTransmit());
    CER(rmt_enable(m_channel));
    channelEnabled = true;
    // Send the initial frame and two repeats with the same rolling code, like a short button press.
    for (unsigned repeat = 0; repeat < 3; ++repeat)
    {
        buildSymbols(repeat == 0);
        CER(rmt_transmit(m_channel, m_encoder, m_symbols.data(),
                         m_symbolCount * sizeof(m_symbols[0]), &transmitConfig));
        result = Result::TransmissionUncertain;
        // Each frame is under 150 ms; allow 500 ms before declaring a peripheral failure.
        CER(rmt_tx_wait_all_done(m_channel, 500));
    }
    result = Result::Transmitted;
    ESP_LOGI(TAG, "Transmitted remote=%06" PRIX32 " command=%u rolling=%u; motor response is unconfirmed",
             m_address, static_cast<unsigned>(command), rollingCode);

Error:
    if (channelEnabled)
    {
        if (rmt_disable(m_channel) != ESP_OK)
        {
            m_ready = false;
            bRet    = false;
            ESP_LOGE(TAG, "Could not disable RMT; TX disabled until restart");
        }
    }
    if (radioAttempted)
    {
        if (!radio.EndTransmit())
        {
            m_ready = false;
            bRet    = false;
            ESP_LOGE(TAG, "Could not restore RX; TX disabled until restart");
        }
    }
    if (!bRet)
    {
        ESP_LOGE(TAG, "Command failed; any reserved rolling code will not be reused");
    }
    if (!bRet && result == Result::Transmitted)
    {
        result = Result::TransmittedWithCleanupError;
    }
    return result;
}

SomfyTransmitter::Status SomfyTransmitter::GetStatus() const
{
    return {m_address, m_nextCode, m_ready && m_nextCode <= kLastRollingCode};
}
