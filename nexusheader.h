
#ifndef NEXUS_H
#define NEXUS_H

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#include <stdbool.h>
#endif
#include <windows.h>

#define NEXUS_API_VERSION 6

/* DataLink identifiers */
#define DL_NEXUS_LINK "DL_NEXUS_LINK"
#ifndef DL_MUMBLE_LINK
#define DL_MUMBLE_LINK "DL_MUMBLE_LINK"
#endif
#ifndef DL_MUMBLE_LINK_IDENTITY
#define DL_MUMBLE_LINK_IDENTITY "DL_MUMBLE_LINK_IDENTITY"
#endif

/* Event identifiers */
#define EV_WINDOW_RESIZED          "EV_WINDOW_RESIZED"          // Payload is nullptr. Refer to Nexus DataLink for dimensions.
#define EV_MUMBLE_IDENTITY_UPDATED "EV_MUMBLE_IDENTITY_UPDATED" // Payload is Mumble::Identity*
#define EV_ADDON_LOADED            "EV_ADDON_LOADED"            // Payload is uint32_t*
#define EV_ADDON_UNLOADED          "EV_ADDON_UNLOADED"          // Payload is uint32_t*

typedef enum ERenderType
{
    RT_PreRender,
    RT_Render,
    RT_PostRender,
    RT_OptionsRender
} ERenderType;

typedef enum EMHStatus
{
    MH_UNKNOWN = -1,
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED,
    MH_ERROR_NOT_CREATED,
    MH_ERROR_ENABLED,
    MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE,
    MH_ERROR_UNSUPPORTED_FUNCTION,
    MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT,
    MH_ERROR_MODULE_NOT_FOUND,
    MH_ERROR_FUNCTION_NOT_FOUND
} EMHStatus;

typedef enum ELogLevel
{
    LOGL_CRITICAL = 1,
    LOGL_WARNING = 2,
    LOGL_INFO = 3,
    LOGL_DEBUG = 4,
    LOGL_TRACE = 5
} ELogLevel;

