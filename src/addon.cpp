#include "addon.h"
#include "Chat.h"
#include "arcdpsheader.h"
#include "mumbleheader.h"
#include "boon_ignore.h"
#include "buff_names.h"
#include "tts_messages.h"
#include <imgui.h>
#include <windows.h>
#include <sapi.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <winhttp.h>
#include "embedded_icons.h"

static AddonAPI_t* g_API = nullptr;
static bool g_ImguiSetupDone = false;

// ── Anchor Points ────────────────────────────────────────────────────────────

enum class AnchorPoint {
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    Center,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    COUNT
};

static const char* g_AnchorNames[] = {
    "Top Left", "Top Center", "Top Right",
    "Middle Left", "Center", "Middle Right",
    "Bottom Left", "Bottom Center", "Bottom Right"
};

struct ConfigPosition {
    AnchorPoint anchor = AnchorPoint::Center;
    float offsetXPct = 0.0f;
    float offsetYPct = 0.0f;
};

static ConfigPosition g_Positions[2] = {
    {AnchorPoint::Center, 0.0f, 0.0f},
    {AnchorPoint::BottomCenter, 0.0f, -2.0f}
};
static int g_ActivePosition = 0;
static bool g_Enabled = true;
static bool g_HoldMode = false;
static bool g_CrosshairEnabled = false;
static float g_CrosshairColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};

// ── Active Mechanic Icons ────────────────────────────────────────────────────

static bool g_IconDisplayEnabled = false;
static float g_IconSize = 32.0f;
static float g_IconSpacing = 4.0f;
static float g_IconOpacity = 1.0f;
static ConfigPosition g_IconPosition = {AnchorPoint::Center, 0.0f, -10.0f};
static std::unordered_map<unsigned int, Texture_t*> g_IconTextures;
static std::unordered_map<unsigned int, std::string> g_IconUrlCache;
static std::unordered_map<unsigned int, DWORD> g_ActiveMechanics;
static const DWORD g_MechanicIconTimeout = 15000;

// ── TTS ──────────────────────────────────────────────────────────────────────

static ISpVoice* g_pVoice = nullptr;
static bool g_TtsReady = false;
static bool g_IsWine = false;
static bool g_WineTtsFallback = false;
static char g_WineTtsPipePath[512] = {0};

static const char* GetWineTtsPipePath()
{
    if (g_WineTtsPipePath[0]) return g_WineTtsPipePath;

    char home[256] = {0};
    DWORD len = GetEnvironmentVariableA("HOME", home, sizeof(home));
    if (len > 0 && len < sizeof(home))
        snprintf(g_WineTtsPipePath, sizeof(g_WineTtsPipePath), "Z:\\%s\\.gw2-tts-pipe", home);
    else
        snprintf(g_WineTtsPipePath, sizeof(g_WineTtsPipePath), "Z:\\home\\todd\\.gw2-tts-pipe");

    return g_WineTtsPipePath;
}

static bool DetectWine()
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
        return true;
    return false;
}

static void SpeakViaWinePipe(const char* utf8)
{
    if (!g_WineTtsFallback) return;

    const char* pipePath = GetWineTtsPipePath();
    HANDLE hPipe = CreateFileA(pipePath, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        char buf[384];
        snprintf(buf, sizeof(buf), "[TTS] Pipe open failed: path=%s error=0x%08lX", pipePath, (unsigned long)err);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
        g_WineTtsFallback = false;
        return;
    }

    std::string line(utf8);
    for (char& c : line) { if (c == '\r' || c == '\n') c = ' '; }
    line += "\n";

    DWORD written = 0;
    WriteFile(hPipe, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(hPipe);
}

static struct {
    bool enabled = false;
    bool party = true;
    bool squad = true;
    bool whisper = true;
    bool map = false;
    bool local = false;
    bool guild = false;
    bool squadBroadcast = true;
    bool error = false;
    bool guildMotD = false;
    bool teamPvP = false;
    bool teamWvW = false;
    bool emote = false;
    bool emoteCustom = false;
} g_Tts;

// ── Raid Mechanics / Combat TTS ──────────────────────────────────────────────

struct EvCombatData {
    ArcDPS::CombatEvent* ev;
    ArcDPS::AgentShort*  src;
    ArcDPS::AgentShort*  dst;
    char*                skillname;
    uint64_t             id;
    uint64_t             revision;
};

struct RaidMechanic {
    unsigned int skillID = 0;
    std::string name;
    bool ttsEnabled = false;
    bool iconEnabled = false;
};

struct BuiltinMechanic {
    unsigned int skillID;
    const char* name;
    const char* ttsMsg; // null means use name
    const char* iconUrl; // null means no icon
};

// Combined built-in mechanics list (raid encounter effects only)
static const BuiltinMechanic g_BuiltinMechanics[] = {
    // Raid encounter effects (effect IDs, not skill IDs)
    {34387, "Volatile Poison", nullptr, "https://wiki.guildwars2.com/images/e/e0/Volatile_Poison.png"},
    {38049, "Shared Agony", nullptr, "https://wiki.guildwars2.com/images/5/53/Shared_Agony.png"},
    {48042, "Soul Shackle", nullptr, "https://wiki.guildwars2.com/images/8/89/Soul_Shackle_%28effect%29.png"},
    {34416, "Corruption", "Corruption, get to fountain", "https://wiki.guildwars2.com/images/3/34/Locust_Trail.png"},
    {34450, "Unstable Blood Magic", "SAK, go to wall and drop", "https://wiki.guildwars2.com/images/3/3e/Unstable_Blood_Magic.png"},
    {47646, "Arcing Affliction", "Bomb, run away", "https://wiki.guildwars2.com/images/5/5a/Arcing_Affliction.png"},
    {34508, "Fixated", nullptr, "https://wiki.guildwars2.com/images/6/66/Fixated.png"},
    {47414, "Necrosis", nullptr, "https://wiki.guildwars2.com/images/4/47/Ichor.png"},
    {79526, "Biting Swarm", "Bees. Get away", "https://wiki.guildwars2.com/images/2/24/Targeted.png"},
};
static const size_t g_BuiltinMechanicCount = sizeof(g_BuiltinMechanics) / sizeof(g_BuiltinMechanics[0]);

// Per-player toggle state for built-in mechanics
static std::unordered_map<unsigned int, bool> g_BuiltinMechanicTts;
static std::unordered_map<unsigned int, bool> g_BuiltinMechanicIcon;

static const BuiltinMechanic* FindBuiltinMechanic(unsigned int skillID)
{
    for (size_t i = 0; i < g_BuiltinMechanicCount; i++)
    {
        if (g_BuiltinMechanics[i].skillID == skillID)
            return &g_BuiltinMechanics[i];
    }
    return nullptr;
}

// Fixated variants all share one toggle keyed on the canonical ID (34508)
static unsigned int NormalizeMechanicID(unsigned int skillID)
{
    switch (skillID)
    {
        case 47434: case 48533: case 39131: case 39928:
        case 38985: case 39558: case 58136: case 79380:
            return 34508;
        default:
            return skillID;
    }
}

static bool IsBuiltinMechanicTtsEnabled(unsigned int skillID)
{
    auto it = g_BuiltinMechanicTts.find(skillID);
    return it != g_BuiltinMechanicTts.end() ? it->second : false;
}

static bool IsBuiltinMechanicIconEnabled(unsigned int skillID)
{
    auto it = g_BuiltinMechanicIcon.find(skillID);
    return it != g_BuiltinMechanicIcon.end() ? it->second : false;
}

static std::vector<RaidMechanic> g_RaidMechanics;

static bool g_MechanicsAnnounceEnabled = false;
static bool g_ReadAllDebuffs = false;
static bool g_ShowMechanicsWindow = false;
static bool g_ShowTestIcon = false;
static DWORD g_LoadTime = 0;
static bool g_PrefetchStarted = false;
static size_t g_PrefetchIndex = 0;
static DWORD g_LastPrefetchTime = 0;
static std::unordered_map<unsigned int, std::string> g_DebuffNameCache;

// Track active buffs with last-seen timestamp so expired debuffs are cleared
// (conditions tick ~1s; if no tick for 3s, assume natural expiry)
static std::unordered_map<unsigned int, DWORD> g_ActiveBuffs;
static const DWORD g_ActiveBuffTimeout = 3000;

// Food/utility expiry tracking
static bool g_FoodUtilityExpiryEnabled = false;
static std::unordered_map<unsigned int, DWORD> g_TrackedFoodUtility;
static const DWORD g_FoodUtilityMinDuration = 60000; // must be active >1 min to be food/utility

// Squad state tracking
static bool g_InSquad = false;

// Ally downed tracking
static bool g_AllyDownedEnabled = false;
static bool g_AllyDownedSquadOnly = false;
static bool g_TofusToggle = false;
static std::unordered_set<uint64_t> g_DownedAgents;
static bool g_BossDeathAnnounced = false;
static bool g_InKeepConstructFight = false;

// Necrosis stack tracking
static int g_NecrosisStacks = 0;
static DWORD g_LastNecrosisTick = 0;

// MumbleLink — player identity
static HANDLE g_MumbleFileMap = nullptr;
static const Mumble::Data* g_MumbleData = nullptr;
static std::string g_PlayerName;

// TTS — messages play sequentially via SAPI's internal async queue
static void SpeakText(const wchar_t* text)
{
    if (!text || !*text) return;

    if (g_pVoice)
    {
        HRESULT hr = g_pVoice->Speak(text, SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
        if (SUCCEEDED(hr)) return;

        if (g_IsWine)
        {
            g_WineTtsFallback = true;
            char dbg[384];
            snprintf(dbg, sizeof(dbg), "[TTS] SAPI Speak failed, activating Wine pipe fallback (pipe=%s)", GetWineTtsPipePath());
            g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
            int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(len > 0 ? static_cast<size_t>(len) - 1 : 0, '\0');
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], len, nullptr, nullptr);
            SpeakViaWinePipe(utf8.c_str());
            return;
        }

        char buf[256];
        int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(len > 0 ? static_cast<size_t>(len) - 1 : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], len, nullptr, nullptr);
        snprintf(buf, sizeof(buf), "[TTS] Speak() failed: 0x%08lX text=\"%s\"", (unsigned long)hr, utf8.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
    else if (g_IsWine && g_WineTtsFallback)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(len > 0 ? static_cast<size_t>(len) - 1 : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], len, nullptr, nullptr);
        SpeakViaWinePipe(utf8.c_str());
    }
}

