#ifndef BUFF_NAMES_H
#define BUFF_NAMES_H

#include <unordered_map>

// Built-in mapping of common buff/effect/debuff IDs from arcdps community wiki
static const std::unordered_map<unsigned int, const char*> g_BuiltinBuffNames = {
    // Boons
    {740, "Might"}, {725, "Fury"}, {1187, "Quickness"}, {717, "Protection"},
    {718, "Regeneration"}, {17674, "Regeneration"}, {719, "Swiftness"},
    {726, "Vigor"}, {743, "Aegis"}, {17675, "Aegis"}, {1122, "Stability"},
    {873, "Retaliation"}, {26980, "Resistance"},
    // Conditions
    {736, "Bleeding"}, {737, "Burning"}, {861, "Confusion"},
    {723, "Poison"}, {19426, "Torment"}, {720, "Blind"},
    {722, "Chilled"}, {721, "Crippled"}, {727, "Immobile"},
    {742, "Weakness"}, {738, "Vulnerability"}, {26766, "Slow"},
    // Control effects (also used as debuff IDs in combat events)
    {791, "Fear"}, {872, "Stun"}, {833, "Daze"}, {27705, "Taunt"},
    {26646, "Battle Scars"},
    // Common effects
    {762, "Determined"}, {895, "Determined"}, {11641, "Determined"},
    {757, "Invulnerable"}, {903, "Righteous Indignation"},
    {36143, "Destruction Immunity"}, {29065, "Tough Hide"},
    {5974, "Super Speed"}, {5543, "Mist Form"},
    {15090, "Petrified"}, {16963, "Petrified"}, {25181, "Trapped"},
    {37211, "Frostbite"}, {18621, "Ichor"},
    // Warrior
    {14222, "Empower Allies"}, {14417, "Banner of Strength"},
    {14449, "Banner of Discipline"}, {14450, "Banner of Tactics"},
    {14543, "Banner of Defence"}, {31708, "Flames of War"},
    {5677, "Fire Shield"}, {34256, "Rock Guard"},
    {30204, "Furious Surge"}, {29466, "Blood Reckoning"},
    // Revenant
    {27732, "Facet of Nature"}, {29379, "Naturalistic Resonance"},
    {27890, "Shiro"}, {27581, "Impossible Odds"},
    {27972, "Ventari"}, {27928, "Mallyx"},
    {28001, "Embrace the Darkness"}, {27205, "Jalis"},
    {27273, "Vengeful Hammers"}, {44272, "Kalla"},
    {42883, "Kalla's Fervor"}, {44682, "Breakrazor's Bastion"},
    {41016, "Razorclaw's Rage"}, {45026, "Soulcleave's Summit"},
    {26854, "Assassin's Presence"},
    // Thief
    {33162, "Bounding Dodge"}, {32200, "Lotus Training"},
    {32931, "Unhindered Combatant"}, {13094, "Devourer Venom"},
    {13036, "Skale Venom"}, {13133, "Basilisk Venom"},
    {890, "Revealed"}, {13017, "Stealth"},
    // Mesmer
    {30328, "Alacrity"}, {10243, "Distortion"},
    {21751, "Signet of the Ether"}, {10335, "Blur"},
    {30426, "Fencer's Finesse"}, {44691, "Phantasmal Force"},
    // Elementalist
    {5575, "Air Attunement"}, {5585, "Fire Attunement"},
    {5586, "Water Attunement"}, {5580, "Earth Attunement"},
    {5587, "Soothing Mist"}, {15789, "Flame Axe"},
    {15792, "Fiery Greatsword"}, {15788, "Earth Shield"},
    {15791, "Lightning Hammer"}, {15790, "Ice Bow"},
    // Guardian
    {9114, "Virtue of Justice"}, {9119, "Virtue of Resolve"},
    {9113, "Virtue of Courage"}, {29632, "Spear of Justice"},
    {30308, "Wings of Resolve"}, {29523, "Shield of Courage"},
    {9103, "Zealot's Flame"},
    // Engineer
    {38333, "Pinpoint Distribution"},
    // Necromancer
    {30285, "Vampiric Aura"}, {29446, "Reaper's Shroud"},
    {790, "Death Shroud"}, {30129, "Infusing Terror"},
    // Ranger
    {14055, "Spotter"}, {34062, "Grace of the Land"},
    {30449, "Natural Healing"}, {31508, "Celestial Avatar"},
    {31584, "Ancestral Grace"}, {12549, "Storm Spirit"},
    {12544, "Frost Spirit"}, {12540, "Sun Spirit"},
    {12547, "Stone Spirit"}, {31803, "Glyph of Empowerment"},
    // Auras
    {5577, "Shocking Aura"}, {5579, "Frost Aura"},
    {5684, "Magnetic Aura"}, {10332, "Chaos Armor"},
    {25518, "Light Aura"},
    // Signets
    {12542, "Signet of the Hunt"}, {13060, "Signet of Shadows"},
    {5572, "Signet of Air"}, {10612, "Signet of the Locust"},
    // Raid encounter effects
    {34387, "Volatile Poison"}, {38210, "Shared Agony"},
    {47414, "Necrosis"}, {47164, "Soul Shackle"},
    {34473, "Corruption"}, {34450, "Unstable Blood Magic"},
    {48121, "Arcing Affliction"}, {52812, "Tidal Pool"},
    {79513, "Biting Swarm"},
    // Fixated variants (each encounter has its own ID)
    {34508, "Fixated"}, {47434, "Fixated"}, {48533, "Fixated"},
    {39131, "Fixated"}, {39928, "Fixated"}, {38985, "Fixated"},
    {39558, "Fixated"}, {58136, "Fixated"}, {79380, "Fixated"},
};

#endif
