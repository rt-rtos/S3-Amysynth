#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "amy.h"               /* AMY_SEQUENCER_PPQ behind SEQ_TICKS_PER_STEP */
#include "seq_core_config.h"   /* SEQ_MELODIC_PORTAMENTO_MAX_MS, SEQ_SWING_* */
#include "seq_clamp.h"
#include <math.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  LAYER SUBMENU PAGE
 * ════════════════════════════════════════════════════════════════════════
 * Everything that belongs to ONE layer, on one page. First the Track selector
 * and the per-track block it drives (level/key/follow/repeat/mute/solo, frame,
 * the Unison/PCM dives; Follow: the row's progression follow mode,
 * sequencer_core_set_track_follow), then the layer-wide rows: step count,
 * swing, the melodic patch scope, the note controls (gate/glide/groove), the
 * manual chord. Reached from the `Layer >` dive
 * row on the main menu; page state and input routing live in ui_screen_menu.c,
 * this file only builds the rows and applies clicks and encoder edits.
 *
 * The layer is always seq_state.active_layer_idx - the one the grid shows -
 * so there is no layer selector to keep in sync. The Track row is the grid's
 * own selected track (seq_state.selected_track): it picks which row
 * Level/Key/Follow/Repeat/Mute/Solo/Frame and the two sub-pages edit, and
 * leaving the menu lands the grid cursor on that row. Level is that row's
 * amp trim (sequencer_core_set_melodic_amp_scale), on every layer type. Key,
 * melodic layers only, is its filter key tracking (seq_filter_t.key_track),
 * written back through sequencer_core_set_melodic_filter.
 *
 * Two dive rows sit above Back: `Unison >` on a melodic layer and `PCM >` on
 * the drum layer while the drum engine is PCM (sub-pages in
 * ui_screen_layer_sub.c). Frame, above them, is the wavetable frame position
 * of the Track row's track ("Auto" or 0..63, sequencer_core.h "Melodic
 * per-track wavetable frame"), shown only while that track plays a wavetable
 * patch; like Unison it follows the patch scope: LAYER scope writes every
 * track, TRACK scope the one track.
 *
 * Drum layer: the melodic-only rows render "--" and their edits no-op, so the
 * page is always safe to open. ClrSolo exists only while something is soloed;
 * it and the dive rows are what make the row list dynamic. Row ids
 * (layer_item_id_t) are in synth_ui_internal.h, so the page router can land
 * on a row by id.
 *
 * The page has three entries (s_lm_kind): the sequencer layer described
 * above, the arp ("ARP") and the live-play voice ("LIVE"). The arp and live
 * entries show only their per-voice rows, the Unison dive among them; Level,
 * Key, Mute, Solo, Gate and Glide here and the Unison sub-page read and write
 * through ui_voice_target.h on every entry. */

static menu_item_view_t s_lm_items[LM_COUNT];
static voice_target_kind_t s_lm_kind = VT_MELODIC;

void layermenu_menu_set_kind(voice_target_kind_t kind)
{
    s_lm_kind = kind;
}

voice_target_kind_t layermenu_menu_kind(void)
{
    return s_lm_kind;
}

/* Swing detents are engine ticks, not swing_pct points: pct values that floor
 * to the same tick play identically, so each detent lands on its tick's
 * smallest pct. */
uint8_t ui_swing_step(uint8_t swing_pct, int dir)
{
    int t = (int)SEQ_SWING_TICKS(swing_pct) + dir;
    t = SEQ_CLAMP_INT(t, 0, (int)SEQ_SWING_TICKS(SEQ_SWING_MAX));
    return SEQ_SWING_PCT_FOR_TICKS(t);
}

/* The long/short ratio of a 16th pair (MPC convention), not the stored
 * percent-of-a-step: 50 is straight, 67T is triplet swing. */
void ui_swing_format(char *buf, size_t len, uint8_t swing_pct)
{
    uint32_t t = SEQ_SWING_TICKS(swing_pct);
    unsigned ratio = (unsigned)((100u * (SEQ_TICKS_PER_STEP + t) + SEQ_TICKS_PER_STEP - 1u)
                                / (2u * SEQ_TICKS_PER_STEP));
    snprintf(buf, len, (3u * t == SEQ_TICKS_PER_STEP) ? "%uT" : "%u%%", ratio);
}

