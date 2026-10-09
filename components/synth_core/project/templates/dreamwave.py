"""Dreamwave template: 108 BPM, Bb minor, straight (Pastel Ghost).

LinnDrum kick on 1 and 3 with a push into 3, the gated power snare on 2
and 4, 8th hats and a soft 16th tambourine. An octave-bouncing 8th saw
bass rides the chord roots (ROOT follow), the stutter drone pumps a saw
chord in 8ths under CHORD follow, and a chorused triangle arpeggio
sparkles over each chord through a dotted-8th echo.
i - v - VI - III (Bbm Fm Gb Db), one bar each.
"""
from spec_lib import *

NAME = 'DREAMWAVE'
BPM = 108
KEY, SCALE = Bb, SCALE_MINOR
PROGRESSION = [(Bb, CHORD_MIN, 1), (F, CHORD_MIN, 1), (Gb, CHORD_MAJ, 1), (Db, CHORD_MAJ, 1)]

KICK = hits('x......x.x......')
SNARE = hits('....x.......x...')
HAT = hits('x.x.x.x.x.x.x.x.', vel=-20)
TAMB = merge(hits('.x.x.x.x.x.x.x.x', vel=-35), {7: dict(prob=70), 15: dict(prob=60)})

BASS = line('x.x.x.x.x.x.x.x.', [0, 12] * 4)


def drums():
    return drum_layer([
        (PCM_LINN_BD,           60, KICK,  None),
        (PCM_POWER_GATED_SNARE, 60, SNARE, None),
        (PCM_LINN_HH,           62, HAT,   vp(flt=filt(FILTER_HPF, 3500, 0.6), melodic=False)),
        (PCM_LINN_TAMB,         60, TAMB,  vp(trim=0.6, melodic=False)),
    ])


def bass():
    L = melodic_layer(0, gate=45, groove=40, voices=1, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SAW_DOWN, 46,                # Bb2
          vp(amp_env=env(2, 150, 55, 70), eg1=env(2, 110, 0, 60),
             flt=filt(FILTER_LPF24, 700, 1.0, eg1_cutoff=1.0), dst=dist(DIST_CLIP, 3, 50),
             trim=0.8),
          BASS, follow=FOLLOW_ROOT)
    L['patch'] = P_SAW_DOWN
    return L


def sparkle():
    return arp(True, P_TRIANGLE, ARP_UP, NOTE_DIV_1_8, [70, 73, 77], octaves=2, gate=40,
               quant=ARP_QUANT_CHORD, root=58, scale=SCALE,
               amp_env=env(2, 220, 20, 300), flt=filt(FILTER_LPF, 4500, 0.8), amp=0.5)


def pump():
    return drone(True, CHORD_MIN, 58, wave=WAVE_SAW_DOWN, peak=0.4, duck=0.65,
                 rate=NOTE_DIV_1_8, sweep=(700.0, 2600.0), sweep_bars=4, res=1.0,
                 amp_env=env(150, 300, 100, 800), follow=DRONE_FOLLOW_CHORD)


PARTS = ('drums', 'bass', 'arp', 'drone')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(chorus_level=50, reverb_level=35, reverb_liveness=88,
                       echo_level=22, echo_feedback=40, echo_tone=-15),
             bus1=dict(level=220, reverb_level=25))
    p = project(g, [drums(), bass()], arp_=sparkle(), drone_=pump(),
                progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