typedef enum EGameBinds
{
    // Movement
    GB_MoveForward = 0,
    GB_MoveBackward = 1,
    GB_MoveLeft = 2,
    GB_MoveRight = 3,
    GB_MoveTurnLeft = 4,
    GB_MoveTurnRight = 5,
    GB_MoveDodge = 6,
    GB_MoveAutoRun = 7,
    GB_MoveWalk = 8,
    GB_MoveJump_SwimUp_FlyUp = 9,
    GB_MoveSwimDown_FlyDown = 11,
    GB_MoveAboutFace = 12,

    // Skills
    GB_SkillWeaponSwap = 17,
    GB_SkillWeapon1 = 18,
    GB_SkillWeapon2 = 19,
    GB_SkillWeapon3 = 20,
    GB_SkillWeapon4 = 21,
    GB_SkillWeapon5 = 22,
    GB_SkillHeal = 23,
    GB_SkillUtility1 = 24,
    GB_SkillUtility2 = 25,
    GB_SkillUtility3 = 26,
    GB_SkillElite = 27,
    GB_SkillProfession1 = 28,
    GB_SkillProfession2 = 29,
    GB_SkillProfession3 = 30,
    GB_SkillProfession4 = 31,
    GB_SkillProfession5 = 79,
    GB_SkillProfession6 = 201,
    GB_SkillProfession7 = 202,
    GB_SkillSpecialAction = 82,

    // Targeting
    GB_TargetAlert = 131,
    GB_TargetCall = 32,
    GB_TargetTake = 33,
    GB_TargetCallLocal = 199,
    GB_TargetTakeLocal = 200,
    GB_TargetEnemyNearest = 34,
    GB_TargetEnemyNext = 35,
    GB_TargetEnemyPrev = 36,
    GB_TargetAllyNearest = 37,
    GB_TargetAllyNext = 38,
    GB_TargetAllyPrev = 39,
    GB_TargetLock = 40,
    GB_TargetSnapGroundTarget = 80,
    GB_TargetSnapGroundTargetToggle = 115,
    GB_TargetAutoTargetingDisable = 116,
    GB_TargetAutoTargetingToggle = 117,
    GB_TargetAllyTargetingMode = 197,
    GB_TargetAllyTargetingModeToggle = 198,

    // UI Binds
    GB_UiCommerce = 41, // TradingPost
    GB_UiContacts = 42,
    GB_UiGuild = 43,
    GB_UiHero = 44,
    GB_UiInventory = 45,
    GB_UiKennel = 46, // Pets
    GB_UiLogout = 47,
    GB_UiMail = 71,
    GB_UiOptions = 48,
    GB_UiParty = 49,
    GB_UiPvp = 73,
    GB_UiPvpBuild = 75,
    GB_UiScoreboard = 50,
    GB_UiSeasonalObjectivesShop = 209, // Wizard's Vault
    GB_UiInformation = 51,
    GB_UiChatToggle = 70,
    GB_UiChatCommand = 52,
    GB_UiChatFocus = 53,
    GB_UiChatReply = 54,
    GB_UiToggle = 55,
    GB_UiSquadBroadcastChatToggle = 85,
    GB_UiSquadBroadcastChatCommand = 83,
    GB_UiSquadBroadcastChatFocus = 84,

    // Camera
    GB_CameraFree = 13,
    GB_CameraZoomIn = 14,
    GB_CameraZoomOut = 15,
    GB_CameraReverse = 16,
    GB_CameraActionMode = 78,
    GB_CameraActionModeDisable = 114,

    // Screenshots
    GB_ScreenshotNormal = 56,
    GB_ScreenshotStereoscopic = 57,

    // Map
    GB_MapToggle = 59,
    GB_MapFocusPlayer = 60,
    GB_MapFloorDown = 61,
    GB_MapFloorUp = 62,
    GB_MapZoomIn = 63,
    GB_MapZoomOut = 64,

    // Mounts
    GB_SpumoniToggle = 152,
    GB_SpumoniMovement = 130,
    GB_SpumoniSecondaryMovement = 153,
    GB_SpumoniMAM01 = 155, // Raptor
    GB_SpumoniMAM02 = 156, // Springer
    GB_SpumoniMAM03 = 157, // Skimmer
    GB_SpumoniMAM04 = 158, // Jackal
    GB_SpumoniMAM05 = 159, // Griffon
    GB_SpumoniMAM06 = 161, // RollerBeetle
    GB_SpumoniMAM07 = 169, // Warclaw
    GB_SpumoniMAM08 = 170, // Skyscale
    GB_SpumoniMAM09 = 203, // SiegeTurtle

    // Spectator Binds
    GB_SpectatorNearestFixed = 102,
    GB_SpectatorNearestPlayer = 103,
    GB_SpectatorPlayerRed1 = 104,
    GB_SpectatorPlayerRed2 = 105,
    GB_SpectatorPlayerRed3 = 106,
    GB_SpectatorPlayerRed4 = 107,
    GB_SpectatorPlayerRed5 = 108,
    GB_SpectatorPlayerBlue1 = 109,
    GB_SpectatorPlayerBlue2 = 110,
    GB_SpectatorPlayerBlue3 = 111,
    GB_SpectatorPlayerBlue4 = 112,
    GB_SpectatorPlayerBlue5 = 113,
    GB_SpectatorFreeCamera = 120,
    GB_SpectatorFreeCameraMode = 127,
    GB_SpectatorFreeMoveForward = 121,
    GB_SpectatorFreeMoveBackward = 122,
    GB_SpectatorFreeMoveLeft = 123,
    GB_SpectatorFreeMoveRight = 124,
    GB_SpectatorFreeMoveUp = 125,
    GB_SpectatorFreeMoveDown = 126,

    // Squad Markers
    GB_SquadMarkerPlaceWorld1 = 86, // Arrow
    GB_SquadMarkerPlaceWorld2 = 87, // Circle
    GB_SquadMarkerPlaceWorld3 = 88, // Heart
    GB_SquadMarkerPlaceWorld4 = 89, // Square
    GB_SquadMarkerPlaceWorld5 = 90, // Star
    GB_SquadMarkerPlaceWorld6 = 91, // Swirl
    GB_SquadMarkerPlaceWorld7 = 92, // Triangle
    GB_SquadMarkerPlaceWorld8 = 93, // Cross
    GB_SquadMarkerClearAllWorld = 119,
    GB_SquadMarkerSetAgent1 = 94, // Arrow
    GB_SquadMarkerSetAgent2 = 95, // Circle
    GB_SquadMarkerSetAgent3 = 96, // Heart
    GB_SquadMarkerSetAgent4 = 97, // Square
    GB_SquadMarkerSetAgent5 = 98, // Star
    GB_SquadMarkerSetAgent6 = 99, // Swirl
    GB_SquadMarkerSetAgent7 = 100, // Triangle
    GB_SquadMarkerSetAgent8 = 101, // Cross
    GB_SquadMarkerClearAllAgent = 118,

    // Mastery Skills
    GB_MasteryAccess = 196,
    GB_MasteryAccess01 = 204, // Fishing
    GB_MasteryAccess02 = 205, // Skiff
    GB_MasteryAccess03 = 206, // Jade Bot Waypoint
    GB_MasteryAccess04 = 207, // Rift Scan
    GB_MasteryAccess05 = 208, // Skyscale
    GB_MasteryAccess06 = 211, // Homestead Doorway

    // Miscellaneous Binds
    GB_MiscAoELoot = 74,
    GB_MiscInteract = 65,
    GB_MiscShowEnemies = 66,
    GB_MiscShowAllies = 67,
    GB_MiscCombatStance = 68, // Stow/Draw
    GB_MiscToggleLanguage = 69,
    GB_MiscTogglePetCombat = 76,
    GB_MiscToggleFullScreen = 160,
    GB_MiscToggleDecorationMode = 210, // DecorateMode

    // Toys/Novelties
    GB_ToyUseDefault = 162,
    GB_ToyUseSlot1 = 163, // Chair
    GB_ToyUseSlot2 = 164, // Instrument
    GB_ToyUseSlot3 = 165, // Held Item
    GB_ToyUseSlot4 = 166, // Toy
    GB_ToyUseSlot5 = 167, // Tonic
    //GB_ToyUseSlot6 unused

    // Build Templates
    GB_Loadout1 = 171,
    GB_Loadout2 = 172,
    GB_Loadout3 = 173,
    GB_Loadout4 = 174,
    GB_Loadout5 = 175,
    GB_Loadout6 = 176,
    GB_Loadout7 = 177,
    GB_Loadout8 = 178,
    GB_Loadout9 = 179,

    // Equipment Templates
    GB_GearLoadout1 = 182,
    GB_GearLoadout2 = 183,
    GB_GearLoadout3 = 184,
    GB_GearLoadout4 = 185,
    GB_GearLoadout5 = 186,
    GB_GearLoadout6 = 187,
    GB_GearLoadout7 = 188,
    GB_GearLoadout8 = 189,
    GB_GearLoadout9 = 190
} EGameBinds;