static void InitMumble()
{
    if (g_MumbleFileMap) return;
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"MumbleLink");
    if (!hMap) return;
    const Mumble::Data* data = (const Mumble::Data*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(Mumble::Data));
    if (!data) { CloseHandle(hMap); return; }
    g_MumbleFileMap = hMap;
    g_MumbleData = data;
    int len = WideCharToMultiByte(CP_UTF8, 0, data->Name, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0)
    {
        g_PlayerName.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, data->Name, -1, &g_PlayerName[0], len, nullptr, nullptr);
        if (!g_PlayerName.empty() && g_PlayerName.back() == '\0')
            g_PlayerName.pop_back();
    }
}

static bool IsWvwMap()
{
    if (!g_MumbleData) return false;
    auto mt = g_MumbleData->Context.MapType;
    return mt >= Mumble::EMapType::WvW_EternalBattlegrounds
        && mt <= Mumble::EMapType::WvW_Lounge;
}

static std::string FetchSkillName(unsigned int skillID, const char* eventSkillName = nullptr)
{
    // Use the name from the combat event if available (provided by ArcdpsIntegration)
    if (eventSkillName && eventSkillName[0])
    {
        g_DebuffNameCache[skillID] = eventSkillName;
        return eventSkillName;
    }

    // Check runtime cache first
    auto it = g_DebuffNameCache.find(skillID);
    if (it != g_DebuffNameCache.end())
        return it->second;

    // Check built-in mapping
    auto builtin = g_BuiltinBuffNames.find(skillID);
    if (builtin != g_BuiltinBuffNames.end())
    {
        g_DebuffNameCache[skillID] = builtin->second;
        return builtin->second;
    }

    // API fallback — only works for actual player skills, not buffs/conditions/raid mechanics
    auto tryFetch = [](const wchar_t* path) -> std::string
    {
        std::string result;
        HINTERNET hSession = WinHttpOpen(L"GW2Accessibility/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (hSession)
        {
            HINTERNET hConnect = WinHttpConnect(hSession, L"api.guildwars2.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (hConnect)
            {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
                if (hRequest)
                {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
                    {
                        if (WinHttpReceiveResponse(hRequest, nullptr))
                        {
                            DWORD statusCode = 0;
                            DWORD statusSize = sizeof(statusCode);
                            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusSize, nullptr);
                            if (statusCode == 200)
                            {
                                DWORD bytesRead = 0;
                                std::vector<char> buffer(4096);
                                while (WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) && bytesRead > 0)
                                    result.append(buffer.data(), bytesRead);
                            }
                        }
                    }
                    WinHttpCloseHandle(hRequest);
                }
                WinHttpCloseHandle(hConnect);
            }
            WinHttpCloseHandle(hSession);
        }
        return result;
    };

    std::string name;
    std::string result = tryFetch((L"/v2/skills?ids=" + std::to_wstring(skillID)).c_str());
    if (result.empty())
        result = tryFetch((L"/v2/skills/" + std::to_wstring(skillID)).c_str());

    if (!result.empty())
    {
        try
        {
            auto json = nlohmann::json::parse(result);
            if (json.is_array() && !json.empty())
                name = json[0].value("name", "");
            else if (json.is_object())
                name = json.value("name", "");
        }
        catch (...)
        {
        }
    }

    if (name.empty())
        name = "Skill " + std::to_string(skillID);

    g_DebuffNameCache[skillID] = name;
    return name;
}

struct ApiResponse {
    bool ok;
    std::string body;
};

static ApiResponse FetchFromGW2Api(HINTERNET hConnect, const wchar_t* path)
{
    ApiResponse result = {false, ""};
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!hRequest) return result;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusSize, nullptr);

        if (statusCode == 200)
        {
            DWORD bytesRead = 0;
            std::vector<char> buffer(4096);
            while (WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) && bytesRead > 0)
                result.body.append(buffer.data(), bytesRead);
            result.ok = true;
        }
    }
    WinHttpCloseHandle(hRequest);
    return result;
}

static std::string ParseIconUrl(const std::string& jsonBody)
{
    try
    {
        auto j = nlohmann::json::parse(jsonBody);
        return j.value("icon", "");
    }
    catch (...) {}
    return "";
}

static std::string FetchSkillIconUrl(unsigned int skillID)
{
    auto cached = g_IconUrlCache.find(skillID);
    if (cached != g_IconUrlCache.end())
        return cached->second;

    HINTERNET hSession = WinHttpOpen(L"GW2Accessibility/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return "";

    std::string iconUrl;
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.guildwars2.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        std::wstring idStr = std::to_wstring(skillID);

        auto resp = FetchFromGW2Api(hConnect, (L"/v2/skills/" + idStr).c_str());
        if (resp.ok)
            iconUrl = ParseIconUrl(resp.body);

        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);

    g_IconUrlCache[skillID] = iconUrl;

    return iconUrl;
}

static std::string GetIconsDir()
{
    const char* dir = g_API->Paths_GetAddonDirectory("GW2Accessibility");
    std::string path = dir ? dir : "";
    if (path.empty())
        path = ".\\";
    if (path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += "icons\\";
    return path;
}

static bool DownloadUrlToFile(const std::string& url, const std::string& filepath)
{
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) return false;

    std::string host = url.substr(hostStart, pathStart - hostStart);
    std::string endpoint = url.substr(pathStart);

    HINTERNET hSession = WinHttpOpen(L"GW2Accessibility/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return false;

    bool success = false;
    std::wstring whost(host.begin(), host.end());
    std::wstring wendpoint(endpoint.begin(), endpoint.end());
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wendpoint.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
        if (hRequest)
        {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusSize, nullptr);
                if (statusCode == 200)
                {
                    std::vector<char> data;
                    DWORD bytesRead = 0;
                    std::vector<char> buffer(8192);
                    while (WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) && bytesRead > 0)
                        data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);

                    if (!data.empty())
                    {
                        std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());
                        std::ofstream file(filepath, std::ios::binary);
                        if (file)
                        {
                            file.write(data.data(), data.size());
                            success = true;
                        }
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return success;
}