/* Rows the active layer has; the Track row wraps inside them. */
static uint8_t layermenu_num_tracks(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return SEQ_TRACKS_DEFAULT;
    return seq_state.layers[li].num_tracks;
}

/* Active layer index if it is melodic, else 0xFF (nothing to edit). */
static uint8_t layermenu_active_melodic_layer(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return 0xFF;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return 0xFF;
    return li;
}

/* The rows the cursor can land on, in navigation order. ClrSolo comes and goes
 * with the global solo state and the dive rows with the layer type and drum
 * engine, so the visible list is built per frame rather than indexed as a
 * fixed range. Returns the count; `rows` holds LM_COUNT. */
static uint8_t lm_row_list(uint8_t *rows)
{
    if (s_lm_kind != VT_MELODIC) {
        /* Per-voice rows only, in enum order. */
        static const uint8_t arp_rows[]  = { LM_LEVEL, LM_KEYTRACK, LM_MUTE, LM_SOLO,
                                             LM_CLRSOLO, LM_UNISON, LM_GATE, LM_GLIDE,
                                             LM_BACK };
        static const uint8_t live_rows[] = { LM_LEVEL, LM_KEYTRACK, LM_UNISON, LM_GLIDE,
                                             LM_BACK };
        /* The drop's focused synth takes the arp's rows. */
        const uint8_t *src = (s_lm_kind == VT_LIVE) ? live_rows : arp_rows;
        uint8_t        cnt = (s_lm_kind == VT_LIVE) ? (uint8_t)sizeof(live_rows)
                                                    : (uint8_t)sizeof(arp_rows);
        uint8_t n = 0;
        for (uint8_t i = 0; i < cnt; i++) {
            if (src[i] == LM_CLRSOLO && !sequencer_core_any_solo()) continue;
            rows[n++] = src[i];
        }
        return n;
    }
    uint8_t li  = seq_state.active_layer_idx;
    bool    mel = (layermenu_active_melodic_layer() != 0xFF);
    bool    pcm = li < seq_state.num_layers &&
                  seq_state.layers[li].type == SEQ_LAYER_DRUM &&
                  sequencer_core_get_drum_engine() == SEQ_DRUM_PCM;
    bool    wt  = mel && sequencer_core_is_wavetable_patch(
                         sequencer_core_get_melodic_track_patch(li, seq_state.selected_track));
    uint8_t n = 0;
    for (uint8_t id = 0; id < LM_COUNT; id++) {
        if (id == LM_CLRSOLO && !sequencer_core_any_solo()) continue;
        if (id == LM_FRAME && !wt) continue;
        if ((id == LM_UNISON || id == LM_KEYTRACK) && !mel) continue;
        if (id == LM_PCM && !pcm) continue;
        rows[n++] = id;
    }
    return n;
}

/* Row id under a visible-list index, or LM_COUNT when out of range. */
static uint8_t lm_row_at(uint8_t idx)
{
    uint8_t rows[LM_COUNT];
    uint8_t n = lm_row_list(rows);
    return (idx < n) ? rows[idx] : (uint8_t)LM_COUNT;
}

void layermenu_menu_reset(void)
{
    if (s_lm_kind != VT_MELODIC) return;
    if (seq_state.selected_track >= layermenu_num_tracks()) seq_state.selected_track = 0;
}

/* Pull the shared menu cursor back into the visible list after ClrSolo has
 * disappeared under it. Input-handler side only - the build path must stay
 * side-effect-free. */
void layermenu_menu_clamp_cursor(void)
{
    uint8_t n = layermenu_menu_item_count();
    if (seq_state.menu_cursor >= n) seq_state.menu_cursor = (uint8_t)(n - 1u);
}

uint8_t layermenu_menu_row_index(uint8_t id)
{
    uint8_t rows[LM_COUNT];
    uint8_t n = lm_row_list(rows);
    for (uint8_t i = 0; i < n; i++) {
        if (rows[i] == id) return i;
    }
    return 0xFF;
}

uint8_t layermenu_menu_track(void)
{
    return seq_state.selected_track;
}

uint8_t layermenu_menu_item_count(void)
{
    uint8_t rows[LM_COUNT];
    return lm_row_list(rows);
}

