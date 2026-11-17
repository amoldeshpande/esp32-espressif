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
#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
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

namespace
{
    constexpr uint32_t kLegacyBackupFormat = 0x31425345; // ESB1 in little-endian byte order.
    constexpr uint32_t kFavoriteFormat     = 0x32425345; // ESB2 adds My favorites.
    constexpr uint32_t kIndividualFormat   = 0x33425345; // ESB3 adds one individual remote per blind.
    constexpr uint32_t kPhysicalFormat     = 0x34425345; // ESB4 adds the remaining individual remote slots.
    constexpr uint32_t kGroupChannelFormat = 0x35425345; // ESB5 adds group channels.
    constexpr uint32_t kBackupFormat       = 0x36425345; // ESB6 adds user-defined blind groups.
    constexpr char     kBlindGroupsKey[]   = "blind_groups_v1";
    constexpr char     kGroupChannelsKey[] = "group_ch_v1"; // Physical channels shared by group members.
    constexpr char     kRestoreKey[] = "restore_v1"; // Durable journal for interrupted restores.
} // namespace

uint32_t SomfyTransmitter::backupChecksum(const Backup &backup)
{
    static_assert(sizeof(Record) == 56 && sizeof(GroupName) == 40 &&
                  offsetof(Backup, individualAddresses) == 776 &&
                  offsetof(Backup, additionalAddresses) == 812 &&
                  offsetof(Backup, groupChannels) == 912 &&
                  offsetof(Backup, blindGroups) == 1076 && sizeof(Backup) == 1624);
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&backup), offsetof(Backup, checksum));
}

size_t SomfyTransmitter::Backup::LegacySize()
{
    return offsetof(Backup, individualAddresses);
}

size_t SomfyTransmitter::Backup::IndividualSize()
{
    return offsetof(Backup, additionalAddresses);
}

size_t SomfyTransmitter::Backup::PhysicalSize()
{
    return offsetof(Backup, groupChannels);
}

size_t SomfyTransmitter::Backup::GroupChannelSize()
{
    return offsetof(Backup, blindGroups);
}

bool SomfyTransmitter::Backup::MatchesSize(size_t size) const
{
    bool matches = false;

    if (format == kBackupFormat)
    {
        matches = size == sizeof(Backup);
    }
    else if (format == kGroupChannelFormat)
    {
        matches = size == GroupChannelSize();
    }
    else if (format == kPhysicalFormat)
    {
        matches = size == PhysicalSize();
    }
    else if (format == kIndividualFormat)
    {
        matches = size == IndividualSize();
    }
    else if (format == kLegacyBackupFormat || format == kFavoriteFormat)
    {
        matches = size == LegacySize();
    }
    return matches;
}

SomfyTransmitter::IndividualAddresses SomfyTransmitter::backupAddresses(const Backup &backup, size_t index)
{
    IndividualAddresses addresses{backup.individualAddresses[index]};

    std::copy(backup.additionalAddresses[index].begin(), backup.additionalAddresses[index].end(),
              addresses.begin() + 1); // The first slot comes from the ESB3 section.
    return addresses;
}

bool SomfyTransmitter::validateBackup(Backup &backup)
{
    bool            bRet = true;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&backup);

    if (backup.format != kBackupFormat && backup.format != kGroupChannelFormat)
    {
        backup.groupChannels = {};
    }

    if (backup.format == kBackupFormat)
    {
        CBR(backup.checksum == backupChecksum(backup));
        CBR(validGroupChannels(backup.groupChannels));
        CBR(validBlindGroups(backup.blindGroups));
    }
    else if (backup.format == kGroupChannelFormat)
    {
        CBR(backup.groupChannelChecksum == esp_rom_crc32_le(0, bytes, offsetof(Backup, groupChannelChecksum)));
        CBR(validGroupChannels(backup.groupChannels));
        backup.groupChannelChecksum = 0;
    }
    else if (backup.format == kPhysicalFormat)
    {
        CBR(backup.physicalChecksum == esp_rom_crc32_le(0, bytes, offsetof(Backup, physicalChecksum)));
        backup.physicalChecksum = 0;
        backup.groupChannels = {};
    }
    else if (backup.format == kIndividualFormat)
    {
        CBR(backup.individualChecksum == esp_rom_crc32_le(0, bytes, offsetof(Backup, individualChecksum)));
        backup.individualChecksum = 0;
        backup.additionalAddresses = {};
    }
    else
    {
        CBR(backup.format == kLegacyBackupFormat || backup.format == kFavoriteFormat);
        CBR(backup.legacyChecksum == esp_rom_crc32_le(0, bytes, offsetof(Backup, legacyChecksum)));
        backup.legacyChecksum = 0;
        backup.individualAddresses = {};
        backup.additionalAddresses = {};
        if (backup.format == kLegacyBackupFormat)
        {
            for (Record &record : backup.blinds)
            {
                record.favorite = 0;
            }
        }
    }

    if (backup.format != kBackupFormat)
    {
        backup.blindGroups = {};
    }

