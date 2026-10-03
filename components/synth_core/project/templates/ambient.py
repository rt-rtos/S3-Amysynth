"""Ambient template: 70 BPM, C major pentatonic, near-beatless.

A sus2 saw drone on C with no pump, its filter sweeping over 16 bars, under
a sparse Karplus-Strong pluck line whose notes drop in and out by
probability and EVERY, plus a soft sine bell every other bar. Long reverb
and a slow dotted-8th echo carry everything. The kit is a soft shaker only;
its other rows are loaded with sounds and left empty (a project needs its
drum layer).
"""
from spec_lib import *

NAME = 'AMBIENT'
BPM = 70
KEY, SCALE = C_, SCALE_MAJ_PENT

PLUCK = {0: dict(ofs=0), 3: dict(ofs=4, prob=70), 6: dict(ofs=7),
         10: dict(ofs=9, prob=60), 13: dict(ofs=12, every=2), 14: dict(ofs=-3, prob=40)}
BELL = {8: dict(every=2), 0: dict(ofs=7, every=4)}
SHAKER = merge(hits('x.x.x.x.x.x.x.x.', vel=-25, prob=60), {4: dict(prob=100), 12: dict(prob=100)})


def drums():
    return drum_layer([
        (PCM_909_BD_LO,    60, {},     None),
        (PCM_SHAKER,       60, SHAKER, vp(trim=0.6, melodic=False)),
        (PCM_909_RIM,      60, {},     None),
        (PCM_909_OH_SHORT, 60, {},     None),
    ])


def melody():
    L = melodic_layer(0, gate=50, groove=30, voices=3)
    ks = filt(FILTER_LPF, 3500, 0.8)
    ks['feedback'] = 0.992
    # KS excites quietly; the clip stage is makeup gain.
    claim(L, 0, P_KS, 72,                     # C5
          vp(amp_env=env(2, 0, 100, 2500), flt=ks, dst=dist(DIST_CLIP, 4, 100),
             mod=lfo(LFO_TRI, LFO_2BAR, LFO_TGT_PAN, depth=80), trim=1.0),
          PLUCK)
    claim(L, 1, P_SINE, 79,                   # G5
          vp(amp_env=env(400, 0, 100, 4000), trim=0.25), BELL)
    L['patch'] = P_KS
    return L


def bed():
    return drone(True, CHORD_SUS2, 48, wave=WAVE_SAW_DOWN, peak=0.35, duck=0.0,
                 rate=DRONE_RATE_1_1, sweep=(250.0, 900.0), sweep_bars=16, res=0.8,
                 amp_env=env(2000, 300, 100, 3000))


PARTS = ('drums', 'melody', 'drone')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(reverb_level=45, reverb_liveness=90, reverb_damping=40,
                       echo_level=30, echo_feedback=55, echo_tone=-20))
    p = project(g, [drums(), melody()], drone_=bed())
    return apply_mutes(p, PARTS, mutes)
