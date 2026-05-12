#ifndef ARCDPS_H
#define ARCDPS_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
namespace ArcDPS
{
#endif 
    /* arcdps export table */

    typedef struct Exports
    {
        uintptr_t   Size;         /* sizeof(arcdps_exports) */
        uint32_t    Signature;    /* pick a number between 0 and uint32 max that is not used by any other some_module */
        uint32_t    ImGuiVersion; /* IMGUI_VERSION_NUM */
        const char* Name;         /* "your module name", name string shown in options */
        const char* Build;        /* "1.2.40", version string */

        void* WndProc; // UINT fn(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
        //     return value is assigned to real uMsg.
        //     unfiltered window event messages.

        void* CombatSquadCallback; // void fn(cbtevent* ev, ag* src, ag* dst, const char* skillname, uint64_t id, uint64_t revision)
        //     called asynchronously.
        //     ev parameter is cbtevent as in evtc documentation.
        //     ev may be null. if so, if src->elite == 1, then src->id is the id of the new targeted agent.
        //                             else, if src->prof, src->id was added
        //                                                       src->name = char name
        //                                                       dst->name = acc names
        //                                                       src->id = id
        //                                                       dst->id = instance id on map
        //                                                       dst->prof = prof
        //                                                       dst->elite = elite spec
        //                                                       dst->self = is self
        //                                                       src->team = team id
        //                                                       dst->team = subgroup
        //                                   else, src->id was removed
        //     use id parameter to re-establish order of events (if id == 0, consider the event unordered).
        //     due to unforseen circumstances after a change in assigning id, the first event will always have id == 2.
        //     refer to ag typedef for src and dst parameters.
        //     revision is cbtevent type revision, will most likely be 1.

        void* ImGuiRenderCallback; // void fn(uint32_t not_charsel_or_loading, uint32_t hide_if_combat_or_ooc)
        //     called before ImGui::Render.
        //     not_charsel_or_loading and hide_if_combat_or_ooc may be used to match arcdps window visibility.

        void* ImGuiOptions; // void fn()
        //     called when drawing module's options tab.

        void* CombatLocalCallback; // void fn(cbtevent* ev, ag* src, ag* dst, char* skillname, uint64_t id, uint64_t revision)
        //     same as combat, but for chatbox events.

        void* WndProcFiltered; // UINT fn(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
        //     return value is assigned to real uMsg.
        //     modifiers-filtered window event messages.

        void* ImGuiWindows; // void fn(char* windowname)
        //     called during options window list, before the checkbox of 'window' is drawn.
    } PluginInfo;

    /* combat event - see evtc docs for details, revision param in combat cb is equivalent of revision byte header */

    struct CombatEvent
    {
        uint64_t Time;
        uint64_t SourceAgent;
        uint64_t DestinationAgent;
        int32_t  Value;
        int32_t  BuffDamage;
        uint32_t OverstackValue;
        uint32_t SkillID;
        uint16_t SourceInstanceID;
        uint16_t DestinationInstanceID;
        uint16_t SrcMasterInstanceID;
        uint16_t DestinationMasterInstanceID;
        uint8_t  IFF;
        uint8_t  Buff;
        uint8_t  Result;
        uint8_t  IsActivation;
        uint8_t  IsBuffRemove;
        uint8_t  IsNinety;
        uint8_t  IsFifty;
        uint8_t  IsMoving;
        uint8_t  IsStatechange;
        uint8_t  IsFlanking;
        uint8_t  IsShields;
        uint8_t  IsOffcycle;
        uint8_t  PAD61;
        uint8_t  PAD62;
        uint8_t  PAD63;
        uint8_t  PAD64;
    };

    /* agent short */

    struct AgentShort
    {
        char*     Name;           /* agent name. may be null. valid only at time of event. utf8 */
        uintptr_t ID;             /* agent unique identifier */
        uint32_t  Profession;     /* profession at time of event. refer to evtc notes for identification */
        uint32_t  Specialization; /* elite spec at time of event. refer to evtc notes for identification */
        uint32_t  IsSelf;         /* 1 if self, 0 if not */
        uint16_t  Team;           /* sep21+ */
    };

    enum ECombatStateChange
    {
        CBTS_NONE, // not used - not this kind of event
        // not used - not this kind of event

        CBTS_ENTERCOMBAT, // agent entered combat
        // src_agent: relates to agent
        // dst_agent: subgroup
        // value: prof id
        // buff_dmg: elite spec id
        // evtc: limited to squad outside instances
        // realtime: limited to squad

        CBTS_EXITCOMBAT, // agent left combat
        // src_agent: relates to agent
        // evtc: limited to squad outside instances
        // realtime: limited to squad

