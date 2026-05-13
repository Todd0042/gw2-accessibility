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
#include <winhttp.h>

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
static ConfigPosition g_IconPosition = {AnchorPoint::Center, 0.0f, -10.0f};
static std::unordered_map<unsigned int, ImTextureID> g_IconTextures;
static std::unordered_map<unsigned int, std::string> g_IconUrlCache;
static std::unordered_map<unsigned int, DWORD> g_ActiveMechanics;
static const DWORD g_MechanicIconTimeout = 30000;

// ── TTS ──────────────────────────────────────────────────────────────────────

static ISpVoice* g_pVoice = nullptr;
static bool g_TtsReady = false;

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
    bool enabled = false;
};

static std::vector<RaidMechanic> g_RaidMechanics;

static bool g_MechanicsAnnounceEnabled = false;
static bool g_ReadAllDebuffs = false;
static std::unordered_map<unsigned int, std::string> g_DebuffNameCache;

// Track active buffs with last-seen timestamp so expired debuffs are cleared
// (conditions tick ~1s; if no tick for 3s, assume natural expiry)
static std::unordered_map<unsigned int, DWORD> g_ActiveBuffs;
static const DWORD g_ActiveBuffTimeout = 3000;

// Food/utility expiry tracking
static bool g_FoodUtilityExpiryEnabled = false;
static std::unordered_map<unsigned int, DWORD> g_TrackedFoodUtility;
static const DWORD g_FoodUtilityMinDuration = 60000; // must be active >1 min to be food/utility

// Ally downed tracking
static bool g_AllyDownedEnabled = false;
static std::unordered_set<uint64_t> g_DownedAgents;

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
    if (!g_pVoice || !text || !*text) return;
    g_pVoice->Speak(text, SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
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
        std::wstring path = L"/v2/skills/" + std::to_wstring(skillID);
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
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
                        std::string result;
                        DWORD bytesRead = 0;
                        std::vector<char> buffer(4096);
                        while (WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) && bytesRead > 0)
                            result.append(buffer.data(), bytesRead);

                        try
                        {
                            auto json = nlohmann::json::parse(result);
                            iconUrl = json.value("icon", "");
                        }
                        catch (...) {}
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);

    if (!iconUrl.empty())
        g_IconUrlCache[skillID] = iconUrl;

    return iconUrl;
}

static ImTextureID LoadIconTexture(unsigned int skillID)
{
    auto cached = g_IconTextures.find(skillID);
    if (cached != g_IconTextures.end())
        return cached->second;

    std::string url = FetchSkillIconUrl(skillID);
    if (url.empty())
        return nullptr;

    // Parse URL into base + path for Nexus texture API
    // URL format: https://render.guildwars2.com/file/ABC123/icon.png
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return nullptr;
    size_t pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos) return nullptr;

    std::string base = url.substr(0, pathStart);
    std::string endpoint = url.substr(pathStart);

    char identifier[64];
    snprintf(identifier, sizeof(identifier), "gw2accessibility_icon_%u", skillID);

    Texture_t* tex = g_API->Textures_GetOrCreateFromURL(identifier, base.c_str(), endpoint.c_str());
    if (tex && tex->Resource)
    {
        g_IconTextures[skillID] = static_cast<ImTextureID>(tex->Resource);
        return g_IconTextures[skillID];
    }

    return nullptr;
}

static void LoadAllConfiguredIcons()
{
    for (const auto& m : g_RaidMechanics)
    {
        if (m.enabled && m.skillID != 0)
            LoadIconTexture(m.skillID);
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
    j["iconAnchor"] = static_cast<int>(g_IconPosition.anchor);
    j["iconOffsetX"] = g_IconPosition.offsetXPct;
    j["iconOffsetY"] = g_IconPosition.offsetYPct;
    j["foodUtilityExpiry"] = g_FoodUtilityExpiryEnabled;
    j["allyDownedEnabled"] = g_AllyDownedEnabled;

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
        entry["skillID"] = m.skillID;
        entry["name"]    = m.name;
        entry["enabled"] = m.enabled;
        mechanics.push_back(entry);
    }
    j["raidMechanics"] = mechanics;
    j["readAllDebuffs"] = g_ReadAllDebuffs;

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
        int iconAnchor = j.value("iconAnchor", static_cast<int>(AnchorPoint::Center));
        iconAnchor = std::clamp(iconAnchor, 0, static_cast<int>(AnchorPoint::COUNT) - 1);
        g_IconPosition.anchor = static_cast<AnchorPoint>(iconAnchor);
        g_IconPosition.offsetXPct = j.value("iconOffsetX", 0.0f);
        g_IconPosition.offsetYPct = j.value("iconOffsetY", -10.0f);
        g_FoodUtilityExpiryEnabled = j.value("foodUtilityExpiry", false);
        g_AllyDownedEnabled = j.value("allyDownedEnabled", false);

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
                m.skillID = entry.value("skillID", 0u);
                m.name    = entry.value("name", "");
                m.enabled = entry.value("enabled", false);
                if (m.skillID != 0 && !m.name.empty())
                    g_RaidMechanics.push_back(m);
            }
        }
    }
    catch (...)
    {
    }
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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                          IID_ISpVoice, (void**)&g_pVoice);
    g_TtsReady = SUCCEEDED(hr) && g_pVoice != nullptr;
}