const char *layermenu_menu_title(void)
{
    /* Names the layer always, even at "L1/1", so it stays discoverable that
     * every layer carries its own set of these values. */
    const char *vt = vt_title(s_lm_kind);
    if (vt) return vt;
    static char s_title[28];
    snprintf(s_title, sizeof(s_title), "LAYER  L%u/%u  T%u",
             (unsigned)(seq_state.active_layer_idx + 1u),
             (unsigned)seq_state.num_layers,
             (unsigned)(seq_state.selected_track + 1u));
    return s_title;
}

const menu_item_view_t *layermenu_menu_build_items(void)
{
    uint8_t rows[LM_COUNT];
    uint8_t n  = lm_row_list(rows);
    uint8_t li = seq_state.active_layer_idx;
    uint8_t mel = layermenu_active_melodic_layer();
    uint8_t tr = seq_state.selected_track;

    bool chord_mode = false; uint8_t chord_root = 0; chord_type_t ct = CHORD_MAJ;
    if (mel != 0xFF) sequencer_core_get_layer_chord(mel, &chord_mode, &chord_root, &ct);
    bool chord_locked = sequencer_core_progression_get_enabled();

    for (uint8_t i = 0; i < n; i++) {
        menu_item_view_t *it = &s_lm_items[i];
        it->value[0] = '\0';
        switch (rows[i]) {
        case LM_STEPS:
            snprintf(it->label, MENU_LABEL_LEN, "Steps");
            snprintf(it->value, MENU_VALUE_LEN, "%u",
                     (unsigned)seq_state.layers[li].num_steps);
            break;
        case LM_SWING:
            snprintf(it->label, MENU_LABEL_LEN, "Swing");
            ui_swing_format(it->value, MENU_VALUE_LEN, sequencer_core_get_layer_swing(li));
            break;
        case LM_PATCH_SCOPE:
            snprintf(it->label, MENU_LABEL_LEN, "Patch");
            if (mel == 0xFF) snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%s",
                          sequencer_core_get_patch_scope(mel) == SEQ_PATCH_SCOPE_TRACK
                              ? "TRACK" : "LAYER");
            break;
        case LM_GATE: {
            snprintf(it->label, MENU_LABEL_LEN, "Gate");
            ui_gate_format(it->value, MENU_VALUE_LEN, vt_get_gate_pct(s_lm_kind));
            break;
        }
        case LM_GLIDE:
            snprintf(it->label, MENU_LABEL_LEN, "Glide");
            if (s_lm_kind == VT_MELODIC && mel == 0xFF)
                snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%ums",
                          (unsigned)vt_get_glide_ms(s_lm_kind));
            break;
        case LM_GROOVE:
            snprintf(it->label, MENU_LABEL_LEN, "Groove");
            if (mel == 0xFF) snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%u%%",
                          (unsigned)sequencer_core_get_melodic_groove_pct(mel));
            break;
        case LM_CHORD:
            snprintf(it->label, MENU_LABEL_LEN, "Chord");
            /* Read-only while the global progression drives harmony - show
             * "(prog)" so edits don't silently no-op. */
            if (mel == 0xFF)        snprintf(it->value, MENU_VALUE_LEN, "--");
            else if (chord_locked)  snprintf(it->value, MENU_VALUE_LEN, "(prog)");
            else snprintf(it->value, MENU_VALUE_LEN, "%s", chord_mode ? "ON" : "OFF");
            break;
        case LM_ROOT:
            snprintf(it->label, MENU_LABEL_LEN, "Root");
            if (mel == 0xFF || chord_locked) snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%s", chord_root_name(chord_root));
            break;
        case LM_TYPE:
            snprintf(it->label, MENU_LABEL_LEN, "Type");
            if (mel == 0xFF || chord_locked) snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%s", chord_type_name(ct));
            break;
        case LM_TRACK:
            snprintf(it->label, MENU_LABEL_LEN, "Track");
            snprintf(it->value, MENU_VALUE_LEN, "%u", (unsigned)(tr + 1u));
            break;
        case LM_LEVEL:
            snprintf(it->label, MENU_LABEL_LEN, "Level");
            snprintf(it->value, MENU_VALUE_LEN, "%u%%",
                     (unsigned)lroundf(vt_get_level(s_lm_kind) * 100.0f));
            break;
        case LM_KEYTRACK: {
            seq_filter_t f = {0};
            vt_get_filter(s_lm_kind, &f);
            snprintf(it->label, MENU_LABEL_LEN, "Key");
            snprintf(it->value, MENU_VALUE_LEN, "%u%%",
                     (unsigned)lroundf(f.key_track * 100.0f));
            break;
        }
        case LM_FOLLOW: {
            static const char *const follow_names[SEQ_FOLLOW_COUNT] = {
                "CHORD", "ROOT", "OFF"
            };
            snprintf(it->label, MENU_LABEL_LEN, "Follow");
            if (mel == 0xFF) snprintf(it->value, MENU_VALUE_LEN, "--");
            else snprintf(it->value, MENU_VALUE_LEN, "%s",
                          follow_names[sequencer_core_get_track_follow(mel, tr)]);
            break;
        }
        case LM_REPEAT:
            snprintf(it->label, MENU_LABEL_LEN, "Repeat");
            snprintf(it->value, MENU_VALUE_LEN, "%u",
                     (unsigned)sequencer_core_get_track_repeat_rate(li, tr));
            break;
        case LM_MUTE:
            snprintf(it->label, MENU_LABEL_LEN, "Mute");
            snprintf(it->value, MENU_VALUE_LEN, "%s",
                     vt_get_mute(s_lm_kind) ? "ON" : "OFF");
            break;
        case LM_SOLO:
            snprintf(it->label, MENU_LABEL_LEN, "Solo");
            snprintf(it->value, MENU_VALUE_LEN, "%s",
                     vt_get_solo(s_lm_kind) ? "ON" : "OFF");
            break;
        case LM_CLRSOLO:
            snprintf(it->label, MENU_LABEL_LEN, "ClrSolo");
            break;
        case LM_FRAME: {
            uint8_t v = sequencer_core_get_track_wt_frame(mel, tr);
            snprintf(it->label, MENU_LABEL_LEN, "Frame");
            if (v == 0u) snprintf(it->value, MENU_VALUE_LEN, "Auto");
            else         snprintf(it->value, MENU_VALUE_LEN, "%u", (unsigned)(v - 1u));
            break;
        }
        case LM_UNISON:
        case LM_PCM:
            snprintf(it->label, MENU_LABEL_LEN, "%s",
                     rows[i] == LM_UNISON ? "Unison" : "PCM");
            snprintf(it->value, MENU_VALUE_LEN, ">");
            break;
        case LM_BACK:
        default:
            snprintf(it->label, MENU_LABEL_LEN, "< Back");
            break;
        }
    }

    return s_lm_items;
}