        CBTS_CHANGEUP, // agent is alive at time of event
        // src_agent: relates to agent
        // evtc: limited to agent table outside instances
        // realtime: limited to squad

        CBTS_CHANGEDEAD, // agent is dead at time of event
        // src_agent: relates to agent
        // evtc: limited to agent table outside instances
        // realtime: limited to squad

        CBTS_CHANGEDOWN, // agent is down at time of event
        // src_agent: relates to agent
        // evtc: limited to agent table outside instances
        // realtime: limited to squad

        CBTS_SPAWN, // agent entered tracking
        // src_agent: relates to agent
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_DESPAWN, // agent left tracking
        // src_agent: relates to agent
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_HEALTHPCTUPDATE, // agent health percentage changed
        // src_agent: relates to agent
        // dst_agent: percent * 10000 eg. 99.5% will be 9950
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_SQCOMBATSTART, // squad combat start, first player enter combat. previously named log start
        // value: as uint32_t, server unix timestamp
        // buff_dmg: local unix timestamp
        // evtc: yes
        // realtime: yes

        CBTS_LOGEND, // squad combat stop, last player left combat. previously named log end
        // value: as uint32_t, server unix timestamp
        // buff_dmg: local unix timestamp
        // evtc: yes
        // realtime: yes

        CBTS_WEAPSWAP, // agent weapon set changed
        // src_agent: relates to agent
        // dst_agent: new weapon set id
        // value: old weapon seet id
        // evtc: yes
        // realtime: yes

        CBTS_MAXHEALTHUPDATE, // agent maximum health changed
        // src_agent: relates to agent
        // dst_agent: new max health
        // evtc: limited to non-players
        // realtime: no

        CBTS_POINTOFVIEW, // "recording" player
        // src_agent: relates to agent
        // evtc: yes
        // realtime: no

        CBTS_LANGUAGE, // text language id
        // src_agent: text language id
        // evtc: yes
        // realtime: no

        CBTS_GWBUILD, // game build
        // src_agent: game build number
        // evtc: yes
        // realtime: no

        CBTS_SHARDID, // server shard id
        // src_agent: shard id
        // evtc: yes
        // realtime: no

        CBTS_REWARD, // wiggly box reward
        // dst_agent: reward id
        // value: reward type
        // evtc: yes
        // realtime: yes

        CBTS_BUFFINITIAL, // buff application for buffs already existing at time of event
        // refer to cbtevent struct, identical to buff application. statechange is set to this
        // evtc: limited to squad outside instances
        // realtime: limited to squad

        CBTS_POSITION, // agent position changed
        // src_agent: relates to agent
        // dst_agent: (float*)&dst_agent is float[3], x/y/z
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_VELOCITY, // agent velocity changed
        // src_agent: relates to agent
        // dst_agent: (float*)&dst_agent is float[3], x/y/z
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_FACING, // agent facing direction changed
        // src_agent: relates to agent
        // dst_agent: (float*)&dst_agent is float[2], x/y
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_TEAMCHANGE, // agent team id changed
        // src_agent: relates to agent
        // dst_agent: new team id
        // value: old team id
        // evtc: limited to agent table outside instances
        // realtime: limited to squad

        CBTS_ATTACKTARGET, // attacktarget to gadget association
        // src_agent: relates to agent, the attacktarget
        // dst_agent: the gadget
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_TARGETABLE, // agent targetable state
        // src_agent: relates to agent
        // dst_agent: new targetable state
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_MAPID, // map id
        // src_agent: map id
        // evtc: yes
        // realtime: no

        CBTS_REPLINFO, // internal use
        // internal use

        CBTS_STACKACTIVE, // buff instance is now active
        // src_agent: relates to agent
        // dst_agent: buff instance id
        // value: current buff duration
        // evtc: limited to squad outside instances
        // realtime: limited to squad

        CBTS_STACKRESET, // buff instance duration changed, value is the duration to reset to (also marks inactive), pad61-pad64 buff instance id
        // src_agent: relates to agent
        // value: new duration
        // evtc: limited to squad outside instances
        // realtime: limited to squad

        CBTS_GUILD, // agent is a member of guild
        // src_agent: relates to agent
        // dst_agent: (uint8_t*)&dst_agent is uint8_t[16], guid of guild
        // value: new duration
        // evtc: limited to squad outside instances
        // realtime: no

        CBTS_BUFFINFO, // buff information
        // skillid: skilldef id of buff
        // overstack_value: max combined duration
        // src_master_instid: 
        // is_src_flanking: likely an invuln
        // is_shields: likely an invert
        // is_offcycle: category
        // pad61: buff stacking type
        // pad62: likely a resistance
        // evtc: yes
        // realtime: no

