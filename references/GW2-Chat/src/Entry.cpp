#include "Configurator.hpp"
#include "Handler.hpp"
#include <Chat.h>
#include <Nexus.h>
#include <exception>
#include <memory>

void AddonLoad(AddonAPI *aApi);
void AddonUnload();

extern "C" __declspec(dllexport) AddonDefinition *GetAddonDef();

AddonDefinition *GetAddonDef() {
  static AddonDefinition def{
      .Signature = GW2_CHAT_SIGNATURE,
      .APIVersion = NEXUS_API_VERSION,
      .Name = "Events: " ADDON_NAME,
      .Version = AddonVersion{ADDON_VERSION_MAJOR, ADDON_VERSION_MINOR,
                              ADDON_VERSION_PATCH, ADDON_VERSION_REVISION},
      .Author = "Vonsh.1427",
      .Description =
          "Provides game's chat messages as events for other addons.",
      .Load = AddonLoad,
      .Unload = AddonUnload,
      .Flags = EAddonFlags_None,
      .Provider = EUpdateProvider_GitHub,
      .UpdateLink = "https://github.com/jsantorek/GW2-" ADDON_NAME,
  };
  return &def;
}

static std::unique_ptr<GW2::Chat::Configurator> Config = nullptr;
void AddonLoad(AddonAPI *aApi) {
  Config = std::make_unique<GW2::Chat::Configurator>(aApi);
  try {
    GW2::Chat::Handler::Enable(aApi);
  } catch (const std::exception &e) {
    aApi->Log(ELogLevel_CRITICAL, ADDON_NAME, "Failed to install handler");
    aApi->Log(ELogLevel_CRITICAL, ADDON_NAME, e.what());
  }
}

void AddonUnload() {
  GW2::Chat::Handler::Disable();
  Config.reset();
}
