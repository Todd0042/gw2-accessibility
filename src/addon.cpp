#include "addon.h"
#include "Chat.h"
#include "arcdpsheader.h"
#include "unofficial_extras.h"
#include "mumbleheader.h"
#include "boon_ignore.h"
#include "buff_names.h"
#include "tts_messages.h"
#include <imgui.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <sapi.h>
#include <nlohmann/json.hpp>
#include "reffect_pack.h"
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

// ── Effect Tracking Manager ──────────────────────────────────────────
static bool g_ShowEffectManager = false;
static std::unordered_map<unsigned int, std::string> g_EncounteredEffects;
static std::unordered_set<unsigned int> g_TrackedConditionsExtra;
static std::unordered_set<unsigned int> g_TrackedBuffsExtra;

// ── Keybind Overlay ─────────────────────────────────────────────────────────
enum KeybindVisibility { KV_Always = 0, KV_InCombatOnly, KV_OutOfCombatOnly };
static bool g_KeybindOverlayEnabled = false;
static int g_KeybindVisibility = KV_Always;
static bool g_KeybindShowMovement   = true;
static bool g_KeybindShowSkills     = true;
static bool g_KeybindShowTargeting  = false;
static bool g_KeybindShowMounts     = true;
static bool g_KeybindShowSquad      = false;
static bool g_KeybindShowCamera     = false;
static bool g_KeybindShowScreenshot = false;
static bool g_KeybindShowMap        = false;
static bool g_KeybindShowUI         = false;
static bool g_KeybindShowTemplates  = false;
static ConfigPosition g_KeybindPosition = {AnchorPoint::MiddleLeft, 2.0f, 0.0f};
static float g_KeybindOverlayOpacity = 0.4f;
static bool g_ShowKeybindConfig = false;
static bool g_InCombat = false;
static bool g_KeybindsFromRealDocs = false;
static char g_KeybindCustomPath[512] = {0};

struct KeybindEntry {
    std::string  category;
    std::string  name;
    std::string  key;        // primary binding display string (may include modifier prefix)
    std::string  key2;       // secondary binding display string (empty if none)
    unsigned int rawButton  = 0;
    unsigned int rawButton2 = 0;
    unsigned int rawMod     = 0;
    unsigned int rawMod2    = 0;
};
static std::vector<KeybindEntry> g_Keybinds; // populated from XML
static bool g_KeybindsLoaded = false;
static std::vector<std::string> g_KeybindFiles; // available XML files
static int g_KeybindFileIndex = -1; // selected file index
static std::string g_ManualKeybindPath; // full path for manually selected file
static std::string g_KeybindBaseDir;   // resolved base directory from last successful scan

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

// Log a Wine-detection message to both Nexus log and OutputDebugStringA.
// g_API may be null if called before AddonLoad assigns it — guard accordingly.
static void WineLog(const char* msg)
{
    OutputDebugStringA(msg);
    if (g_API) g_API->Log(LOGL_INFO, "GW2Accessibility", msg);
}