typedef enum EAddonFlags
{
    AF_None              = 0,
    AF_IsVolatile        = 1 << 0, /* Makes the addon automatically disable, if the game updated. */
    AF_DisableHotloading = 1 << 1, /* Prevents the addon from being unloaded at runtime. Unload will still be called on shutdown, if defined. */
    AF_LaunchOnly        = 1 << 2  /* Prevents the addon from getting loaded at runtime after the initial game launch. */
} EAddonFlags;

typedef enum EUpdateProvider
{
    UP_None     = 0, /* No provider. Nexus does not check for updates. */
    UP_Raidcore = 1, /* Updates are being provided by raidcore.gg */
    UP_GitHub   = 2, /* Updates are being provided via GitHub releases. */
    UP_Direct   = 3, /* Updates are being provided via a direct file link. */
    UP_Self     = 4  /* Provider is the addon itself. Addon is supposed to check for an update and provide Nexus with a link. */
} EUpdateProvider;

typedef struct Keybind_t
{
    uint16_t Key;
    bool     Alt;
    bool     Ctrl;
    bool     Shift;
} Keybind_t;

typedef struct Texture_t
{
    uint32_t Width;
    uint32_t Height;
    void*    Resource; // ID3D11ShaderResourceView*
} Texture_t;

typedef struct NexusLinkData_t
{
    uint32_t Width;
    uint32_t Height;
    float    Scaling;

    bool     IsMoving;
    bool     IsCameraMoving;
    bool     IsGameplay;

    void*    Font;    // ImFont*
    void*    FontBig; // ImFont*
    void*    FontUI;  // ImFont*
} NexusLinkData_t;

typedef struct AddonVersion_t
{
    uint16_t Major;
    uint16_t Minor;
    uint16_t Build;
    uint16_t Revision;
} AddonVersion_t;

struct AddonAPI_t; /* prototype for load function */