Error:
    return bRet;
}

SomfyTransmitter::Backup SomfyTransmitter::ExportBackup() const
{
    Backup backup{};

    if (m_ready && !m_storageFailed)
    {
        backup.format   = kBackupFormat;
        backup.blinds   = m_blinds;
        backup.groups   = m_groupNames;
        backup.groupChannels = m_groupChannels;
        backup.blindGroups = m_blindGroups;
        for (size_t i = 0; i < m_individualAddresses.size(); ++i)
        {
            backup.individualAddresses[i] = m_individualAddresses[i].front();
            std::copy(m_individualAddresses[i].begin() + 1, m_individualAddresses[i].end(),
                      backup.additionalAddresses[i].begin());
        }
        backup.checksum = backupChecksum(backup);
    }
    return backup;
}

bool SomfyTransmitter::applyBackup(const Backup &backup)
{
    bool          bRet = true;
    char          key[sizeof("blind_v1_8")];
    const Record *record;
    StoredRecord  stored;
    esp_err_t     result;

    for (uint32_t id = 1; id <= backup.blinds.size(); ++id)
    {
        snprintf(key, sizeof(key), "blind_v1_%" PRIu32, id);
        record = &backup.blinds[id - 1];
        if (record->address)
        {
            stored = {*record, kPhysicalFormat, backupAddresses(backup, id - 1)};
            CER(nvs_set_blob(m_storage, key, &stored, sizeof(stored)));
        }
        else
        {
            result = nvs_erase_key(m_storage, key);
            if (result != ESP_ERR_NVS_NOT_FOUND)
            {
                CER(result);
            }
        }
    }
    CER(nvs_set_blob(m_storage, "group_names_v1", &backup.groups, sizeof(backup.groups)));
    CER(nvs_set_blob(m_storage, kGroupChannelsKey, &backup.groupChannels, sizeof(backup.groupChannels)));
    CER(nvs_set_blob(m_storage, kBlindGroupsKey, &backup.blindGroups, sizeof(backup.blindGroups)));
    CER(nvs_commit(m_storage));
    CER(nvs_erase_key(m_storage, kRestoreKey));
    CER(nvs_commit(m_storage));

Error:
    return bRet;
}

bool SomfyTransmitter::recoverBackup()
{
    bool            bRet = true;
    Backup          backup{};
    size_t          size   = sizeof(backup);
    const esp_err_t result = nvs_get_blob(m_storage, kRestoreKey, &backup, &size);

    if (result != ESP_ERR_NVS_NOT_FOUND)
    {
        CER(result);
        CBR(backup.MatchesSize(size));
        CBR(validateBackup(backup));
        CBR(applyBackup(backup));
    }

Error:
    return bRet;
}