static void OnTextureLoaded(const char* aIdentifier, Texture_t* aTexture)
{
    if (!aIdentifier) return;

    unsigned int skillID = 0;
    if (sscanf(aIdentifier, "gw2accessibility_icon_%u", &skillID) == 1 && skillID != 0)
    {
        char buf[256];
        if (aTexture && aTexture->Resource)
        {
            g_IconTextures[skillID] = aTexture;
            snprintf(buf, sizeof(buf), "[ICON] OnTextureLoaded: %s skill=%u W=%u H=%u (cached)", aIdentifier, skillID, aTexture->Width, aTexture->Height);
        }
        else
        {
            snprintf(buf, sizeof(buf), "[ICON] OnTextureLoaded: %s skill=%u FAILED — aTexture=%p Resource=%p", aIdentifier, skillID, (void*)aTexture, aTexture ? aTexture->Resource : nullptr);
        }
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
}

static ImTextureID LoadIconTexture(unsigned int skillID)
{
    auto cached = g_IconTextures.find(skillID);
    if (cached != g_IconTextures.end())
    {
        if (cached->second && cached->second->Resource)
            return static_cast<ImTextureID>(cached->second->Resource);
        return nullptr;
    }

    std::string iconsDir = GetIconsDir();
    std::string filepath = iconsDir + std::to_string(skillID) + ".png";

    char identifier[64];
    snprintf(identifier, sizeof(identifier), "gw2accessibility_icon_%u", skillID);

    Texture_t* tex = g_API->Textures_Get(identifier);
    if (tex && tex->Resource)
    {
        g_IconTextures[skillID] = tex;
        return static_cast<ImTextureID>(tex->Resource);
    }

    static std::unordered_set<unsigned int> s_loadRequested;
    if (s_loadRequested.find(skillID) != s_loadRequested.end())
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[ICON] LoadIconTexture: skill=%u already requested — waiting for callback", skillID);
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
        return nullptr;
    }

    // If file doesn't exist on disk, try deploying from embedded icons first,
    // then fall back to wiki download.
    if (!std::filesystem::exists(filepath))
    {
        bool deployed = false;
        {
            auto embedIt = g_EmbeddedIcons.find(skillID);
            if (embedIt != g_EmbeddedIcons.end())
            {
                std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());
                std::ofstream file(filepath, std::ios::binary);
                if (file)
                {
                    file.write(reinterpret_cast<const char*>(embedIt->second.data), embedIt->second.size);
                    deployed = file.good();
                    char buf[256];
                    snprintf(buf, sizeof(buf), "[ICON] Deployed embedded icon skill=%u (%u bytes) %s", skillID, static_cast<unsigned>(embedIt->second.size), deployed ? "OK" : "write failed");
                    g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
                }
            }
        }

        if (!deployed)
        {
            std::string url = FetchSkillIconUrl(skillID);
            if (url.empty())
            {
                const BuiltinMechanic* bm = FindBuiltinMechanic(skillID);
                if (bm && bm->iconUrl)
                    url = bm->iconUrl;
                else if (bm)
                {
                    for (size_t i = 0; i < g_BuiltinMechanicCount; i++)
                    {
                        if (g_BuiltinMechanics[i].iconUrl && strcmp(g_BuiltinMechanics[i].name, bm->name) == 0)
                        { url = g_BuiltinMechanics[i].iconUrl; break; }
                    }
                }
            }
            if (url.empty())
                return nullptr;
            DownloadUrlToFile(url, filepath);
        }

        if (!std::filesystem::exists(filepath))
            return nullptr;
    }

    s_loadRequested.insert(skillID);
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[ICON] LoadIconTexture: skill=%u loading async from file \"%s\"", skillID, filepath.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
    g_API->Textures_LoadFromFile(identifier, filepath.c_str(), OnTextureLoaded);
    return nullptr;
}

static void LoadAllConfiguredIcons()
{
    for (const auto& m : g_RaidMechanics)
    {
        if (m.iconEnabled && m.skillID != 0)
            LoadIconTexture(m.skillID);
    }
    // Preload built-in mechanic icons
    for (size_t i = 0; i < g_BuiltinMechanicCount; i++)
    {
        if (IsBuiltinMechanicIconEnabled(g_BuiltinMechanics[i].skillID))
            LoadIconTexture(g_BuiltinMechanics[i].skillID);
    }
}

static void PruneExpiredMechanics(DWORD now)
{
    for (auto it = g_ActiveMechanics.begin(); it != g_ActiveMechanics.end(); )
    {
        if (now - it->second > g_MechanicIconTimeout)
            it = g_ActiveMechanics.erase(it);
        else
            ++it;
    }
}

static const char* EV_ARCDPS_COMBAT_SQUAD = "EV_ARCDPS_COMBATEVENT_SQUAD_RAW";
static const char* EV_ARCDPS_COMBAT_LOCAL = "EV_ARCDPS_COMBATEVENT_LOCAL_RAW";

// ── Config Path ──────────────────────────────────────────────────────────────