static bool DetectWine()
{
    bool detected = false;
    char dbg[768];

    WineLog("[WINE] ── DetectWine() start ──────────────────────────────");

    // ── Method 1: ntdll wine_get_version / wine_get_build_id exports ─────────
    // These are the most reliable indicators — real Windows ntdll never has them.
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    bool hasWineGetVersion  = ntdll && GetProcAddress(ntdll, "wine_get_version")      != nullptr;
    bool hasWineGetBuildId  = ntdll && GetProcAddress(ntdll, "wine_get_build_id")     != nullptr;
    bool hasWineGetHost     = ntdll && GetProcAddress(ntdll, "wine_get_host_version") != nullptr;
    snprintf(dbg, sizeof(dbg),
        "[WINE] [1] ntdll=%p wine_get_version=%d wine_get_build_id=%d wine_get_host_version=%d",
        (void*)ntdll, hasWineGetVersion?1:0, hasWineGetBuildId?1:0, hasWineGetHost?1:0);
    WineLog(dbg);

    if (hasWineGetVersion)
    {
        typedef const char* (*Fn)();
        auto fn = (Fn)GetProcAddress(ntdll, "wine_get_version");
        snprintf(dbg, sizeof(dbg), "[WINE] [1] wine_get_version() = \"%s\"", fn ? fn() : "(null)");
        WineLog(dbg);
        detected = true;
    }
    if (hasWineGetBuildId)
    {
        typedef const char* (*Fn)();
        auto fn = (Fn)GetProcAddress(ntdll, "wine_get_build_id");
        snprintf(dbg, sizeof(dbg), "[WINE] [1] wine_get_build_id() = \"%s\"", fn ? fn() : "(null)");
        WineLog(dbg);
        detected = true;
    }

    // ── Method 2: Wine-specific DLLs loaded in process ───────────────────────
    // winex11.drv / winewayland.drv are Wine graphics drivers, never present on Windows.
    const char* wineDlls[] = { "winex11.drv", "winewayland.drv", "winebus.sys", "winemac.drv" };
    for (const char* dll : wineDlls)
    {
        HMODULE h = GetModuleHandleA(dll);
        snprintf(dbg, sizeof(dbg), "[WINE] [2] GetModuleHandle(\"%s\") = %p (%s)",
            dll, (void*)h, h ? "FOUND" : "not loaded");
        WineLog(dbg);
        if (h) detected = true;
    }

    // ── Method 3: Environment variables ──────────────────────────────────────
    struct { const char* name; bool triggersDetect; } envVars[] = {
        { "WINE",            true  },
        { "WINEPREFIX",      true  },
        { "PROTON_VERSION",  true  },
        { "STEAM_RUNTIME",   false }, // present but not conclusive alone
        { "HOME",            false }, // present on Linux, also set by some Windows tools
        { "XDG_RUNTIME_DIR", true  }, // Linux-only runtime dir
        { "DISPLAY",         false }, // X11 display — Linux indicator
        { "WAYLAND_DISPLAY", false }, // Wayland display
    };
    for (const auto& ev : envVars)
    {
        char val[512] = {0};
        DWORD len = GetEnvironmentVariableA(ev.name, val, sizeof(val));
        snprintf(dbg, sizeof(dbg), "[WINE] [3] %s=%s (len=%lu)",
            ev.name, len > 0 ? val : "(not set)", (unsigned long)len);
        WineLog(dbg);
        if (len > 0 && ev.triggersDetect) detected = true;
    }

    // ── Method 4: Registry key HKLM\Software\Wine ───────────────────────────
    HKEY hKey = nullptr;
    LONG regRes = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Wine", 0, KEY_READ, &hKey);
    snprintf(dbg, sizeof(dbg), "[WINE] [4] HKLM\\Software\\Wine: %s (code=%ld)",
        regRes == ERROR_SUCCESS ? "EXISTS" : "not found", (long)regRes);
    WineLog(dbg);
    if (regRes == ERROR_SUCCESS) { RegCloseKey(hKey); detected = true; }

    // ── Method 5: Windows version via RtlGetVersion (bypasses compat shim) ──
    typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW rtlVer = {};
    rtlVer.dwOSVersionInfoSize = sizeof(rtlVer);
    if (ntdll)
    {
        auto RtlGetVersion = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
        if (RtlGetVersion && RtlGetVersion(&rtlVer) == 0)
        {
            snprintf(dbg, sizeof(dbg),
                "[WINE] [5] RtlGetVersion: %lu.%lu build=%lu sp=%u productType=%u",
                (unsigned long)rtlVer.dwMajorVersion, (unsigned long)rtlVer.dwMinorVersion,
                (unsigned long)rtlVer.dwBuildNumber, rtlVer.wServicePackMajor, rtlVer.wProductType);
            WineLog(dbg);
        }
        else WineLog("[WINE] [5] RtlGetVersion not available");
    }

    // ── Method 6: Module file path — Wine uses Windows-style paths from Linux FS
    char modulePath[512] = {0};
    GetModuleFileNameA(nullptr, modulePath, sizeof(modulePath));
    snprintf(dbg, sizeof(dbg), "[WINE] [6] GetModuleFileNameA = \"%s\"", modulePath);
    WineLog(dbg);

    // ── Method 7: Z:\ drive root — Wine always maps Z:\ to Linux / ───────────
    DWORD zAttrib = GetFileAttributesA("Z:\\");
    snprintf(dbg, sizeof(dbg), "[WINE] [7] GetFileAttributes(\"Z:\\\\\") = 0x%08lX (%s)",
        (unsigned long)zAttrib,
        zAttrib == INVALID_FILE_ATTRIBUTES ? "INVALID (not present)" : "EXISTS");
    WineLog(dbg);
    if (zAttrib != INVALID_FILE_ATTRIBUTES) detected = true;

    // ── Method 8: Z:\proc\version — exists on Linux, never on Windows ────────
    DWORD procAttrib = GetFileAttributesA("Z:\\proc\\version");
    snprintf(dbg, sizeof(dbg), "[WINE] [8] GetFileAttributes(\"Z:\\\\proc\\\\version\") = 0x%08lX (%s)",
        (unsigned long)procAttrib,
        procAttrib == INVALID_FILE_ATTRIBUTES ? "INVALID (not present)" : "EXISTS");
    WineLog(dbg);
    if (procAttrib != INVALID_FILE_ATTRIBUTES) detected = true;

    // ── Method 9: D3D / DXGI renderer string ─────────────────────────────────
    // Wine/Proton typically reports the real Linux GPU via Mesa/RADV/DXVK.
    // We can query DXGI without creating a swap chain by using CreateDXGIFactory.
    // This is read-only and safe to call at load time.
    {
        typedef HRESULT(WINAPI* CreateDXGIFactoryFn)(REFIID, void**);
        HMODULE dxgi = LoadLibraryA("dxgi.dll");
        snprintf(dbg, sizeof(dbg), "[WINE] [9] dxgi.dll loaded: %s (%p)", dxgi ? "YES" : "NO", (void*)dxgi);
        WineLog(dbg);
        if (dxgi)
        {
            auto createFactory = (CreateDXGIFactoryFn)GetProcAddress(dxgi, "CreateDXGIFactory");
            if (createFactory)
            {
                // IDXGIFactory GUID: {7b7166ec-21c7-44ae-b21a-c9ae321ae369}
                static const GUID IID_IDXGIFactory_ = {
                    0x7b7166ec, 0x21c7, 0x44ae,
                    {0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69}
                };
                void* pFactory = nullptr;
                HRESULT hr = createFactory(IID_IDXGIFactory_, &pFactory);
                snprintf(dbg, sizeof(dbg), "[WINE] [9] CreateDXGIFactory hr=0x%08lX factory=%p",
                    (unsigned long)hr, pFactory);
                WineLog(dbg);

                if (SUCCEEDED(hr) && pFactory)
                {
                    // IDXGIFactory::EnumAdapters(0) → IDXGIAdapter → GetDesc → Description
                    // Use vtable index: EnumAdapters is slot 7 on IDXGIFactory
                    struct IDXGIFactoryVtbl { void* slots[8]; };
                    typedef HRESULT(WINAPI* EnumAdaptersFn)(void*, UINT, void**);
                    auto vtbl = *(IDXGIFactoryVtbl**)pFactory;
                    auto enumAdapters = (EnumAdaptersFn)vtbl->slots[7];
                    void* pAdapter = nullptr;
                    HRESULT hrA = enumAdapters(pFactory, 0, &pAdapter);
                    snprintf(dbg, sizeof(dbg), "[WINE] [9] EnumAdapters(0) hr=0x%08lX adapter=%p",
                        (unsigned long)hrA, pAdapter);
                    WineLog(dbg);

                    if (SUCCEEDED(hrA) && pAdapter)
                    {
                        // IDXGIAdapter::GetDesc is vtable slot 8
                        // DXGI_ADAPTER_DESC: Description[128] + 5 DWORDs + 3 SIZE_Ts + LUID
                        struct SimpleAdapterDesc {
                            WCHAR Description[128];
                            UINT VendorId, DeviceId, SubSysId, Revision;
                            SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
                        };
                        typedef HRESULT(WINAPI* GetDescFn)(void*, SimpleAdapterDesc*);
                        auto adapterVtbl = *(IDXGIFactoryVtbl**)pAdapter;
                        auto getDesc = (GetDescFn)adapterVtbl->slots[8];
                        SimpleAdapterDesc desc = {};
                        HRESULT hrD = getDesc(pAdapter, &desc);
                        if (SUCCEEDED(hrD))
                        {
                            char descUtf8[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                descUtf8, sizeof(descUtf8), nullptr, nullptr);
                            snprintf(dbg, sizeof(dbg),
                                "[WINE] [9] Adapter: \"%s\" VendorId=0x%04X DeviceId=0x%04X VRAM=%zuMB",
                                descUtf8, desc.VendorId, desc.DeviceId,
                                desc.DedicatedVideoMemory / (1024*1024));
                            WineLog(dbg);
                            // DXVK/VKD3D vendors: 0x1002=AMD, 0x10DE=NVIDIA, 0x8086=Intel
                            // Wine software renderer reports "Wine D3D" in description
                            if (strstr(descUtf8, "Wine") || strstr(descUtf8, "llvmpipe") ||
                                strstr(descUtf8, "DXVK")  || strstr(descUtf8, "softpipe"))
                            {
                                WineLog("[WINE] [9] Adapter description matches Wine/DXVK/software renderer");
                                detected = true;
                            }
                        }
                        // Release adapter (vtable slot 2 = Release on IUnknown)
                        typedef ULONG(WINAPI* ComReleaseFn)(void*);
                        auto releaseAdapter = (ComReleaseFn)adapterVtbl->slots[2];
                        releaseAdapter(pAdapter);
                    }
                    // Release factory (vtable slot 2 = IUnknown::Release)
                    typedef ULONG(WINAPI* ComReleaseFn)(void*);
                    auto releaseFactory = (ComReleaseFn)(*(void***)pFactory)[2];
                    releaseFactory(pFactory);
                }
            }
            FreeLibrary(dxgi);
        }
    }

    // ── Method 10: System directory path ──────────────────────────────────────
    // On Wine, GetSystemDirectoryA typically returns C:\windows\system32.
    // On real Windows it matches the actual install, usually also system32 but
    // the drive letter and casing may differ; not conclusive alone but useful context.
    {
        char sysDir[512] = {0};
        GetSystemDirectoryA(sysDir, sizeof(sysDir));
        snprintf(dbg, sizeof(dbg), "[WINE] [10] GetSystemDirectoryA = \"%s\"", sysDir);
        WineLog(dbg);

        char tempDir[512] = {0};
        GetTempPathA(sizeof(tempDir), tempDir);
        snprintf(dbg, sizeof(dbg), "[WINE] [10] GetTempPathA = \"%s\"", tempDir);
        WineLog(dbg);
    }

    // ── Method 11: Computer name / user name (Wine uses Linux hostname) ───────
    {
        char compName[256] = {0};
        DWORD compLen = sizeof(compName);
        GetComputerNameA(compName, &compLen);
        snprintf(dbg, sizeof(dbg), "[WINE] [11] ComputerName = \"%s\"", compName);
        WineLog(dbg);

        char userName[256] = {0};
        DWORD userLen = sizeof(userName);
        GetUserNameA(userName, &userLen);
        snprintf(dbg, sizeof(dbg), "[WINE] [11] UserName = \"%s\"", userName);
        WineLog(dbg);
    }

    snprintf(dbg, sizeof(dbg),
        "[WINE] ── DetectWine() result: %s ─────────────────────────",
        detected ? "TRUE (Wine/Proton)" : "FALSE (native Windows)");
    WineLog(dbg);
    return detected;
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
// Detects Malnourished (46587) and Diminished (46668) being applied — these
// are the debuffs arcdps fires when Nourishment/Enhancement wear off.
static bool g_FoodUtilityExpiryEnabled = false;

// TTS voice and output device selection
struct TtsDeviceEntry { std::string name; std::string tokenId; };
static std::vector<TtsDeviceEntry> g_TtsVoices;
static std::vector<TtsDeviceEntry> g_TtsOutputDevices;
static std::string g_TtsVoiceTokenId;   // persisted: full HKEY_... path; empty = system default
static std::string g_TtsOutputTokenId;  // persisted: full HKEY_... path; empty = system default
static int   g_TtsVoiceIdx   = 0;
static int   g_TtsOutputIdx  = 0;
static int   g_TtsVolume     = 100;    // SAPI volume 0-100
static int   g_TtsRate       = 0;      // SAPI rate -10 to 10

// Squad state tracking
static bool g_InSquad = false;

// Ready check detection (Unofficial Extras)
static bool g_ReadyCheckTtsEnabled = false;
static std::unordered_map<std::string, bool> g_SquadReadyStatus;

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

// Hardcoded render CDN URLs for common conditions and boons.
// These are extracted from skill facts and the wiki; the GW2 API
// does NOT expose a /v2/effects endpoint.
static const char* GetEffectIconUrl(unsigned int effectID)
{
    switch (effectID)
    {
        // ── Conditions ──
        case 736: return "https://render.guildwars2.com/file/79FF0046A5F9ADA3B4C4EC19ADB4CB124D5F0021/102848.png"; // Bleeding
        case 737: return "https://render.guildwars2.com/file/B47BF5803FED2718D7474EAF9617629AD068EE10/102849.png"; // Burning
        case 738: return "https://render.guildwars2.com/file/3A394C1A0A3257EB27A44842DDEEF0DF000E1241/102850.png"; // Vulnerability
        case 720: return "https://render.guildwars2.com/file/09770136BB76FD0DBE1CC4267DEED54774CB20F6/102837.png"; // Blinded
        case 721: return "https://render.guildwars2.com/file/070325E519C178D502A8160523766070D30C0C19/102838.png"; // Crippled
        case 722: return "https://render.guildwars2.com/file/28C4EC547A3516AF0242E826772DA43A5EAC3DF3/102839.png"; // Chilled
        case 723: return "https://render.guildwars2.com/file/559B0AF9FB5E1243D2649FAAE660CCB338AACC19/102840.png"; // Poisoned
        case 727: return "https://render.guildwars2.com/file/397A613651BFCA2832B6469CE34735580A2C120E/102844.png"; // Immobilized
        case 742: return "https://render.guildwars2.com/file/1781E2522B28272DA1C8B2DB1F4D0C32AD3CF781/102851.png"; // Weakness
        case 791: return "https://render.guildwars2.com/file/103314B67C76E34E257335B48F2FC68988CEFC69/102853.png"; // Fear
        case 861: return "https://render.guildwars2.com/file/546242E58F6E4E1DEB5B8205B0B24983E5A8F1DC/102847.png"; // Confusion
        case 19426: return "https://render.guildwars2.com/file/99B0E3B9C5A06E252A28680E98A36F6D82CD2FB6/102846.png"; // Torment
        case 26766: return "https://render.guildwars2.com/file/7CC00A3E50106EB0A0A57F0A56D32529A64C42B0/102845.png"; // Slow
        case 27705: return "https://render.guildwars2.com/file/6B1946FE00975D1E68A0D1A42C7A9B0B546CA896/102841.png"; // Taunt
        // ── Boons ──
        case 717: return "https://render.guildwars2.com/file/CD77D1FAB7B270223538A8F8ECDA1CFB044D65F4/102834.png"; // Protection
        case 718: return "https://render.guildwars2.com/file/F69996772B9E18FD18AD0AABAB25D7E3FC42F261/102835.png"; // Regeneration
        case 719: return "https://render.guildwars2.com/file/20CFC14967E67F7A3FD4A4B8722B4CF5B8565E11/102836.png"; // Swiftness
        case 725: return "https://render.guildwars2.com/file/96D90DF84CAFE008233DD1C2606A12C1A0E68048/102842.png"; // Fury
        case 726: return "https://render.guildwars2.com/file/58E92EBAF0DB4DA7C4AC04D9B22BCA5ECF0100DE/102843.png"; // Vigor
        case 740: return "https://render.guildwars2.com/file/2FA9DF9D6BC17839BBEA14723F1C53D645DDB5E1/102852.png"; // Might
        case 743: return "https://render.guildwars2.com/file/DFB4D1B50AE4D6A275B349E15B179261EE3EB0AF/102854.png"; // Aegis
        case 1122: return "https://render.guildwars2.com/file/04A84DAB1ADB575773DDA3E352A5B6A80D56FC7D/102855.png"; // Stability
        case 1187: return "https://render.guildwars2.com/file/3E3A1A8FDEBB3379D49270DC0F5725A63056FC56/102832.png"; // Quickness
        case 26980: return "https://render.guildwars2.com/file/50BAC1B8E10CFAB9E749A5D910D4A9DCF29EBB7C/961398.png"; // Resistance
        case 27794: return "https://render.guildwars2.com/file/D104A6B9344A2E2096424A3C300E46BC2926E4D7/2440718.png"; // Resolution
        case 30328: return "https://render.guildwars2.com/file/DCD34C28FF23D54D0EED9B5A47D6A06E3B9DC527/102833.png"; // Alacrity
        default: return nullptr;
    }
}

static void AddIcon(nlohmann::json& members, unsigned int id, const char* name, float x, float y, float size);
static void AddBackgroundBar(nlohmann::json& members, unsigned int bgId, float x, float y, float width, float height, bool intensity);
static nlohmann::json MakeBaseFilter();
static nlohmann::json MakeEmptyFilter();

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

static std::string GetReffectPacksDir()
{
    const char* reffectDir = g_API->Paths_GetAddonDirectory("reffect");
    if (reffectDir && reffectDir[0])
    {
        std::string path = reffectDir;
        if (path.back() != '\\' && path.back() != '/')
            path += '\\';
        path += "packs\\";
        return path;
    }

    HMODULE hMod = GetModuleHandleA("GW2Accessibility.dll");
    if (!hMod) return "";

    char ourPath[MAX_PATH];
    DWORD len = GetModuleFileNameA(hMod, ourPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";

    std::string path(ourPath);
    size_t pos = path.find("\\addons\\GW2Accessibility\\");
    if (pos == std::string::npos) return "";

    return path.substr(0, pos) + "\\addons\\reffect\\packs\\";
}

// Read packVersion from a Reffect JSON file. Returns 0 if absent or unparseable.
static int ReadPackVersion(const std::string& filepath)
{
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) return 0;
    try
    {
        nlohmann::json j;
        f >> j;
        return j.value("packVersion", 0);
    }
    catch (...)
    {
        return 0;
    }
}

// Get packVersion from the embedded JSON.
static int GetEmbeddedPackVersion()
{
    try
    {
        auto j = nlohmann::json::parse(g_ReffectPackJson);
        return j.value("packVersion", 0);
    }
    catch (...)
    {
        return 0;
    }
}

static void DeployReffectPack()
{
    std::string dir = GetReffectPacksDir();
    if (dir.empty()) return;

    std::filesystem::create_directories(dir);
    std::string filepath = dir + "GW2Accessibility.json";

    int embeddedVer = GetEmbeddedPackVersion();
    char buf[256];

    if (std::filesystem::exists(filepath))
    {
        int onDiskVer = ReadPackVersion(filepath);
        if (onDiskVer >= embeddedVer)
        {
            snprintf(buf, sizeof(buf), "[REFFECT] Pack up-to-date (version %d), skipping deploy", onDiskVer);
            g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
            return;
        }
        snprintf(buf, sizeof(buf), "[REFFECT] Upgrading pack %d -> %d", onDiskVer, embeddedVer);
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }

    std::ofstream file(filepath, std::ios::binary);
    if (file.is_open())
    {
        file << g_ReffectPackJson;
        snprintf(buf, sizeof(buf), "[REFFECT] Deployed pack v%d to %s", embeddedVer, filepath.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }
}

static void AddIcon(nlohmann::json& members, unsigned int id, const char* name, float x, float y, float size)
{
    nlohmann::json icon;
    icon["enabled"] = true;
    icon["name"] = name;
    icon["anchor"] = "Screen";
    icon["pos"] = {x, y};
    icon["opacity"] = 1.0;

    nlohmann::json trigger;
    trigger["source"]["type"] = "Buff";
    trigger["source"]["combatant"] = "Player";
    trigger["source"]["ids"] = nlohmann::json::array({id});
    trigger["threshold"]["threshold_type"] = "Present";
    trigger["threshold"]["amount_type"] = "Intensity";
    icon["trigger"] = trigger;

    nlohmann::json filter;
    filter["player"]["combat"] = nullptr;
    filter["player"]["weapons"] = nlohmann::json::array();
    filter["player"]["weapon_mode"] = "Any";
    filter["player"]["sigils"] = nlohmann::json::array();
    filter["player"]["sigil_mode"] = "Any";
    filter["player"]["relics"] = nlohmann::json::array();
    filter["player"]["traits"] = nlohmann::json::array();
    filter["player"]["trait_mode"] = "All";
    filter["player"]["specs"] = nlohmann::json::array();
    filter["player"]["skill_selections"] = nlohmann::json::array();
    filter["player"]["skill_selections_mode"] = "All";
    filter["player"]["prof_selections"] = nlohmann::json::array();
    filter["player"]["mounts"] = nlohmann::json::array();
    filter["map"]["category"] = nlohmann::json::array();
    filter["map"]["whitelist"] = true;
    filter["map"]["ids"] = nlohmann::json::array();
    icon["filter"] = filter;

    icon["animation"] = nullptr;
    icon["type"] = "Icon";
    icon["icon"] = "Automatic";
    icon["tint"] = {1.0, 1.0, 1.0, 1.0};
    icon["zoom"] = 1.0;
    icon["round"] = 0.0;
    icon["border_size"] = 0.0;
    icon["border_color"] = {0.0, 0.0, 0.0, 1.0};
    icon["conditions"] = nlohmann::json::array();
    icon["duration_bar"] = true;
    icon["duration_text"] = true;
    icon["stacks_text"] = false;
    icon["size"] = {size, size};

    members.push_back(icon);
}

static void AddBackgroundBar(nlohmann::json& members, unsigned int bgId, float x, float y, float width, float height, bool intensity)
{
    nlohmann::json bar;
    bar["enabled"] = true;
    bar["name"] = "Background";
    bar["anchor"] = "Screen";
    bar["pos"] = {x, y};
    bar["opacity"] = 1.0;

    nlohmann::json trigger;
    if (bgId != 0)
    {
        trigger["source"]["type"] = "Buff";
        trigger["source"]["combatant"] = "Player";
        trigger["source"]["ids"] = nlohmann::json::array({bgId});
        trigger["threshold"]["threshold_type"] = "Present";
        trigger["threshold"]["amount_type"] = "Intensity";
    }
    else
    {
        trigger["source"]["type"] = "Inherit";
        trigger["threshold"]["threshold_type"] = "Always";
        trigger["threshold"]["amount_type"] = "Intensity";
    }
    bar["trigger"] = trigger;

    nlohmann::json filter;
    filter["player"]["combat"] = nullptr;
    filter["player"]["weapons"] = nlohmann::json::array();
    filter["player"]["weapon_mode"] = "Any";
    filter["player"]["sigils"] = nlohmann::json::array();
    filter["player"]["sigil_mode"] = "Any";
    filter["player"]["relics"] = nlohmann::json::array();
    filter["player"]["traits"] = nlohmann::json::array();
    filter["player"]["trait_mode"] = "All";
    filter["player"]["specs"] = nlohmann::json::array();
    filter["player"]["skill_selections"] = nlohmann::json::array();
    filter["player"]["skill_selections_mode"] = "All";
    filter["player"]["prof_selections"] = nlohmann::json::array();
    filter["player"]["mounts"] = nlohmann::json::array();
    filter["map"]["category"] = nlohmann::json::array();
    filter["map"]["whitelist"] = true;
    filter["map"]["ids"] = nlohmann::json::array();
    bar["filter"] = filter;

    bar["animation"] = nullptr;
    bar["type"] = "Bar";
    bar["progress_kind"] = intensity ? "Intensity" : "Duration";
    bar["max"] = 25.0;
    bar["lower_bound"] = 0.0;
    bar["upper_bound"] = intensity ? 0.01 : 1.0;
    bar["fill"] = {9.9999e-7, 0.000001, 9.9999e-7, 0.9378531f};
    bar["background"] = {0.0, 0.0, 0.0, 0.0};
    bar["border_size"] = 1.0;
    bar["border_color"] = {0.0, 0.0, 0.0, 1.0};
    bar["tick_size"] = 1.0;
    bar["tick_color"] = {0.0, 0.0, 0.0, 1.0};
    bar["conditions"] = nlohmann::json::array();
    bar["size"] = {width, height};
    bar["align"] = "Center";
    bar["direction"] = "Right";
    bar["tick_unit"] = "Percent";
    bar["ticks"] = nlohmann::json::array();

    members.push_back(bar);
}

static nlohmann::json MakeBaseFilter()
{
    nlohmann::json filter;
    filter["player"]["combat"] = nullptr;
    filter["player"]["weapons"] = nlohmann::json::array();
    filter["player"]["weapon_mode"] = "Any";
    filter["player"]["sigils"] = nlohmann::json::array();
    filter["player"]["sigil_mode"] = "Any";
    filter["player"]["relics"] = nlohmann::json::array();
    filter["player"]["traits"] = nlohmann::json::array();
    filter["player"]["trait_mode"] = "All";
    filter["player"]["specs"] = nlohmann::json::array();
    filter["player"]["skill_selections"] = nlohmann::json::array();
    filter["player"]["skill_selections_mode"] = "All";
    filter["player"]["prof_selections"] = nlohmann::json::array();
    filter["player"]["mounts"] = nlohmann::json::array();
    filter["map"]["category"] = nlohmann::json::array({"PvE", "Instance", "Other"});
    filter["map"]["whitelist"] = true;
    filter["map"]["ids"] = nlohmann::json::array();
    return filter;
}

static nlohmann::json MakeEmptyFilter()
{
    nlohmann::json filter;
    filter["player"]["combat"] = nullptr;
    filter["player"]["weapons"] = nlohmann::json::array();
    filter["player"]["weapon_mode"] = "Any";
    filter["player"]["sigils"] = nlohmann::json::array();
    filter["player"]["sigil_mode"] = "Any";
    filter["player"]["relics"] = nlohmann::json::array();
    filter["player"]["traits"] = nlohmann::json::array();
    filter["player"]["trait_mode"] = "All";
    filter["player"]["specs"] = nlohmann::json::array();
    filter["player"]["skill_selections"] = nlohmann::json::array();
    filter["player"]["skill_selections_mode"] = "All";
    filter["player"]["prof_selections"] = nlohmann::json::array();
    filter["player"]["mounts"] = nlohmann::json::array();
    filter["map"]["category"] = nlohmann::json::array();
    filter["map"]["whitelist"] = true;
    filter["map"]["ids"] = nlohmann::json::array();
    return filter;
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

// ── Keybind XML Parser ───────────────────────────────────────────────────────

// Resolves a Wine drive letter (e.g. 'c', 's', 'x') to a Unix path
// using the dosdevices symlinks in the prefix
// Resolves a Wine drive letter using the dosdevices symlink in the prefix.
// On MinGW we parse the symlink target file directly since readlink is unavailable.
static std::string ResolveWineDrive(char drive, const std::string& remaining, const std::string& prefix)
{
    char dosDevice[512];
    snprintf(dosDevice, sizeof(dosDevice), "%s/dosdevices/%c:", prefix.c_str(), drive);

    // Read symlink target by opening as text file (MinGW provides this via cygwin compat)
    std::ifstream linkFile(dosDevice);
    if (!linkFile.is_open()) return "";

    char linkTarget[512] = {0};
    linkFile.getline(linkTarget, sizeof(linkTarget));
    linkFile.close();

    // Trim any trailing whitespace / carriage return
    size_t len = strlen(linkTarget);
    while (len > 0 && (linkTarget[len-1] == '\r' || linkTarget[len-1] == '\n' || linkTarget[len-1] == ' '))
    {
        linkTarget[--len] = '\0';
    }
    if (len == 0) return "";

    std::string target(linkTarget);
    std::string rem = remaining;
    while (!rem.empty() && rem[0] == '/') rem.erase(rem.begin());

    if (!target.empty() && target[0] == '/')
    {
        // Absolute path
        return target + "/" + rem;
    }
    else
    {
        // Relative path — resolve from prefix/dosdevices
        std::string base = prefix + "/dosdevices";
        std::string resolved = base + "/" + target;
        std::string result;
        std::vector<std::string> parts;
        size_t start = 0;
        while (start < resolved.size())
        {
            size_t next = resolved.find('/', start);
            std::string comp = (next == std::string::npos) ? resolved.substr(start) : resolved.substr(start, next - start);
            if (comp == "..")
            {
                if (!parts.empty()) parts.pop_back();
            }
            else if (!comp.empty() && comp != ".")
            {
                parts.push_back(comp);
            }
            if (next == std::string::npos) break;
            start = next + 1;
        }
        for (auto& part : parts) result += "/" + part;
        if (result.empty()) result = "/";
        return result + "/" + rem;
    }
}

// Converts a Wine-style path (e.g. "C:\Users\...\Documents") to a Unix path
// Z: on this system maps to /home/<user> (desktop integration)
static std::string WinePathToUnix(const char* winePath)
{
    if (!winePath) return "";

    std::string p = winePath;

    // Normalize slashes first
    for (size_t i = 0; i < p.size(); i++)
        if (p[i] == '\\') p[i] = '/';

    // Check for drive letter (e.g. C: or Z:)
    if (p.size() >= 2 && p[1] == ':')
    {
        char drive = std::tolower(p[0]);
        char home[256] = {0};
        GetEnvironmentVariableA("HOME", home, sizeof(home));
        char buf[512];

        if (drive == 'z')
        {
            // Z: maps to / on this Wine setup
            snprintf(buf, sizeof(buf), "%s", home);
            p = std::string(buf) + p.substr(2);
        }
        else if (drive == 'c')
        {
            // C: — check for Users/<user> pattern and use WINEPREFIX/drive_c mapping
            char winePrefix[512] = {0};
            GetEnvironmentVariableA("WINEPREFIX", winePrefix, sizeof(winePrefix));
            std::string remaining = p.substr(2);

            // Derive prefix from our own DLL path (pfx is in the path)
            char ourDll[MAX_PATH] = {0};
            HMODULE hMod = GetModuleHandleA("GW2Accessibility.dll");
            if (hMod) GetModuleFileNameA(hMod, ourDll, MAX_PATH);
            std::string ourPathStr(ourDll);
            std::string detectedPfx;
            size_t pfxPos = ourPathStr.find("/pfx/");
            if (pfxPos != std::string::npos)
                detectedPfx = ourPathStr.substr(0, pfxPos + 4);

            // Strip leading '/' from remaining to avoid double-slash on concatenation
            std::string remStripped = remaining;
            while (!remStripped.empty() && remStripped[0] == '/') remStripped.erase(remStripped.begin());

            // /Users/<user> or \Users\<user> after drive letter
            bool isUsersPath = false;
            std::string check = remaining;
            if (check.size() >= 7 && std::tolower(check[0]) == 'u')
            {
                std::string lower = check;
                for (size_t i = 0; i < lower.size(); i++) lower[i] = std::tolower(lower[i]);
                if (lower.compare(0, 7, "users/") == 0 || lower.compare(0, 8, "\\users\\") == 0)
                    isUsersPath = true;
            }

            if (isUsersPath)
            {
                if (winePrefix[0])
                    snprintf(buf, sizeof(buf), "%s/drive_c", winePrefix);
                else if (!detectedPfx.empty())
                    snprintf(buf, sizeof(buf), "%s/drive_c", detectedPfx.c_str());
                else
                {
                    const char* candidates[] = {
                        "/home/todd/Documents/Games/guild-wars-2/pfx",
                        "/home/todd/Documents/Games/guild-wars-2",
                        "/home/todd/.wine"
                    };
                    for (auto& c : candidates)
                    {
                        if (std::filesystem::exists(std::string(c) + "/drive_c"))
                        {
                            snprintf(buf, sizeof(buf), "%s/drive_c", c);
                            break;
                        }
                    }
                }
                p = std::string(buf) + "/" + remStripped;
            }
            else
            {
                // Other C: paths — first try dosdevices/c: symlink
                if (!detectedPfx.empty())
                {
                    std::string resolved = ResolveWineDrive('c', remaining, detectedPfx);
                    if (!resolved.empty() && std::filesystem::exists(resolved.substr(0, resolved.find("/", 10))))
                    {
                        p = resolved;
                    }
                    else
                    {
                        // Fallback to prefix/drive_c
                        if (winePrefix[0])
                            snprintf(buf, sizeof(buf), "%s/drive_c", winePrefix);
                        else if (!detectedPfx.empty())
                            snprintf(buf, sizeof(buf), "%s/drive_c", detectedPfx.c_str());
                        else
                        {
                            const char* candidates[] = {
                                "/home/todd/Documents/Games/guild-wars-2/pfx",
                                "/home/todd/Documents/Games/guild-wars-2",
                                "/home/todd/.wine"
                            };
                            for (auto& c : candidates)
                            {
                                if (std::filesystem::exists(std::string(c) + "/drive_c"))
                                {
                                    snprintf(buf, sizeof(buf), "%s/drive_c", c);
                                    break;
                                }
                            }
                        }
                        p = std::string(buf) + "/" + remStripped;
                    }
                }
                else
                {
                    if (winePrefix[0])
                        snprintf(buf, sizeof(buf), "%s/drive_c", winePrefix);
                    else
                        snprintf(buf, sizeof(buf), "%s/.wine/drive_c", home);
                    p = std::string(buf) + "/" + remStripped;
                }
            }
        }
        else
        {
            // Other drives (S:, X:, etc.) — try dosdevices symlinks first
            char ourDll[MAX_PATH] = {0};
            HMODULE hMod = GetModuleHandleA("GW2Accessibility.dll");
            if (hMod) GetModuleFileNameA(hMod, ourDll, MAX_PATH);
            std::string ourPathStr(ourDll);
            std::string detectedPfx;
            size_t pfxPos = ourPathStr.find("/pfx/");
            if (pfxPos != std::string::npos)
                detectedPfx = ourPathStr.substr(0, pfxPos + 4);

            if (!detectedPfx.empty())
            {
                std::string remaining = p.substr(2);
                std::string resolved = ResolveWineDrive(drive, remaining, detectedPfx);
                if (!resolved.empty())
                {
                    p = resolved;
                }
                else
                {
                    // Fallback: WINEPREFIX env var or home/.wine
                    char winePrefix[512] = {0};
                    GetEnvironmentVariableA("WINEPREFIX", winePrefix, sizeof(winePrefix));
                    if (winePrefix[0])
                        snprintf(buf, sizeof(buf), "%s/drive_%c", winePrefix, drive);
                    else
                        snprintf(buf, sizeof(buf), "%s/.wine/drive_%c", home, drive);
                    p = std::string(buf) + remaining;
                }
            }
            else
            {
                char winePrefix[512] = {0};
                GetEnvironmentVariableA("WINEPREFIX", winePrefix, sizeof(winePrefix));
                if (winePrefix[0])
                    snprintf(buf, sizeof(buf), "%s/drive_%c", winePrefix, drive);
                else
                    snprintf(buf, sizeof(buf), "%s/.wine/drive_%c", home, drive);
                p = std::string(buf) + p.substr(2);
            }
        }
    }

    return p;
}

static void ScanKeybindFiles()
{
    g_KeybindFiles.clear();
    g_KeybindFileIndex = -1;

    char dbg[512];
    snprintf(dbg, sizeof(dbg), "[KEYBINDS] ScanKeybindFiles start (IsWine=%d)", g_IsWine ? 1 : 0);
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

    // On Wine, convert the Windows user docs path to Unix via WinePathToUnix
    if (g_IsWine)
    {
        const char* winePaths[] = {
            "C:\\users\\steamuser\\Documents\\Guild Wars 2\\InputBinds\\",
            "C:\\Users\\steamuser\\Documents\\Guild Wars 2\\InputBinds\\",
        };

        for (const char* winPath : winePaths)
        {
            std::string unixPath = WinePathToUnix(winPath);
            if (unixPath.empty()) continue;

            snprintf(dbg, sizeof(dbg), "[KEYBINDS] Trying Wine path: %s (from %s)", unixPath.c_str(), winPath);
            g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

            if (std::filesystem::exists(unixPath))
            {
                g_KeybindsFromRealDocs = true;
                for (const auto& entry : std::filesystem::directory_iterator(unixPath))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".xml")
                    {
                        g_KeybindFiles.push_back(entry.path().filename().string());
                    }
                }
                if (!g_KeybindFiles.empty())
                {
                    g_KeybindFileIndex = 0;
                    g_KeybindBaseDir = unixPath;
                    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Found %zu files via Wine path", g_KeybindFiles.size());
                    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
                    return;
                }
            }
            else
            {
                snprintf(dbg, sizeof(dbg), "[KEYBINDS] Path does not exist: %s", unixPath.c_str());
                g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
            }
        }

        // Also try HOME-based path if HOME is set
        char home[256] = {0};
        GetEnvironmentVariableA("HOME", home, sizeof(home));
        if (home[0] != '\0')
        {
            char realPath[512];
            snprintf(realPath, sizeof(realPath), "%s/Documents/Guild Wars 2/InputBinds/", home);
            snprintf(dbg, sizeof(dbg), "[KEYBINDS] Trying HOME docs path: %s", realPath);
            g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

            if (std::filesystem::exists(realPath))
            {
                g_KeybindsFromRealDocs = true;
                for (const auto& entry : std::filesystem::directory_iterator(realPath))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".xml")
                    {
                        g_KeybindFiles.push_back(entry.path().filename().string());
                    }
                }
                if (!g_KeybindFiles.empty())
                {
                    g_KeybindFileIndex = 0;
                    g_KeybindBaseDir = std::string(realPath);
                    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Found %zu files via HOME path", g_KeybindFiles.size());
                    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
                    return;
                }
            }
        }
    }

    // On native Windows, try SHGetKnownFolderPath(FOLDERID_Documents) before the Nexus
    // common-directory fallback. This API resolves the real Documents folder even when
    // OneDrive folder redirection moves it to %USERPROFILE%\OneDrive\Documents\.
    if (!g_IsWine)
    {
        PWSTR pszPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &pszPath)))
        {
            char narrowPath[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0, pszPath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr);
            CoTaskMemFree(pszPath);

            std::string basePath = narrowPath;
            if (!basePath.empty() && basePath.back() != '\\')
                basePath += '\\';
            basePath += "Guild Wars 2\\InputBinds\\";

            snprintf(dbg, sizeof(dbg), "[KEYBINDS] Trying FOLDERID_Documents path: %s", basePath.c_str());
            g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

            if (std::filesystem::exists(basePath))
            {
                for (const auto& entry : std::filesystem::directory_iterator(basePath))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".xml")
                        g_KeybindFiles.push_back(entry.path().filename().string());
                }
                if (!g_KeybindFiles.empty())
                {
                    g_KeybindFileIndex = 0;
                    g_KeybindBaseDir = basePath;
                    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Found %zu files via FOLDERID_Documents", g_KeybindFiles.size());
                    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
                    return;
                }
            }
            else
            {
                snprintf(dbg, sizeof(dbg), "[KEYBINDS] FOLDERID_Documents path does not exist: %s", basePath.c_str());
                g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
            }
        }
    }

    // Fall back to standard common directory path
    const char* commonDir = g_API->Paths_GetCommonDirectory();
    if (!commonDir)
    {
        g_API->Log(LOGL_INFO, "GW2Accessibility", "[KEYBINDS] Paths_GetCommonDirectory returned null");
        return;
    }

    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Paths_GetCommonDirectory: %s", commonDir);
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

    std::string basePath;
    if (g_IsWine)
    {
        basePath = WinePathToUnix(commonDir);
        if (!basePath.empty() && basePath.back() != '/')
            basePath += '/';
        basePath += "Guild Wars 2/InputBinds/";
    }
    else
    {
        basePath = commonDir;
        if (!basePath.empty() && basePath.back() != '\\' && basePath.back() != '/')
            basePath += '\\';
        basePath += "Guild Wars 2\\InputBinds\\";
    }

    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Trying common dir path: %s", basePath.c_str());
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

    if (!std::filesystem::exists(basePath))
    {
        snprintf(dbg, sizeof(dbg), "[KEYBINDS] Path does not exist: %s", basePath.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(basePath))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xml")
        {
            g_KeybindFiles.push_back(entry.path().filename().string());
        }
    }

    if (!g_KeybindFiles.empty())
    {
        g_KeybindFileIndex = 0;
        g_KeybindBaseDir = basePath;
    }

    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Scanned %zu files from %s", g_KeybindFiles.size(), basePath.c_str());
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
}