const char *SomfyTransmitter::RestoreBackup(Backup backup)
{
    bool          bRet    = true;
    bool          writing = false;
    const char   *outcome = "invalid_backup";
    Record       *record;
    const Record *saved;
    IndividualAddresses addresses;

    CBR(m_ready && !m_storageFailed);
    CBR(validateBackup(backup));
    CBR(backup.blinds.front().address != 0);
    for (size_t i = 0; i < backup.blinds.size(); ++i)
    {
        record = &backup.blinds[i];
        addresses = backupAddresses(backup, i);
        for (uint32_t address : addresses)
        {
            CBR(address < kAddressMask);
            CBR(!address || (record->address && record->pairing != Pairing::Removed));
        }
        if (record->address)
        {
            CBR(record->favorite <= 101); // Encoded range includes zero for unset and 101 for fully open.
            CBR(record->address < kAddressMask && record->remoteAddress < kAddressMask);
            CBR(record->nextCode != 0 && record->nextCode <= kLastRollingCode + 1);
            CBR(record->pairing <= Pairing::Removed && record->name[0] && record->name[sizeof(record->name) - 1] == '\0');
            // Travel times use the same ten-minute limit as calibration.
            CBR(record->openMs <= 600'000 && record->closeMs <= 600'000);
            for (size_t j = 0; j < i; ++j)
            {
                CBR(backup.blinds[j].address != record->address);
            }
        }
    }
    for (const GroupName &group : backup.groups)
    {
        CBR(group.address < kAddressMask && group.name[sizeof(group.name) - 1] == '\0');
    }

    outcome = "backup_identity_conflict";
    for (size_t i = 0; i < m_blinds.size(); ++i)
    {
        record = &backup.blinds[i];
        saved  = &m_blinds[i];
        if (saved->address == record->address)
        {
            record->nextCode = std::max(record->nextCode, saved->nextCode);
        }
        else
        {
            // Only unused identities can be replaced. Keep known counters with their saved IDs.
            CBR(!saved->address || (saved->pairing == Pairing::Unpaired && saved->nextCode == 1));
        }
    }

    outcome         = "restore_write_failed";
    writing         = true;
    backup.format   = kBackupFormat;
    backup.checksum = backupChecksum(backup);
    CER(nvs_set_blob(m_storage, kRestoreKey, &backup, sizeof(backup)));
    CER(nvs_commit(m_storage));
    CBR(applyBackup(backup));
    m_blinds     = backup.blinds;
    m_groupNames = backup.groups;
    m_groupChannels = backup.groupChannels;
    m_blindGroups = backup.blindGroups;
    for (size_t i = 0; i < m_individualAddresses.size(); ++i)
    {
        m_individualAddresses[i] = backupAddresses(backup, i);
    }
    outcome      = "backup_restored";

Error:
    if (!bRet && writing)
    {
        m_storageFailed = true;
        m_ready         = false;
        ESP_LOGE(TAG, "Restore did not finish. Restart to recover the journal.");
    }
    return outcome;
}

bool SomfyTransmitter::saveBlind(uint32_t id, const Record &record)
{
    return saveBlind(id, record, record.pairing == Pairing::Removed ? IndividualAddresses{} : m_individualAddresses[id - 1]);
}

