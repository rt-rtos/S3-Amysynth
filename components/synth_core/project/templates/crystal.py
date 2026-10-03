"""Crystal template: 128 BPM, F# minor, straight (Crystal Castles).

909 four-on-the-floor under a bitcrushed power snare on 2 and 4 and
driving 16th hats, with a noise burst that sweeps down on the last beat of
every other bar. A clipped saw bass in 8ths rides the chord roots (ROOT
follow), a crushed two-octave 16th pulse arpeggio spells each chord, and a
shimmering saw drone in 16th tremolo follows the progression's chords.
i - VI - III - VII (F#m D A E), one bar each; chorus, a big room and a
dotted-8th echo.
"""
from spec_lib import *

NAME = 'CRYSTAL'
BPM = 128
KEY, SCALE = Gb, SCALE_MINOR
PROGRESSION = [(Gb, CHORD_MIN, 1), (D, CHORD_MAJ, 1), (A, CHORD_MAJ, 1), (E, CHORD_MAJ, 1)]

KICK = hits('x...x...x...x...')
SNARE = hits('....X.......X...')
HAT = merge(hits('xxxxxxxxxxxxxxxx', vel=-30), {2: dict(vel=-15), 6: dict(vel=-15),
                                                 10: dict(vel=-15), 14: dict(vel=-15),
                                                 15: dict(prob=60)})
OPEN = hits('..............x.', every=2, vel=-10)

BASS = line('x.x.x.x.x.x.x.x.', [0, 0, 12, 0, 0, 0, 12, 7])
NOISE = hits('............x...', every=2)


def drums():
    return drum_layer([
        (PCM_909_BD,       60, KICK,  vp(dst=dist(DIST_CLIP, 3, 40), melodic=False)),
        (PCM_POWER_SNARE,  62, SNARE, vp(dst=dist(DIST_CRUSH, 2, 70, bits=6, rate=3),
                                         melodic=False)),
        (PCM_909_HH_SHORT, 64, HAT,   vp(flt=filt(FILTER_HPF, 5000, 0.7), trim=0.8,
                                         melodic=False)),
        (PCM_909_OH_SHORT, 62, OPEN,  vp(flt=filt(FILTER_HPF, 3000, 0.7), trim=0.7,
                                         melodic=False)),
    ])


def bass():
    L = melodic_layer(0, gate=55, groove=40, voices=1, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SAW_DOWN, 42,                # F#2
          vp(amp_env=env(2, 160, 50, 60), eg1=env(2, 140, 0, 60),
             flt=filt(FILTER_LPF24, 600, 1.2, eg1_cutoff=1.2), dst=dist(DIST_CLIP, 4, 60),
             trim=0.8),
          BASS, follow=FOLLOW_ROOT)
    # The burst: noise through a resonant band-pass whose EG1 sweep falls
    # over the beat.
    claim(L, 1, P_NOISE, 72,
          vp(amp_env=env(1, 350, 0, 200), eg1=env(1, 380, 0, 200),
             flt=filt(FILTER_BPF, 900, 2.5, eg1_cutoff=2.5), trim=0.35),
          NOISE, follow=FOLLOW_OFF)
    L['patch'] = P_SAW_DOWN
    return L


def arpeggio():
    a = arp(True, P_PULSE, ARP_UP, ARP_RATE_1_16, [66, 69, 73], octaves=2, gate=35,
            quant=ARP_QUANT_CHORD, root=54, scale=SCALE,
            amp_env=env(1, 120, 0, 80), flt=filt(FILTER_LPF, 5000, 1.0), amp=0.5)
    a['dist'] = dist(DIST_CRUSH, 2, 80, bits=5, rate=4)
    return a


def wash():
    return drone(True, CHORD_MIN, 54, wave=WAVE_SAW_DOWN, peak=0.3, duck=0.45,
                 rate=DRONE_RATE_1_16, sweep=(900.0, 3500.0), sweep_bars=8, res=1.0,
                 amp_env=env(400, 300, 100, 900), follow=DRONE_FOLLOW_CHORD)


PARTS = ('drums', 'bass', 'arp', 'drone')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(chorus_level=30, reverb_level=40, reverb_liveness=90,
                       echo_level=18, echo_feedback=35, echo_tone=-10),
             bus1=dict(level=180, reverb_level=15))
    p = project(g, [drums(), bass()], arp_=arpeggio(), drone_=wash(),
                progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