bool layermenu_menu_item_is_back(uint8_t idx)
{
    return lm_row_at(idx) == LM_BACK;
}

/* True for rows a turn can change on this layer: the "--" rows of a drum layer
 * and the progression-locked chord rows are inert. */
static bool lm_row_is_editable(uint8_t row)
{
    if (s_lm_kind != VT_MELODIC) {
        switch (row) {
        case LM_LEVEL:
        case LM_KEYTRACK:
            return true;
        case LM_MUTE:  return vt_has_mute(s_lm_kind);
        case LM_SOLO:  return vt_has_solo(s_lm_kind);
        case LM_GATE:  return vt_has_gate(s_lm_kind);
        case LM_GLIDE: return vt_has_glide(s_lm_kind);
        default:       return false;
        }
    }
    bool mel = (layermenu_active_melodic_layer() != 0xFF);
    switch (row) {
    case LM_PATCH_SCOPE:
    case LM_GLIDE:
    case LM_GROOVE:
    case LM_FOLLOW:
    case LM_FRAME:
    case LM_KEYTRACK:
        return mel;
    case LM_CHORD:
    case LM_ROOT:
    case LM_TYPE:
        return mel && !sequencer_core_progression_get_enabled();
    case LM_STEPS:
    case LM_SWING:
    case LM_GATE:
    case LM_TRACK:
    case LM_LEVEL:
    case LM_REPEAT:
    case LM_MUTE:
    case LM_SOLO:
        return true;
    default:
        return false;   /* ClrSolo is an action, dive rows and Back are
                           navigation */
    }
}

