#pragma once
#include <Game/Chat/EChatChannel.h>
#include <Nexus.h>
#include <array>
#include <bitset>
#include <magic_enum/magic_enum.hpp>

namespace GW2::Chat {
class Configurator {
public:
  Configurator(AddonAPI *aApi);
  ~Configurator();

  static std::bitset<magic_enum::enum_count<GW2RE::EChatChannel>()> ChannelMask;

private:
  static void Draw();
  static constexpr auto ConfigFilename = "config.json";
  static constexpr auto AvailableChannels = std::array<GW2RE::EChatChannel, 13>{
      GW2RE::EChatChannel::Squad,   GW2RE::EChatChannel::Local,
      GW2RE::EChatChannel::Map,     GW2RE::EChatChannel::Party,
      GW2RE::EChatChannel::Team,    GW2RE::EChatChannel::Guild_1,
      GW2RE::EChatChannel::Guild_2, GW2RE::EChatChannel::Guild_3,
      GW2RE::EChatChannel::Guild_4, GW2RE::EChatChannel::Guild_5,
      GW2RE::EChatChannel::Guild_6, GW2RE::EChatChannel::Whisper,
      GW2RE::EChatChannel::Emotes};
  AddonAPI *Api = nullptr;
};
} // namespace GW2::Chat
