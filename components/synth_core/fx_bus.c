/* fx_bus.c - synth-group -> AMY bus routing table. Pure state, no AMY calls;
 * the header carries the contract. Kept separate from amy_fx.c so the ingress
 * seam (amy_helpers.c) can ask "which bus does this slot render on" without
 * pulling in the FX cache or the event helpers. */

#include "fx_bus.h"
#include "synth_slots.h"

/* Split flags, indexed by fx_group_t. MELODIC's entry is never written; it is
 * kept so the array indexes directly by group. All false at boot: everything
 * renders on FX_BUS_HOME until the user splits a group off. */
static bool s_split[FX_GROUP_COUNT];

fx_group_t fx_group_of_slot(uint8_t slot)
{
    if (slot == SEQ_ARP_SYNTH) return FX_GROUP_ARP;
    if (slot >= DRONE_SYNTH_MAIN && slot <= DRONE_STD_SYNTH_SUB)
        return FX_GROUP_DRONES;
    if (slot >= SEQ_DRUM_SYNTH_BASE && slot < SEQ_DRUM_SYNTH_BASE + 4)
        return FX_GROUP_DRUMS;
    return FX_GROUP_MELODIC;
}

uint8_t fx_group_own_bus(fx_group_t g)
{
    switch (g) {
        case FX_GROUP_DRUMS:  return 1;
        case FX_GROUP_ARP:    return 2;
        case FX_GROUP_DRONES: return 3;
        default:              return FX_BUS_HOME;
    }
}

fx_group_t fx_group_of_bus(uint8_t bus)
{
    switch (bus) {
        case 1:  return FX_GROUP_DRUMS;
        case 2:  return FX_GROUP_ARP;
        case 3:  return FX_GROUP_DRONES;
        default: return FX_GROUP_MELODIC;
    }
}

bool fx_group_is_split(fx_group_t g)
{
    if (g == FX_GROUP_MELODIC || g >= FX_GROUP_COUNT) return false;
    return s_split[g];
}

bool fx_bus_set_split(fx_group_t g, bool on)
{
    if (g == FX_GROUP_MELODIC || g >= FX_GROUP_COUNT) return false;
    if (s_split[g] == on) return false;
    s_split[g] = on;
    return true;
}

uint8_t fx_bus_of_group(fx_group_t g)
{
    return fx_group_is_split(g) ? fx_group_own_bus(g) : FX_BUS_HOME;
}

uint8_t fx_bus_for_synth(uint8_t slot)
{
    return fx_bus_of_group(fx_group_of_slot(slot));
}

bool fx_bus_is_active(uint8_t bus)
{
    if (bus == FX_BUS_HOME) return true;
    return fx_group_is_split(fx_group_of_bus(bus));
}

const char *fx_group_name(fx_group_t g)
{
    switch (g) {
        case FX_GROUP_DRUMS:  return "Drums";
        case FX_GROUP_ARP:    return "Arp";
        case FX_GROUP_DRONES: return "Drones";
        default:              return "Melodic";
    }
}

const char *fx_bus_label(uint8_t bus)
{
    if (bus == FX_BUS_HOME) return "Main";
    return fx_group_name(fx_group_of_bus(bus));
}

uint8_t fx_group_slots(fx_group_t g, uint8_t out[8])
{
    switch (g) {
        case FX_GROUP_DRUMS:
            for (uint8_t i = 0; i < 4; i++)
                out[i] = (uint8_t)(SEQ_DRUM_SYNTH_BASE + i);
            return 4;
        case FX_GROUP_ARP:
            out[0] = SEQ_ARP_SYNTH;
            return 1;
        case FX_GROUP_DRONES:
            out[0] = DRONE_SYNTH_MAIN;
            out[1] = DRONE_SYNTH_SUB;
            out[2] = DRONE_STD_SYNTH_MAIN;
            out[3] = DRONE_STD_SYNTH_SUB;
            return 4;
        default:
            return 0;
    }
}