static std::string GetConfigPath()
{
    const char* dir = g_API->Paths_GetAddonDirectory("GW2Accessibility");
    std::string path = dir ? dir : "";
    if (path.empty())
        path = ".\\";
    if (path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += "config.json";
    return path;
}

// ── Save / Load ──────────────────────────────────────────────────────────────

static void SaveSettings()
{
    nlohmann::json j;
    j["enabled"] = g_Enabled;

    for (int i = 0; i < 2; i++)
    {
        nlohmann::json pos;
        pos["anchor"]     = static_cast<int>(g_Positions[i].anchor);
        pos["offsetXPct"] = g_Positions[i].offsetXPct;
        pos["offsetYPct"] = g_Positions[i].offsetYPct;
        j["positions"].push_back(pos);
    }

    j["holdMode"] = g_HoldMode;
    j["crosshairEnabled"] = g_CrosshairEnabled;
    j["crosshairColor"] = { g_CrosshairColor[0], g_CrosshairColor[1], g_CrosshairColor[2], g_CrosshairColor[3] };
    j["iconDisplayEnabled"] = g_IconDisplayEnabled;
    j["iconSize"] = g_IconSize;
    j["iconSpacing"] = g_IconSpacing;
    j["iconOpacity"] = g_IconOpacity;
    j["iconAnchor"] = static_cast<int>(g_IconPosition.anchor);
    j["iconOffsetX"] = g_IconPosition.offsetXPct;
    j["iconOffsetY"] = g_IconPosition.offsetYPct;
    j["foodUtilityExpiry"] = g_FoodUtilityExpiryEnabled;
    j["allyDownedEnabled"] = g_AllyDownedEnabled;
    j["allyDownedSquadOnly"] = g_AllyDownedSquadOnly;
    j["tofusToggle"] = g_TofusToggle;

    nlohmann::json tts;
    tts["enabled"]       = g_Tts.enabled;
    tts["party"]         = g_Tts.party;
    tts["squad"]         = g_Tts.squad;
    tts["whisper"]       = g_Tts.whisper;
    tts["map"]           = g_Tts.map;
    tts["local"]         = g_Tts.local;
    tts["guild"]         = g_Tts.guild;
    tts["squadBroadcast"] = g_Tts.squadBroadcast;
    tts["error"] = g_Tts.error;
    tts["guildMotD"] = g_Tts.guildMotD;
    tts["teamPvP"] = g_Tts.teamPvP;
    tts["teamWvW"] = g_Tts.teamWvW;
    tts["emote"] = g_Tts.emote;
    tts["emoteCustom"] = g_Tts.emoteCustom;
    j["tts"] = tts;

    j["mechanicsAnnounce"] = g_MechanicsAnnounceEnabled;
    nlohmann::json mechanics = nlohmann::json::array();
    for (const auto& m : g_RaidMechanics)
    {
        nlohmann::json entry;
        entry["skillID"]    = m.skillID;
        entry["name"]       = m.name;
        entry["ttsEnabled"] = m.ttsEnabled;
        entry["iconEnabled"] = m.iconEnabled;
        mechanics.push_back(entry);
    }
    j["raidMechanics"] = mechanics;
    j["readAllDebuffs"] = g_ReadAllDebuffs;

    // Save built-in mechanic toggles
    nlohmann::json builtinTts = nlohmann::json::object();
    nlohmann::json builtinIcon = nlohmann::json::object();
    for (size_t i = 0; i < g_BuiltinMechanicCount; i++)
    {
        unsigned int id = g_BuiltinMechanics[i].skillID;
        auto ttsIt = g_BuiltinMechanicTts.find(id);
        auto iconIt = g_BuiltinMechanicIcon.find(id);
        if (ttsIt != g_BuiltinMechanicTts.end())
            builtinTts[std::to_string(id)] = ttsIt->second;
        if (iconIt != g_BuiltinMechanicIcon.end())
            builtinIcon[std::to_string(id)] = iconIt->second;
    }
    j["builtinMechanicTts"] = builtinTts;
    j["builtinMechanicIcon"] = builtinIcon;

    std::string path = GetConfigPath();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream file(path, std::ios::binary);
    if (file.is_open())
        file << j.dump(2);
}

static void LoadSettings()
{
    std::string path = GetConfigPath();
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;

    try
    {
        nlohmann::json j;
        file >> j;

        g_Enabled = j.value("enabled", true);
        g_HoldMode = j.value("holdMode", false);
        g_CrosshairEnabled = j.value("crosshairEnabled", false);
        auto& chColor = j["crosshairColor"];
        if (chColor.is_array() && chColor.size() >= 4)
            for (int i = 0; i < 4; i++)
                g_CrosshairColor[i] = chColor[i].get<float>();
        g_IconDisplayEnabled = j.value("iconDisplayEnabled", false);
        g_IconSize = j.value("iconSize", 32.0f);
        g_IconSpacing = j.value("iconSpacing", 4.0f);
        g_IconOpacity = j.value("iconOpacity", 1.0f);
        int iconAnchor = j.value("iconAnchor", static_cast<int>(AnchorPoint::Center));
        iconAnchor = std::clamp(iconAnchor, 0, static_cast<int>(AnchorPoint::COUNT) - 1);
        g_IconPosition.anchor = static_cast<AnchorPoint>(iconAnchor);
        g_IconPosition.offsetXPct = j.value("iconOffsetX", 0.0f);
        g_IconPosition.offsetYPct = j.value("iconOffsetY", -10.0f);
        g_FoodUtilityExpiryEnabled = j.value("foodUtilityExpiry", false);
        g_AllyDownedEnabled = j.value("allyDownedEnabled", false);
        g_AllyDownedSquadOnly = j.value("allyDownedSquadOnly", false);
        g_TofusToggle = j.value("tofusToggle", false);

        auto& positions = j["positions"];
        if (positions.is_array())
        {
            for (int i = 0; i < 2 && i < static_cast<int>(positions.size()); i++)
            {
                int anchor = positions[i].value("anchor", static_cast<int>(AnchorPoint::Center));
                anchor = std::clamp(anchor, 0, static_cast<int>(AnchorPoint::COUNT) - 1);
                g_Positions[i].anchor     = static_cast<AnchorPoint>(anchor);
                g_Positions[i].offsetXPct = positions[i].value("offsetXPct", 0.0f);
                g_Positions[i].offsetYPct = positions[i].value("offsetYPct", 0.0f);
            }
        }

        auto& tts = j["tts"];
        if (tts.is_object())
        {
            g_Tts.enabled        = tts.value("enabled", false);
            g_Tts.party          = tts.value("party", true);
            g_Tts.squad          = tts.value("squad", true);
            g_Tts.whisper        = tts.value("whisper", true);
            g_Tts.map            = tts.value("map", false);
            g_Tts.local          = tts.value("local", false);
            g_Tts.guild          = tts.value("guild", false);
            g_Tts.squadBroadcast = tts.value("squadBroadcast", true);
            g_Tts.error      = tts.value("error", false);
            g_Tts.guildMotD  = tts.value("guildMotD", false);
            g_Tts.teamPvP    = tts.value("teamPvP", false);
            g_Tts.teamWvW    = tts.value("teamWvW", false);
            g_Tts.emote      = tts.value("emote", false);
            g_Tts.emoteCustom = tts.value("emoteCustom", false);
        }

        g_MechanicsAnnounceEnabled = j.value("mechanicsAnnounce", false);
        g_ReadAllDebuffs = j.value("readAllDebuffs", false);
        g_RaidMechanics.clear();
        auto& savedMechanics = j["raidMechanics"];
        if (savedMechanics.is_array())
        {
            for (const auto& entry : savedMechanics)
            {
                RaidMechanic m;
                m.skillID    = entry.value("skillID", 0u);
                m.name       = entry.value("name", "");
                m.ttsEnabled = entry.value("ttsEnabled", entry.value("enabled", false));
                m.iconEnabled = entry.value("iconEnabled", entry.value("enabled", false));
                if (m.skillID != 0 && !m.name.empty())
                    g_RaidMechanics.push_back(m);
            }
        }

        // Load built-in mechanic toggles
        auto& builtinTts = j["builtinMechanicTts"];
        auto& builtinIcon = j["builtinMechanicIcon"];
        if (builtinTts.is_object())
        {
            for (auto it = builtinTts.begin(); it != builtinTts.end(); ++it)
            {
                try { g_BuiltinMechanicTts[static_cast<unsigned int>(std::stoul(it.key()))] = it.value().get<bool>(); } catch (...) {}
            }
        }
        if (builtinIcon.is_object())
        {
            for (auto it = builtinIcon.begin(); it != builtinIcon.end(); ++it)
            {
                try { g_BuiltinMechanicIcon[static_cast<unsigned int>(std::stoul(it.key()))] = it.value().get<bool>(); } catch (...) {}
            }
        }

        // Migrate old skill IDs to new effect IDs
        static const std::pair<unsigned int, unsigned int> s_IDMigration[] = {
            {38210, 38049}, {47164, 48042}, {34473, 34416},
            {48121, 47646}, {79513, 79526},
        };
        for (const auto& pair : s_IDMigration)
        {
            unsigned int oldId = pair.first;
            unsigned int newId = pair.second;
            auto ttsIt = g_BuiltinMechanicTts.find(oldId);
            if (ttsIt != g_BuiltinMechanicTts.end())
            {
                g_BuiltinMechanicTts[newId] = ttsIt->second;
                g_BuiltinMechanicTts.erase(ttsIt);
            }
            auto iconIt = g_BuiltinMechanicIcon.find(oldId);
            if (iconIt != g_BuiltinMechanicIcon.end())
            {
                g_BuiltinMechanicIcon[newId] = iconIt->second;
                g_BuiltinMechanicIcon.erase(iconIt);
            }
        }
        // Merge Fixated variant toggles into canonical ID 34508
        {
            unsigned int fixatedIds[] = {47434, 48533, 39131, 39928, 38985, 39558, 58136, 79380};
            bool fixatedTts = false, fixatedIcon = false;
            for (unsigned int fid : fixatedIds)
            {
                auto ttsIt = g_BuiltinMechanicTts.find(fid);
                if (ttsIt != g_BuiltinMechanicTts.end()) { fixatedTts = fixatedTts || ttsIt->second; g_BuiltinMechanicTts.erase(ttsIt); }
                auto iconIt = g_BuiltinMechanicIcon.find(fid);
                if (iconIt != g_BuiltinMechanicIcon.end()) { fixatedIcon = fixatedIcon || iconIt->second; g_BuiltinMechanicIcon.erase(iconIt); }
            }
            if (fixatedTts) g_BuiltinMechanicTts[34508] = true;
            if (fixatedIcon) g_BuiltinMechanicIcon[34508] = true;
        }
    }
    catch (...)
    {
    }

    LoadAllConfiguredIcons();
}

// ── Mouse Positioning ────────────────────────────────────────────────────────

static void GetTargetPosition(const ConfigPosition& pos, int& outX, int& outY)
{
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int idx = static_cast<int>(pos.anchor);
    int col = idx % 3;
    int row = idx / 3;

    switch (col)
    {
        case 0: outX = 0; break;
        case 1: outX = screenW / 2; break;
        case 2: outX = screenW; break;
    }
    switch (row)
    {
        case 0: outY = 0; break;
        case 1: outY = screenH / 2; break;
        case 2: outY = screenH; break;
    }

    outX += static_cast<int>(screenW * pos.offsetXPct / 100.0f);
    outY += static_cast<int>(screenH * pos.offsetYPct / 100.0f);

    outX = std::clamp(outX, 0, screenW - 1);
    outY = std::clamp(outY, 0, screenH - 1);
}

static void MoveToPosition(const ConfigPosition& pos)
{
    int x, y;
    GetTargetPosition(pos, x, y);
    SetCursorPos(x, y);
}

static void ToggleMousePosition()
{
    g_ActivePosition = 1 - g_ActivePosition;
    MoveToPosition(g_Positions[g_ActivePosition]);
}

static void OnMouseToggleKeybind(const char* aIdentifier, bool aIsRelease)
{
    if (!g_Enabled) return;

    if (g_HoldMode)
    {
        if (aIsRelease)
        {
            g_ActivePosition = 0;
            MoveToPosition(g_Positions[0]);
        }
        else
        {
            g_ActivePosition = 1;
            MoveToPosition(g_Positions[1]);
        }
    }
    else
    {
        if (aIsRelease) return;
        ToggleMousePosition();
    }
}

// ── SAPI / TTS ───────────────────────────────────────────────────────────────

static std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring wide(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);
    return wide;
}

