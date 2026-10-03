"""Witch house template: 70 BPM (140 half-time), F minor, straight.

808 kit pitched down: a long boomy kick, the snare on 2 and 4 drowned in
reverb, and hats in 8th triplets (per-step nudge: the 140 BPM half-time
roll). A sine sub with a pitch drop rides the chord roots (ROOT follow), a
dark saw drone follows the progression's chords under a slow filter sweep,
a Juno choir pad is voiced over each chord, and a sparse sine bell in
quarter notes snaps to each chord through a long echo. One FX bus for
everything, so the kit sits in the same room as the pads.
i - VI - iv - V (Fm Db Bbm C), two bars each.
"""
from spec_lib import *

NAME = 'WITCH HOUSE'
BPM = 70
KEY, SCALE = F, SCALE_HARM_MINOR
PROGRESSION = [(F, CHORD_MIN, 2), (Db, CHORD_MAJ, 2), (Bb, CHORD_MIN, 2), (C_, CHORD_MAJ, 2)]
P_JUNO_CHOIR = 6

KICK = merge(hits('x.........x.....'), {7: dict(every=2, vel=-15)})
SNARE = hits('....x.......x...')


def triplet_hats():
    """8th triplets at 12 ticks per 16th: per beat the hits sit on ticks 0,
    16 and 32, i.e. step 0, step 1 nudged +4, step 3 nudged -4."""
    out = {}
    for beat in range(4):
        s = beat * 4
        out[s] = dict(vel=-20)
        out[s + 1] = dict(nudge=4, vel=-35)
        out[s + 3] = dict(nudge=-4, vel=-30)
    out[15]['prob'] = 70
    out[13]['prob'] = 80
    return out


HAT = triplet_hats()
OPEN = hits('..........x.....', every=2, vel=-20)

SUB = line('x.........x.....', [0, 0])
BELL = {0: dict(ofs=0), 4: dict(ofs=7, prob=70), 8: dict(ofs=3), 12: dict(ofs=12, every=2)}
PAD = hits('x...............', every=2)


def drums():
    return drum_layer([
        (PCM_808_BD3,  52, KICK,  vp(amp_env=env(0, 900, 0, 400), melodic=False)),
        (PCM_808_SD2,  53, SNARE, vp(trim=0.9, melodic=False)),
        (PCM_808_HH,   58, HAT,   vp(flt=filt(FILTER_HPF, 3500, 0.7), trim=0.7, melodic=False)),
        (PCM_808_OH,   56, OPEN,  vp(trim=0.5, melodic=False)),
    ])


def sub():
    L = melodic_layer(0, gate=GATE_HOLD, groove=20, voices=1, porta=40, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SINE, 29,                    # F1
          vp(amp_env=env(1, 0, 100, 900), eg1=env(0, 90, 0, 60),
             flt=filt(FILTER_LPF, 900, 0.7, eg1_pitch=0.8), dst=dist(DIST_CLIP, 2, 40),
             trim=0.5),
          SUB, follow=FOLLOW_ROOT)
    L['patch'] = P_SINE
    return L


def choir():
    L = melodic_layer(0, gate=100, groove=0, voices=1, chord=PROGRESSION[0][:2])
    chord_rows(L, P_JUNO_CHOIR, [53, 56, 60, 65],   # F3 Ab3 C4 F4 anchors
               vp(amp_env=env(900, 0, 100, 1200), flt=filt(FILTER_LPF, 1800, 0.8), trim=0.55),
               PAD)
    L['patch'] = P_JUNO_CHOIR
    return L


def bell():
    return arp(True, P_SINE, ARP_SLOT, ARP_RATE_1_4, [77, 80, 84, ARP_REST, 82, 80, 79, ARP_REST],
               gate=30, quant=ARP_QUANT_CHORD, root=53, scale=SCALE,
               amp_env=env(2, 900, 0, 900), flt=filt(FILTER_LPF, 4000, 0.7), amp=1.0)


def haze():
    return drone(True, CHORD_MIN, 41, wave=WAVE_SAW_DOWN, peak=0.22, duck=0.0,
                 rate=DRONE_RATE_1_1, sweep=(220.0, 900.0), sweep_bars=16, res=1.4,
                 amp_env=env(1500, 300, 100, 2500), follow=DRONE_FOLLOW_CHORD)


PARTS = ('drums', 'sub', 'choir', 'arp', 'drone')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(reverb_level=45, reverb_liveness=95, reverb_damping=60,
                       echo_level=25, echo_feedback=55, echo_tone=-30, eq_low_db=3),
             split=SPLIT_CLIPS)    # one bus: the whole kit sits in the room
    p = project(g, [drums(), sub(), choir()], arp_=bell(), drone_=haze(),
                progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
