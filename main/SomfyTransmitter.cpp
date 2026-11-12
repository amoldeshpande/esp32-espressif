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
#include <cstdio>
#include <cstring>
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

bool SomfyTransmitter::saveBlind(uint32_t id, const Record &record)
{
    bool bRet = true;
    char key[sizeof("blind_v1_8")]; // One versioned NVS record per blind.

    snprintf(key, sizeof(key), "blind_v1_%" PRIu32, id);
    CER(nvs_set_blob(m_storage, key, &record, sizeof(record)));
    CER(nvs_commit(m_storage));
    m_blinds[id - 1] = record;

Error:
    if (!bRet)
    {
        // A write may have reached flash. Reload the records before using TX again.
        m_ready = false;
        ESP_LOGE(TAG, "Could not save blind %" PRIu32, id);
    }
    return bRet;
}

bool SomfyTransmitter::hasAddress(uint32_t address) const
{
    bool found = false;

    for (const Record &record : m_blinds)
    {
        if (record.address == address)
        {
            found = true;
            break;
        }
    }
    return found;
}

bool SomfyTransmitter::migrateFirstBlind()
{
    bool      bRet = true;
    Record    record{};
    esp_err_t addressResult;
    esp_err_t codeResult;

    addressResult = nvs_get_u32(m_storage, "address", &record.address);
    codeResult    = nvs_get_u32(m_storage, "next_code", &record.nextCode);
    if (addressResult == ESP_ERR_NVS_NOT_FOUND && codeResult == ESP_ERR_NVS_NOT_FOUND)
    {
        // A fresh board starts with one unpaired entry for the serial controls.
        CBR(createBlind("Blind 1") == 1);
    }
    else
    {
        // Preserve the bench identity and its next unused code together.
        CER(addressResult);
        CER(codeResult);
        CBR(record.address != 0 && record.address < kAddressMask);
        CBR(record.nextCode != 0 && record.nextCode <= kLastRollingCode + 1);
        record.pairing = Pairing::Paired;
        memcpy(record.name, "Blind 1", sizeof("Blind 1"));
        CBR(saveBlind(1, record));
    }

Error:
    return bRet;
}

bool SomfyTransmitter::loadBlinds()
{
    bool      bRet    = true;
    bool      missing = false;
    Record    record;
    char      key[sizeof("blind_v1_8")];
    size_t    size;
    esp_err_t result;

    CER(nvs_flash_init());
    CER(nvs_open("somfy_bench", NVS_READWRITE, &m_storage));
    for (uint32_t id = 1; id <= m_blinds.size(); ++id)
    {
        snprintf(key, sizeof(key), "blind_v1_%" PRIu32, id);
        size   = sizeof(record);
        result = nvs_get_blob(m_storage, key, &record, &size);
        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            missing = true;
        }
        else
        {
            CER(result);
            CBR(!missing && size == sizeof(record));
            CBR(record.address != 0 && record.address < kAddressMask && !hasAddress(record.address));
            CBR(record.nextCode != 0 && record.nextCode <= kLastRollingCode + 1);
            CBR(record.pairing <= Pairing::Paired);
            CBR(record.name[0] && record.name[sizeof(record.name) - 1] == '\0');
            m_blinds[id - 1] = record;
        }
    }
    if (m_blinds.front().address == 0)
    {
        CBR(migrateFirstBlind());
    }

Error:
    return bRet;
}

uint32_t SomfyTransmitter::AddBlind(const char *name)
{
    return m_ready ? createBlind(name) : 0;
}

uint32_t SomfyTransmitter::createBlind(const char *name)
{
    bool         bRet   = true;
    const size_t length = strlen(name);
    uint32_t     id     = 0;
    Record       record{};

    CBR(m_storage != 0 && length > 0 && length < sizeof(record.name));
    for (uint32_t candidate = 1; candidate <= m_blinds.size(); ++candidate)
    {
        if (m_blinds[candidate - 1].address == 0)
        {
            id = candidate;
            break;
        }
    }
    CBR(id != 0);
    record.address = esp_random() & kAddressMask;
    while (record.address == 0 || record.address == kAddressMask || hasAddress(record.address))
    {
        record.address = (record.address + 1) & kAddressMask;
    }
    record.nextCode = 1; // First nonzero RTS rolling code for a new address.
    record.pairing  = Pairing::Unpaired;
    memcpy(record.name, name, length);
    CBR(saveBlind(id, record));

Error:
    return bRet ? id : 0;
}