        CBTS_BUFFFORMULA, // buff formula, one per event of this type
        // skillid: skilldef id of buff
        // time: (float*)&time is float[9], type attribute1 attribute2 parameter1 parameter2 parameter3 trait_condition_source trait_condition_self content_reference
        // src_instid: (float*)&src_instid is float[2], buff_condition_source buff_condition_self
        // evtc: yes
        // realtime: no

        CBTS_SKILLINFO, // skill information
        // skillid: skilldef id of skill
        // time: (float*)&time is float[4], cost range0 range1 tooltiptime
        // evtc: yes
        // realtime: no

        CBTS_SKILLTIMING, // skill timing, one per event of this type
        // skillid: skilldef id of skill
        // src_agent: timing type
        // dst_agent: at time since activation in milliseconds
        // evtc: yes
        // realtime: no

        CBTS_BREAKBARSTATE, // agent breakbar state changed
        // src_agent: relates to agent
        // dst_agent: new breakbar state
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_BREAKBARPERCENT, // agent breakbar percentage changed
        // src_agent: relates to agent
        // value: (float*)&value is float[1], new percentage
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_INTEGRITY, // one event per message. previously named error
        // time: (char*)&time is char[32], a short null-terminated message with reason
        // evtc: yes
        // realtime: no

        CBTS_MARKER, // one event per marker on an agent
        // src_agent: relates to agent
        // value: markerdef id. if value is 0, remove all markers presently on agent
        // buff: marker is a commander tag
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_BARRIERPCTUPDATE, // agent barrier percentage changed
        // src_agent: relates to agent
        // dst_agent: percent * 10000 eg. 99.5% will be 9950
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_STATRESET, // arcdps stats reset 
        // src_agent: species id of agent that triggered the reset, eg boss species id
        // evtc: yes
        // realtime: yes

        CBTS_EXTENSION, // for extension use. not managed by arcdps
        // evtc: yes
        // realtime: yes

        CBTS_APIDELAYED, // one per cbtevent that got deemed unsafe for realtime but safe for posting after squadcombat
        // evtc: no
        // realtime: yes

        CBTS_INSTANCESTART, // map instance start
        // src_agent: milliseconds ago instance was started 
        // evtc: yes
        // realtime: yes

        CBTS_RATEHEALTH, // tick health. previously named tickrate
        // src_agent: 25 - tickrate, when tickrate <= 20 
        // evtc: yes
        // realtime: no

        CBTS_LAST90BEFOREDOWN, // retired, not used since 240529+
        // retired

        CBTS_EFFECT, // retired, not used since 230716+
        // retired

        CBTS_IDTOGUID, // content id to guid association for volatile types
        // src_agent: (uint8_t*)&src_agent is uint8_t[16] guid of content
        // overstack_value: is of enum contentlocal
        // evtc: yes
        // realtime: no

        CBTS_LOGNPCUPDATE, // log boss agent changed
        // src_agent: species id of agent
        // dst_agent: related to agent
        // value: as uint32_t, server unix timestamp
        // evtc: yes
        // realtime: yes

        CBTS_IDLEEVENT, // internal use
        // internal use

        CBTS_EXTENSIONCOMBAT, // for extension use. not managed by arcdps
        // assumed to be cbtevent struct, skillid will be processed as such for purpose of buffinfo/skillinfo
        // evtc: yes
        // realtime: yes

        CBTS_FRACTALSCALE, // fractal scale for fractals
        // src_agent: scale
        // evtc: yes
        // realtime: no

        CBTS_EFFECT2, // play graphical effect
        // src_agent: related to agent
        // dst_agent: effect at location of agent (if applicable)
        // value: (float*)&value is float[3], location x/y/z (if not at agent location)
        // iff: (uint32_t*)&iff is uint32_t[1], effect duration
        // buffremove: (uint32_t*)&buffremove is uint32_t[1], trackable id of effect. id dst_agent and location is 0/0/0, effect was stopped
        // is_shields: (int16_t*)&is_shields is int16_t[3], orientation x/y/z, values are original*1000
        // is_flanking: effect is on a non-static platform
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_RULESET, // ruleset for self
        // src_agent: bit0: pve, bit1: wvw, bit2: pvp
        // evtc: yes
        // realtime: no

        CBTS_SQUADMARKER, // squad ground markers
        // src_agent: (float*)&src_agent is float[3], x/y/z of marker location. if values are all zero or infinity, this marker is removed
        // skillid: index of marker eg. 0 is arrow
        // evtc: yes
        // realtime: no

        CBTS_ARCBUILD, // arc build info
        // src_agent: (char*)&src_agent is a null-terminated string matching the full build string in arcdps.log
        // evtc: yes
        // realtime: no

