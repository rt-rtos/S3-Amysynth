"""House template: 124 BPM, C minor, light swing.

909 four-on-the-floor with the clap on 2 and 4, closed hats on the 16th
off-beats and the open hat on every 8th off-beat. Filtered-saw bass on the
off-beats, rootless minor-9 chord stabs (Bb D Eb G over C) on a syncopated
3-3-4-3 rhythm. i - iv progression, two bars each.
"""
from spec_lib import *

NAME = 'HOUSE'
BPM = 124
KEY, SCALE = C_, SCALE_MINOR
PROGRESSION = [(C_, CHORD_MIN7, 2), (F, CHORD_MIN7, 2)]
SLOT_STAB = 0
CHORDS = {SLOT_STAB: [58, 62, 63, 67]}      # Bb3 D4 Eb4 G4
SWING = 8

KICK = hits('x...x...x...x...')
CLAP = hits('....x.......x...')
HAT = merge(hits('.x.x.x.x.x.x.x.x', vel=-25), {7: dict(prob=80), 15: dict(prob=60)})
OPEN = hits('..x...x...x...x.')

BASS = line('..x...x..xx...x.', [0, 0, 12, 0, 7])
STAB = hits('...x..x...x..x..')


def drums():
    return drum_layer([
        (PCM_909_BD,       58, KICK, None),
        (PCM_909_CLAP,     60, CLAP, None),
        (PCM_909_HH_SHORT, 62, HAT,  vp(flt=filt(FILTER_HPF, 4000, 0.7), melodic=False)),
        (PCM_909_OH_SHORT, 60, OPEN, vp(flt=filt(FILTER_HPF, 3000, 0.7), trim=0.8, melodic=False)),
    ], swing=SWING)


def bass():
    L = melodic_layer(SWING, gate=60, groove=50, voices=1, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SAW_DOWN, 36,  # C2
          vp(amp_env=env(2, 180, 40, 80), eg1=env(2, 150, 0, 80),
             flt=filt(FILTER_LPF24, 350, 1.5, eg1_cutoff=1.5), dst=dist(DIST_CLIP, 4, 60),
             trim=1.0),
          BASS, follow=FOLLOW_ROOT)
    L['patch'] = P_SAW_DOWN
    return L


def stabs():
    L = melodic_layer(SWING, gate=40, groove=60, voices=2, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SAW_DOWN, SLOT_STAB,
          vp(amp_env=env(2, 280, 0, 250), eg1=env(2, 200, 0, 150),
             flt=filt(FILTER_LPF, 1400, 1.0, eg1_cutoff=1.5), trim=0.7),
          STAB)
    L['patch'] = P_SAW_DOWN
    return L


PARTS = ('drums', 'bass', 'stabs')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE, bus0=dict(eq_low_db=-2, reverb_level=20),
             bus1=dict(eq_low_db=2, level=180))
    p = project(g, [drums(), bass(), stabs()], progression=PROGRESSION, chords=CHORDS)
    return apply_mutes(p, PARTS, mutes)