static void OnCombatEvent(void* aEventArgs)
{
    auto* cbt = static_cast<const EvCombatData*>(aEventArgs);
    if (!cbt->ev || !cbt->dst) return;

    // ── Ally downed tracking (state change event) ────────────────────────────
    if (g_AllyDownedEnabled && cbt->ev->IsStatechange == 5) // CBTS_CHANGEDOWN
    {
        uint64_t agentId = cbt->ev->SourceAgent;
        if (!g_DownedAgents.count(agentId))
        {
            g_DownedAgents.insert(agentId);
            const char* agentName = cbt->src && cbt->src->Name ? cbt->src->Name : "Ally";
            char buf[256];
            snprintf(buf, sizeof(buf), "[COMBAT] [ALLY_DOWNED] agent %llu (%s) downed", (unsigned long long)agentId, agentName);
            g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
            InitSAPI();
            if (g_TtsReady)
            {
                std::string speech = std::string(agentName) + " downed";
                SpeakText(Utf8ToWide(speech.c_str()).c_str());
                { char buf2[128]; snprintf(buf2, sizeof(buf2), "[COMBAT] [SPOKEN] \"%s\"", speech.c_str()); g_API->Log(LOGL_INFO, "GW2Accessibility", buf2); }
            }
        }
        return;
    }

    // ── Ally revived (clear from downed set) ─────────────────────────────────
    if (cbt->ev->IsStatechange == 3) // CBTS_CHANGEUP
    {
        uint64_t agentId = cbt->ev->SourceAgent;
        if (g_DownedAgents.erase(agentId))
        {
            g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] [ALLY_DOWNED] agent revived, cleared from downed set");
        }
        return;
    }

    if (!g_MechanicsAnnounceEnabled && !g_ReadAllDebuffs)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] event received but both MechanicsAnnounce and ReadAllDebuffs are disabled — ignored");
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

    unsigned int skillID = cbt->ev->SkillID;
    bool isRemove = (cbt->ev->IsBuffRemove != 0);
    DWORD now = GetTickCount();

    // Log every buff event received
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[COMBAT] skill=%u name=%s action=%s value=%d buffDmg=%d result=%d",
                 skillID,
                 FetchSkillName(skillID, cbt->skillname).c_str(),
                 isRemove ? "remove" : "add",
                 cbt->ev->Value,
                 cbt->ev->BuffDamage,
                 cbt->ev->Result);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }

    // ── Track active mechanics for icon display ──────────────────────────────
    if (isRemove)
    {
        g_ActiveMechanics.erase(skillID);
        { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] icon tracking: removed skill %u from active", skillID); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
    }
    else
    {
        for (const auto& m : g_RaidMechanics)
        {
            if (m.enabled && m.skillID == skillID)
            {
                g_ActiveMechanics[skillID] = now;
                LoadIconTexture(skillID);
                { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] icon tracking: added skill %u (%s) to active", skillID, m.name.c_str()); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
                break;
            }
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
                // Food: ends with Steak/Flatbread or contains Salad/Soup/Pancake
                if (buffName.size() >= 6)
                {
                    std::string end6 = buffName.substr(buffName.size() - 6);
                    if (end6 == "Steak" || end6 == "steak" || end6 == "adbread") // Flatbread ends with "adbread"
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
                { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] [FOOD/UTILITY] skill %u (%s) expired after %ums — speaking \"%s\"", skillID, buffName.c_str(), now - fuIt->second, speech); g_API->Log(LOGL_INFO, "GW2Accessibility", buf); }
                InitSAPI();
                if (g_TtsReady)
                {
                    SpeakText(Utf8ToWide(speech).c_str());
                    { char buf[64]; snprintf(buf, sizeof(buf), "[COMBAT] [SPOKEN] \"%s\"", speech); g_API->Log(LOGL_INFO, "GW2Accessibility", buf); }
                }
            }
            g_TrackedFoodUtility.erase(skillID);
        }
        else
        {
            // Track new buff applications that aren't boons/conditions/raid mechanics
            bool isKnown = IsBoon(skillID);
            if (!isKnown)
            {
                auto builtin = g_BuiltinBuffNames.find(skillID);
                if (builtin != g_BuiltinBuffNames.end())
                {
                    static const std::unordered_set<std::string> conditionNames = {
                        "Bleeding", "Burning", "Confusion", "Poison", "Torment",
                        "Blind", "Chilled", "Crippled", "Immobile", "Weakness",
                        "Vulnerability", "Slow", "Fear", "Stun", "Daze", "Taunt", "Battle Scars"
                    };
                    if (conditionNames.count(builtin->second))
                        isKnown = true;
                }
            }
            if (!isKnown)
            {
                for (const auto& m : g_RaidMechanics)
                {
                    if (m.enabled && m.skillID == skillID) { isKnown = true; break; }
                }
            }
            if (!isKnown)
            {
                g_TrackedFoodUtility[skillID] = now;
                { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] [FOOD/UTILITY] tracking skill %u (%s)", skillID, FetchSkillName(skillID, cbt->skillname).c_str()); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
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
                    g_API->Log(LOGL_INFO, "GW2Accessibility", "[COMBAT] [SPOKEN] \"Necrosis removed\" (Necrosis full removal)");
                }
            }
            else
            {
                g_NecrosisStacks = std::max(0, g_NecrosisStacks - 1);
                { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] Necrosis stack decreased to %d (single stack removal)", g_NecrosisStacks); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
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
                    { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] [SPOKEN] \"%s\" (Necrosis stacks=%d)", msg.c_str(), g_NecrosisStacks); g_API->Log(LOGL_INFO, "GW2Accessibility", buf); }
                }
            }
            else
            {
                { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] Necrosis at %d stacks — no speech (not first or 4+)", g_NecrosisStacks); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
            }
        }
        return;
    }

    // ── Buff removals — clear from active set so re-application is spoken again ──
    if (isRemove)
    {
        g_ActiveBuffs.erase(skillID);
        { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] [SKIP] buff removed — cleared from active set for skill %u", skillID); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
        return;
    }

    // ── Buff additions — only speak if not already active on player ──
    auto activeIt = g_ActiveBuffs.find(skillID);
    if (activeIt != g_ActiveBuffs.end())
    {
        if (now - activeIt->second > g_ActiveBuffTimeout)
        {
            // No tick received within timeout window — debuff expired naturally
            g_ActiveBuffs.erase(activeIt);
            { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] skill %u expired (no tick for %ums), treating as new application", skillID, now - activeIt->second); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
        }
        else
        {
            // Recent tick — this is just a repeat, skip
            g_ActiveBuffs[skillID] = now;
            { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] [SKIP] skill %u already active on player (tick, last seen %ums ago)", skillID, now - activeIt->second); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
            return;
        }
    }
    g_ActiveBuffs[skillID] = now;

    std::string text;

    // Check for built-in custom TTS message (action-oriented)
    auto ttsMsg = g_BuiltinTtsMessages.find(skillID);
    if (ttsMsg != g_BuiltinTtsMessages.end())
    {
        text = ttsMsg->second;
        { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] matched built-in TTS message: \"%s\"", text.c_str()); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
    }
    else
    {
        if (g_MechanicsAnnounceEnabled)
        {
            for (const auto& m : g_RaidMechanics)
            {
                if (m.enabled && m.skillID == skillID)
                {
                    text = m.name;
                    { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] matched raid mechanic #%u: \"%s\"", skillID, text.c_str()); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
                    break;
                }
            }
        }

        // In "Read All Debuffs" mode, skip known boons — only speak conditions/debuffs
        if (text.empty() && g_ReadAllDebuffs)
        {
            if (IsBoon(skillID))
            {
                { char buf[128]; snprintf(buf, sizeof(buf), "[COMBAT] [SKIP] known boon (skill %u), filtered out", skillID); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
                return;
            }
            text = FetchSkillName(skillID, cbt->skillname);
            { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] ReadAllDebuffs: resolved name \"%s\" for skill %u", text.c_str(), skillID); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
        }
    }

    if (text.empty())
    {
        { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] [SKIP] no name resolved for skill %u (not in mechanics list, not in ReadAllDebuffs mode, no built-in message)", skillID); g_API->Log(LOGL_DEBUG, "GW2Accessibility", buf); }
        return;
    }

    InitSAPI();
    if (!g_TtsReady)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[COMBAT] [SKIP] SAPI not ready");
        return;
    }
    auto w = Utf8ToWide(text.c_str());
    SpeakText(w.c_str());
    { char buf[256]; snprintf(buf, sizeof(buf), "[COMBAT] [SPOKEN] \"%s\"", text.c_str()); g_API->Log(LOGL_INFO, "GW2Accessibility", buf); }
}