static void InitSAPI()
{
    if (g_TtsReady) return;

    g_IsWine = DetectWine();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    char buf[256];
    if (FAILED(hr))
    {
        snprintf(buf, sizeof(buf), "[TTS] CoInitializeEx(MT) failed: 0x%08lX, retrying STA", (unsigned long)hr);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    if (FAILED(hr))
    {
        snprintf(buf, sizeof(buf), "[TTS] CoInitializeEx(STA) failed: 0x%08lX", (unsigned long)hr);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }

    if (SUCCEEDED(hr))
    {
        hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                              IID_ISpVoice, (void**)&g_pVoice);
        g_TtsReady = SUCCEEDED(hr) && g_pVoice != nullptr;
    }

    if (g_TtsReady)
        g_API->Log(LOGL_INFO, "GW2Accessibility", "[TTS] SAPI voice initialized successfully");
    else if (g_IsWine)
    {
        g_API->Log(LOGL_INFO, "GW2Accessibility", "[TTS] SAPI unavailable (Wine detected), enabling espeak-ng pipe fallback");
        g_WineTtsFallback = true;
    }
    else
    {
        snprintf(buf, sizeof(buf), "[TTS] CoCreateInstance(CLSID_SpVoice) failed: 0x%08lX", (unsigned long)hr);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
}

static void OnCombatEvent(void* aEventArgs)
{
    auto* cbt = static_cast<const EvCombatData*>(aEventArgs);
    if (!cbt->ev) return;

    // ── Tofu's Toggle — boss death detection ────────────────────────────
    // Detects Keep Construct specifically by tracking dst name from buff events.
    // ENTERCOMBAT (1) resets flags, REWARD (10) fires if KC was tracked.
    if (g_TofusToggle)
    {
        uint32_t state = cbt->ev->IsStatechange;

        if (state == 1)
        {
            g_BossDeathAnnounced = false;
            g_InKeepConstructFight = false;
            unsigned int mapId = g_MumbleData ? g_MumbleData->Context.MapID : 0;
            char buf[128];
            snprintf(buf, sizeof(buf), "[BOSS] Entered combat, reset. MapID=%u", mapId);
            g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
            return;
        }

        if (state == 10 && !g_BossDeathAnnounced && g_InKeepConstructFight)
        {
            g_BossDeathAnnounced = true;
            InitSAPI();
            SpeakText(L"Tofu, KC is dead, check your gear.");
            g_API->Log(LOGL_INFO, "GW2Accessibility", "[BOSS] KC death via REWARD");
            return;
        }
    }

    // ── Squad tracking via team ID change ─────────────────────────────────
    if (cbt->ev->IsStatechange == 22) // CBTS_TEAMCHANGE
    {
        if (cbt->src && cbt->src->IsSelf)
        {
            uint16_t newTeam = static_cast<uint16_t>(cbt->ev->DestinationAgent);
            g_InSquad = (newTeam > 0);
        }
        return;
    }

    if (!cbt->dst) return;

    // ── Track Keep Construct from buff application events ─────────────────
    if (g_TofusToggle && !g_InKeepConstructFight)
    {
        if (cbt->ev->IsStatechange == 0 && cbt->ev->Buff == 1 && !cbt->ev->IsBuffRemove)
        {
            const char* name = cbt->dst->Name;
            if (name)
            {
                std::string dstName(name);
                if (dstName.find("Keep Construct") != std::string::npos)
                {
                    g_InKeepConstructFight = true;
                    g_API->Log(LOGL_INFO, "GW2Accessibility", "[BOSS] Tracking Keep Construct");
                }
            }
        }
    }

    if (!cbt->dst) return;

    // ── Ally downed tracking (state change event) ────────────────────────────
    if (cbt->ev->IsStatechange == 5) // CBTS_CHANGEDOWN
    {
        if (!g_AllyDownedEnabled) return;
        if (g_AllyDownedSquadOnly && !g_InSquad) return;
        uint64_t agentId = cbt->ev->SourceAgent;
        if (!g_DownedAgents.count(agentId))
        {
            g_DownedAgents.insert(agentId);
            const char* agentName = cbt->src && cbt->src->Name ? cbt->src->Name : "Ally";
            InitSAPI();
            if (g_TtsReady)
            {
                std::string speech = std::string(agentName) + " downed";
                SpeakText(Utf8ToWide(speech.c_str()).c_str());
            }
        }
        return;
    }

    // ── Ally revived (clear from downed set) ─────────────────────────────────
    if (cbt->ev->IsStatechange == 3) // CBTS_CHANGEUP
    {
        uint64_t agentId = cbt->ev->SourceAgent;
        g_DownedAgents.erase(agentId);
        return;
    }

    if (!cbt->ev || !cbt->dst)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] event received but ev or dst is null — ignored");
        return;
    }

    if (cbt->ev->Buff != 1)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] Buff != 1 (not a buff event) — ignored");
        return;
    }
    if (cbt->ev->IsStatechange != 0)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] IsStatechange != 0 (state change event) — ignored");
        return;
    }
    if (cbt->dst->IsSelf != 1)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] dst->IsSelf != 1 (not on player) — ignored");
        return;
    }

    // Skip debuff reading in WvW (too much noise from enemy conditions)
    if (IsWvwMap())
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] on WvW map — ignored");
        return;
    }

    unsigned int skillID = NormalizeMechanicID(cbt->ev->SkillID);
    bool isRemove = (cbt->ev->IsBuffRemove != 0);
    DWORD now = GetTickCount();

    // ── Track active mechanics for icon display ──────────────────────────────
    // Runs independently of TTS/Announce settings
    if (isRemove)
    {
        // Only clear on full remove (IsBuffRemove == 1).
        // Single-stack remove (IsBuffRemove == 2) means a stack was consumed
        // but the effect is still active (e.g. Biting Swarm).
        if (cbt->ev->IsBuffRemove == 1)
            g_ActiveMechanics.erase(skillID);
    }
    else
    {
        bool iconTracked = false;
        if (IsBuiltinMechanicIconEnabled(skillID))
        {
            g_ActiveMechanics[skillID] = now;
            LoadIconTexture(skillID);
            iconTracked = true;
        }
        if (!iconTracked)
        {
            for (const auto& m : g_RaidMechanics)
            {
                if (m.iconEnabled && m.skillID == skillID)
                {
                    g_ActiveMechanics[skillID] = now;
                    LoadIconTexture(skillID);
                    break;
                }
            }
        }
    }

    // ── Gate TTS speech behind settings ──────────────────────────────────────
    if (!g_MechanicsAnnounceEnabled && !g_ReadAllDebuffs)
    {
        return;
    }

    // Only log unknown skills (not in any known list) for curation
    {
        bool isKnown = IsBoon(skillID) || IsCondition(skillID) || (skillID == 47414);
        if (!isKnown) isKnown = (g_BuiltinBuffNames.find(skillID) != g_BuiltinBuffNames.end());
        if (!isKnown) isKnown = (g_BuiltinTtsMessages.find(skillID) != g_BuiltinTtsMessages.end());
        if (!isKnown) isKnown = (FindBuiltinMechanic(skillID) != nullptr);
        if (!isKnown)
        {
            for (const auto& m : g_RaidMechanics)
            {
                if ((m.ttsEnabled || m.iconEnabled) && m.skillID == skillID) { isKnown = true; break; }
            }
        }
        if (!isKnown)
        {
            const char* name = (cbt->skillname && cbt->skillname[0]) ? cbt->skillname : "(no name from arcdps)";
            char buf[512];
            snprintf(buf, sizeof(buf), "[UNKNOWN] skill=%u name=%s action=%s", skillID, name, isRemove ? "remove" : "add");
            g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
        }
    }

    // ── Food/Utility expiry tracking (runs for all buff events) ──────────────
    if (g_FoodUtilityExpiryEnabled)
    {
        if (isRemove)
        {
            auto fuIt = g_TrackedFoodUtility.find(skillID);
            if (fuIt != g_TrackedFoodUtility.end() && now - fuIt->second >= g_FoodUtilityMinDuration)
            {
                std::string buffName = FetchSkillName(skillID, cbt->skillname);
                bool isFood = false;
                if (buffName.size() >= 6)
                {
                    std::string end6 = buffName.substr(buffName.size() - 6);
                    if (end6 == "Steak" || end6 == "steak" || end6 == "adbread")
                        isFood = true;
                }
                if (buffName.find("Salad") != std::string::npos ||
                    buffName.find("salad") != std::string::npos ||
                    buffName.find("Soup") != std::string::npos ||
                    buffName.find("soup") != std::string::npos ||
                    buffName.find("Pancake") != std::string::npos ||
                    buffName.find("pancake") != std::string::npos)
                    isFood = true;

                const char* speech = isFood ? "food expired" : "utility expired";
                InitSAPI();
                if (g_TtsReady)
                {
                    SpeakText(Utf8ToWide(speech).c_str());
                }
            }
            g_TrackedFoodUtility.erase(skillID);
        }
        else
        {
            bool isKnown = IsBoon(skillID) || IsCondition(skillID) || (FindBuiltinMechanic(skillID) != nullptr);
            if (!isKnown)
            {
                for (const auto& m : g_RaidMechanics)
                {
                    if ((m.ttsEnabled || m.iconEnabled) && m.skillID == skillID) { isKnown = true; break; }
                }
            }
            if (!isKnown)
            {
                g_TrackedFoodUtility[skillID] = now;
            }
        }
    }

    // ── Necrosis special handling ──────────────────────────────────────────
    if (skillID == 47414)
    {
        if (isRemove)
        {
            if (cbt->ev->IsBuffRemove >= 2)
            {
                g_NecrosisStacks = 0;
                g_ActiveBuffs.erase(47414);
                InitSAPI();
                if (g_TtsReady)
                {
                    SpeakText(L"Necrosis removed");
                }
            }
            else
            {
                g_NecrosisStacks = std::max(0, g_NecrosisStacks - 1);
            }
            return;
        }

        g_NecrosisStacks++;
        g_ActiveBuffs[47414] = now;

        if (now - g_LastNecrosisTick >= 5000)
        {
            g_LastNecrosisTick = now;
            std::string msg;
            if (g_NecrosisStacks == 1)
                msg = "Necrosis";
            else if (g_NecrosisStacks >= 4)
                msg = "Necrosis at 4 stacks";

            if (!msg.empty())
            {
                InitSAPI();
                if (g_TtsReady)
                {
                    auto w = Utf8ToWide(msg.c_str());
                    SpeakText(w.c_str());
                }
            }
        }
        return;
    }

    // ── Buff removals — clear from active set so re-application is spoken again ──
    if (isRemove)
    {
        g_ActiveBuffs.erase(skillID);
        return;
    }

    // ── Buff additions — only speak if not already active on player ──
    auto activeIt = g_ActiveBuffs.find(skillID);
    if (activeIt != g_ActiveBuffs.end())
    {
        if (now - activeIt->second > g_ActiveBuffTimeout)
        {
            g_ActiveBuffs.erase(activeIt);
        }
        else
        {
            g_ActiveBuffs[skillID] = now;
            return;
        }
    }
    g_ActiveBuffs[skillID] = now;

    std::string text;

    // Check for built-in TTS message (action-oriented)
    auto ttsMsg = g_BuiltinTtsMessages.find(skillID);
    if (ttsMsg != g_BuiltinTtsMessages.end())
    {
        text = ttsMsg->second;
    }
    else
    {
        // Check built-in mechanics with TTS enabled
        if (text.empty())
        {
            const BuiltinMechanic* bm = FindBuiltinMechanic(skillID);
            if (bm && IsBuiltinMechanicTtsEnabled(skillID))
            {
                text = bm->ttsMsg ? bm->ttsMsg : bm->name;
            }
        }

        // Check user mechanics with TTS enabled
        if (text.empty() && g_MechanicsAnnounceEnabled)
        {
            for (const auto& m : g_RaidMechanics)
            {
                if (m.ttsEnabled && m.skillID == skillID)
                {
                    text = m.name;
                    break;
                }
            }
        }

        // In "Read All Debuffs" mode, only speak known conditions — skip everything else
        if (text.empty() && g_ReadAllDebuffs)
        {
            if (!IsCondition(skillID))
            {
                return;
            }
            text = FetchSkillName(skillID, cbt->skillname);
        }
    }

    if (text.empty())
    {
        return;
    }

    InitSAPI();
    if (!g_TtsReady)
    {
        return;
    }
    auto w = Utf8ToWide(text.c_str());
    SpeakText(w.c_str());
}

