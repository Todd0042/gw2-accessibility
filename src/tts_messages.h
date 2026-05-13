#ifndef TTS_MESSAGES_H
#define TTS_MESSAGES_H

#include <unordered_map>

// Built-in action-oriented TTS messages (what to do, not just the buff name)
static const std::unordered_map<unsigned int, const char*> g_BuiltinTtsMessages = {
    {34416, "Corruption, get to fountain"},
    {34450, "SAK, go to wall and drop"},
    {47646, "Bomb, run away"},
    {79526, "Bees. Get away"},
};

#endif