bool SomfyTransmitter::ConfirmPairing(uint32_t id)
{
    bool   bRet = true;
    Record record;

    CBR(m_ready);
    CBR(id != 0 && id <= m_blinds.size());
    record = m_blinds[id - 1];
    CBR(record.address != 0 && record.pairing == Pairing::AwaitingConfirmation);
    record.pairing = Pairing::Paired;
    CBR(saveBlind(id, record));

Error:
    return bRet;
}

bool SomfyTransmitter::Init(gpio_num_t txPin)
{
    bool                      bRet = true;
    rmt_tx_channel_config_t   channelConfig{};
    rmt_copy_encoder_config_t encoderConfig{};

    CBRA(!m_ready && !m_channel && !m_encoder && !m_storage);
    CBR(loadBlinds());
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
        m_blinds = {};
        ESP_LOGE(TAG, "TX initialization failed; NVS has not been erased");
    }
    return bRet;
}

void SomfyTransmitter::buildFrame(Command command, uint32_t address, uint16_t rollingCode)
{
    uint8_t checksum = 0;

    // Match the reference RTS key range A0-AF to the low nibble of the rolling code.
    m_frame[0]       = 0xA0 | (rollingCode & kNibbleMask);
    m_frame[1]       = static_cast<uint8_t>(command) << kNibbleBits;
    // Rolling code and 24-bit remote address are transmitted most significant byte first.
    m_frame[2]       = static_cast<uint8_t>(rollingCode >> CHAR_BIT);
    m_frame[3]       = static_cast<uint8_t>(rollingCode);
    m_frame[4]       = static_cast<uint8_t>(address >> (2 * CHAR_BIT));
    m_frame[5]       = static_cast<uint8_t>(address >> CHAR_BIT);
    m_frame[6]       = static_cast<uint8_t>(address);
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

SomfyTransmitter::Result SomfyTransmitter::Send(TI_CC1101::CC1101Device &radio, uint32_t id, Command command)
{
    bool                  bRet           = true;
    bool                  radioAttempted = false;
    bool                  newPairing     = false;
    bool                  channelEnabled = false;
    rmt_transmit_config_t transmitConfig{}; // No looping; idle output is low (carrier off).
    uint16_t              rollingCode;
    Record                record;
    Result                result = Result::NotSent;

    CBR(m_ready);
    CBR(command == Command::My || command == Command::Open ||
        command == Command::Close || command == Command::Program);
    CBR(id != 0 && id <= m_blinds.size());
    record = m_blinds[id - 1];
    CBR(record.address != 0);
    if (command == Command::Program)
    {
        CBR(record.pairing != Pairing::Paired);
        newPairing     = record.pairing == Pairing::Unpaired;
        // Save pairing intent with the reserved code before any pulse can reach the motor.
        record.pairing = Pairing::AwaitingConfirmation;
    }
    else
    {
        CBR(record.pairing == Pairing::Paired);
    }
    if (record.nextCode > kLastRollingCode)
    {
        ESP_LOGE(TAG, "Rolling codes exhausted; this virtual remote needs replacement and pairing");
    }
    CBR(record.nextCode <= kLastRollingCode);
    rollingCode = static_cast<uint16_t>(record.nextCode++);
    CBR(saveBlind(id, record));
    buildFrame(command, record.address, rollingCode);
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
             record.address, static_cast<unsigned>(command), rollingCode);

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
    if (newPairing && radioAttempted && result == Result::NotSent)
    {
        // The motor could not have seen this attempt. Keep the reserved code consumed.
        record.pairing = Pairing::Unpaired;
        saveBlind(id, record);
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

SomfyTransmitter::Status SomfyTransmitter::GetStatus(uint32_t id) const
{
    Status        status{};
    const Record *record;

    if (id != 0 && id <= m_blinds.size())
    {
        record          = &m_blinds[id - 1];
        status.address  = record->address;
        status.nextCode = record->nextCode;
        status.pairing  = record->pairing;
        status.ready    = m_ready && record->address != 0 && record->pairing == Pairing::Paired &&
                       record->nextCode <= kLastRollingCode;
        memcpy(status.name, record->name, sizeof(status.name));
    }
    return status;
}

SomfyTransmitter::Blinds SomfyTransmitter::GetBlinds() const
{
    Blinds blinds{};

    for (uint32_t id = 1; id <= blinds.size(); ++id)
    {
        blinds[id - 1] = GetStatus(id);
    }
    return blinds;
}