// GW2 uses its own button encoding — not ASCII, not Windows VK codes.
// Confirmed from user keybind data:
//   Codes  1-31  : custom codes for special/symbol keys
//   Codes 32-43  : F1-F12 (override what would be ASCII punctuation)
//   Codes 44-90  : mostly match ASCII uppercase/symbols (A-Z, digits, symbols)
//   Code  91     : Numpad + (overrides ASCII '[')
//   Codes 96-104 : Numpad 1-9 (override ASCII backtick/lowercase letters)
static const char* Gw2ButtonToKeyName(unsigned int button)
{
    switch (button)
    {
        // Mouse buttons
        case 1:   return "LMB";
        case 2:   return "RMB";

        // Symbol keys (GW2 codes, confirmed)
        case 3:   return "'";
        case 4:   return "\\";
        case 6:   return ",";
        case 10:  return "[";
        case 12:  return ".";
        case 13:  return "]";
        case 14:  return ";";
        case 15:  return "/";

        // Other special keys (codes < 32)
        case 9:   return "Tab";
        case 17:  return "Caps Lock";
        case 18:  return "Backspace";
        case 20:  return "Enter";
        case 22:  return "Tab";
        case 27:  return "Escape";

        // F1–F12 at codes 32–43 (GW2 encoding; these override ASCII punctuation)
        case 32:  return "F1";
        case 33:  return "F2";
        case 34:  return "F3";
        case 35:  return "F4";
        case 36:  return "F5";
        case 37:  return "F6";
        case 38:  return "F7";
        case 39:  return "F8";
        case 40:  return "F9";
        case 41:  return "F10";
        case 42:  return "F11";
        case 43:  return "F12";

        // Numpad keys (GW2 encoding; these override ASCII in the same range)
        case 91:  return "Num+";
        case 96:  return "F1";
        case 97:  return "F2";
        case 98:  return "F3";
        case 99:  return "F4";
        case 100: return "F5";
        case 101: return "F6";
        case 102: return "F7";
        case 103: return "F8";
        case 104: return "F9";

        default: break;
    }

    // Remaining printable range: letter keys (A-Z = 65-90) and other symbols
    // use their ASCII code directly. Skip the ranges overridden above (32-43, 91, 96-104).
    if ((button >= 44 && button <= 90) ||
        (button >= 92 && button <= 95) ||
        (button >= 105 && button <= 126))
    {
        static char buf[2];
        buf[0] = static_cast<char>(button);
        buf[1] = '\0';
        return buf;
    }

    return "?";
}

