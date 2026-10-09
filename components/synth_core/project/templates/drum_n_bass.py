"""Drum and bass template: 174 BPM, F minor, straight.

Two-step: kick on 1 and the and of 3, snare on 2 and 4 with ghost notes,
off-beat hats with 16th ghosts, 8th ride. Detuned reese bass (the
pulse + detuned saw bass preset) on long notes following the chord root,
with a one-bar filter wobble. Slow-attack saw pad voiced over i - VI,
two bars each.
"""
from spec_lib import *

NAME = 'DRUM N BASS'
BPM = 174
KEY, SCALE = F, SCALE_MINOR
PROGRESSION = [(F, CHORD_MIN7, 2), (Db, CHORD_MAJ7, 2)]

KICK = merge(hits('x.........x.....'), {13: dict(every=2, vel=-15)})
SNARE = merge(hits('....x.......x...'),
              {7: dict(vel=-45, prob=60), 15: dict(vel=-40, prob=50)})
HAT = merge(hits('..x...x...x...x.', vel=-10), {s: dict(vel=-40, prob=50) for s in (1, 9, 13)})
RIDE = hits('x.x.x.x.x.x.x.x.', vel=-30)

BASS = line('x.........x..x..', [0, 0, 12])
PAD = hits('x.......x.......')


def drums():
    return drum_layer([
        (PCM_909_BD,       60, KICK,  None),
        (PCM_909_SD,       63, SNARE, None),
        (PCM_909_HH_SHORT, 64, HAT,   vp(flt=filt(FILTER_HPF, 5000, 0.7), melodic=False)),
        (PCM_909_RIDE,     62, RIDE,  vp(trim=0.6, melodic=False)),
    ])


def bass():
    L = melodic_layer(0, gate=100, groove=30, voices=1, porta=20, chord=PROGRESSION[0][:2])
    claim(L, 0, P_BASS_SUB_DETUNE, 41,  # F2
          vp(amp_env=env(5, 0, 100, 350), flt=filt(FILTER_LPF24, 500, 1.2),
             mod=lfo(LFO_TRI, NOTE_DIV_1_1, LFO_TGT_FILTER, depth=40, flt_oct_q=4), trim=0.4),
          BASS, follow=FOLLOW_ROOT)
    L['patch'] = P_BASS_SUB_DETUNE
    return L


def pad():
    L = melodic_layer(0, gate=100, groove=0, voices=2, chord=PROGRESSION[0][:2])
    chord_rows(L, P_SAW_DOWN, [56, 60, 63, 67],   # Ab3 C4 Eb4 G4 anchors
               vp(amp_env=env(250, 0, 100, 1800), flt=filt(FILTER_LPF, 1800, 0.8),
                  mod=lfo(LFO_SINE, NOTE_DIV_4BAR, LFO_TGT_FILTER, depth=40, flt_oct_q=3),
                  trim=0.3),
               PAD)
    L['patch'] = P_SAW_DOWN
    return L


PARTS = ('drums', 'bass', 'pad')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE, bus0=dict(reverb_level=25, reverb_liveness=80),
             bus1=dict(eq_low_db=1, bus_dist_type=DIST_CLIP, bus_dist_drive=2, bus_dist_mix=25,
                       level=170))
    p = project(g, [drums(), bass(), pad()], progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
