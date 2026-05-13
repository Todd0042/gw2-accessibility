#ifndef BOON_IGNORE_H
#define BOON_IGNORE_H

#include <unordered_set>

// Skill IDs excluded from "Read All Debuffs" mode — boons, profession buffs, and other effects to ignore
static const std::unordered_set<unsigned int> g_BoonIds = {
    // Standard boons
    740, 725, 1187, 717, 718, 719, 726, 743, 1122, 873, 26980, 30328,
    // Profession buffs
    14222, 14417, 14449, 14450, 29379, 26854, 38333, 14055, 34062, 31803,
    // Spirits, druid
    12549, 12544, 12540, 12547, 30449, 31508,
    // Revenant
    27732, 29379, 27890, 27581, 27972, 27928, 28001, 27205, 27273,
    // Attunements
    5575, 5585, 5586, 5580, 5587,
    // Guardian virtues
    9114, 9119, 9113, 9103,
    // Mesmer distortion/blur
    10243, 10335, 30426,
    // Necro shrouds and damage modifiers
    30285, 29446, 790, 30129, 30539, 72975,
};

static bool IsBoon(unsigned int id) { return g_BoonIds.count(id); }

#endif