static std::string FirstName(const char* name)
{
    if (!name || !*name) return "";
    std::string s(name);
    size_t space = s.find(' ');
    if (space != std::string::npos)
        s.resize(space);
    return s;
}

static const char* ChannelName(MessageType t)
{
    switch (t)
    {
        case Party: return "Party";
        case Squad: return "Squad";
        case Map: return "Map";
        case Local: return "Local";
        case Whisper: return "Whisper";
        case SquadBroadcast: return "SquadBroadcast";
        case Guild: return "Guild";
        case SquadMessage: return "SquadMessage";
        case Error: return "Error";
        case GuildMotD: return "GuildMotD";
        case TeamPvP: return "TeamPvP";
        case TeamWvW: return "TeamWvW";
        case Emote: return "Emote";
        case EmoteCustom: return "EmoteCustom";
        default: return "Unknown";
    }
}

static std::string StripChatLinks(const char* text)
{
    if (!text) return "";
    std::string result;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len)
    {
        if (i + 1 < len && text[i] == '[' && text[i + 1] == '&')
        {
            const char* end = strchr(text + i + 2, ']');
            if (end)
            {
                i = static_cast<size_t>(end - text) + 1;
                continue;
            }
        }
        result += text[i];
        i++;
    }
    return result;
}

static void OnChatMessage(void* aEventArgs)
{
    if (!g_Tts.enabled && !g_TofusToggle)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[CHAT] event received but TTS is disabled — ignored");
        return;
    }
    auto* msg = static_cast<const Message*>(aEventArgs);

    // ── Tofu's Toggle — detect boss death via "defeated" chat message ───────
    if (g_TofusToggle && g_InKeepConstructFight)
    {
        const char* chatText = nullptr;
        switch (msg->Type)
        {
            case Party: case Squad: case Map: case Local:
                chatText = msg->Local.Content; break;
            case Whisper: chatText = msg->Whisper.Content; break;
            case Guild: chatText = msg->Guild.Base.Content; break;
            case SquadBroadcast: case SquadMessage: chatText = msg->SquadMessage; break;
            default: break;
        }
        if (chatText)
        {
            std::string t(chatText);
            if (t.find("defeated") != std::string::npos || t.find("Defeated") != std::string::npos)
            {
                if (!g_BossDeathAnnounced)
                {
                    g_BossDeathAnnounced = true;
                    InitSAPI();
                    SpeakText(L"Tofu, KC is dead, check your gear.");
                    g_API->Log(LOGL_INFO, "GW2Accessibility", "[BOSS] Boss death via chat 'defeated' message");
                }
                return;
            }
        }
    }

    if (!g_Tts.enabled) return;

    InitSAPI();
    if (!g_TtsReady)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] event received but SAPI not ready — ignored", ChannelName(msg->Type));
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
        return;
    }

    std::string ttsText;
    const char* text = nullptr;
    const char* name = nullptr;
    bool wasIgnored = false;
    const char* ignoreReason = nullptr;

    switch (msg->Type)
    {
        case Party:
        {
            if (!g_Tts.party) { wasIgnored = true; ignoreReason = "Party TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            if (msg->Flags & Party_IsFromCommander)
                ttsText = "Party comm " + std::string(text);
            else
                ttsText = "Party " + FirstName(name) + " " + text;
            break;
        }

        case Squad:
        {
            if (!g_Tts.squad) { wasIgnored = true; ignoreReason = "Squad TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            if (msg->Flags & Squad_IsFromCommander)
                ttsText = "Squad comm " + std::string(text);
            else
                ttsText = "Squad " + FirstName(name) + " " + text;
            break;
        }

        case Map:
        {
            if (!g_Tts.map) { wasIgnored = true; ignoreReason = "Map TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Map " + FirstName(name) + " " + text;
            break;
        }

        case Local:
        {
            if (!g_Tts.local) { wasIgnored = true; ignoreReason = "Local TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Local " + FirstName(name) + " " + text;
            break;
        }

        case Whisper:
        {
            if (!g_Tts.whisper) { wasIgnored = true; ignoreReason = "Whisper TTS disabled"; break; }
            if (msg->Flags & Whisper_IsFromMe) { wasIgnored = true; ignoreReason = "from self (Whisper_IsFromMe flag)"; break; }
            text = msg->Whisper.Content;
            name = msg->Whisper.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Whisper " + FirstName(name) + " " + text;
            break;
        }

        case SquadBroadcast:
        {
            if (!g_Tts.squadBroadcast) { wasIgnored = true; ignoreReason = "SquadBroadcast TTS disabled"; break; }
            text = msg->SquadMessage;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = std::string("Squad broadcast: ") + text;
            break;
        }

        case Guild:
        {
            if (!g_Tts.guild) { wasIgnored = true; ignoreReason = "Guild TTS disabled"; break; }
            text = msg->Guild.Base.Content;
            name = msg->Guild.Base.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Guild " + FirstName(name) + " " + text;
            break;
        }

        case SquadMessage:
        {
            if (!g_Tts.squadBroadcast) { wasIgnored = true; ignoreReason = "SquadBroadcast TTS disabled"; break; }
            text = msg->SquadMessage;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = std::string("Squad message: ") + text;
            break;
        }

        case Error:
        {
            if (!g_Tts.error) { wasIgnored = true; ignoreReason = "Error TTS disabled"; break; }
            ttsText = "Error message";
            break;
        }

        case GuildMotD:
        {
            if (!g_Tts.guildMotD) { wasIgnored = true; ignoreReason = "GuildMotD TTS disabled"; break; }
            text = msg->GuildMotD.Content;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = std::string("Guild MotD: ") + text;
            break;
        }

        case TeamPvP:
        {
            if (!g_Tts.teamPvP) { wasIgnored = true; ignoreReason = "TeamPvP TTS disabled"; break; }
            text = msg->TeamPvP.Content;
            name = msg->TeamPvP.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Team PvP " + FirstName(name) + " " + text;
            break;
        }

        case TeamWvW:
        {
            if (!g_Tts.teamWvW) { wasIgnored = true; ignoreReason = "TeamWvW TTS disabled"; break; }
            text = msg->TeamWvW.Base.Content;
            name = msg->TeamWvW.Base.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Team WvW " + FirstName(name) + " " + text;
            break;
        }

        case Emote:
        {
            if (!g_Tts.emote) { wasIgnored = true; ignoreReason = "Emote TTS disabled"; break; }
            name = msg->Emote.CharacterName;
            if (!name || !*name) { wasIgnored = true; ignoreReason = "empty character name"; break; }
            ttsText = "Emote " + FirstName(name);
            break;
        }

        case EmoteCustom:
        {
            if (!g_Tts.emoteCustom) { wasIgnored = true; ignoreReason = "EmoteCustom TTS disabled"; break; }
            name = msg->EmoteCustom.CharacterName;
            if (!name || !*name) { wasIgnored = true; ignoreReason = "empty character name"; break; }
            ttsText = "Custom emote " + FirstName(name);
            break;
        }

        default:
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[CHAT] [UNKNOWN] unhandled message type %d — ignored", msg->Type);
            g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
            return;
        }
    }

    if (wasIgnored)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] [IGNORED] %s", ChannelName(msg->Type), ignoreReason);
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
        return;
    }

    // Skip messages from ourselves
    if (name && !g_PlayerName.empty() && g_PlayerName == name)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] [IGNORED] from self (name matches Mumble identity: \"%s\")", ChannelName(msg->Type), name);
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
        return;
    }

    // Strip chat links ([&...]) from TTS output
    ttsText = StripChatLinks(ttsText.c_str());

    if (ttsText.empty())
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] [IGNORED] ttsText is empty after processing", ChannelName(msg->Type));
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf);
        return;
    }

    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] event: sender=\"%s\" text=\"%s\" -> TTS=\"%s\"",
                 ChannelName(msg->Type),
                 name ? name : "(none)",
                 text ? text : "(none)",
                 ttsText.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }

    std::wstring wide = Utf8ToWide(ttsText.c_str());
    SpeakText(wide.c_str());
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[CHAT] [%s] [SPOKEN] \"%s\"", ChannelName(msg->Type), ttsText.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
}

// ── ImGui Setup ──────────────────────────────────────────────────────────────

static void SetupImGui()
{
    if (!g_ImguiSetupDone)
    {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(g_API->ImguiContext));
        ImGui::SetAllocatorFunctions(
            (void* (*)(size_t, void*))g_API->ImguiMalloc,
            (void  (*)(void*, void*))g_API->ImguiFree);
        g_ImguiSetupDone = true;
    }
}

