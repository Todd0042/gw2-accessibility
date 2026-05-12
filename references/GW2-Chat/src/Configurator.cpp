#include "Configurator.hpp"
#include <Nexus.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

std::bitset<magic_enum::enum_count<GW2RE::EChatChannel>()>
    GW2::Chat::Configurator::ChannelMask = {};

GW2::Chat::Configurator::Configurator(AddonAPI *aApi) : Api(aApi) {
  ChannelMask = {};
  auto filepath =
      std::filesystem::path(Api->Paths.GetAddonDirectory(ADDON_NAME)) /
      ConfigFilename;
  if (std::filesystem::exists(filepath)) {
    auto stream = std::ifstream(filepath);
    auto json = nlohmann::json::object();
    json = nlohmann::json::parse(stream, nullptr, false);
    std::vector<std::string> channels;
    json.at("EnabledChannels").get_to(channels);
    for (const auto &channel : channels) {
      if (auto parsed = magic_enum::enum_cast<GW2RE::EChatChannel>(channel)) {
        if (auto it = std::find(AvailableChannels.begin(),
                                AvailableChannels.end(), parsed.value());
            it != AvailableChannels.end())
          ChannelMask.set(static_cast<std::size_t>(parsed.value()));
        else
          aApi->Log(
              ELogLevel_WARNING, ADDON_NAME,
              std::format("Config file contains unavailable channel \"{}\"",
                          channel)
                  .c_str());
      } else
        aApi->Log(ELogLevel_WARNING, ADDON_NAME,
                  std::format("Config file contains unparsable channel \"{}\"",
                              channel)
                      .c_str());
    }
  } else {
    for (const auto &channel : AvailableChannels)
      ChannelMask.set(static_cast<std::size_t>(channel));
  }
  ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext *>(Api->ImguiContext));
  ImGui::SetAllocatorFunctions(
      reinterpret_cast<void *(*)(size_t, void *)>(Api->ImguiMalloc),
      reinterpret_cast<void (*)(void *, void *)>(Api->ImguiFree));
  Api->Renderer.Register(ERenderType_OptionsRender, Configurator::Draw);
}
GW2::Chat::Configurator::~Configurator() {
  auto filepath =
      std::filesystem::path(Api->Paths.GetAddonDirectory(ADDON_NAME)) /
      ConfigFilename;
  auto channels = std::vector<std::string>();
  if (ChannelMask.any()) {
    for (const auto &channel : AvailableChannels) {
      if (ChannelMask.test(static_cast<std::size_t>(channel)))
        channels.emplace_back(magic_enum::enum_name(channel));
    }
  }
  if (channels.empty()) {
    if (std::filesystem::exists(filepath))
      std::filesystem::remove_all(filepath.parent_path());
  } else {
    if (!std::filesystem::exists(filepath.parent_path()))
      std::filesystem::create_directories(filepath.parent_path());
    auto json = nlohmann::json::object();
    json["EnabledChannels"] = channels;
    std::ofstream(filepath) << json;
  }
  Api->Renderer.Deregister(Configurator::Draw);
}
void GW2::Chat::Configurator::Draw() {
  for (const auto &channel : AvailableChannels) {
    auto enabled = ChannelMask.test(static_cast<std::size_t>(channel));
    if (ImGui::Checkbox(magic_enum::enum_name(channel).data(), &enabled))
      ChannelMask.flip(static_cast<std::size_t>(channel));
  }
}