/* Click ladder: ClrSolo fires on press and stays out of edit mode, every other
 * editable row toggles editing. Returns the new editing state. */
bool layermenu_menu_handle_click(uint8_t idx)
{
    uint8_t row = lm_row_at(idx);
    if (row == LM_CLRSOLO) {
        /* The row vanishes with the last solo; the cursor then lands on Back,
         * which sits at the same index. */
        sequencer_core_clear_all_solos();
        return false;
    }
    if (!lm_row_is_editable(row)) return false;
    return !seq_state.menu_editing;
}

/* Next repeat rate in the 1->2->4->8->1 cycle. */
static uint8_t lm_next_repeat_rate(uint8_t rr, int delta)
{
    static const uint8_t rates[] = { 1, 2, 4, 8 };
    int n = (int)(sizeof(rates) / sizeof(rates[0]));
    int idx = 0;
    for (int i = 0; i < n; i++) if (rates[i] == rr) idx = i;
    idx = (idx + delta) % n;
    if (idx < 0) idx += n;
    return rates[idx];
}

/* Gate detents: 5% steps up to one step (mirroring the arp GATE control), then
 * whole steps, then Hold. Ascending, so SEQ_GATE_HOLD sorts last. */
static const uint16_t lm_gate_ladder[] = {
    10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100,
    200, 300, 400, 600, 800, SEQ_GATE_HOLD
};

/* Next gate on the ladder in the turn direction, clamped at both ends. A value
 * between detents moves to the nearest detent on the turn's side. */
uint16_t ui_gate_ladder_step(uint16_t pct, int dir)
{
    int n = (int)(sizeof(lm_gate_ladder) / sizeof(lm_gate_ladder[0]));
    if (dir == 0) return pct;
    if (dir > 0) {
        for (int i = 0; i < n; i++) if (lm_gate_ladder[i] > pct) return lm_gate_ladder[i];
        return lm_gate_ladder[n - 1];
    }
    for (int i = n - 1; i >= 0; i--) if (lm_gate_ladder[i] < pct) return lm_gate_ladder[i];
    return lm_gate_ladder[0];
}

void ui_gate_format(char *buf, size_t len, uint16_t gate_pct)
{
    if (gate_pct == SEQ_GATE_HOLD) snprintf(buf, len, "Hold");
    else if (gate_pct > 100u)      snprintf(buf, len, "%u st", (unsigned)(gate_pct / 100u));
    else                           snprintf(buf, len, "%u%%", (unsigned)gate_pct);
}

