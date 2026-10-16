#include "sdkconfig.h"
#include "synth_ui/ui_voice_target.h"
#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "seq_core_config.h"   /* SEQ_MELODIC_PORTAMENTO_MAX_MS */
#include "arp_core.h"
#include "drop_core.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"
#endif

/* Active layer index if it is melodic, else 0xFF. The filter and glide live
 * on melodic layers only. */
static uint8_t vt_melodic_layer(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return 0xFF;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return 0xFF;
    return li;
}

float vt_get_level(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC:
        return sequencer_core_get_melodic_amp_scale(seq_state.active_layer_idx,
                                                    seq_state.selected_track);
    case VT_ARP:
        return arp_get_amp_scale();
    case VT_DROP:
        return drop_get_amp_scale(drop_get_focus());
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        return live_play_get_amp_scale();
#endif
    default:
        return 1.0f;
    }
}

void vt_set_level(voice_target_kind_t kind, float v)
{
    switch (kind) {
    case VT_MELODIC:
        sequencer_core_set_melodic_amp_scale(seq_state.active_layer_idx,
                                             seq_state.selected_track, v);
        break;
    case VT_ARP:
        arp_set_amp_scale(v);
        break;
    case VT_DROP:
        drop_set_amp_scale(drop_get_focus(), v);
        break;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        live_play_set_amp_scale(v);
        break;
#endif
    default:
        break;
    }
}

bool vt_get_filter(voice_target_kind_t kind, seq_filter_t *out)
{
    switch (kind) {
    case VT_MELODIC: {
        uint8_t mel = vt_melodic_layer();
        if (mel == 0xFF) return false;
        return sequencer_core_get_melodic_filter(mel, seq_state.selected_track, out);
    }
    case VT_ARP:
        arp_get_filter(out);
        return true;
    case VT_DROP:
        drop_get_filter(drop_get_focus(), out);
        return true;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        live_play_get_filter(out);
        return true;
#endif
    default:
        return false;
    }
}

void vt_set_filter(voice_target_kind_t kind, const seq_filter_t *f)
{
    switch (kind) {
    case VT_MELODIC: {
        uint8_t mel = vt_melodic_layer();
        if (mel == 0xFF) return;
        sequencer_core_set_melodic_filter(mel, seq_state.selected_track, f);
        break;
    }
    case VT_ARP:
        arp_set_filter(f);
        break;
    case VT_DROP:
        drop_set_filter(drop_get_focus(), f);
        break;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        live_play_set_filter(f);
        break;
#endif
    default:
        break;
    }
}

bool vt_has_mute(voice_target_kind_t kind)
{
    return kind == VT_MELODIC || kind == VT_ARP || kind == VT_DROP;
}

bool vt_get_mute(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC:
        return sequencer_core_get_track_mute(seq_state.active_layer_idx,
                                             seq_state.selected_track);
    case VT_ARP:
        return arp_get_muted();
    case VT_DROP:
        return drop_get_muted(drop_get_focus());
    default:
        return false;
    }
}

void vt_set_mute(voice_target_kind_t kind, bool muted)
{
    switch (kind) {
    case VT_MELODIC:
        sequencer_core_set_track_mute(seq_state.active_layer_idx,
                                      seq_state.selected_track, muted);
        break;
    case VT_ARP:
        arp_set_muted(muted);
        break;
    case VT_DROP:
        drop_set_muted(drop_get_focus(), muted);
        break;
    default:
        break;
    }
}

bool vt_has_solo(voice_target_kind_t kind)
{
    return kind == VT_MELODIC || kind == VT_ARP || kind == VT_DROP;
}

bool vt_get_solo(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC:
        return sequencer_core_get_track_solo(seq_state.active_layer_idx,
                                             seq_state.selected_track);
    case VT_ARP:
        return sequencer_core_get_arp_solo();
    case VT_DROP:
        return sequencer_core_get_drop_solo((uint8_t)drop_get_focus());
    default:
        return false;
    }
}

void vt_set_solo(voice_target_kind_t kind, bool solo)
{
    switch (kind) {
    case VT_MELODIC:
        sequencer_core_set_track_solo(seq_state.active_layer_idx,
                                      seq_state.selected_track, solo);
        break;
    case VT_ARP:
        sequencer_core_set_arp_solo(solo);
        break;
    case VT_DROP:
        sequencer_core_set_drop_solo((uint8_t)drop_get_focus(), solo);
        break;
    default:
        break;
    }
}

bool vt_has_gate(voice_target_kind_t kind)
{
    return kind == VT_MELODIC || kind == VT_ARP || kind == VT_DROP;
}

uint16_t vt_get_gate_pct(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC:
        return sequencer_core_get_layer_gate_pct(seq_state.active_layer_idx);
    case VT_ARP:
        return arp_get_gate_pct();
    case VT_DROP:
        return drop_get_gate_pct(drop_get_focus());
    default:
        return 0;
    }
}