static void OnRender()
{
    SetupImGui();

    // Staggered pre-fetch: start 30s after load, fetch one icon every 5s
    if (!g_PrefetchStarted && g_LoadTime != 0 && GetTickCount() - g_LoadTime >= 30000)
    {
        g_PrefetchStarted = true;
        g_PrefetchIndex = 0;
        g_LastPrefetchTime = 0;
    }
    if (g_PrefetchStarted && g_PrefetchIndex < g_BuiltinMechanicCount)
    {
        DWORD now = GetTickCount();
        if (g_LastPrefetchTime == 0 || now - g_LastPrefetchTime >= 5000)
        {
            LoadIconTexture(g_BuiltinMechanics[g_PrefetchIndex].skillID);
            g_PrefetchIndex++;
            g_LastPrefetchTime = now;
        }
    }

    if (g_CrosshairEnabled)
    {
        ImDrawList* dl = ImGui::GetOverlayDrawList();
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        ImU32 color = ImGui::GetColorU32(ImVec4(g_CrosshairColor[0], g_CrosshairColor[1], g_CrosshairColor[2], g_CrosshairColor[3]));
        float thickness = 2.0f;
        dl->AddLine(ImVec2(0.0f, mousePos.y), ImVec2(displaySize.x, mousePos.y), color, thickness);
        dl->AddLine(ImVec2(mousePos.x, 0.0f), ImVec2(mousePos.x, displaySize.y), color, thickness);
    }

    // Test icon toggle: show Necrosis icon so user can configure position/size/opacity
    if (g_ShowTestIcon)
    {
        LoadIconTexture(47414);
        g_ActiveMechanics[47414] = GetTickCount();
    }
    else
    {
        g_ActiveMechanics.erase(47414);
    }

    if (g_IconDisplayEnabled && !g_ActiveMechanics.empty())
    {
        ImDrawList* dl = ImGui::GetOverlayDrawList();
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        DWORD now = GetTickCount();
        PruneExpiredMechanics(now);

        int screenW = static_cast<int>(displaySize.x);
        int screenH = static_cast<int>(displaySize.y);
        int idx = static_cast<int>(g_IconPosition.anchor);
        int col = idx % 3;
        int row = idx / 3;

        float baseX = 0.0f;
        switch (col)
        {
            case 0: baseX = 0.0f; break;
            case 1: baseX = displaySize.x * 0.5f; break;
            case 2: baseX = displaySize.x; break;
        }
        float baseY = 0.0f;
        switch (row)
        {
            case 0: baseY = 0.0f; break;
            case 1: baseY = displaySize.y * 0.5f; break;
            case 2: baseY = displaySize.y; break;
        }

        float offsetX = (g_IconPosition.offsetXPct / 100.0f) * displaySize.x;
        float offsetY = (g_IconPosition.offsetYPct / 100.0f) * displaySize.y;

        float x = baseX + offsetX - (g_ActiveMechanics.size() * (g_IconSize + g_IconSpacing) - g_IconSpacing) * 0.5f;
        float y = baseY + offsetY - g_IconSize * 0.5f;

        for (const auto& entry : g_ActiveMechanics)
        {
            ImTextureID tex = LoadIconTexture(entry.first);
            if (tex)
            {
                ImVec2 pMin(x, y);
                ImVec2 pMax(x + g_IconSize, y + g_IconSize);
                ImU32 tint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, g_IconOpacity));
                dl->AddImage(tex, pMin, pMax, ImVec2(0, 0), ImVec2(1, 1), tint);
            }
            x += g_IconSize + g_IconSpacing;
        }
    }
}

// ── Options UI ───────────────────────────────────────────────────────────────