void layermenu_menu_edit_value(uint8_t idx, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;

    uint8_t row = lm_row_at(idx);
    if (!lm_row_is_editable(row)) return;

    uint8_t li  = seq_state.active_layer_idx;
    uint8_t mel = layermenu_active_melodic_layer();
    uint8_t tr  = seq_state.selected_track;

    switch (row) {
    case LM_STEPS:
        synth_ui_set_layer_steps(li, (seq_state.layers[li].num_steps == SEQ_MAX_STEPS)
                                     ? SEQ_STEPS : SEQ_MAX_STEPS);
        break;
    case LM_SWING:
        sequencer_core_set_layer_swing(
            li, ui_swing_step(sequencer_core_get_layer_swing(li), dir));
        break;
    case LM_PATCH_SCOPE:
        sequencer_core_set_patch_scope(
            mel, sequencer_core_get_patch_scope(mel) == SEQ_PATCH_SCOPE_TRACK
                     ? (uint8_t)SEQ_PATCH_SCOPE_LAYER : (uint8_t)SEQ_PATCH_SCOPE_TRACK);
        break;
    case LM_GATE:
        if (s_lm_kind == VT_ARP || s_lm_kind == VT_DROP) {
            /* 5%/detent within 10..100, as the arp screen's GATE control. */
            int v = SEQ_CLAMP_INT((int)vt_get_gate_pct(s_lm_kind) + dir * 5, 10, 100);
            vt_set_gate_pct(s_lm_kind, (uint16_t)v);
        } else {
            vt_set_gate_pct(s_lm_kind, ui_gate_ladder_step(vt_get_gate_pct(s_lm_kind), dir));
        }
        break;
    case LM_GLIDE: {
        /* 1ms/detent, matching the arp glide resolution; the provider clamps
         * to the target's ceiling. */
        int v = (int)vt_get_glide_ms(s_lm_kind) + dir * 1;
        if (v < 0) v = 0;
        vt_set_glide_ms(s_lm_kind, (uint16_t)v);
        break;
    }
    case LM_GROOVE: {
        /* 5%/detent, matching GATE. */
        int v = SEQ_CLAMP_INT(
            (int)sequencer_core_get_melodic_groove_pct(mel) + dir * 5, 0, 100);
        sequencer_core_set_melodic_groove_pct(mel, (uint8_t)v);
        break;
    }
    case LM_CHORD: {
        bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
        sequencer_core_get_layer_chord(mel, &mode, &root, &ct);
        if (mode) sequencer_core_progression_clear_layer_chord(mel);
        else      sequencer_core_progression_set_layer_chord(mel, root, ct);
        break;
    }
    case LM_ROOT:
    case LM_TYPE: {
        bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
        sequencer_core_get_layer_chord(mel, &mode, &root, &ct);
        if (!mode) break;
        if (row == LM_ROOT) {
            int nr = ((int)root + dir) % 12;
            if (nr < 0) nr += 12;
            root = (uint8_t)nr;
        } else {
            int nct = (int)ct + dir;
            /* Real chords only - CHORD_OFF is a drone affordance. */
            while (nct < 0) nct += CHORD_REAL_COUNT;
            nct %= CHORD_REAL_COUNT;
            ct = (chord_type_t)nct;
        }
        sequencer_core_progression_set_layer_chord(mel, root, ct);
        break;
    }
    case LM_TRACK: {
        int n  = (int)layermenu_num_tracks();
        int nt = (int)seq_state.selected_track + dir;
        if (nt < 0) nt += n; else if (nt >= n) nt -= n;
        seq_state.selected_track = (uint8_t)nt;
        break;
    }
    case LM_LEVEL: {
        /* 5%/detent, matching GATE; the setter re-emits the track. */
        int v = SEQ_CLAMP_INT(
            (int)lroundf(vt_get_level(s_lm_kind) * 100.0f) + dir * 5, 0, 100);
        vt_set_level(s_lm_kind, (float)v / 100.0f);
        break;
    }
    case LM_KEYTRACK: {
        /* 5%/detent. The setter authors the filter and fans out to the rows
         * sharing the voice block. */
        seq_filter_t f;
        if (!vt_get_filter(s_lm_kind, &f)) break;
        sequencer_core_filter_seed_default(&f);
        int v = SEQ_CLAMP_INT((int)lroundf(f.key_track * 100.0f) + dir * 5, 0, 100);
        f.key_track = (float)v / 100.0f;
        vt_set_filter(s_lm_kind, &f);
        break;
    }
    case LM_FOLLOW: {
        int f = ((int)sequencer_core_get_track_follow(mel, tr) + dir) % SEQ_FOLLOW_COUNT;
        if (f < 0) f += SEQ_FOLLOW_COUNT;
        sequencer_core_set_track_follow(mel, tr, (seq_follow_t)f);
        break;
    }
    case LM_REPEAT: {
        uint8_t rr = (uint8_t)sequencer_core_get_track_repeat_rate(li, tr);
        rr = lm_next_repeat_rate(rr, dir);
        sequencer_core_set_track_repeat_rate(li, tr, (seq_repeat_rate_t)rr);
        break;
    }
    case LM_MUTE:
        vt_set_mute(s_lm_kind, !vt_get_mute(s_lm_kind));
        break;
    case LM_SOLO:
        vt_set_solo(s_lm_kind, !vt_get_solo(s_lm_kind));
        break;
    case LM_FRAME: {
        /* One frame per detent; Auto (0) sits below frame 0 (stored 1). */
        uint8_t v = (uint8_t)SEQ_CLAMP_INT(
            (int)sequencer_core_get_track_wt_frame(mel, tr) + dir, 0, 64);
        if (sequencer_core_get_patch_scope(mel) == SEQ_PATCH_SCOPE_TRACK)
            sequencer_core_set_track_wt_frame(mel, tr, v);
        else
            sequencer_core_set_layer_wt_frame(mel, v);
        break;
    }
    default:
        break;
    }
}