// mod bitmask: bit0=Shift(1), bit1=Ctrl(2), bit2=Alt(4); combinations e.g. 3=Ctrl+Shift
static std::string FormatBind(unsigned int button, unsigned int mod)
{
    std::string s;
    if (mod & 2) s += "Ctrl+";
    if (mod & 1) s += "Shift+";
    if (mod & 4) s += "Alt+";
    s += Gw2ButtonToKeyName(button);
    return s;
}

static std::string CategorizeAction(const std::string& n)
{
    const auto np = std::string::npos;

    // ── Movement ─────────────────────────────────────────────────────────────
    if (n == "Move Forward"   || n == "Move Backward" ||
        n == "Strafe Left"    || n == "Strafe Right"  ||
        n == "Turn Left"      || n == "Turn Right"    ||
        n == "Dodge"          || n == "Autorun"       || n == "Walk" ||
        n == "About Face"     ||
        n.find("Jump")   != np || n.find("Swim") != np ||
        n.find("Fly Up") != np || n.find("Fly Down") != np)
        return "Movement";

    // ── Skills ───────────────────────────────────────────────────────────────
    if (n == "Swap Weapons"        || n == "Stow/Draw Weapons"   ||
        n == "Healing Skill"       || n == "Elite Skill"          ||
        n == "Special Action"      || n == "Activate Mastery Skill" ||
        n.find("Weapon Skill")    != np ||
        n.find("Utility Skill")   != np ||
        n.find("Profession Skill") != np ||
        n.find("Ranger Pet")      != np)
        return "Skills";

    // ── Targeting ────────────────────────────────────────────────────────────
    if (n == "Alert Target"         || n == "Call Target"           ||
        n == "Take Target"          || n == "Set Personal Target"   ||
        n == "Take Personal Target" ||
        n == "Nearest Enemy"        || n == "Next Enemy"            ||
        n == "Previous Enemy"       ||
        n == "Nearest Ally"         || n == "Next Ally"             ||
        n == "Previous Ally"        ||
        n.find("Autotarget")       != np ||
        n.find("Targeting")        != np ||
        n.find("Snap Ground Target") != np)
        return "Targeting";

    // ── Templates ────────────────────────────────────────────────────────────
    if (n.find("Template") != np)
        return "Templates";

    // ── Mounts ───────────────────────────────────────────────────────────────
    if (n.find("Mount")        != np || n == "Dismount"       ||
        n.find("Springer")     != np || n.find("Raptor")      != np ||
        n.find("Skimmer")      != np || n.find("Jackal")      != np ||
        n.find("Roller Beetle") != np || n.find("Warclaw")    != np ||
        n.find("Griffon")      != np || n.find("Siege Turtle") != np ||
        n.find("Skyscale")     != np)
        return "Mounts";

    // ── Squad (markers + broadcasts + spectators) ─────────────────────────────
    if (n == "Arrow"   || n == "Circle" || n == "Heart"  ||
        n == "Square"  || n == "Star"   || n == "Spiral" ||
        n == "Triangle" || n == "X"     ||
        n.find("Object Marker")   != np ||
        n.find("Location Marker") != np ||
        n.find("Spectator")       != np ||
        n.find("Squad")           != np)
        return "Squad";

    // ── Camera ───────────────────────────────────────────────────────────────
    if (n.find("Action Camera") != np ||
        n == "Free Camera"       ||
        n == "Zoom In"           || n == "Zoom Out")
        return "Camera";

    // ── Screenshot ───────────────────────────────────────────────────────────
    if (n == "Stereoscopic" || n.find("Screenshot") != np)
        return "Screenshot";

    // ── Map ──────────────────────────────────────────────────────────────────
    if (n == "Open/Close"        ||
        n.find("Recenter") != np ||
        n.find("Floor")    != np ||
        n == "Scan for Rift"     ||
        n == "Zoom In"           || n == "Zoom Out")
        return "Map";

    // ── UI (panels, dialogs, chat, show/hide, toggles) ────────────────────────
    if (n.find("Dialog")    != np || n.find("Dialogue")   != np ||
        n.find("Show/Hide") != np || n.find("Chat")       != np ||
        n == "Scoreboard"        || n == "Log Out"         ||
        n.find("Options")  != np || n.find("Information") != np ||
        n.find("Vault")    != np ||
        n == "Toggle Full Screen" || n == "Toggle Language" ||
        n.find("Show Enemy Names") != np ||
        n.find("Show Ally Names")  != np ||
        n.find("Conjured Doorway") != np)
        return "UI";

    return "General";
}

