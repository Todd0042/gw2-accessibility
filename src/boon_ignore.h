#ifndef BOON_IGNORE_H
#define BOON_IGNORE_H

#include <unordered_set>

// Skill IDs that are conditions/debuffs — only these are spoken in "Read All Debuffs" mode
// This is the inverse approach: whitelist conditions instead of blacklisting boons
// (far fewer conditions than boons, much safer)
static const std::unordered_set<unsigned int> g_ConditionIds = {
    // Core conditions
    736,  // Bleeding
    737,  // Burning
    861,  // Confusion
    723,  // Poison
    19426,// Torment
    720,  // Blind
    722,  // Chilled
    721,  // Crippled
    727,  // Immobile
    742,  // Weakness
    738,  // Vulnerability
    26766,// Slow
    // Control effects
    791,  // Fear
    872,  // Stun
    833,  // Daze
    27705,// Taunt
    26646,// Battle Scars
    // Special conditions
    890,  // Revealed
    47414,// Necrosis
    34473,// Corruption
    34387,// Volatile Poison
    38210,// Shared Agony
    48121,// Arcing Affliction
    79513,// Biting Swarm
    37211,// Frostbite
    25181,// Trapped
    15090,// Petrified
    16963,// Petrified
    // Doom/Hex conditions (common raid mechanic patterns)
    34450,// Unstable Blood Magic
    47164,// Soul Shackle
    52812,// Tidal Pool
};

static bool IsCondition(unsigned int id) { return g_ConditionIds.count(id); }

// Legacy boon ignore list (kept for backward compatibility with any external references)
static const std::unordered_set<unsigned int> g_BoonIds = {
    740, 725, 1187, 717, 718, 719, 726, 743, 1122, 873, 26980, 30328,
    14222, 14417, 14449, 14450, 29379, 26854, 38333, 14055, 34062, 31803,
    12549, 12544, 12540, 12547, 30449, 31508,
    27732, 29379, 27890, 27581, 27972, 27928, 28001, 27205, 27273,
    5575, 5585, 5586, 5580, 5587,
    9114, 9119, 9113, 9103,
    10243, 10335, 30426,
    30285, 29446, 790, 30129, 30539, 72975,
};

static bool IsBoon(unsigned int id) { return g_BoonIds.count(id); }

#endif