typedef void        (*ADDON_LOAD)                       (AddonAPI_t* aAPI);
typedef void        (*ADDON_UNLOAD)                     ();

typedef void        (*GUI_RENDER)                       ();
typedef void        (*GUI_ADDRENDER)                    (ERenderType aRenderType, GUI_RENDER aRenderCallback);
typedef void        (*GUI_REMRENDER)                    (GUI_RENDER aRenderCallback);
typedef void        (*GUI_REGISTERCLOSEONESCAPE)        (const char* aWindowName, bool* aIsVisible);
typedef void        (*GUI_DEREGISTERCLOSEONESCAPE)      (const char* aWindowName);

typedef void        (*UPDATER_REQUESTUPDATE)            (uint32_t aSignature, const char* aUpdateURL);

typedef const char* (*PATHS_GETGAMEDIR)                 ();
typedef const char* (*PATHS_GETADDONDIR)                (const char* aName);
typedef const char* (*PATHS_GETCOMMONDIR)               ();

typedef EMHStatus   (__stdcall* MINHOOK_CREATE)         (LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal);
typedef EMHStatus   (__stdcall* MINHOOK_REMOVE)         (LPVOID pTarget);
typedef EMHStatus   (__stdcall* MINHOOK_ENABLE)         (LPVOID pTarget);
typedef EMHStatus   (__stdcall* MINHOOK_DISABLE)        (LPVOID pTarget);

typedef void        (*LOGGER_LOG)                       (ELogLevel aLogLevel, const char* aChannel, const char* aStr);

typedef void        (*ALERTS_NOTIFY)                    (const char* aMessage);

typedef void        (*EVENT_CONSUME)                    (void* aEventArgs);
typedef void        (*EVENTS_RAISE)                     (const char* aIdentifier, void* aEventData);
typedef void        (*EVENTS_RAISENOTIFICATION)         (const char* aIdentifier);
typedef void        (*EVENTS_RAISE_TARGETED)            (uint32_t aSignature, const char* aIdentifier, void* aEventData);
typedef void        (*EVENTS_RAISENOTIFICATION_TARGETED)(uint32_t aSignature, const char* aIdentifier);
typedef void        (*EVENTS_SUBSCRIBE)                 (const char* aIdentifier, EVENT_CONSUME aConsumeEventCallback);