static void OnMechanicsWindowRender()
{
    SetupImGui();
    bool changed = false;

    if (!g_ShowMechanicsWindow)
        return;

    if (!ImGui::Begin("Raid Mechanics", &g_ShowMechanicsWindow))
    {
        ImGui::End();
        return;
    }

    // ── Built-in Mechanics ──────────────────────────────────────────────────
    ImGui::TextUnformatted("Built-in Mechanics");
    ImGui::Separator();

    for (size_t i = 0; i < g_BuiltinMechanicCount; i++)
    {
        const BuiltinMechanic& bm = g_BuiltinMechanics[i];
        ImGui::PushID(static_cast<int>(bm.skillID));

        bool ttsOn = IsBuiltinMechanicTtsEnabled(bm.skillID);
        bool iconOn = IsBuiltinMechanicIconEnabled(bm.skillID);

        if (ImGui::Checkbox("TTS", &ttsOn))
        {
            g_BuiltinMechanicTts[bm.skillID] = ttsOn;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Icon", &iconOn))
        {
            g_BuiltinMechanicIcon[bm.skillID] = iconOn;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("%s (%u)", bm.name, bm.skillID);

        ImGui::PopID();
    }

    // ── Custom Mechanics ────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("Custom Mechanics");
    ImGui::Separator();

    if (ImGui::Button("Add Mechanic"))
    {
        RaidMechanic m;
        m.name = "New Mechanic";
        m.skillID = 0;
        m.ttsEnabled = true;
        m.iconEnabled = true;
        g_RaidMechanics.push_back(m);
        changed = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Add Example: Dhuum Shackles"))
    {
        RaidMechanic m;
        m.skillID = 50553;
        m.name = "Shackles";
        m.ttsEnabled = true;
        m.iconEnabled = true;
        g_RaidMechanics.push_back(m);
        changed = true;
    }

    ImGui::Separator();

    int toRemove = -1;
    for (size_t i = 0; i < g_RaidMechanics.size(); i++)
    {
        auto& m = g_RaidMechanics[i];
        ImGui::PushID(static_cast<int>(i));

        changed |= ImGui::Checkbox("TTS", &m.ttsEnabled);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Icon", &m.iconEnabled);
        ImGui::SameLine();

        char nameBuf[128] = {};
        strncpy_s(nameBuf, m.name.c_str(), sizeof(nameBuf) - 1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
        {
            m.name = nameBuf;
            changed = true;
        }

        ImGui::SameLine();
        int skillID = static_cast<int>(m.skillID);
        if (ImGui::InputInt("Skill ID", &skillID, 0, 0))
        {
            m.skillID = static_cast<unsigned int>(std::max(0, skillID));
            changed = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Remove"))
        {
            toRemove = static_cast<int>(i);
            changed = true;
        }

        ImGui::PopID();
    }

    if (toRemove >= 0)
        g_RaidMechanics.erase(g_RaidMechanics.begin() + toRemove);

    ImGui::Separator();
    ImGui::TextDisabled("Toggle TTS and/or Icon alerts per mechanic.");
    ImGui::TextDisabled("Skill IDs come from ArcdpsIntegration combat events.");

    ImGui::End();

    if (changed)
        SaveSettings();
}

static void OnOptionsRender()
{
    SetupImGui();
    bool changed = false;

    // ── Motor ─────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Motor", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        // ── Mouse Position ──
        ImGui::TextUnformatted("Mouse Position");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Enable", &g_Enabled);
        changed |= ImGui::Checkbox("Hold Mode", &g_HoldMode);
        ImGui::TextDisabled(g_HoldMode
            ? "Hold keybind to go to Position B, release to return to Position A"
            : "Press keybind to toggle between Position A and B");

        static int s_EditingSlot = 0;
        const char* slotNames[] = { "Position A (toggle off)", "Position B (toggle on)" };
        ImGui::Combo("Edit Slot", &s_EditingSlot, slotNames, 2);

        ImGui::Indent();
        ConfigPosition& pos = g_Positions[s_EditingSlot];

        int anchorIdx = static_cast<int>(pos.anchor);
        char mouseAnchorLabel[64];
        snprintf(mouseAnchorLabel, sizeof(mouseAnchorLabel), "Position %s Anchor", s_EditingSlot == 0 ? "A" : "B");
        if (ImGui::Combo(mouseAnchorLabel, &anchorIdx, g_AnchorNames,
                         static_cast<int>(AnchorPoint::COUNT)))
        {
            pos.anchor = static_cast<AnchorPoint>(anchorIdx);
            changed = true;
        }

        char mouseLabelX[64], mouseLabelY[64];
        snprintf(mouseLabelX, sizeof(mouseLabelX), "Position %s Offset X %%", s_EditingSlot == 0 ? "A" : "B");
        snprintf(mouseLabelY, sizeof(mouseLabelY), "Position %s Offset Y %%", s_EditingSlot == 0 ? "A" : "B");
        changed |= ImGui::SliderFloat(mouseLabelX, &pos.offsetXPct, -50.0f, 50.0f, "%.1f%%");
        changed |= ImGui::SliderFloat(mouseLabelY, &pos.offsetYPct, -50.0f, 50.0f, "%.1f%%");

        if (ImGui::Button("Test"))
        {
            if (g_Enabled)
                MoveToPosition(pos);
        }
        ImGui::Unindent();

        ImGui::Separator();
        if (ImGui::Button("Toggle Now"))
        {
            if (g_Enabled)
                ToggleMousePosition();
        }

        ImGui::SameLine();
        {
            int x, y;
            GetTargetPosition(g_Positions[g_ActivePosition], x, y);
            ImGui::TextDisabled("Active: %s (%d, %d)",
                g_AnchorNames[static_cast<int>(g_Positions[g_ActivePosition].anchor)], x, y);
        }

        // ── Crosshair ──
        ImGui::Dummy(ImVec2(0, 4));
        changed |= ImGui::Checkbox("Show Crosshair", &g_CrosshairEnabled);
        if (g_CrosshairEnabled)
        {
            ImGui::Indent();
            changed |= ImGui::ColorEdit4("Crosshair Color", g_CrosshairColor);
            ImGui::Unindent();
        }

        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ── Auditory ──────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Auditory"))
    {
        ImGui::Indent();

        // ── Chat TTS ──
        ImGui::TextUnformatted("Chat TTS");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Enable TTS", &g_Tts.enabled);

        if (g_Tts.enabled)
        {
            ImGui::Indent();
            ImGui::TextUnformatted("Speak for:");
            changed |= ImGui::Checkbox("Party",       &g_Tts.party);
            changed |= ImGui::Checkbox("Squad",        &g_Tts.squad);
            changed |= ImGui::Checkbox("Whisper",      &g_Tts.whisper);
            changed |= ImGui::Checkbox("Squad Broadcast", &g_Tts.squadBroadcast);
            changed |= ImGui::Checkbox("Map",          &g_Tts.map);
            changed |= ImGui::Checkbox("Local Say",    &g_Tts.local);
            changed |= ImGui::Checkbox("Guild",         &g_Tts.guild);
            changed |= ImGui::Checkbox("Guild MotD",   &g_Tts.guildMotD);
            changed |= ImGui::Checkbox("Error",         &g_Tts.error);
            changed |= ImGui::Checkbox("Team PvP",      &g_Tts.teamPvP);
            changed |= ImGui::Checkbox("Team WvW",      &g_Tts.teamWvW);
            changed |= ImGui::Checkbox("Emote",         &g_Tts.emote);
            changed |= ImGui::Checkbox("Emote Custom",  &g_Tts.emoteCustom);

            if (ImGui::Button("Test TTS"))
            {
                InitSAPI();
                if (g_TtsReady)
                    SpeakText(L"Chat TTS is working. Testing one two three.");
            }

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("Requires \"Events: Chat\" addon by Vonsh.1427");
            ImGui::TextDisabled("to be installed and enabled.");
            ImGui::Unindent();
        }

        // ── Alerts (downed, food, tofu) ──
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextUnformatted("Alerts");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Announce Ally Downed", &g_AllyDownedEnabled);
        if (g_AllyDownedEnabled)
        {
            ImGui::Indent();
            changed |= ImGui::Checkbox("Squad Only", &g_AllyDownedSquadOnly);
            ImGui::TextDisabled("Only announce when in a squad (filters party-only groups).");
            ImGui::Unindent();
        }
        ImGui::TextDisabled("Speaks \"PlayerName downed\" when a squad member goes downed.");

        changed |= ImGui::Checkbox("Announce Food/Utility Expiry", &g_FoodUtilityExpiryEnabled);
        ImGui::TextDisabled("Speaks \"food expired\" or \"utility expired\" when a buff expires after 1+ minute.");

        // ── Raid Mechanics TTS ──
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextUnformatted("Raid Mechanic Alerts");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Enable Raid Mechanic Alerts", &g_MechanicsAnnounceEnabled);
        changed |= ImGui::Checkbox("Read All Debuffs on Player", &g_ReadAllDebuffs);
        if (g_ReadAllDebuffs)
            ImGui::TextDisabled("Only conditions/debuffs are spoken (boons are filtered out).");
        ImGui::TextDisabled("Requires \"Arcdps Integration\" addon (bundled with Nexus).");

        if (g_MechanicsAnnounceEnabled || g_ReadAllDebuffs)
        {
            ImGui::Indent();
            if (ImGui::Button("Open Mechanics"))
            {
                g_ShowMechanicsWindow = true;
            }
            ImGui::Unindent();
        }

        if (g_ReadAllDebuffs && !g_MechanicsAnnounceEnabled)
        {
            ImGui::TextDisabled("All debuff mode enabled — every debuff applied to you will be spoken.");
        }

        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ── Visual ────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Visual"))
    {
        ImGui::Indent();

        ImGui::TextUnformatted("Active Mechanic Icons");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Show Active Mechanic Icons", &g_IconDisplayEnabled);
        if (g_IconDisplayEnabled)
        {
            ImGui::Indent();
            changed |= ImGui::SliderFloat("Icon Size", &g_IconSize, 16.0f, 512.0f, "%.0f");
            changed |= ImGui::SliderFloat("Icon Spacing", &g_IconSpacing, 0.0f, 32.0f, "%.0f");
            changed |= ImGui::SliderFloat("Opacity", &g_IconOpacity, 0.0f, 1.0f, "%.2f");

            int iconAnchorIdx = static_cast<int>(g_IconPosition.anchor);
            if (ImGui::Combo("Icon Anchor", &iconAnchorIdx, g_AnchorNames, static_cast<int>(AnchorPoint::COUNT)))
            {
                g_IconPosition.anchor = static_cast<AnchorPoint>(iconAnchorIdx);
                changed = true;
            }

            changed |= ImGui::SliderFloat("Icon Offset X %", &g_IconPosition.offsetXPct, -50.0f, 50.0f, "%.1f%%");
            changed |= ImGui::SliderFloat("Icon Offset Y %", &g_IconPosition.offsetYPct, -50.0f, 50.0f, "%.1f%%");

            ImGui::Checkbox("Show", &g_ShowTestIcon);
            ImGui::TextDisabled("Shows a test icon to help set up position, size, and opacity.");

            ImGui::Unindent();
        }

        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0, 4));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Mouse keybind: GW2ACCESSIBILITY_MOUSE_TOGGLE");
    ImGui::TextDisabled("Set in Nexus Settings > Keybinds > GW2Accessibility");

    ImGui::Separator();
    changed |= ImGui::Checkbox("Tofu's Toggle", &g_TofusToggle);
    if (g_TofusToggle)
        ImGui::TextDisabled("Speaks \"Tofu, KC is dead, check your gear.\" when Keep Construct dies.");

    if (changed)
        SaveSettings();
}

// ── Addon Lifecycle ──────────────────────────────────────────────────────────

void AddonLoad(AddonAPI_t* aAPI)
{
    g_API = aAPI;
    LoadSettings();
    InitMumble();

    g_API->InputBinds_RegisterWithString(
        "GW2ACCESSIBILITY_MOUSE_TOGGLE",
        OnMouseToggleKeybind,
        "(null)");

    g_API->GUI_Register(RT_Render, OnRender);
    g_API->GUI_Register(RT_OptionsRender, OnOptionsRender);
    g_API->GUI_Register(RT_Render, OnMechanicsWindowRender);
    g_API->Events_Subscribe(GW2_CHAT_EVENT, OnChatMessage);
    g_API->Events_Subscribe(EV_ARCDPS_COMBAT_SQUAD, OnCombatEvent);
    g_API->Events_Subscribe(EV_ARCDPS_COMBAT_LOCAL, OnCombatEvent);

    g_LoadTime = GetTickCount();
    g_PrefetchStarted = false;
    g_PrefetchIndex = 0;
    g_LastPrefetchTime = 0;
}

void AddonUnload()
{
    SaveSettings();

    if (g_API)
    {
        g_API->InputBinds_Deregister("GW2ACCESSIBILITY_MOUSE_TOGGLE");
        g_API->GUI_Deregister(OnRender);
        g_API->GUI_Deregister(OnOptionsRender);
        g_API->GUI_Deregister(OnMechanicsWindowRender);
        g_API->Events_Unsubscribe(GW2_CHAT_EVENT, OnChatMessage);
        g_API->Events_Unsubscribe(EV_ARCDPS_COMBAT_SQUAD, OnCombatEvent);
        g_API->Events_Unsubscribe(EV_ARCDPS_COMBAT_LOCAL, OnCombatEvent);
    }

    if (g_pVoice)
    {
        g_pVoice->Release();
        g_pVoice = nullptr;
    }

    g_API = nullptr;
    g_ImguiSetupDone = false;
    g_TtsReady = false;

    if (g_MumbleData)
    {
        UnmapViewOfFile(g_MumbleData);
        g_MumbleData = nullptr;
    }
    if (g_MumbleFileMap)
    {
        CloseHandle(g_MumbleFileMap);
        g_MumbleFileMap = nullptr;
    }
    g_PlayerName.clear();
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    static AddonVersion_t s_Version = { 0, 1, 2, 0 };

    static AddonDefinition_t s_Def = {};
    s_Def.Signature  = -2;
    s_Def.APIVersion = NEXUS_API_VERSION;
    s_Def.Name       = "GW2Accessibility";
    s_Def.Version    = s_Version;
    s_Def.Author     = "Todd0042";
    s_Def.Description= "Accessibility features for Guild Wars 2";
    s_Def.Load       = AddonLoad;
    s_Def.Unload     = AddonUnload;
    s_Def.Flags      = AF_None;
    s_Def.Provider   = UP_GitHub;
    s_Def.UpdateLink = "https://github.com/Todd0042/gw2-accessibility";

    return &s_Def;
}
