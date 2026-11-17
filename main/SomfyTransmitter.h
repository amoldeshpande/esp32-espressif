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

// Owns virtual remote identities and rolling codes. Call from the radio task.
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

    enum class Pairing : uint8_t
    {
        Unpaired,
        AwaitingConfirmation,
        Paired,
        Removed
    };

    // Up to four separately linked physical channels per blind, plus the group channel.
    using IndividualAddresses = std::array<uint32_t, 4>;

    struct Status
    {
        uint32_t address;
        uint32_t nextCode;
        bool     ready;
        Pairing  pairing;
        char     name[33]; // Up to 32 bytes of UTF-8 plus the terminator.
        uint32_t remoteAddress;
        IndividualAddresses individualAddresses;
        IndividualAddresses additionalGroupAddresses;
        uint32_t openMs;
        uint32_t closeMs;
        char     groupName[sizeof(name)];
        int      favoritePosition;
    };

    // Bound the initial registry to eight blinds, with IDs starting at one.
    using Blinds = std::array<Status, 8>;

    using BlindIds = std::array<uint32_t, Blinds{}.size()>;

    struct BlindGroup
    {
        // Virtual addresses keep a replacement blind from inheriting an old member's slot.
        BlindIds members;
        char     name[sizeof(Status::name)];
    };
    // Blind group IDs are one-based slots, like blind IDs.
    using BlindGroups = std::array<BlindGroup, Blinds{}.size()>;

    bool     Init(gpio_num_t txPin);
    uint32_t AddBlind(const char *name);
    bool     RenameBlind(uint32_t id, const char *name);
    bool     RemoveBlind(uint32_t id);
    bool     RenameGroup(uint32_t address, const char *name);
    bool     DeleteGroup(uint32_t address);
    uint32_t SaveBlindGroup(uint32_t id, const char *name, const BlindIds &members);
    bool     DeleteBlindGroup(uint32_t id);
    const BlindGroups &GetBlindGroups() const;
    const char *UpdateGroupChannel(uint32_t groupAddress, uint32_t address, bool linked);
    bool     SetRemote(uint32_t id, uint32_t address);
    bool     SetIndividualRemote(uint32_t id, uint32_t address);
    const char *UpdatePhysicalRemote(uint32_t id, uint32_t address, bool linked);
    bool     SetTravelTimes(uint32_t id, uint32_t openMs, uint32_t closeMs);
    bool     SetFavoritePosition(uint32_t id, int percent);
    bool     ConfirmPairing(uint32_t id);
    Result   Send(TI_CC1101::CC1101Device &radio, uint32_t id, Command command);
    Result   ProgramFavorite(TI_CC1101::CC1101Device &radio, uint32_t id);
    Status   GetStatus(uint32_t id) const;
    Blinds   GetBlinds() const;

  private:
    struct Record
    {
        uint32_t address;
        uint32_t nextCode; // Wider than the wire counter so exhaustion never wraps to zero.
        Pairing  pairing;
        char     name[sizeof(Status::name)];
        uint16_t favorite; // Percent open plus one. Zero means unset.
        uint32_t remoteAddress;
        uint32_t openMs;
        uint32_t closeMs;
    };

    struct StoredRecord
    {
        Record   record;
        uint32_t format; // Identifies the fields available in the stored record.
        IndividualAddresses individualAddresses;
    };

    struct GroupName
    {
        uint32_t address;
        char     name[sizeof(Status::name)];
    };
    using GroupNames = std::array<GroupName, Blinds{}.size()>;

    struct GroupChannels
    {
        uint32_t primary;
        IndividualAddresses additional;
    };
    using GroupChannelTable = std::array<GroupChannels, Blinds{}.size()>;

  public:
    // Versioned ESP32 backup. Keep the layout stable across builds.
    class Backup
    {
        friend class SomfyTransmitter;
        uint32_t                            format{};
        std::array<Record, Blinds{}.size()> blinds{};
        GroupNames                          groups{};
        uint32_t                            legacyChecksum{};
        std::array<uint32_t, Blinds{}.size()> individualAddresses{};
        uint32_t                            individualChecksum{};
        // The first channel stays in the ESB3 section; the extra slots follow it.
        std::array<std::array<uint32_t, IndividualAddresses{}.size() - 1>, Blinds{}.size()> additionalAddresses{};
        uint32_t                            physicalChecksum{};
        GroupChannelTable                   groupChannels{};
        uint32_t                            groupChannelChecksum{};
        BlindGroups                         blindGroups{};
        uint32_t                            checksum{};

      public:
        static size_t LegacySize();
        static size_t IndividualSize();
        static size_t PhysicalSize();
        static size_t GroupChannelSize();
        bool MatchesSize(size_t size) const;
    };

    Backup      ExportBackup() const;
    const char *RestoreBackup(Backup backup);

  private:
    static uint32_t                     backupChecksum(const Backup &backup);
    static bool                         validateBackup(Backup &backup);
    static IndividualAddresses          backupAddresses(const Backup &backup, size_t index);
    bool                                applyBackup(const Backup &backup);
    bool                                recoverBackup();
    bool                                hasGroup(uint32_t address) const;
    static bool                         validGroupChannels(const GroupChannelTable &table);
    static bool                         validBlindGroups(const BlindGroups &groups);
    bool                                saveBlindGroups(const BlindGroups &groups);
    bool                                saveGroupChannels(const GroupChannelTable &table);

    bool                                exists(uint32_t id) const;
    bool                                loadBlinds();
    bool                                migrateFirstBlind();
    uint32_t                            createBlind(const char *name);
    bool                                saveBlind(uint32_t id, const Record &record);
    bool                                saveBlind(uint32_t id, const Record &record, const IndividualAddresses &individualAddresses);
    bool                                hasAddress(uint32_t address) const;
    Result                              send(TI_CC1101::CC1101Device &radio, uint32_t id, Command command, unsigned frames);
    void                                buildFrame(Command command, uint32_t address, uint16_t rollingCode);
    void                                buildSymbols(bool firstFrame);

    nvs_handle_t                        m_storage = 0;
    rmt_channel_handle_t                m_channel = nullptr;
    rmt_encoder_handle_t                m_encoder = nullptr;
    std::array<Record, Blinds{}.size()> m_blinds{};
    GroupNames                          m_groupNames{};
    GroupChannelTable                   m_groupChannels{};
    BlindGroups                         m_blindGroups{};
    std::array<IndividualAddresses, Blinds{}.size()> m_individualAddresses{};
    bool                                m_ready         = false;
    bool                                m_storageFailed = false;
    // Standard RTS payload: key, command/checksum, two-byte rolling code, three-byte address.
    std::array<uint8_t, 7>              m_frame{};
    // Wake-up, seven hardware syncs, software sync, 56 Manchester bits, and silence.
    std::array<rmt_symbol_word_t, 66>   m_symbols{};
    size_t                              m_symbolCount = 0;
};