static bool ParseKeybindXml(const std::string& xmlContent, std::vector<KeybindEntry>& out)
{
    out.clear();

    const char* xml = xmlContent.c_str();
    const char* actionTag = "<action name=\"";
    size_t pos = 0;

    while (true)
    {
        const char* actionStart = strstr(xml + pos, actionTag);
        if (!actionStart) break;

        size_t nameStart = (actionStart - xml) + strlen(actionTag);
        const char* nameEnd = strchr(xml + nameStart, '"');
        if (!nameEnd) break;

        std::string actionName(xml + nameStart, nameEnd - (xml + nameStart));

        // Bound all searches to the current action tag to prevent cross-action bleed
        const char* tagEnd = strstr(nameEnd, "/>");
        if (!tagEnd) { pos = nameStart; continue; }

        // Primary binding: device="Keyboard" button="N" mod="M"
        bool hasPrimary = false;
        unsigned int button = 0, mod = 0;
        const char* deviceStr = strstr(nameEnd, "device=\"Keyboard\"");
        if (deviceStr && deviceStr < tagEnd)
        {
            const char* buttonStr = strstr(deviceStr, "button=\"");
            if (buttonStr && buttonStr < tagEnd)
            {
                button = static_cast<unsigned int>(strtoul(buttonStr + 8, nullptr, 10));
                const char* modStr = strstr(buttonStr + 8, " mod=\"");
                if (modStr && modStr < tagEnd)
                    mod = static_cast<unsigned int>(strtoul(modStr + 6, nullptr, 10));
                hasPrimary = true;
            }
        }

        // Secondary binding: device2="Keyboard" button2="N" mod2="M"
        bool hasSecondary = false;
        unsigned int button2 = 0, mod2 = 0;
        const char* device2Str = strstr(nameEnd, "device2=\"Keyboard\"");
        if (device2Str && device2Str < tagEnd)
        {
            const char* button2Str = strstr(device2Str, "button2=\"");
            if (button2Str && button2Str < tagEnd)
            {
                button2 = static_cast<unsigned int>(strtoul(button2Str + 9, nullptr, 10));
                const char* mod2Str = strstr(button2Str + 9, " mod2=\"");
                if (mod2Str && mod2Str < tagEnd)
                    mod2 = static_cast<unsigned int>(strtoul(mod2Str + 7, nullptr, 10));
                hasSecondary = true;
            }
        }

        // Skip actions with no keyboard binding at all
        if (!hasPrimary && !hasSecondary)
        {
            pos = nameStart;
            continue;
        }

        KeybindEntry e;
        e.category = CategorizeAction(actionName);
        e.name     = actionName;
        if (hasPrimary)
        {
            e.rawButton = button;
            e.rawMod    = mod;
            e.key       = FormatBind(button, mod);
        }
        if (hasSecondary)
        {
            e.rawButton2 = button2;
            e.rawMod2    = mod2;
            e.key2       = FormatBind(button2, mod2);
        }

        out.push_back(e);

        pos = nameStart;
    }

    return !out.empty();
}

static void LoadKeybindFile(int index)
{
    if (index < 0 || index >= static_cast<int>(g_KeybindFiles.size())) return;

    std::string xmlPath;

    if (!g_ManualKeybindPath.empty() && index == g_KeybindFileIndex)
    {
        xmlPath = g_ManualKeybindPath;
    }
    else if (!g_KeybindBaseDir.empty())
    {
        xmlPath = g_KeybindBaseDir + g_KeybindFiles[index];
    }
    else
    {
        // Fallback: reconstruct from API common directory (path style matches Wine/native)
        const char* commonDir = g_API->Paths_GetCommonDirectory();
        if (!commonDir) return;

        if (g_IsWine)
        {
            std::string basePath = WinePathToUnix(commonDir);
            if (!basePath.empty() && basePath.back() != '/')
                basePath += '/';
            xmlPath = basePath + "Guild Wars 2/InputBinds/" + g_KeybindFiles[index];
        }
        else
        {
            std::string basePath = commonDir;
            if (!basePath.empty() && basePath.back() != '\\' && basePath.back() != '/')
                basePath += '\\';
            xmlPath = basePath + "Guild Wars 2\\InputBinds\\" + g_KeybindFiles[index];
        }
    }

    std::ifstream file(xmlPath);
    if (!file.is_open())
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[KEYBINDS] Failed to open: %s", xmlPath.c_str());
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
        return;
    }

    std::string xmlContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    char dbg[256];
    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Parsing %zu bytes from %s", xmlContent.size(), g_KeybindFiles[index].c_str());
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);

    ParseKeybindXml(xmlContent, g_Keybinds);

    g_KeybindsLoaded = true;
    snprintf(dbg, sizeof(dbg), "[KEYBINDS] Loaded %zu bindings from %s", g_Keybinds.size(), g_KeybindFiles[index].c_str());
    g_API->Log(LOGL_INFO, "GW2Accessibility", dbg);
}

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
    j["keybindOverlayEnabled"] = g_KeybindOverlayEnabled;
    j["keybindVisibility"] = g_KeybindVisibility;
    j["keybindShowMovement"]   = g_KeybindShowMovement;
    j["keybindShowSkills"]     = g_KeybindShowSkills;
    j["keybindShowTargeting"]  = g_KeybindShowTargeting;
    j["keybindShowMounts"]     = g_KeybindShowMounts;
    j["keybindShowSquad"]      = g_KeybindShowSquad;
    j["keybindShowCamera"]     = g_KeybindShowCamera;
    j["keybindShowScreenshot"] = g_KeybindShowScreenshot;
    j["keybindShowMap"]        = g_KeybindShowMap;
    j["keybindShowUI"]         = g_KeybindShowUI;
    j["keybindShowTemplates"]  = g_KeybindShowTemplates;
    j["keybindPositionAnchor"] = static_cast<int>(g_KeybindPosition.anchor);
    j["keybindPositionX"] = g_KeybindPosition.offsetXPct;
    j["keybindPositionY"] = g_KeybindPosition.offsetYPct;
    j["keybindOpacity"] = g_KeybindOverlayOpacity;
    j["keybindFileIndex"] = g_KeybindFileIndex;
    j["keybindFile"] = (g_KeybindFileIndex >= 0 && g_KeybindFileIndex < static_cast<int>(g_KeybindFiles.size())) ? g_KeybindFiles[g_KeybindFileIndex] : "";
    j["manualKeybindPath"] = g_ManualKeybindPath;
    j["iconDisplayEnabled"] = g_IconDisplayEnabled;
    j["iconSize"] = g_IconSize;
    j["iconSpacing"] = g_IconSpacing;
    j["iconOpacity"] = g_IconOpacity;
    j["iconAnchor"] = static_cast<int>(g_IconPosition.anchor);
    j["iconOffsetX"] = g_IconPosition.offsetXPct;
    j["iconOffsetY"] = g_IconPosition.offsetYPct;
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto id : g_TrackedConditionsExtra)
            arr.push_back(id);
        j["trackedConditionsExtra"] = arr;
    }
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto id : g_TrackedBuffsExtra)
            arr.push_back(id);
        j["trackedBuffsExtra"] = arr;
    }

    j["foodUtilityExpiry"] = g_FoodUtilityExpiryEnabled;
    j["readyCheckTts"] = g_ReadyCheckTtsEnabled;
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
    tts["emoteCustom"]   = g_Tts.emoteCustom;
    tts["voiceTokenId"]  = g_TtsVoiceTokenId;
    tts["outputTokenId"] = g_TtsOutputTokenId;
    tts["volume"]        = g_TtsVolume;
    tts["rate"]          = g_TtsRate;
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
        g_KeybindOverlayEnabled = j.value("keybindOverlayEnabled", false);
        g_KeybindVisibility = j.value("keybindVisibility", KV_Always);
        g_KeybindShowMovement   = j.value("keybindShowMovement",   true);
        g_KeybindShowSkills     = j.value("keybindShowSkills",     true);
        g_KeybindShowTargeting  = j.value("keybindShowTargeting",  false);
        g_KeybindShowMounts     = j.value("keybindShowMounts",     true);
        g_KeybindShowSquad      = j.value("keybindShowSquad",      false);
        g_KeybindShowCamera     = j.value("keybindShowCamera",     false);
        g_KeybindShowScreenshot = j.value("keybindShowScreenshot", false);
        g_KeybindShowMap        = j.value("keybindShowMap",        false);
        g_KeybindShowUI         = j.value("keybindShowUI",         false);
        g_KeybindShowTemplates  = j.value("keybindShowTemplates",  false);
        int keybindAnchor = j.value("keybindPositionAnchor", static_cast<int>(AnchorPoint::MiddleLeft));
        keybindAnchor = std::clamp(keybindAnchor, 0, static_cast<int>(AnchorPoint::COUNT) - 1);
        g_KeybindPosition.anchor = static_cast<AnchorPoint>(keybindAnchor);
        g_KeybindPosition.offsetXPct = j.value("keybindPositionX", 2.0f);
        g_KeybindPosition.offsetYPct = j.value("keybindPositionY", 0.0f);
        g_KeybindOverlayOpacity = j.value("keybindOpacity", 0.4f);
        ScanKeybindFiles();
        g_ManualKeybindPath = j.value("manualKeybindPath", "");
        std::string savedFile = j.value("keybindFile", "");
        if (!savedFile.empty())
        {
            for (int i = 0; i < static_cast<int>(g_KeybindFiles.size()); i++)
            {
                if (g_KeybindFiles[i] == savedFile)
                {
                    g_KeybindFileIndex = i;
                    break;
                }
            }
        }
        if (g_KeybindFileIndex >= 0)
            LoadKeybindFile(g_KeybindFileIndex);
        g_IconDisplayEnabled = j.value("iconDisplayEnabled", false);
        g_IconSize = j.value("iconSize", 32.0f);
        g_IconSpacing = j.value("iconSpacing", 4.0f);
        g_IconOpacity = j.value("iconOpacity", 1.0f);
        int iconAnchor = j.value("iconAnchor", static_cast<int>(AnchorPoint::Center));
        iconAnchor = std::clamp(iconAnchor, 0, static_cast<int>(AnchorPoint::COUNT) - 1);
        g_IconPosition.anchor = static_cast<AnchorPoint>(iconAnchor);
        g_IconPosition.offsetXPct = j.value("iconOffsetX", 0.0f);
        g_IconPosition.offsetYPct = j.value("iconOffsetY", -10.0f);
        g_TrackedConditionsExtra.clear();
        auto& tce = j["trackedConditionsExtra"];
        if (tce.is_array())
            for (const auto& v : tce)
                g_TrackedConditionsExtra.insert(v.get<unsigned int>());

        g_TrackedBuffsExtra.clear();
        auto& tbe = j["trackedBuffsExtra"];
        if (tbe.is_array())
            for (const auto& v : tbe)
                g_TrackedBuffsExtra.insert(v.get<unsigned int>());

        g_FoodUtilityExpiryEnabled = j.value("foodUtilityExpiry", false);
        g_ReadyCheckTtsEnabled = j.value("readyCheckTts", false);
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
            g_Tts.emoteCustom    = tts.value("emoteCustom", false);
            g_TtsVoiceTokenId    = tts.value("voiceTokenId",  "");
            g_TtsOutputTokenId   = tts.value("outputTokenId", "");
            g_TtsVolume          = tts.value("volume", 100);
            g_TtsVolume          = std::clamp(g_TtsVolume, 0, 100);
            g_TtsRate            = tts.value("rate", 0);
            g_TtsRate            = std::clamp(g_TtsRate, -10, 10);
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