void vt_set_gate_pct(voice_target_kind_t kind, uint16_t pct)
{
    switch (kind) {
    case VT_MELODIC:
        sequencer_core_set_layer_gate_pct(seq_state.active_layer_idx, pct);
        break;
    case VT_ARP:
        arp_set_gate_pct((uint8_t)(pct > 100u ? 100u : pct));
        break;
    case VT_DROP:
        drop_set_gate_pct(drop_get_focus(), (uint8_t)(pct > 100u ? 100u : pct));
        break;
    default:
        break;
    }
}

bool vt_has_glide(voice_target_kind_t kind)
{
#if CONFIG_SYNTH_WIRELESS
    (void)kind;
    return true;
#else
    return kind != VT_LIVE;
#endif
}

uint16_t vt_get_glide_ms(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC: {
        uint8_t mel = vt_melodic_layer();
        return (mel == 0xFF) ? 0u : sequencer_core_get_melodic_portamento_ms(mel);
    }
    case VT_ARP:
        return arp_get_portamento_ms();
    case VT_DROP:
        return drop_get_portamento_ms(drop_get_focus());
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        return live_play_get_glide_ms();
#endif
    default:
        return 0;
    }
}

void vt_set_glide_ms(voice_target_kind_t kind, uint16_t ms)
{
    switch (kind) {
    case VT_MELODIC: {
        uint8_t mel = vt_melodic_layer();
        if (mel == 0xFF) return;
        if (ms > SEQ_MELODIC_PORTAMENTO_MAX_MS) ms = SEQ_MELODIC_PORTAMENTO_MAX_MS;
        sequencer_core_set_melodic_portamento_ms(mel, ms);
        break;
    }
    case VT_ARP:
        if (ms > ARP_PORTAMENTO_MAX_MS) ms = ARP_PORTAMENTO_MAX_MS;
        arp_set_portamento_ms(ms);
        break;
    case VT_DROP:
        if (ms > DROP_PORTAMENTO_MAX_MS) ms = DROP_PORTAMENTO_MAX_MS;
        drop_set_portamento_ms(drop_get_focus(), ms);
        break;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        if (ms > LIVE_PLAY_GLIDE_MAX_MS) ms = LIVE_PLAY_GLIDE_MAX_MS;
        live_play_set_glide_ms(ms);
        break;
#endif
    default:
        break;
    }
}

bool vt_has_unison(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC: return vt_melodic_layer() != 0xFF;
    case VT_ARP:     return true;
    case VT_DROP:    return true;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:    return true;
#endif
    default:         return false;
    }
}

void vt_get_unison(voice_target_kind_t kind, voice_unison_t *out)
{
    switch (kind) {
    case VT_MELODIC:
        *out = sequencer_core_get_track_unison(vt_melodic_layer(),
                                               layermenu_menu_track());
        break;
    case VT_ARP:
        arp_get_unison(out);
        break;
    case VT_DROP:
        drop_get_unison(drop_get_focus(), out);
        break;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        live_play_get_unison(out);
        break;
#endif
    default:
        *out = sequencer_core_get_track_unison(0xFF, 0);   /* the defaults */
        break;
    }
}

uint8_t vt_unison_max(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_MELODIC:
        return sequencer_core_track_unison_max(vt_melodic_layer(),
                                               layermenu_menu_track());
    case VT_ARP:
        return arp_unison_max();
    case VT_DROP:
        return drop_unison_max(drop_get_focus());
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        return live_play_unison_max();
#endif
    default:
        return 0u;
    }
}

void vt_set_unison(voice_target_kind_t kind, const voice_unison_t *u)
{
    switch (kind) {
    case VT_MELODIC: {
        uint8_t li = vt_melodic_layer();
        if (li == 0xFF) return;
        if (sequencer_core_get_patch_scope(li) == SEQ_PATCH_SCOPE_TRACK)
            sequencer_core_set_track_unison(li, layermenu_menu_track(), u);
        else
            sequencer_core_set_layer_unison(li, u);
        break;
    }
    case VT_ARP:
        arp_set_unison(u);
        break;
    case VT_DROP:
        drop_set_unison(drop_get_focus(), u);
        break;
#if CONFIG_SYNTH_WIRELESS
    case VT_LIVE:
        live_play_set_unison(u);
        break;
#endif
    default:
        break;
    }
}

const char *vt_title(voice_target_kind_t kind)
{
    switch (kind) {
    case VT_ARP:  return "ARP";
    case VT_DROP: return drop_get_focus() == DROP_VOICE_SUB ? "SUB" : "DROP";
    case VT_LIVE: return "LIVE";
    default:      return NULL;
    }
}
