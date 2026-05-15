#pragma once
#include <cstdint>
#include <windows.h>

// Minimal Unofficial Extras definitions needed for squad ready check detection.
// Based on https://github.com/Krappa322/arcdps_unofficial_extras_releases

namespace UnofficialExtras
{

enum class UserRole : uint8_t
{
    SquadLeader = 0,
    Lieutenant  = 1,
    Member      = 2,
    Invited     = 3,
    Applied     = 4,
    None        = 5,  // user was removed from squad
    Invalid     = 6
};

enum class ChannelType : uint8_t
{
    Party    = 0,
    Squad    = 1,
    Reserved = 2,
    Invalid  = 3
};

struct UserInfo
{
    const char* AccountName; // null-terminated, includes leading ':', valid only during callback
    __time64_t  JoinTime;    // unix timestamp; 0 if unavailable
    UserRole    Role;
    uint8_t     Subgroup;    // 0 = first subgroup / no subgroup
    bool        ReadyStatus;
    ChannelType GroupType;
    uint32_t    _Unused2 = 0;
};

struct ExtrasAddonInfo
{
    uint32_t    ApiVersion;      // must be 2
    uint32_t    MaxInfoVersion;
    const char* StringVersion;
    const char* SelfAccountName;
};

typedef void (*SquadUpdateCallbackSignature)(const UserInfo* pUpdatedUsers, uint64_t pUpdatedUsersCount);

struct ExtrasSubscriberInfoV1
{
    uint32_t                      InfoVersion;             // set to 1
    const char*                   SubscriberName;
    SquadUpdateCallbackSignature  SquadUpdateCallback;
    void*                         LanguageChangedCallback; // set to nullptr if unused
    void*                         KeyBindChangedCallback;  // set to nullptr if unused
};

} // namespace UnofficialExtras