static std::string First5(const char* name)
{
    if (!name || !*name) return "";
    std::string s(name);
    if (s.size() > 5)
        s.resize(5);
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

static void OnChatMessage(void* aEventArgs)
{
    if (!g_Tts.enabled)
    {
        g_API->Log(LOGL_DEBUG, "GW2Accessibility", "[CHAT] event received but TTS is disabled — ignored");
        return;
    }
    auto* msg = static_cast<const Message*>(aEventArgs);

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
                ttsText = "Party " + First5(name) + " " + text;
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
                ttsText = "Squad " + First5(name) + " " + text;
            break;
        }

        case Map:
        {
            if (!g_Tts.map) { wasIgnored = true; ignoreReason = "Map TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Map " + First5(name) + " " + text;
            break;
        }

        case Local:
        {
            if (!g_Tts.local) { wasIgnored = true; ignoreReason = "Local TTS disabled"; break; }
            text = msg->Local.Content;
            name = msg->Local.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Local " + First5(name) + " " + text;
            break;
        }

        case Whisper:
        {
            if (!g_Tts.whisper) { wasIgnored = true; ignoreReason = "Whisper TTS disabled"; break; }
            if (msg->Flags & Whisper_IsFromMe) { wasIgnored = true; ignoreReason = "from self (Whisper_IsFromMe flag)"; break; }
            text = msg->Whisper.Content;
            name = msg->Whisper.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Whisper " + First5(name) + " " + text;
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
            ttsText = "Guild " + First5(name) + " " + text;
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
            ttsText = "Team PvP " + First5(name) + " " + text;
            break;
        }

        case TeamWvW:
        {
            if (!g_Tts.teamWvW) { wasIgnored = true; ignoreReason = "TeamWvW TTS disabled"; break; }
            text = msg->TeamWvW.Base.Content;
            name = msg->TeamWvW.Base.CharacterName;
            if (!text || !*text) { wasIgnored = true; ignoreReason = "empty content"; break; }
            ttsText = "Team WvW " + First5(name) + " " + text;
            break;
        }

        case Emote:
        {
            if (!g_Tts.emote) { wasIgnored = true; ignoreReason = "Emote TTS disabled"; break; }
            name = msg->Emote.CharacterName;
            if (!name || !*name) { wasIgnored = true; ignoreReason = "empty character name"; break; }
            ttsText = "Emote " + First5(name);
            break;
        }

        case EmoteCustom:
        {
            if (!g_Tts.emoteCustom) { wasIgnored = true; ignoreReason = "EmoteCustom TTS disabled"; break; }
            name = msg->EmoteCustom.CharacterName;
            if (!name || !*name) { wasIgnored = true; ignoreReason = "empty character name"; break; }
            ttsText = "Custom emote " + First5(name);
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
                dl->AddImage(tex, pMin, pMax);
            }
            x += g_IconSize + g_IconSpacing;
        }
    }
}