static std::string WideToUtf8(const wchar_t* w)
{
    if (!w || !w[0]) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string s(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// Populate g_TtsVoices from HKLM speech voice token registries.
// Called once at load; call again to refresh after settings load.
static void EnumerateTtsVoices()
{
    g_TtsVoices.clear();
    g_TtsVoices.push_back({"System Default", ""});

    struct { const wchar_t* subkey; const char* hkeyPrefix; } src[] = {
        { L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens\\" },
        { L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\" },
    };
    for (auto& s : src)
    {
        HKEY hCat = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, s.subkey, 0, KEY_READ, &hCat) != ERROR_SUCCESS) continue;
        DWORD idx = 0;
        wchar_t skName[512]; DWORD skLen = 512;
        while (RegEnumKeyExW(hCat, idx++, skName, &skLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            skLen = 512;
            HKEY hTok = nullptr;
            if (RegOpenKeyExW(hCat, skName, 0, KEY_READ, &hTok) == ERROR_SUCCESS)
            {
                wchar_t dispName[512] = {}; DWORD cbName = sizeof(dispName);
                RegQueryValueExW(hTok, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(dispName), &cbName);
                RegCloseKey(hTok);
                std::string display = WideToUtf8(dispName);
                if (display.empty()) display = WideToUtf8(skName);
                std::string tokenId = s.hkeyPrefix + WideToUtf8(skName);
                g_TtsVoices.push_back({display, tokenId});
            }
        }
        RegCloseKey(hCat);
    }
    g_TtsVoiceIdx = 0;
    for (int i = 0; i < static_cast<int>(g_TtsVoices.size()); i++)
        if (g_TtsVoices[i].tokenId == g_TtsVoiceTokenId) { g_TtsVoiceIdx = i; break; }
}

// Populate g_TtsOutputDevices using waveOutGetDevCaps (avoids SAPI HKCU token issues).
static void EnumerateTtsOutputDevices()
{
    g_TtsOutputDevices.clear();
    g_TtsOutputDevices.push_back({"Default (System)", ""});

    UINT numDevs = waveOutGetNumDevs();
    for (UINT i = 0; i < numDevs; i++)
    {
        WAVEOUTCAPSW caps = {};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            std::string name = WideToUtf8(caps.szPname);
            if (name.empty())
                name = "Audio Output " + std::to_string(i);
            g_TtsOutputDevices.push_back({name, std::to_string(i)});
        }
    }

    g_TtsOutputIdx = 0;
    for (int i = 0; i < static_cast<int>(g_TtsOutputDevices.size()); i++)
        if (g_TtsOutputDevices[i].tokenId == g_TtsOutputTokenId) { g_TtsOutputIdx = i; break; }
}

static void InitSAPI()
{
    if (g_TtsReady) return;

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
    {
        // Apply user-selected voice (from addon dropdown).
        // ISpObjectToken::SetId(nullptr, fullRegistryPath) is used because
        // ISpObjectTokenCategory::SetId fails in GW2's STA COM apartment.
        if (!g_TtsVoiceTokenId.empty())
        {
            std::wstring wId = Utf8ToWide(g_TtsVoiceTokenId.c_str());
            ISpObjectToken* pToken = nullptr;
            HRESULT hrTok = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                             IID_ISpObjectToken, (void**)&pToken);
            if (SUCCEEDED(hrTok) && pToken)
            {
                hrTok = pToken->SetId(nullptr, wId.c_str(), FALSE);
                if (SUCCEEDED(hrTok))
                {
                    HRESULT hrSV = g_pVoice->SetVoice(pToken);
                    snprintf(buf, sizeof(buf), "[TTS] SetVoice(%s) = 0x%08lX (%s)",
                        g_TtsVoiceTokenId.c_str(), (unsigned long)hrSV, SUCCEEDED(hrSV) ? "OK" : "FAILED");
                }
                else
                    snprintf(buf, sizeof(buf), "[TTS] voice token SetId failed: 0x%08lX", (unsigned long)hrTok);
                g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
                pToken->Release();
            }
        }

        // Apply user-selected output device via ISpMMSysAudio::SetDeviceId
        // (avoids HKCU token + SetId approach which fails with 0x8004503A).
        {
            ISpMMSysAudio* pMMAudio = nullptr;
            HRESULT hrOut = CoCreateInstance(CLSID_SpMMAudioOut, nullptr, CLSCTX_ALL,
                                             IID_ISpMMSysAudio, (void**)&pMMAudio);
            if (SUCCEEDED(hrOut) && pMMAudio)
            {
                if (!g_TtsOutputTokenId.empty())
                {
                    int waveIdx = std::atoi(g_TtsOutputTokenId.c_str());
                    hrOut = pMMAudio->SetDeviceId(static_cast<UINT>(waveIdx));
                    if (FAILED(hrOut))
                        snprintf(buf, sizeof(buf), "[TTS] SetDeviceId(%d) failed: 0x%08lX", waveIdx, (unsigned long)hrOut);
                    else
                        snprintf(buf, sizeof(buf), "[TTS] SetDeviceId(%d) = OK", waveIdx);
                }
                else
                {
                    // Default device — WAVE_MAPPER.
                    snprintf(buf, sizeof(buf), "[TTS] using default device (WAVE_MAPPER)");
                }
                g_API->Log(LOGL_INFO, "GW2Accessibility", buf);

                HRESULT hrSO = g_pVoice->SetOutput(pMMAudio, FALSE);
                if (FAILED(hrSO))
                {
                    snprintf(buf, sizeof(buf), "[TTS] SetOutput(ISpMMSysAudio) failed: 0x%08lX", (unsigned long)hrSO);
                    g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
                }
                pMMAudio->Release();
            }
            else
            {
                snprintf(buf, sizeof(buf), "[TTS] CoCreateInstance(CLSID_SpMMAudioOut) failed: 0x%08lX", (unsigned long)hrOut);
                g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
            }
        }

        // Log the voice name actually in use.
        {
            ISpObjectToken* pCurToken = nullptr;
            if (SUCCEEDED(g_pVoice->GetVoice(&pCurToken)) && pCurToken)
            {
                wchar_t* descW = nullptr;
                if (SUCCEEDED(pCurToken->GetStringValue(nullptr, &descW)) && descW)
                {
                    char descUtf8[512] = {};
                    WideCharToMultiByte(CP_UTF8, 0, descW, -1, descUtf8, sizeof(descUtf8), nullptr, nullptr);
                    snprintf(buf, sizeof(buf), "[TTS] Active voice: \"%s\"", descUtf8);
                    g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
                    CoTaskMemFree(descW);
                }
                pCurToken->Release();
            }
        }

        g_pVoice->SetVolume(static_cast<USHORT>(g_TtsVolume));
        g_pVoice->SetRate(std::clamp(g_TtsRate, -10, 10));
        g_API->Log(LOGL_INFO, "GW2Accessibility", "[TTS] SAPI voice initialized successfully");
    }
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

    // ev == null: agent tracking event (add/remove). Use to detect initial squad state.
    if (!cbt->ev)
    {
        if (cbt->src && cbt->src->Profession && cbt->dst && cbt->dst->IsSelf)
            g_InSquad = (cbt->src->Team > 0);
        return;
    }

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
            g_InSquad = (cbt->ev->DestinationAgent > 0);
        }
        return;
    }


    // ── Combat state tracking ────────────────────────────────────────────
    {
        uint32_t state = cbt->ev->IsStatechange;
        if (state == 1 && cbt->src && cbt->src->IsSelf) g_InCombat = true;
        if (state == 2 && cbt->src && cbt->src->IsSelf) g_InCombat = false;
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
        // Squad Only: filter out the player's own downed events (you already know when you go down).
        if (g_AllyDownedSquadOnly && cbt->src && cbt->src->IsSelf) return;
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
        return;
    }

    // IsStatechange=69: arcdps reports persistent buffs active at map load with this value.
    // Also handle 18 (CBTS_BUFFINITIAL) as a fallback for other arcdps versions.
    if ((cbt->ev->IsStatechange == 69 || cbt->ev->IsStatechange == 18) &&
        cbt->dst && cbt->dst->IsSelf == 1)
    {
        // Skip login/character-select screen (MapID == 0)
        uint32_t mapId = g_MumbleData ? g_MumbleData->Context.MapID : 0;
        if (mapId == 0) return;

        // Log a header the first time we see a BUFF_INIT for a new map
        static uint32_t s_LastDumpMapId = 0;
        if (mapId != s_LastDumpMapId)
        {
            s_LastDumpMapId = mapId;
            char hdr[128];
            snprintf(hdr, sizeof(hdr), "[BUFF_DUMP] === Map load MapID=%u — active buffs follow ===", mapId);
            g_API->Log(LOGL_INFO, "GW2Accessibility", hdr);
        }

        unsigned int sid = NormalizeMechanicID(cbt->ev->SkillID);

        // Cache the name so FetchSkillName can find it on the remove event
        if (cbt->skillname && cbt->skillname[0])
            g_DebuffNameCache[sid] = cbt->skillname;

        char fdbg[512];
        snprintf(fdbg, sizeof(fdbg), "[BUFF_DUMP] skill=%u name=\"%s\"",
            sid, cbt->skillname ? cbt->skillname : "(null)");
        g_API->Log(LOGL_INFO, "GW2Accessibility", fdbg);

        // Malnourished/Diminished appearing at map load means the player entered
        // the map without food/utility. Speak TTS immediately.
        if (g_FoodUtilityExpiryEnabled && (sid == 46587 || sid == 46668))
        {
            const wchar_t* speech = (sid == 46587) ? L"Malnourished" : L"Diminished";
            char tdbg[128];
            snprintf(tdbg, sizeof(tdbg),
                "[FOOD] skill=%u (%s) in BUFF_DUMP — player entered map without consumable, speaking TTS",
                sid, (sid == 46587) ? "Malnourished" : "Diminished");
            g_API->Log(LOGL_INFO, "GW2Accessibility", tdbg);
            InitSAPI();
            if (g_TtsReady) SpeakText(speech);
        }

        return;
    }

    if (cbt->ev->IsStatechange != 0)
    {
        // IsStatechange=72 is the buff-apply event type in current arcdps versions
        // (arcdps stopped using IsStatechange=0 for buff applies). Fall through to
        // normal buff processing instead of returning.
        if (cbt->ev->IsStatechange != 72)
        {
            g_API->Log(LOGL_DEBUG, "GW2Accessibility", "ignored non-buff statechange");
            return;
        }
    }

    unsigned int skillID = NormalizeMechanicID(cbt->ev->SkillID);
    bool isRemove = (cbt->ev->IsBuffRemove != 0);
    DWORD now = GetTickCount();

    bool isOnSelf = (cbt->dst && cbt->dst->IsSelf == 1) || (cbt->src && cbt->src->IsSelf == 1);

    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[COMBAT] skill=%u Buff=%u SC=%u remove=%u dst.self=%u src.self=%u name=%s",
            skillID, (unsigned)cbt->ev->Buff, (unsigned)cbt->ev->IsStatechange, isRemove,
            cbt->dst ? (unsigned)cbt->dst->IsSelf : 99,
            cbt->src ? (unsigned)cbt->src->IsSelf : 99,
            cbt->skillname ? cbt->skillname : "(null)");
        g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
    }

    // ── Track active mechanics for icon display ──────────────────────────────
    // Runs independently of TTS/Announce settings
    if (isRemove)
    {
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

    // ── Collect non-standard effects for management window ──────────────
    // Only collects effects ON the player
    if (isOnSelf && !isRemove && !IsCondition(skillID) && !IsBoon(skillID))
    {
        if (g_EncounteredEffects.find(skillID) == g_EncounteredEffects.end())
        {
            const char* name = nullptr;
            if (cbt->skillname && cbt->skillname[0])
                name = cbt->skillname;
            else
            {
                auto builtin = g_BuiltinBuffNames.find(skillID);
                if (builtin != g_BuiltinBuffNames.end())
                    name = builtin->second;
            }

            char nameBuf[256];
            if (name)
                snprintf(nameBuf, sizeof(nameBuf), "%s", name);
            else
                snprintf(nameBuf, sizeof(nameBuf), "Unknown (%u)", skillID);

            g_EncounteredEffects[skillID] = nameBuf;
            char buf[256];
            snprintf(buf, sizeof(buf), "[EFFECT_MGR] discovered skill=%u name=\"%s\"", skillID, nameBuf);
            g_API->Log(LOGL_INFO, "GW2Accessibility", buf);
        }
    }

    // ── TTS filters (dst or src must be self, skip in WvW) ──────────────────
    // In newer arcdps (IsStatechange=72 events), the receiving agent may be in
    // src rather than dst. Accept either.
    bool dstSelf = cbt->dst && cbt->dst->IsSelf == 1;
    bool srcSelf = cbt->src && cbt->src->IsSelf == 1;
    if (!dstSelf && !srcSelf)
    {
        return;
    }
    if (IsWvwMap())
    {
        return;
    }

    // ── Gate TTS speech behind settings ──────────────────────────────────────
    if (!g_MechanicsAnnounceEnabled && !g_ReadAllDebuffs && !g_FoodUtilityExpiryEnabled)
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

    // ── Food/Utility expiry detection ────────────────────────────────────────
    // arcdps applies Malnourished (46587) when Nourishment expires and
    // Diminished (46668) when Enhancement expires. Detect these being APPLIED
    // on self and speak TTS immediately. We do NOT track 17825/9963 removes —
    // arcdps does not reliably fire remove events for natural food expiry.
    if (g_FoodUtilityExpiryEnabled && !isRemove && (skillID == 46587 || skillID == 46668))
    {
        const wchar_t* speech = (skillID == 46587) ? L"Malnourished" : L"Diminished";
        char fdbg[256];
        snprintf(fdbg, sizeof(fdbg),
            "[FOOD] skill=%u (%s) applied on self (dstSelf=%d srcSelf=%d) — speaking TTS",
            skillID, (skillID == 46587) ? "Malnourished" : "Diminished",
            (int)dstSelf, (int)srcSelf);
        g_API->Log(LOGL_INFO, "GW2Accessibility", fdbg);
        InitSAPI();
        if (g_TtsReady) SpeakText(speech);
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

// ── Effect Management Window ───────────────────────────────────────────

static void OnEffectManagerRender()
{
    SetupImGui();
    if (!g_ShowEffectManager) return;

    if (!ImGui::Begin("Effect Manager", &g_ShowEffectManager, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Non-standard effects encountered during gameplay:");
    ImGui::TextDisabled("Check 'Cond Table' or 'Buff Table' to track them in those overlays.");
    ImGui::TextDisabled("Standard conditions/boons are always included automatically.");
    ImGui::Separator();

    if (g_EncounteredEffects.empty())
    {
        ImGui::TextDisabled("No effects discovered yet. Play the game to encounter effects.");
    }
    else
    {
        bool changed = false;

        // Sort by name for easier browsing
        std::vector<std::pair<unsigned int, std::string>> sorted(
            g_EncounteredEffects.begin(), g_EncounteredEffects.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        ImGui::Columns(3, nullptr, false);
        ImGui::TextUnformatted("Effect"); ImGui::NextColumn();
        ImGui::TextUnformatted("Cond Table"); ImGui::NextColumn();
        ImGui::TextUnformatted("Buff Table"); ImGui::NextColumn();
        ImGui::Separator();

        for (const auto& entry : sorted)
        {
            unsigned int id = entry.first;
            const std::string& name = entry.second;

            ImGui::PushID(static_cast<int>(id));
            ImGui::Text("%s (%u)", name.c_str(), id); ImGui::NextColumn();

            bool inCond = g_TrackedConditionsExtra.count(id) > 0;
            if (ImGui::Checkbox("##cond", &inCond))
            {
                if (inCond) g_TrackedConditionsExtra.insert(id);
                else g_TrackedConditionsExtra.erase(id);
                changed = true;
            }
            ImGui::NextColumn();

            bool inBuff = g_TrackedBuffsExtra.count(id) > 0;
            if (ImGui::Checkbox("##buff", &inBuff))
            {
                if (inBuff) g_TrackedBuffsExtra.insert(id);
                else g_TrackedBuffsExtra.erase(id);
                changed = true;
            }
            ImGui::NextColumn();

            ImGui::PopID();
        }

        ImGui::Columns(1);

        // Clear button
        ImGui::Separator();
        if (ImGui::Button("Clear All Discovered Effects"))
        {
            g_EncounteredEffects.clear();
            g_TrackedConditionsExtra.clear();
            g_TrackedBuffsExtra.clear();
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Cond Table Extras"))
        {
            g_TrackedConditionsExtra.clear();
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Buff Table Extras"))
        {
            g_TrackedBuffsExtra.clear();
            changed = true;
        }

        if (changed)
            SaveSettings();
    }

    ImGui::End();
}

// ── Keybind Overlay Window ─────────────────────────────────────────────

static void OnKeybindOverlayRender()
{
    SetupImGui();

    if (!g_KeybindOverlayEnabled) return;
    if (g_KeybindVisibility == KV_InCombatOnly && !g_InCombat) return;
    if (g_KeybindVisibility == KV_OutOfCombatOnly && g_InCombat) return;

    if (!g_KeybindsLoaded)
        LoadKeybindFile(g_KeybindFileIndex);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoMouseInputs |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    int idx = static_cast<int>(g_KeybindPosition.anchor);
    int col = idx % 3;
    int row = idx / 3;
    float baseX = (col == 0) ? 0.0f : (col == 1) ? displaySize.x * 0.5f : displaySize.x;
    float baseY = (row == 0) ? 0.0f : (row == 1) ? displaySize.y * 0.5f : displaySize.y;
    float offsetX = (g_KeybindPosition.offsetXPct / 100.0f) * displaySize.x;
    float offsetY = (g_KeybindPosition.offsetYPct / 100.0f) * displaySize.y;
    // Pivot matches the anchor so the window's corresponding corner sits exactly at the anchor point.
    // e.g. TopRight → pivot (1,0) means the window's top-right corner lands at the right edge of the screen.
    float pivotX = col * 0.5f;
    float pivotY = row * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(baseX + offsetX, baseY + offsetY), ImGuiCond_Always, ImVec2(pivotX, pivotY));
    ImGui::SetNextWindowBgAlpha(g_KeybindOverlayOpacity);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_KeybindOverlayOpacity);

    if (!ImGui::Begin("##KeybindOverlay", nullptr, flags))
    {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    auto findKey = [](const char* actionName) -> std::string {
        for (const auto& kb : g_Keybinds)
        {
            if (kb.name == actionName)
            {
                // Primary missing but secondary present (e.g. Jump with only a secondary set)
                if (kb.key.empty())
                    return kb.key2.empty() ? "?" : kb.key2;
                if (!kb.key2.empty() && kb.key2 != kb.key)
                    return kb.key + "/" + kb.key2;
                return kb.key;
            }
        }
        // GW2 omits default bindings from the XML export entirely.
        // These are the standard defaults used when no XML entry exists.
        static const struct { const char* name; const char* key; } DEFAULTS[] = {
            { "Move Forward",            "W"     },
            { "Move Backward",           "S"     },
            { "Jump / Swim Up / Fly Up", "Space" },
            { "Swim Down / Fly Down",    "Space" },
            { "Autorun",                 "R"     },
        };
        for (const auto& d : DEFAULTS)
            if (strcmp(actionName, d.name) == 0) return d.key;
        return "?";
    };

    // Second column starts at this x offset from window left
    const float COL2 = 105.0f;

    // Generic category renderer: prints all keybinds in that category
    auto renderCat = [](const char* title, const char* catName) {
        ImGui::TextDisabled(title);
        ImGui::Separator();
        for (const auto& kb : g_Keybinds)
        {
            if (kb.category != catName) continue;
            if (kb.key.empty() && kb.key2.empty()) continue;
            if (!kb.key2.empty() && kb.key2 != kb.key)
                ImGui::Text("%-24s %s/%s", kb.name.c_str(), kb.key.c_str(), kb.key2.c_str());
            else
                ImGui::Text("%-24s %s", kb.name.c_str(), kb.key.c_str());
        }
        ImGui::Dummy(ImVec2(0, 3));
    };

    if (g_KeybindShowMovement)
    {
        ImGui::TextDisabled("Movement");
        ImGui::Separator();
        ImGui::Text("Fwd:   %s", findKey("Move Forward").c_str());
        ImGui::SameLine(COL2); ImGui::Text("Back:  %s", findKey("Move Backward").c_str());
        ImGui::Text("Left:  %s", findKey("Strafe Left").c_str());
        ImGui::SameLine(COL2); ImGui::Text("Right: %s", findKey("Strafe Right").c_str());
        ImGui::Text("Jump:  %s", findKey("Jump / Swim Up / Fly Up").c_str());
        ImGui::SameLine(COL2); ImGui::Text("Dodge: %s", findKey("Dodge").c_str());
        ImGui::Text("Run:   %s", findKey("Autorun").c_str());
        ImGui::SameLine(COL2); ImGui::Text("Face:  %s", findKey("About Face").c_str());
        ImGui::Dummy(ImVec2(0, 3));
    }

    if (g_KeybindShowSkills)
    {
        ImGui::TextDisabled("Skills");
        ImGui::Separator();
        ImGui::Text("Skill 1:  %s", findKey("Weapon Skill 1").c_str());
        ImGui::Text("Skill 2:  %s", findKey("Weapon Skill 2").c_str());
        ImGui::Text("Skill 3:  %s", findKey("Weapon Skill 3").c_str());
        ImGui::Text("Skill 4:  %s", findKey("Weapon Skill 4").c_str());
        ImGui::Text("Skill 5:  %s", findKey("Weapon Skill 5").c_str());
        ImGui::Text("Heal:     %s", findKey("Healing Skill").c_str());
        ImGui::Text("Util 1:   %s", findKey("Utility Skill 1").c_str());
        ImGui::Text("Util 2:   %s", findKey("Utility Skill 2").c_str());
        ImGui::Text("Util 3:   %s", findKey("Utility Skill 3").c_str());
        ImGui::Text("Elite:    %s", findKey("Elite Skill").c_str());
        ImGui::Text("Special:  %s", findKey("Special Action").c_str());
        ImGui::Text("Swap:     %s", findKey("Swap Weapons").c_str());
        ImGui::Dummy(ImVec2(0, 3));
    }

    if (g_KeybindShowTargeting)
        renderCat("Targeting", "Targeting");

    if (g_KeybindShowMounts)
    {
        ImGui::TextDisabled("Mounts");
        ImGui::Separator();
        ImGui::Text("Mount:    %s", findKey("Mount/Dismount").c_str());
        ImGui::SameLine(COL2); ImGui::Text("Ability1: %s", findKey("Mount Ability 1").c_str());
        static const char* MOUNT_NAMES[] = {
            "Raptor Mount/Dismount",  "Springer Mount/Dismount",
            "Skimmer Mount/Dismount", "Jackal Mount/Dismount",
            "Roller Beetle Mount/Dismount", "Warclaw Mount/Dismount",
            "Griffon Mount/Dismount", "Skyscale Mount/Dismount"
        };
        static const char* MOUNT_LABELS[] = {
            "Raptor", "Springer", "Skimmer", "Jackal",
            "R.Beetle", "Warclaw", "Griffon", "Skyscale"
        };
        for (int i = 0; i < 8; i++)
        {
            std::string k = findKey(MOUNT_NAMES[i]);
            if (k != "?")
                ImGui::Text("%-10s %s", MOUNT_LABELS[i], k.c_str());
        }
        ImGui::Dummy(ImVec2(0, 3));
    }

    if (g_KeybindShowSquad)
        renderCat("Squad", "Squad");

    if (g_KeybindShowCamera)
        renderCat("Camera", "Camera");

    if (g_KeybindShowScreenshot)
        renderCat("Screenshot", "Screenshot");

    if (g_KeybindShowMap)
        renderCat("Map", "Map");

    if (g_KeybindShowUI)
        renderCat("UI", "UI");

    if (g_KeybindShowTemplates)
        renderCat("Templates", "Templates");

    ImGui::PopStyleVar();
    ImGui::End();
}

// ── Keybind Config Window ───────────────────────────────────────────────

static void OnKeybindConfigRender()
{
    SetupImGui();
    if (!g_ShowKeybindConfig) return;

    bool changed = false;
    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 0), ImVec2(600, 900));
    if (!ImGui::Begin("Keybind Overlay Settings", &g_ShowKeybindConfig,
                      ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // ── Enable ───────────────────────────────────────────────────────────────
    changed |= ImGui::Checkbox("Enable Keybind Overlay", &g_KeybindOverlayEnabled);

    // ── Data Source ──────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Data Source");
    ImGui::Separator();
    ImGui::TextDisabled("Documents\\Guild Wars 2\\InputBinds\\");
    ImGui::TextDisabled("Export keybinds from GW2 Settings > Controls > Export.");

    {
        const char* currentFile = (g_KeybindFileIndex >= 0 &&
                                   g_KeybindFileIndex < static_cast<int>(g_KeybindFiles.size()))
                                  ? g_KeybindFiles[g_KeybindFileIndex].c_str()
                                  : "None found";
        ImGui::Text("File: %s", currentFile);
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            OPENFILENAMEA ofn = {};
            char filePath[MAX_PATH] = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "Keybind Files\0*.xml\0All Files\0*.*\0\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = "Select GW2 Keybind File";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn))
            {
                std::string path = filePath;
                auto lastSlash = path.find_last_of("/\\");
                std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

                bool found = false;
                for (int i = 0; i < static_cast<int>(g_KeybindFiles.size()); i++)
                {
                    if (g_KeybindFiles[i] == filename)
                    {
                        g_KeybindFileIndex = i;
                        LoadKeybindFile(i);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    std::string unixPath = WinePathToUnix(path.c_str());
                    std::ifstream f(unixPath);
                    if (f.is_open())
                    {
                        std::string xmlContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        f.close();
                        auto lastSlash2 = path.find_last_of("/\\");
                        std::string filename2 = (lastSlash2 != std::string::npos) ? path.substr(lastSlash2 + 1) : path;
                        g_KeybindFiles.clear();
                        g_KeybindFiles.push_back(filename2);
                        g_KeybindFileIndex = 0;
                        g_ManualKeybindPath = path;
                        ParseKeybindXml(xmlContent, g_Keybinds);
                        g_KeybindsLoaded = true;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            g_KeybindsLoaded = false;
            LoadKeybindFile(g_KeybindFileIndex);
        }
    }

    // ── Visibility ───────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("When to Show");
    ImGui::Separator();
    const char* visNames[] = { "Always", "In Combat", "Out of Combat" };
    for (int i = 0; i < 3; i++)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(visNames[i], g_KeybindVisibility == i))
        {
            g_KeybindVisibility = i;
            changed = true;
        }
    }

    // ── Position ─────────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Position");
    ImGui::Separator();
    {
        int anchorIdx = static_cast<int>(g_KeybindPosition.anchor);
        if (ImGui::Combo("Anchor##kb", &anchorIdx, g_AnchorNames,
                         static_cast<int>(AnchorPoint::COUNT)))
        {
            g_KeybindPosition.anchor = static_cast<AnchorPoint>(anchorIdx);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Offset X%##kb", &g_KeybindPosition.offsetXPct,
                                      -50.0f, 50.0f, "%.1f%%");
        changed |= ImGui::SliderFloat("Offset Y%##kb", &g_KeybindPosition.offsetYPct,
                                      -50.0f, 50.0f, "%.1f%%");
        changed |= ImGui::SliderFloat("Opacity##kb", &g_KeybindOverlayOpacity, 0.1f, 1.0f, "%.2f");
    }

    // ── Sections ─────────────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Sections to Display");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Movement",   &g_KeybindShowMovement);
    changed |= ImGui::Checkbox("Skills",     &g_KeybindShowSkills);
    changed |= ImGui::Checkbox("Targeting",  &g_KeybindShowTargeting);
    changed |= ImGui::Checkbox("Mounts",     &g_KeybindShowMounts);
    changed |= ImGui::Checkbox("Squad",      &g_KeybindShowSquad);
    changed |= ImGui::Checkbox("Camera",     &g_KeybindShowCamera);
    changed |= ImGui::Checkbox("Screenshot", &g_KeybindShowScreenshot);
    changed |= ImGui::Checkbox("Map",        &g_KeybindShowMap);
    changed |= ImGui::Checkbox("UI",         &g_KeybindShowUI);
    changed |= ImGui::Checkbox("Templates",  &g_KeybindShowTemplates);

    // ── Keybind Viewer ───────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Text("Loaded Keybinds (%zu)", g_Keybinds.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Dump to File"))
    {
        const char* addonDir = g_API->Paths_GetAddonDirectory("GW2Accessibility");
        std::string dumpPath = addonDir ? addonDir : ".\\";
        if (dumpPath.back() != '\\' && dumpPath.back() != '/')
            dumpPath += '\\';
        dumpPath += "keybinds_dump.txt";

        std::ofstream df(dumpPath);
        if (df.is_open())
        {
            df << "GW2 Keybind Dump\n";
            df << "================\n";
            df << "Category       | Action                          | Bind1 (btn/mod) | Bind2 (btn/mod)\n";
            df << "---------------|---------------------------------|-----------------|----------------\n";
            for (const auto& kb : g_Keybinds)
            {
                char line[320];
                if (!kb.key2.empty())
                    snprintf(line, sizeof(line), "%-14s | %-31s | %-14s (%u/%u) | %s (%u/%u)\n",
                             kb.category.c_str(), kb.name.c_str(),
                             kb.key.c_str(), kb.rawButton, kb.rawMod,
                             kb.key2.c_str(), kb.rawButton2, kb.rawMod2);
                else
                    snprintf(line, sizeof(line), "%-14s | %-31s | %-14s (%u/%u) |\n",
                             kb.category.c_str(), kb.name.c_str(),
                             kb.key.c_str(), kb.rawButton, kb.rawMod);
                df << line;
            }
            df.close();
            g_API->Log(LOGL_INFO, "GW2Accessibility", ("Keybind dump written to: " + dumpPath).c_str());
        }
    }
    ImGui::TextDisabled("Dump writes to the GW2Accessibility addon folder.");
    ImGui::Separator();
    if (ImGui::BeginChild("KeybindList", ImVec2(0, 190), true))
    {
        static std::string s_CategoryFilter;
        struct { const char* label; const char* cat; } kFilterBtns[] = {
            {"All", ""},
            {"Movement",   "Movement"},
            {"Skills",     "Skills"},
            {"Targeting",  "Targeting"},
            {"Mounts",     "Mounts"},
            {"Squad",      "Squad"},
            {"Camera",     "Camera"},
            {"Map",        "Map"},
            {"UI",         "UI"},
            {"Templates",  "Templates"},
            {"Screenshot", "Screenshot"},
            {"General",    "General"},
        };
        // Two rows of filter buttons so they don't overflow the window
        const int ROW_BREAK = 6; // buttons per row
        for (int fi = 0; fi < static_cast<int>(sizeof(kFilterBtns)/sizeof(kFilterBtns[0])); fi++)
        {
            auto& fb = kFilterBtns[fi];
            if (fi > 0 && fi % ROW_BREAK != 0) ImGui::SameLine();
            bool active = (s_CategoryFilter == fb.cat);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(fb.label)) s_CategoryFilter = fb.cat;
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Separator();

        for (const auto& kb : g_Keybinds)
        {
            if (!s_CategoryFilter.empty() && kb.category != s_CategoryFilter)
                continue;
            if (!kb.key2.empty())
                ImGui::Text("%-10s %-30s %s / %s  (%u / %u)",
                            kb.category.c_str(), kb.name.c_str(),
                            kb.key.c_str(), kb.key2.c_str(),
                            kb.rawButton, kb.rawButton2);
            else
                ImGui::Text("%-10s %-30s %s  (%u)",
                            kb.category.c_str(), kb.name.c_str(),
                            kb.key.c_str(), kb.rawButton);
        }
    }
    ImGui::EndChild();

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

        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ── Auditory ──────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Auditory"))
    {
        ImGui::Indent();

        // ── TTS Device Settings ──────────────────────────────────────────────────
        ImGui::TextUnformatted("TTS Voice & Output");
        ImGui::Separator();

        // Voice dropdown
        {
            std::vector<const char*> voiceNames;
            voiceNames.reserve(g_TtsVoices.size());
            for (auto& v : g_TtsVoices) voiceNames.push_back(v.name.c_str());

            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::Combo("Voice##ttsvoice", &g_TtsVoiceIdx,
                             voiceNames.data(), static_cast<int>(voiceNames.size())))
            {
                g_TtsVoiceTokenId = g_TtsVoices[g_TtsVoiceIdx].tokenId;
                // Force SAPI reinit so new voice takes effect immediately on next speak
                if (g_pVoice) { g_pVoice->Release(); g_pVoice = nullptr; }
                g_TtsReady = false;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh##voicerefresh"))
            {
                EnumerateTtsVoices();
                EnumerateTtsOutputDevices();
            }
        }

        // Output device dropdown
        {
            std::vector<const char*> devNames;
            devNames.reserve(g_TtsOutputDevices.size());
            for (auto& d : g_TtsOutputDevices) devNames.push_back(d.name.c_str());

            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::Combo("Output##ttsoutput", &g_TtsOutputIdx,
                             devNames.data(), static_cast<int>(devNames.size())))
            {
                g_TtsOutputTokenId = g_TtsOutputDevices[g_TtsOutputIdx].tokenId;
                if (g_pVoice) { g_pVoice->Release(); g_pVoice = nullptr; }
                g_TtsReady = false;
                changed = true;
            }
        }

        // Volume slider
        {
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::SliderInt("Volume##ttsvolume", &g_TtsVolume, 0, 100))
            {
                g_TtsVolume = std::clamp(g_TtsVolume, 0, 100);
                if (g_pVoice) g_pVoice->SetVolume(static_cast<USHORT>(g_TtsVolume));
                changed = true;
            }
        }

        // Rate (speed) slider
        {
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::SliderInt("Speed##ttsrate", &g_TtsRate, -10, 10))
            {
                g_TtsRate = std::clamp(g_TtsRate, -10, 10);
                if (g_pVoice) g_pVoice->SetRate(g_TtsRate);
                changed = true;
            }
        }

        // Test button
        if (ImGui::Button("Test TTS##ttsdevtest"))
        {
            InitSAPI();
            if (g_TtsReady) SpeakText(L"TTS voice test.");
        }

        ImGui::Dummy(ImVec2(0, 6));

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
        ImGui::TextDisabled("Speaks \"Malnourished\" or \"Diminished\" at map load and every 5 minutes if consumables are missing.");

        changed |= ImGui::Checkbox("Announce Ready Check", &g_ReadyCheckTtsEnabled);
        ImGui::TextDisabled("Speaks \"Ready check\" when the squad leader initiates one.");
        ImGui::TextDisabled("Requires Unofficial Extras (arcdps extension).");

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

        ImGui::TextUnformatted("Crosshair");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Show Crosshair", &g_CrosshairEnabled);
        if (g_CrosshairEnabled)
        {
            ImGui::Indent();
            changed |= ImGui::ColorEdit4("Crosshair Color", g_CrosshairColor);
            ImGui::Unindent();
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextDisabled("For more visual accessibility features install");
        ImGui::TextDisabled("Reffect from the link below.");
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Open Reffect on GitHub"))
        {
            ShellExecuteA(nullptr, "open", "https://github.com/Zerthox/gw2-reffect", nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextDisabled("Then configure visual accessibility features in the");
        ImGui::TextDisabled("GW2Accessibility pack in Reffect settings.");

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Deploy Accessibility Pack to Reffect"))
        {
            std::string dir = GetReffectPacksDir();
            if (!dir.empty())
            {
                std::filesystem::create_directories(dir);
                std::string filepath = dir + "GW2Accessibility.json";
                if (std::filesystem::exists(filepath))
                    std::filesystem::remove(filepath);
                DeployReffectPack();
                g_API->Log(LOGL_INFO, "GW2Accessibility", "[REFFECT] Accessibility pack deployed.");
            }
        }
        ImGui::TextDisabled("Writes GW2Accessibility.json to the Reffect packs folder.");

        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0, 4));
    }

    if (ImGui::CollapsingHeader("Cognitive"))
    {
        ImGui::Indent();

        ImGui::TextUnformatted("Keybind Overlay");
        ImGui::Separator();

        changed |= ImGui::Checkbox("Show Keybind Overlay", &g_KeybindOverlayEnabled);
        if (g_KeybindOverlayEnabled)
        {
            ImGui::Indent();
            const char* visNames[] = { "Always", "In Combat Only", "Out of Combat Only" };
            changed |= ImGui::Combo("Visibility", &g_KeybindVisibility, visNames, 3);
            ImGui::Unindent();
        }
        if (ImGui::Button("Configure Keybinds..."))
            g_ShowKeybindConfig = true;
        ImGui::TextDisabled("Select an exported keybind file from Documents\\Guild Wars 2\\InputBinds\\");

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

// ── Unofficial Extras — Ready Check Detection ──────────────────────────────

static void OnSquadUpdate(const UnofficialExtras::UserInfo* pUsers, uint64_t pCount)
{
    using UnofficialExtras::UserRole;

    for (uint64_t i = 0; i < pCount; i++)
    {
        const UnofficialExtras::UserInfo& u = pUsers[i];
        if (!u.AccountName) continue;

        std::string account = u.AccountName;

        bool prevReady = false;
        auto it = g_SquadReadyStatus.find(account);
        if (it != g_SquadReadyStatus.end())
            prevReady = it->second;

        // Squad leader transitions from not-ready to ready = ready check initiated
        if (u.Role == UserRole::SquadLeader && !prevReady && u.ReadyStatus)
        {
            if (g_ReadyCheckTtsEnabled)
            {
                InitSAPI();
                if (g_TtsReady)
                    SpeakText(L"Ready Check Initiated.");
            }
        }

        // Role::None means the user left the squad — remove from cache
        if (u.Role == UserRole::None)
            g_SquadReadyStatus.erase(account);
        else
            g_SquadReadyStatus[account] = u.ReadyStatus;
    }
}

extern "C" __declspec(dllexport) void arcdps_unofficial_extras_subscriber_init(
    const UnofficialExtras::ExtrasAddonInfo* pExtrasInfo, void* pSubscriberInfo)
{
    if (!pExtrasInfo || pExtrasInfo->ApiVersion != 2) return;

    auto* info = static_cast<UnofficialExtras::ExtrasSubscriberInfoV1*>(pSubscriberInfo);
    info->InfoVersion            = 1;
    info->SubscriberName         = "GW2Accessibility";
    info->SquadUpdateCallback    = OnSquadUpdate;
    info->LanguageChangedCallback = nullptr;
    info->KeyBindChangedCallback  = nullptr;
}

void AddonLoad(AddonAPI_t* aAPI)
{
    g_API = aAPI;
    g_IsWine = DetectWine();
    LoadSettings();
    EnumerateTtsVoices();
    EnumerateTtsOutputDevices();
    InitMumble();
    DeployReffectPack();

    g_API->InputBinds_RegisterWithString(
        "GW2ACCESSIBILITY_MOUSE_TOGGLE",
        OnMouseToggleKeybind,
        "(null)");

    g_API->GUI_Register(RT_Render, OnRender);
    g_API->GUI_Register(RT_OptionsRender, OnOptionsRender);
    g_API->GUI_Register(RT_Render, OnMechanicsWindowRender);
    g_API->GUI_Register(RT_Render, OnEffectManagerRender);
    g_API->GUI_Register(RT_Render, OnKeybindOverlayRender);
    g_API->GUI_Register(RT_Render, OnKeybindConfigRender);
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
        g_API->GUI_Deregister(OnEffectManagerRender);
        g_API->GUI_Deregister(OnKeybindOverlayRender);
        g_API->GUI_Deregister(OnKeybindConfigRender);
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
    static AddonVersion_t s_Version = { 0, 2, 3, 0 };

    static AddonDefinition_t s_Def = {};
    s_Def.Signature  = 897849539; // 0x358418C3 — random 4 bytes, no longer -2
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