        CBTS_GLIDER, // glider status change
        // src_agent: related to agent
        // value: 1 deployed, 0 stowed
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_STUNBREAK, // disable stopped early
        // src_agent: related to agent
        // value: duration remaining
        // evtc: limited to agent table outside instances
        // realtime: no

        CBTS_UNKNOWN // unknown/unsupported type newer than this list maybe
    };

    /* is friend/foe */
    enum EIsFriendFoe
    {
        IFF_FRIEND,
        IFF_FOE,
        IFF_UNKNOWN
    };

    /* combat result (direct) */
    enum ECombatResult
    {
        CBTR_NORMAL, // strike was neither crit or glance
        CBTR_CRIT, // strike was crit
        CBTR_GLANCE, // strike was glance
        CBTR_BLOCK, // strike was blocked eg. mesmer shield 4
        CBTR_EVADE, // strike was evaded, eg. dodge or mesmer sword 2
        CBTR_INTERRUPT, // strike interrupted something
        CBTR_ABSORB, // strike was "invulned" or absorbed eg. guardian elite
        CBTR_BLIND, // strike missed
        CBTR_KILLINGBLOW, // not a damage strike, target was killed by skill
        CBTR_DOWNED, // not a damage strike, target was downed by skill
        CBTR_BREAKBAR, // not a damage strike, target had value of breakbar damage dealt
        CBTR_ACTIVATION, // not a damage strike, skill activation event
        CBTR_CROWDCONTROL, // not a damage strike, target was crowdcontrolled
        CBTR_UNKNOWN
    };

    /* combat activation */
    enum ECombatActivation {
        ACTV_NONE, // not used - not this kind of event
        ACTV_START, // started skill/animation activation
        ACTV_QUICKNESS_UNUSED, // unused as of nov 5 2019
        ACTV_CANCEL_FIRE, // stopped skill activation with reaching tooltip time
        ACTV_CANCEL_CANCEL, // stopped skill activation without reaching tooltip time
        ACTV_RESET, // animation completed fully
        ACTV_UNKNOWN
    };

    /* combat buff remove type */
    enum ECombatBuffRemove {
        CBTB_NONE, // not used - not this kind of event
        CBTB_ALL, // last/all stacks removed (sent by server)
        CBTB_SINGLE, // single stack removed (sent by server). will happen for each stack on cleanse
        CBTB_MANUAL, // single stack removed (auto by arc on ooc or all stack, ignore for strip/cleanse calc, use for in/out volume)
        CBTB_UNKNOWN
    };

    /* combat buff cycle type */
    enum ECombatBuffCycle {
        CBTC_CYCLE, // damage happened on tick timer
        CBTC_NOTCYCLE, // damage happened outside tick timer (resistable)
        CBTC_NOTCYCLENORESIST, // BEFORE MAY 2021: the others were lumped here, now retired
        CBTC_NOTCYCLEDMGTOTARGETONHIT, // damage happened to target on hitting target
        CBTC_NOTCYCLEDMGTOSOURCEONHIT, // damage happened to source on htiting target
        CBTC_NOTCYCLEDMGTOTARGETONSTACKREMOVE, // damage happened to target on source losing a stack
        CBTC_UNKNOWN
    };

    /* language */
    enum EGWLanguage {
        GWL_ENG = 0,
        GWL_FRE = 2,
        GWL_GEM = 3,
        GWL_SPA = 4,
        GWL_CN = 5,
    };

    /* content local enum */
    enum EContentLocal {
        CONTENTLOCAL_EFFECT,
        CONTENTLOCAL_MARKER
    };

    struct UISettings
    {
        uint64_t IsHidden               : 1;
        uint64_t IsAlwaysDrawn          : 1;
        uint64_t IsModMoveLocked        : 1;
        uint64_t IsModClickLocked       : 1;
        uint64_t IsClosingWithEscape    : 1;
    };

    struct Modifiers
    {
        WORD Mod1;
        WORD Mod2;
        WORD ModMulti;
    };

    /* exports */
    typedef uint64_t(*Export_GetU64)();

    static void* LogFile;
    static void* LogArc;

    static void LogToFile(char* str) /* log to arcdps.log, thread/async safe */
    {
        size_t(*log)(char*) = (size_t(*)(char*))LogFile;
        if (log) (*log)(str);
        return;
    }
    static void LogToArc(char* str) /* log to extensions tab in arcdps log window, thread/async safe */
    {
        size_t(*log)(char*) = (size_t(*)(char*))LogArc;
        if (log) (*log)(str);
        return;
    }
    static void Log(char* str) /* log to arcdps.log and log window*/
    {
        LogToFile(str);
        LogToArc(str);
        return;
    }
#ifdef __cplusplus
}
#endif

#endif