bool SomfyTransmitter::saveBlind(uint32_t id, const Record &record,
                                 const IndividualAddresses &individualAddresses)
{
    bool               bRet = true;
    char               key[sizeof("blind_v1_8")]; // One versioned NVS record per blind.
    const StoredRecord stored{record, kPhysicalFormat, individualAddresses};

    CBR(!m_storageFailed);
    snprintf(key, sizeof(key), "blind_v1_%" PRIu32, id);
    CER(nvs_set_blob(m_storage, key, &stored, sizeof(stored)));
    CER(nvs_commit(m_storage));
    m_blinds[id - 1] = record;
    m_individualAddresses[id - 1] = individualAddresses;

Error:
    if (!bRet)
    {
        // A write may have reached flash. Reload the records before using TX again.
        m_ready         = false;
        m_storageFailed = true;
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
    bool      bRet = true;

    StoredRecord stored;
    Record      &record = stored.record;
    char         key[sizeof("blind_v1_8")];
    size_t    size;
    esp_err_t result;

    CER(nvs_flash_init());
    CER(nvs_open("somfy_bench", NVS_READWRITE, &m_storage));
    CBR(recoverBackup());
    for (uint32_t id = 1; id <= m_blinds.size(); ++id)
    {
        snprintf(key, sizeof(key), "blind_v1_%" PRIu32, id);
        stored = {};
        size   = sizeof(stored);
        result = nvs_get_blob(m_storage, key, &stored, &size);
        if (result != ESP_ERR_NVS_NOT_FOUND)
        {
            CER(result);
            // Legacy records contain padding where the favorite is stored now.
            if (size == sizeof(record) || size == offsetof(Record, remoteAddress))
            {
                record.favorite = 0;
            }
            else
            {
                CBR((size == sizeof(stored) && stored.format == kPhysicalFormat) ||
                    (size == offsetof(StoredRecord, individualAddresses) + sizeof(uint32_t) &&
                     stored.format == kIndividualFormat) ||
                    (size == offsetof(StoredRecord, individualAddresses) && stored.format == kFavoriteFormat));
                CBR(record.favorite <= 101); // Encoded fully open.
            }
            CBR(record.address != 0 && record.address < kAddressMask && !hasAddress(record.address));
            CBR(record.nextCode != 0 && record.nextCode <= kLastRollingCode + 1);
            CBR(record.pairing <= Pairing::Removed);
            CBR(record.remoteAddress < kAddressMask);
            CBR(record.name[0] && record.name[sizeof(record.name) - 1] == '\0');
            for (uint32_t address : stored.individualAddresses)
            {
                CBR(address < kAddressMask);
            }
            m_blinds[id - 1] = record;
            m_individualAddresses[id - 1] = stored.individualAddresses;
        }
    }
    size   = sizeof(m_groupNames);
    result = nvs_get_blob(m_storage, "group_names_v1", &m_groupNames, &size);
    if (result != ESP_ERR_NVS_NOT_FOUND)
    {
        CER(result);
        CBR(size == sizeof(m_groupNames));
        for (const GroupName &group : m_groupNames)
        {
            CBR(group.address < kAddressMask && group.name[sizeof(group.name) - 1] == '\0');
        }
    }
    size   = sizeof(m_groupChannels);
    result = nvs_get_blob(m_storage, kGroupChannelsKey, &m_groupChannels, &size);
    if (result != ESP_ERR_NVS_NOT_FOUND)
    {
        CER(result);
        CBR(size == sizeof(m_groupChannels) && validGroupChannels(m_groupChannels));
    }
    size   = sizeof(m_blindGroups);
    result = nvs_get_blob(m_storage, kBlindGroupsKey, &m_blindGroups, &size);
    if (result != ESP_ERR_NVS_NOT_FOUND)
    {
        CER(result);
        CBR(size == sizeof(m_blindGroups) && validBlindGroups(m_blindGroups));
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
    const auto   usedByGroup = [&record](const BlindGroup &group) {
        return std::find(group.members.begin(), group.members.end(), record.address) != group.members.end();
    };

    CBR(m_storage != 0 && length > 0 && length < sizeof(record.name));
    for (uint32_t candidate = 1; candidate <= m_blinds.size(); ++candidate)
    {
        if (!exists(candidate))
        {
            id = candidate;
            break;
        }
    }
    CBR(id != 0);
    record.address = esp_random() & kAddressMask;
    while (record.address == 0 || record.address == kAddressMask || hasAddress(record.address) ||
           std::any_of(m_blindGroups.begin(), m_blindGroups.end(), usedByGroup))
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

bool SomfyTransmitter::exists(uint32_t id) const
{
    return id != 0 && id <= m_blinds.size() && m_blinds[id - 1].address != 0 &&
           m_blinds[id - 1].pairing != Pairing::Removed;
}

bool SomfyTransmitter::RenameBlind(uint32_t id, const char *name)
{
    bool         bRet   = true;
    const size_t length = strlen(name);
    Record       record;

    CBR(exists(id));
    record = m_blinds[id - 1];
    CBR(length > 0 && length < sizeof(record.name));
    memset(record.name, 0, sizeof(record.name));
    memcpy(record.name, name, length);
    CBR(saveBlind(id, record));

Error:
    return bRet;
}

bool SomfyTransmitter::RemoveBlind(uint32_t id)
{
    bool   bRet = true;
    Record record;

    CBR(exists(id));
    record               = m_blinds[id - 1];
    // Retain the tombstone so reboot cannot import the old bench pairing again.
    record.pairing       = Pairing::Removed;
    record.remoteAddress = 0;
    CBR(saveBlind(id, record));

Error:
    return bRet;
}

bool SomfyTransmitter::validBlindGroups(const BlindGroups &groups)
{
    bool bRet = true;

    for (const BlindGroup &group : groups)
    {
        CBR(group.name[sizeof(group.name) - 1] == '\0');
        for (uint32_t address : group.members)
        {
            CBR(address < kAddressMask);
            CBR(!address || group.name[0]);
        }
    }

Error:
    return bRet;
}

bool SomfyTransmitter::saveBlindGroups(const BlindGroups &groups)
{
    bool bRet = true;

    CBR(m_ready && !m_storageFailed);
    CER(nvs_set_blob(m_storage, kBlindGroupsKey, &groups, sizeof(groups)));
    CER(nvs_commit(m_storage));
    m_blindGroups = groups;

Error:
    return bRet;
}

uint32_t SomfyTransmitter::SaveBlindGroup(uint32_t id, const char *name, const BlindIds &members)
{
    bool         bRet = true;
    const size_t length = strlen(name);
    BlindGroups  groups = m_blindGroups;
    BlindGroup   group{};
    uint32_t     address;
    size_t       count = 0;

    CBR(length > 0 && length < sizeof(group.name));
    CBR(id <= groups.size());
    if (id)
    {
        CBR(groups[id - 1].name[0]);
    }
    else
    {
        for (uint32_t slot = 1; slot <= groups.size(); ++slot)
        {
            if (!groups[slot - 1].name[0])
            {
                id = slot;
                break;
            }
        }
        CBR(id);
    }
    for (uint32_t member : members)
    {
        if (member)
        {
            CBR(exists(member));
            address = m_blinds[member - 1].address;
            CBR(std::find(group.members.begin(), group.members.begin() + count, address) ==
                group.members.begin() + count);
            group.members[count++] = address;
        }
    }
    CBR(count != 0);
    memcpy(group.name, name, length);
    groups[id - 1] = group;
    CBR(saveBlindGroups(groups));

Error:
    return bRet ? id : 0;
}

bool SomfyTransmitter::DeleteBlindGroup(uint32_t id)
{
    bool        bRet = true;
    BlindGroups groups = m_blindGroups;

    CBR(id != 0 && id <= groups.size() && groups[id - 1].name[0]);
    groups[id - 1] = {};
    CBR(saveBlindGroups(groups));

Error:
    return bRet;
}

const SomfyTransmitter::BlindGroups &SomfyTransmitter::GetBlindGroups() const
{
    return m_blindGroups;
}

bool SomfyTransmitter::hasGroup(uint32_t address) const
{
    bool found = false;

    for (const Record &record : m_blinds)
    {
        if (address && record.pairing != Pairing::Removed && record.remoteAddress == address)
        {
            found = true;
            break;
        }
    }
    return found;
}

bool SomfyTransmitter::RenameGroup(uint32_t address, const char *name)
{
    bool         bRet   = true;
    const size_t length = strlen(name);
    GroupNames   names  = m_groupNames;
    GroupName   *entry  = nullptr;

    CBR(!m_storageFailed && hasGroup(address));
    CBR(length > 0 && length < sizeof(GroupName::name));
    for (GroupName &group : names)
    {
        if (group.address == address)
        {
            entry = &group;
            break;
        }
        if (!entry && !hasGroup(group.address))
        {
            entry = &group;
        }
    }
    CBR(entry != nullptr);
    *entry         = {};
    entry->address = address;
    memcpy(entry->name, name, length);
    CER(nvs_set_blob(m_storage, "group_names_v1", &names, sizeof(names)));
    CER(nvs_commit(m_storage));
    m_groupNames = names;

Error:
    return bRet;
}

bool SomfyTransmitter::validGroupChannels(const GroupChannelTable &table)
{
    bool bRet = true;

    for (size_t i = 0; i < table.size(); ++i)
    {
        const GroupChannels &group = table[i];
        CBR(group.primary < kAddressMask);
        for (size_t j = 0; group.primary && j < i; ++j)
        {
            CBR(table[j].primary != group.primary);
        }
        for (size_t j = 0; j < group.additional.size(); ++j)
        {
            const uint32_t address = group.additional[j];
            if (address)
            {
                CBR(group.primary && address < kAddressMask && address != group.primary);
                CBR(std::find(group.additional.begin(), group.additional.begin() + j, address) == group.additional.begin() + j);
            }
        }
    }

Error:
    return bRet;
}

bool SomfyTransmitter::saveGroupChannels(const GroupChannelTable &table)
{
    bool bRet = true;

    CBR(!m_storageFailed);
    CER(nvs_set_blob(m_storage, kGroupChannelsKey, &table, sizeof(table)));
    CER(nvs_commit(m_storage));
    m_groupChannels = table;

Error:
    return bRet;
}

const char *SomfyTransmitter::UpdateGroupChannel(uint32_t groupAddress, uint32_t address, bool linked)
{
    bool bRet = true;
    const char *outcome = "group_channel_save_failed";
    GroupChannelTable table = m_groupChannels;
    GroupChannels *entry = nullptr;
    GroupChannels *available = nullptr;
    IndividualAddresses::iterator slot;

    CBR(!m_storageFailed && hasGroup(groupAddress));
    CBR(address && address < kAddressMask && address != groupAddress);
    for (GroupChannels &group : table)
    {
        if (group.primary == groupAddress)
        {
            entry = &group;
            break;
        }
        if (!available && !hasGroup(group.primary))
        {
            available = &group;
        }
    }
    if (!entry && linked)
    {
        outcome = "group_channel_limit";
        CBR(available);
        entry = available;
        *entry = {groupAddress, {}};
    }
    if (entry)
    {
        slot = std::find(entry->additional.begin(), entry->additional.end(), address);
        if (linked && slot == entry->additional.end())
        {
            slot = std::find(entry->additional.begin(), entry->additional.end(), 0U); // Zero marks an empty slot.
            outcome = "group_channel_limit";
            CBR(slot != entry->additional.end());
            *slot = address;
        }
        else if (!linked && slot != entry->additional.end())
        {
            *slot = 0;
        }
        else
        {
            goto Error; // The requested link state is already saved.
        }
        outcome = "group_channel_save_failed";
        CBR(saveGroupChannels(table));
    }

Error:
    return bRet ? "group_channels_saved" : outcome;
}

bool SomfyTransmitter::DeleteGroup(uint32_t address)
{
    bool       bRet  = true;
    GroupNames names = m_groupNames;
    GroupChannelTable channels = m_groupChannels;

    CBR(!m_storageFailed && address != 0 && address < kAddressMask);
    // Clear the group settings before unlinking members.
    for (GroupChannels &group : channels)
    {
        if (group.primary == address)
        {
            group = {};
            CBR(saveGroupChannels(channels));
            break;
        }
    }
    for (GroupName &group : names)
    {
        if (group.address == address)
        {
            group = {};
            CER(nvs_set_blob(m_storage, "group_names_v1", &names, sizeof(names)));
            CER(nvs_commit(m_storage));
            m_groupNames = names;
            break;
        }
    }
    for (uint32_t id = 1; id <= m_blinds.size(); ++id)
    {
        if (exists(id) && m_blinds[id - 1].remoteAddress == address)
        {
            CBR(SetRemote(id, 0));
        }
    }

Error:
    return bRet;
}

bool SomfyTransmitter::SetRemote(uint32_t id, uint32_t address)
{
    bool   bRet = true;
    Record record;

    CBR(exists(id));
    CBR(address < kAddressMask);
    record               = m_blinds[id - 1];
    record.remoteAddress = address;
    CBR(saveBlind(id, record));

Error:
    return bRet;
}

bool SomfyTransmitter::SetIndividualRemote(uint32_t id, uint32_t address)
{
    bool bRet = true;
    IndividualAddresses addresses;

    CBR(exists(id));
    CBR(address < kAddressMask);
    CBR(!address || address != m_blinds[id - 1].remoteAddress);
    addresses = m_individualAddresses[id - 1];
    for (uint32_t &saved : addresses)
    {
        if (saved == address)
        {
            saved = 0;
        }
    }
    addresses.front() = address;
    CBR(saveBlind(id, m_blinds[id - 1], addresses));

Error:
    return bRet;
}

const char *SomfyTransmitter::UpdatePhysicalRemote(uint32_t id, uint32_t address, bool linked)
{
    bool                bRet = true;
    const char         *outcome = "physical_remote_save_failed";
    IndividualAddresses addresses;
    IndividualAddresses::iterator slot;

    CBR(exists(id));
    CBR(address != 0 && address < kAddressMask);
    CBR(!linked || address != m_blinds[id - 1].remoteAddress);
    addresses = m_individualAddresses[id - 1];
    slot = std::find(addresses.begin(), addresses.end(), address);
    if (linked && slot == addresses.end())
    {
        slot = std::find(addresses.begin(), addresses.end(), 0U); // Zero marks an empty slot.
        outcome = "physical_remote_limit";
        CBR(slot != addresses.end());
        *slot = address;
    }
    else if (!linked && slot != addresses.end())
    {
        *slot = 0;
    }
    if (addresses != m_individualAddresses[id - 1])
    {
        outcome = "physical_remote_save_failed";
        CBR(saveBlind(id, m_blinds[id - 1], addresses));
    }

Error:
    return bRet ? "physical_remotes_saved" : outcome;
}

bool SomfyTransmitter::SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs)
{
    bool   bRet = true;
    Record record;

    CBR(exists(id));
    CBR(openMs > 0 && closeMs > 0);
    record         = m_blinds[id - 1];
    record.openMs  = openMs;
    record.closeMs = closeMs;
    CBR(saveBlind(id, record));

Error:
    return bRet;
}

bool SomfyTransmitter::SetFavoritePosition(uint32_t id, int percent)
{
    bool   bRet = true;
    Record record;

    CBR(exists(id));
    CBR(percent >= -1 && percent <= 100); // Unset or whole percent open.
    record          = m_blinds[id - 1];
    record.favorite = static_cast<uint16_t>(percent + 1); // Store unset as zero.
    CBR(saveBlind(id, record));

Error:
    return bRet;
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
    return send(radio, id, command, 3); // A short press sends one initial frame and two repeats.
}

SomfyTransmitter::Result SomfyTransmitter::ProgramFavorite(TI_CC1101::CC1101Device &radio, uint32_t id)
{
    // Match the ESPSomfy-RTS long My press: one initial frame and 35 repeats.
    return send(radio, id, Command::My, 36);
}

SomfyTransmitter::Result SomfyTransmitter::send(TI_CC1101::CC1101Device &radio, uint32_t id, Command command,
                                                unsigned frames)
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
    CBR(exists(id));
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
    // Each repeat uses the same frame and rolling code as the initial frame.
    for (unsigned repeat = 0; repeat < frames; ++repeat)
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

    if (exists(id))
    {
        record = &m_blinds[id - 1];
        for (const GroupName &group : m_groupNames)
        {
            if (record->remoteAddress && group.address == record->remoteAddress)
            {
                memcpy(status.groupName, group.name, sizeof(status.groupName));
                break;
            }
        }
        if (record->remoteAddress)
        {
            for (const GroupChannels &group : m_groupChannels)
            {
                if (group.primary == record->remoteAddress)
                {
                    status.additionalGroupAddresses = group.additional;
                    break;
                }
            }
        }
        status.remoteAddress = record->remoteAddress;
        status.individualAddresses = m_individualAddresses[id - 1];
        status.openMs        = record->openMs;
        status.closeMs       = record->closeMs;
        status.favoritePosition = static_cast<int>(record->favorite) - 1;
        status.address       = record->address;
        status.nextCode      = record->nextCode;
        status.pairing       = record->pairing;
        status.ready         = m_ready && record->address != 0 && record->pairing == Pairing::Paired &&
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