// ── Options UI ───────────────────────────────────────────────────────────────

static void OnOptionsRender()
{
    SetupImGui();
    bool changed = false;

    // ── Mouse Position Section ──
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
    if (ImGui::Combo("Anchor", &anchorIdx, g_AnchorNames,
                     static_cast<int>(AnchorPoint::COUNT)))
    {
        pos.anchor = static_cast<AnchorPoint>(anchorIdx);
        changed = true;
    }

    changed |= ImGui::SliderFloat("Offset X %", &pos.offsetXPct, -50.0f, 50.0f, "%.1f%%");
    changed |= ImGui::SliderFloat("Offset Y %", &pos.offsetYPct, -50.0f, 50.0f, "%.1f%%");

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

    ImGui::Separator();
    changed |= ImGui::Checkbox("Show Crosshair", &g_CrosshairEnabled);
    if (g_CrosshairEnabled)
    {
        ImGui::Indent();
        changed |= ImGui::ColorEdit4("Crosshair Color", g_CrosshairColor);
        ImGui::Unindent();
    }

    // ── Active Mechanic Icons Section ──
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("Active Mechanic Icons");
    ImGui::Separator();

    changed |= ImGui::Checkbox("Show Active Mechanic Icons", &g_IconDisplayEnabled);
    if (g_IconDisplayEnabled)
    {
        ImGui::Indent();
        changed |= ImGui::SliderFloat("Icon Size", &g_IconSize, 16.0f, 128.0f, "%.0f");
        changed |= ImGui::SliderFloat("Icon Spacing", &g_IconSpacing, 0.0f, 32.0f, "%.0f");

        int iconAnchorIdx = static_cast<int>(g_IconPosition.anchor);
        if (ImGui::Combo("Anchor", &iconAnchorIdx, g_AnchorNames,
                         static_cast<int>(AnchorPoint::COUNT)))
        {
            g_IconPosition.anchor = static_cast<AnchorPoint>(iconAnchorIdx);
            changed = true;
        }

        changed |= ImGui::SliderFloat("Offset X %", &g_IconPosition.offsetXPct, -50.0f, 50.0f, "%.1f%%");
        changed |= ImGui::SliderFloat("Offset Y %", &g_IconPosition.offsetYPct, -50.0f, 50.0f, "%.1f%%");
        ImGui::Unindent();
    }

    // ── Alerts Section ──
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("Alerts");
    ImGui::Separator();

    changed |= ImGui::Checkbox("Announce Food/Utility Expiry", &g_FoodUtilityExpiryEnabled);
    ImGui::TextDisabled("Speaks \"food expired\" or \"utility expired\" when a buff expires after 1+ minute.");

    changed |= ImGui::Checkbox("Announce Ally Downed", &g_AllyDownedEnabled);
    ImGui::TextDisabled("Speaks \"PlayerName downed\" when a squad member goes downed.");

    // ── Chat TTS Section ──
    ImGui::Dummy(ImVec2(0, 8));
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

    // ── Raid Mechanics TTS Section ──
    ImGui::Dummy(ImVec2(0, 8));
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

        if (ImGui::Button("Add Mechanic"))
        {
            RaidMechanic m;
            m.name = "New Mechanic";
            g_RaidMechanics.push_back(m);
            changed = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Add Example: Dhuum Shackles"))
        {
            RaidMechanic m;
            m.skillID = 50553;
            m.name = "Shackles";
            m.enabled = true;
            g_RaidMechanics.push_back(m);
            changed = true;
        }

        int toRemove = -1;
        for (size_t i = 0; i < g_RaidMechanics.size(); i++)
        {
            auto& m = g_RaidMechanics[i];
            ImGui::PushID(static_cast<int>(i));

            char nameBuf[128] = {};
            strncpy_s(nameBuf, m.name.c_str(), sizeof(nameBuf) - 1);
            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            {
                m.name = nameBuf;
                changed = true;
            }

            int skillID = static_cast<int>(m.skillID);
            if (ImGui::InputInt("Skill ID", &skillID))
            {
                m.skillID = static_cast<unsigned int>(std::max(0, skillID));
                changed = true;
            }

            changed |= ImGui::Checkbox("Enabled", &m.enabled);

            if (ImGui::Button("Remove"))
            {
                toRemove = static_cast<int>(i);
                changed = true;
            }

            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 2));
        }

        if (toRemove >= 0)
            g_RaidMechanics.erase(g_RaidMechanics.begin() + toRemove);

        ImGui::TextDisabled("Enter Skill IDs from the GW2 API or combat logs.");
        ImGui::TextDisabled("Mechanics are announced each time they are applied to the player.");
        ImGui::Unindent();
    }

    if (g_ReadAllDebuffs && !g_MechanicsAnnounceEnabled)
    {
        ImGui::TextDisabled("All debuff mode enabled — every debuff applied to you will be spoken.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Mouse keybind: GW2ACCESSIBILITY_MOUSE_TOGGLE");
    ImGui::TextDisabled("Set in Nexus Settings > Keybinds > GW2Accessibility");

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
    g_API->Events_Subscribe(GW2_CHAT_EVENT, OnChatMessage);
    g_API->Events_Subscribe(EV_ARCDPS_COMBAT_SQUAD, OnCombatEvent);
    g_API->Events_Subscribe(EV_ARCDPS_COMBAT_LOCAL, OnCombatEvent);
}

void AddonUnload()
{
    SaveSettings();

    if (g_API)
    {
        g_API->InputBinds_Deregister("GW2ACCESSIBILITY_MOUSE_TOGGLE");
        g_API->GUI_Deregister(OnRender);
        g_API->GUI_Deregister(OnOptionsRender);
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
    static AddonVersion_t s_Version = { 0, 1, 0, 0 };

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