typedef UINT        (*WNDPROC_CALLBACK)                 (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
typedef void        (*WNDPROC_ADDREM)                   (WNDPROC_CALLBACK aWndProcCallback);
typedef LRESULT     (*WNDPROC_SENDTOGAME)               (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

typedef void        (*INPUTBINDS_PROCESS)               (const char* aIdentifier, bool aIsRelease);
typedef void        (*INPUTBINDS_INVOKE)                (const char* aIdentifier, bool aIsRelease);
typedef void        (*INPUTBINDS_REGISTERWITHSTRING)    (const char* aIdentifier, INPUTBINDS_PROCESS aKeybindHandler, const char* aKeybind);
typedef void        (*INPUTBINDS_REGISTERWITHSTRUCT)    (const char* aIdentifier, INPUTBINDS_PROCESS aKeybindHandler, Keybind_t aKeybind);
typedef void        (*INPUTBINDS_DEREGISTER)            (const char* aIdentifier);

typedef void        (*GAMEBINDS_PRESSASYNC)             (EGameBinds aGameBind);
typedef void        (*GAMEBINDS_RELEASEASYNC)           (EGameBinds aGameBind);
typedef void        (*GAMEBINDS_INVOKEASYNC)            (EGameBinds aGameBind, int32_t aDuration);
typedef void        (*GAMEBINDS_PRESS)                  (EGameBinds aGameBind);
typedef void        (*GAMEBINDS_RELEASE)                (EGameBinds aGameBind);
typedef bool        (*GAMEBINDS_ISBOUND)                (EGameBinds aGameBind);

typedef void*       (*DATALINK_GET)                     (const char* aIdentifier);
typedef void*       (*DATALINK_SHARE)                   (const char* aIdentifier, uint64_t aResourceSize);

typedef void        (*TEXTURES_RECEIVECALLBACK)         (const char* aIdentifier, Texture_t* aTexture);
typedef Texture_t*  (*TEXTURES_GET)                     (const char* aIdentifier);
typedef Texture_t*  (*TEXTURES_GETORCREATEFROMFILE)     (const char* aIdentifier, const char* aFilename);
typedef Texture_t*  (*TEXTURES_GETORCREATEFROMRESOURCE) (const char* aIdentifier, uint32_t aResourceID, HMODULE aModule);
typedef Texture_t*  (*TEXTURES_GETORCREATEFROMURL)      (const char* aIdentifier, const char* aRemote, const char* aEndpoint);
typedef Texture_t*  (*TEXTURES_GETORCREATEFROMMEMORY)   (const char* aIdentifier, void* aData, uint64_t aSize);
typedef void        (*TEXTURES_LOADFROMFILE)            (const char* aIdentifier, const char* aFilename, TEXTURES_RECEIVECALLBACK aCallback);
typedef void        (*TEXTURES_LOADFROMRESOURCE)        (const char* aIdentifier, uint32_t aResourceID, HMODULE aModule, TEXTURES_RECEIVECALLBACK aCallback);
typedef void        (*TEXTURES_LOADFROMURL)             (const char* aIdentifier, const char* aRemote, const char* aEndpoint, TEXTURES_RECEIVECALLBACK aCallback);
typedef void        (*TEXTURES_LOADFROMMEMORY)          (const char* aIdentifier, void* aData, uint64_t aSize, TEXTURES_RECEIVECALLBACK aCallback);

typedef void        (*QUICKACCESS_ADDSHORTCUT)          (const char* aIdentifier, const char* aTextureIdentifier, const char* aTextureHoverIdentifier, const char* aKeybindIdentifier, const char* aTooltipText);
typedef void        (*QUICKACCESS_ADDSIMPLE)            (const char* aIdentifier, const char* aTargetShortcutIdentifier, GUI_RENDER aShortcutRenderCallback);
typedef void        (*QUICKACCESS_GENERIC)              (const char* aIdentifier);

typedef const char* (*LOCALIZATION_TRANSLATE)           (const char* aIdentifier);
typedef const char* (*LOCALIZATION_TRANSLATETO)         (const char* aIdentifier, const char* aLanguageIdentifier);
typedef void        (*LOCALIZATION_SET)                 (const char* aIdentifier, const char* aLanguageIdentifier, const char* aString);

typedef void        (*FONTS_RECEIVECALLBACK)            (const char* aIdentifier, void* aFont);
typedef void        (*FONTS_GETRELEASE)                 (const char* aIdentifier, FONTS_RECEIVECALLBACK aCallback);
typedef void        (*FONTS_ADDFROMFILE)                (const char* aIdentifier, float aFontSize, const char* aFilename, FONTS_RECEIVECALLBACK aCallback, void* aConfig);
typedef void        (*FONTS_ADDFROMRESOURCE)            (const char* aIdentifier, float aFontSize, uint32_t aResourceID, HMODULE aModule, FONTS_RECEIVECALLBACK aCallback, void* aConfig);
typedef void        (*FONTS_ADDFROMMEMORY)              (const char* aIdentifier, float aFontSize, void* aData, uint64_t aSize, FONTS_RECEIVECALLBACK aCallback, void* aConfig);
typedef void        (*FONTS_RESIZE)                     (const char* aIdentifier, float aFontSize);

typedef struct AddonDefinition_t
{
    /* required */
    uint32_t        Signature;   /* Addon ID: Set to unique integer. */
    uint32_t        APIVersion;  /* Determines which AddonAPI struct revision the Loader will pass, use the NEXUS_API_VERSION define from Nexus.h */
    const char*     Name;        /* Name of the addon as shown in the library */
    AddonVersion_t  Version;
    const char*     Author;      /* Author of the addon */
    const char*     Description; /* Short description */
    ADDON_LOAD      Load;        /* Pointer to Load Function of the addon */
    ADDON_UNLOAD    Unload;      /* Pointer to Unload Function of the addon. Not required if EAddonFlags::DisableHotloading is set. */
    EAddonFlags     Flags;       /* Information about the addon */

    /* update fallback */
    EUpdateProvider Provider;    /* What platform is the the addon hosted on */
    const char*     UpdateLink;  /* Link to the update resource */
} AddonDefinition_t;

typedef struct AddonAPI_t
{
    /* Renderer */
    void* SwapChain;    // IDXGISwapChain*
    void* ImguiContext; // ImGuiContext*
    void* ImguiMalloc;
    void* ImguiFree;

    GUI_ADDRENDER                     GUI_Register;
    GUI_REMRENDER                     GUI_Deregister;

    UPDATER_REQUESTUPDATE             RequestUpdate;

    /* Logging */
    LOGGER_LOG                        Log;

    ALERTS_NOTIFY                     GUI_SendAlert;
    GUI_REGISTERCLOSEONESCAPE         GUI_RegisterCloseOnEscape;
    GUI_DEREGISTERCLOSEONESCAPE       GUI_DeregisterCloseOnEscape;

    PATHS_GETGAMEDIR                  Paths_GetGameDirectory;
    PATHS_GETADDONDIR                 Paths_GetAddonDirectory;
    PATHS_GETCOMMONDIR                Paths_GetCommonDirectory;

    MINHOOK_CREATE                    MinHook_Create;
    MINHOOK_REMOVE                    MinHook_Remove;
    MINHOOK_ENABLE                    MinHook_Enable;
    MINHOOK_DISABLE                   MinHook_Disable;

    EVENTS_RAISE                      Events_Raise;
    EVENTS_RAISENOTIFICATION          Events_RaiseNotification;
    EVENTS_RAISE_TARGETED             Events_RaiseTargeted;
    EVENTS_RAISENOTIFICATION_TARGETED Events_RaiseNotificationTargeted;
    EVENTS_SUBSCRIBE                  Events_Subscribe;
    EVENTS_SUBSCRIBE                  Events_Unsubscribe;

    WNDPROC_ADDREM                    WndProc_Register;
    WNDPROC_ADDREM                    WndProc_Deregister;
    WNDPROC_SENDTOGAME                WndProc_SendToGameOnly;

    INPUTBINDS_INVOKE                 InputBinds_Invoke;
    INPUTBINDS_REGISTERWITHSTRING     InputBinds_RegisterWithString;
    INPUTBINDS_REGISTERWITHSTRUCT     InputBinds_RegisterWithStruct;
    INPUTBINDS_DEREGISTER             InputBinds_Deregister;

    GAMEBINDS_PRESSASYNC              GameBinds_PressAsync;
    GAMEBINDS_RELEASEASYNC            GameBinds_ReleaseAsync;
    GAMEBINDS_INVOKEASYNC             GameBinds_InvokeAsync;
    GAMEBINDS_PRESS                   GameBinds_Press;
    GAMEBINDS_RELEASE                 GameBinds_Release;
    GAMEBINDS_ISBOUND                 GameBinds_IsBound;

    DATALINK_GET                      DataLink_Get;
    DATALINK_SHARE                    DataLink_Share;

    TEXTURES_GET                      Textures_Get;
    TEXTURES_GETORCREATEFROMFILE      Textures_GetOrCreateFromFile;
    TEXTURES_GETORCREATEFROMRESOURCE  Textures_GetOrCreateFromResource;
    TEXTURES_GETORCREATEFROMURL       Textures_GetOrCreateFromURL;
    TEXTURES_GETORCREATEFROMMEMORY    Textures_GetOrCreateFromMemory;
    TEXTURES_LOADFROMFILE             Textures_LoadFromFile;
    TEXTURES_LOADFROMRESOURCE         Textures_LoadFromResource;
    TEXTURES_LOADFROMURL              Textures_LoadFromURL;
    TEXTURES_LOADFROMMEMORY           Textures_LoadFromMemory;

    QUICKACCESS_ADDSHORTCUT           QuickAccess_Add;
    QUICKACCESS_GENERIC               QuickAccess_Remove;
    QUICKACCESS_GENERIC               QuickAccess_Notify;
    QUICKACCESS_ADDSIMPLE             QuickAccess_AddContextMenu;
    QUICKACCESS_GENERIC               QuickAccess_RemoveContextMenu;

    LOCALIZATION_TRANSLATE            Localization_Translate;
    LOCALIZATION_TRANSLATETO          Localization_TranslateTo;
    LOCALIZATION_SET                  Localization_Set;

    FONTS_GETRELEASE                  Fonts_Get;
    FONTS_GETRELEASE                  Fonts_Release;
    FONTS_ADDFROMFILE                 Fonts_AddFromFile;
    FONTS_ADDFROMRESOURCE             Fonts_AddFromResource;
    FONTS_ADDFROMMEMORY               Fonts_AddFromMemory;
    FONTS_RESIZE                      Fonts_Resize;
} AddonAPI_t;

#endif