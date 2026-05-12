#include "addon.h"
#include "Chat.h"
#include <imgui.h>
#include <windows.h>
#include <sapi.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <algorithm>

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
} g_Tts;

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

    nlohmann::json tts;
    tts["enabled"]       = g_Tts.enabled;
    tts["party"]         = g_Tts.party;
    tts["squad"]         = g_Tts.squad;
    tts["whisper"]       = g_Tts.whisper;
    tts["map"]           = g_Tts.map;
    tts["local"]         = g_Tts.local;
    tts["guild"]         = g_Tts.guild;
    tts["squadBroadcast"] = g_Tts.squadBroadcast;
    j["tts"] = tts;

    std::string path = GetConfigPath();
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
    if (!g_Enabled || aIsRelease) return;
    ToggleMousePosition();
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

static void SpeakText(const wchar_t* text)
{
    if (!g_pVoice || !text || !*text) return;
    g_pVoice->Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
}

static void OnChatMessage(void* aEventArgs)
{
    if (!g_Tts.enabled) return;
    auto* msg = static_cast<const Message*>(aEventArgs);

    InitSAPI();
    if (!g_TtsReady) return;

    std::string ttsText;

    switch (msg->Type)
    {
        case Party:
        {
            if (!g_Tts.party) return;
            const char* name = msg->Local.CharacterName;
            const char* text = msg->Local.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Party: ") + name + " says: " + text;
            else
                ttsText = std::string("Party: ") + text;
            break;
        }

        case Squad:
        {
            if (!g_Tts.squad) return;
            const char* name = msg->Local.CharacterName;
            const char* text = msg->Local.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Squad: ") + name + " says: " + text;
            else
                ttsText = std::string("Squad: ") + text;
            break;
        }

        case Map:
        {
            if (!g_Tts.map) return;
            const char* name = msg->Local.CharacterName;
            const char* text = msg->Local.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Map: ") + name + " says: " + text;
            else
                ttsText = std::string("Map: ") + text;
            break;
        }

        case Local:
        {
            if (!g_Tts.local) return;
            const char* name = msg->Local.CharacterName;
            const char* text = msg->Local.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Local: ") + name + " says: " + text;
            else
                ttsText = std::string("Local: ") + text;
            break;
        }

        case Whisper:
        {
            if (!g_Tts.whisper) return;
            if (msg->Flags & Whisper_IsFromMe) return;
            const char* name = msg->Whisper.CharacterName;
            const char* text = msg->Whisper.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Whisper from ") + name + ": " + text;
            else
                ttsText = std::string("Whisper: ") + text;
            break;
        }

        case SquadBroadcast:
        {
            if (!g_Tts.squadBroadcast) return;
            const char* text = msg->SquadMessage;
            if (!text || !*text) return;
            ttsText = std::string("Squad broadcast: ") + text;
            break;
        }

        case Guild:
        {
            if (!g_Tts.guild) return;
            const char* name = msg->Guild.Base.CharacterName;
            const char* text = msg->Guild.Base.Content;
            if (!text || !*text) return;
            if (name && *name)
                ttsText = std::string("Guild: ") + name + " says: " + text;
            else
                ttsText = std::string("Guild: ") + text;
            break;
        }

        case SquadMessage:
        {
            if (!g_Tts.squadBroadcast) return;
            const char* text = msg->SquadMessage;
            if (!text || !*text) return;
            ttsText = std::string("Squad message: ") + text;
            break;
        }

        default:
            return;
    }

    if (ttsText.empty()) return;
    std::wstring wide = Utf8ToWide(ttsText.c_str());
    SpeakText(wide.c_str());
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
}

// ── Options UI ───────────────────────────────────────────────────────────────

static void OnOptionsRender()
{
    SetupImGui();
    bool changed = false;

    // ── Mouse Position Section ──
    ImGui::TextUnformatted("Mouse Position Toggle");
    ImGui::Separator();

    changed |= ImGui::Checkbox("Enable", &g_Enabled);

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
        changed |= ImGui::Checkbox("Guild",        &g_Tts.guild);

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

    g_API->InputBinds_RegisterWithString(
        "GW2ACCESSIBILITY_MOUSE_TOGGLE",
        OnMouseToggleKeybind,
        "(null)");

    g_API->GUI_Register(RT_Render, OnRender);
    g_API->GUI_Register(RT_OptionsRender, OnOptionsRender);
    g_API->Events_Subscribe(GW2_CHAT_EVENT, OnChatMessage);
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
    }

    if (g_pVoice)
    {
        g_pVoice->Release();
        g_pVoice = nullptr;
    }

    g_API = nullptr;
    g_ImguiSetupDone = false;
    g_TtsReady = false;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    static AddonVersion_t s_Version = { 1, 0, 0, 0 };

    static AddonDefinition_t s_Def = {};
    s_Def.Signature  = -2;
    s_Def.APIVersion = NEXUS_API_VERSION;
    s_Def.Name       = "GW2Accessibility";
    s_Def.Version    = s_Version;
    s_Def.Author     = "anomalyco";
    s_Def.Description= "Accessibility features for Guild Wars 2";
    s_Def.Load       = AddonLoad;
    s_Def.Unload     = AddonUnload;
    s_Def.Flags      = AF_None;
    s_Def.Provider   = UP_None;
    s_Def.UpdateLink = nullptr;

    return &s_Def;
}
