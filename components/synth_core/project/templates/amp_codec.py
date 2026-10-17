"""Reader/writer for S3-Amysynth project files (Pnn.amp).

Mirrors components/synth_core/project/project_snapshot.c field for field:
GLOB v7, LAYR v27, ARP v18, DROP v4, DRON v8, DSTD v5, PROG v2, CHRD v2, CLIP v3, MIXR v3,
PGEN v2, WTCU v3, FMCU v2,
inside project_store.c's 32-byte header (magic "AMYP", fmt 1, name, len, CRC32).
Field order IS the format, and the record layout follows the firmware's: every
struct record is a block, {len u16, payload}, written with W.block() and opened
with R.block(). A field is appended at the end of its record's block without
a version bump; when the firmware bumps a section version (a reshape), update
the matching read_/write_ pair and VER here (gen_templates.py fails the build
until they agree).

Readers start each record from its seed and fill fields in writer order until
the block runs out, so a record written before a field existed reads that
field at the seed; bytes after the last known field are ignored. The seed is
the record's default_* dict below, or zero where the record has none (as the
firmware's parser seeds it). decode_file() skips an optional section at
another version with a note in p['skipped']; GLOB or LAYR at another version
raises.

State is plain dicts/lists so a spec can build it declaratively. The helpers
default_* return the firmware's init values (voice_params_init_defaults etc.),
except default_layer() and default_fx(): those are the authoring baselines the
templates are written against and differ from the firmware's boot values
(unison 1/12/50/100 instead of 0, repeat_rate, num_voices, the FX levels and
bus distortion). The templates' sound depends on them, so they stay as they
are.
"""
import struct
import zlib

MAGIC = 0x50594D41
FMT_VERSION = 1
NAME_LEN = 16

TAG = {k: struct.unpack('<I', k.encode())[0] for k in ('GLOB', 'LAYR', 'ARP ', 'DROP', 'DRON', 'DSTD', 'PROG', 'CHRD',
                                                       'CLIP', 'MIXR', 'PGEN', 'WTCU', 'FMCU')}
VER = {'GLOB': 7, 'LAYR': 27, 'ARP ': 18, 'DROP': 4, 'DRON': 8, 'DSTD': 5, 'PROG': 2, 'CHRD': 2,
       'CLIP': 3, 'MIXR': 3, 'PGEN': 2, 'WTCU': 3, 'FMCU': 2}
REQUIRED = ('GLOB', 'LAYR')   # another version of these refuses the file

MAX_LAYERS = 4
SEQ_TRACKS = 5
SEQ_MAX_STEPS = 32
FX_BUS_COUNT = 4
ARP_MAX_SLOTS = 8
DROP_SEG_MAX = 16
DROP_JACKPOT_BYTES = 16   # one bit per cell of the 16 x 8 field
DROP_BOUNCE_DEFAULT = 179  # restitution 0.70 in Q8
SEQ_CHORD_SLOTS = 8
SEQ_CHORD_MAX_NOTES = 5
CLIP_SLOT_COUNT = 2
MIX_MISC_COUNT = 6   # ARP, DRONE_STD, DRONE_STUT, LIVE, CLIP0, CLIP1
SEQ_EGT_COUNT = 5
FX_PARAM_UNSET = -32768


class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack('<B', v & 0xFF)
    def i8(self, v): self.b += struct.pack('<b', v)
    def u16(self, v): self.b += struct.pack('<H', v)
    def i16(self, v): self.b += struct.pack('<h', v)
    def u32(self, v): self.b += struct.pack('<I', v)
    def f32(self, v): self.b += struct.pack('<f', v)
    def raw(self, bs): self.b += bytes(bs)

    def put(self, d, fields):
        """fields: [(key, type)]; type 'b1' is a bool stored as one byte."""
        for k, t in fields:
            if t == 'b1':
                self.u8(1 if d[k] else 0)
            else:
                getattr(self, t)(d[k])

    def block(self, body):
        """One record: u16 length, then body (bytes or a W)."""
        body = bytes(body.b if isinstance(body, W) else body)
        if len(body) > 0xFFFF:
            raise ValueError('record of %d bytes does not fit a block' % len(body))
        self.u16(len(body)); self.raw(body)

    def section(self, name, body):
        self.u32(TAG[name]); self.u8(VER[name]); self.u32(len(body)); self.raw(body)


class R:
    """Reader over a section body or a record. A read past the end returns None
    and every later read from this reader does too, as the firmware's
    tlv_reader_t."""

    def __init__(self, b):
        self.b = bytes(b); self.p = 0; self.err = False

    def _take(self, fmt):
        n = struct.calcsize(fmt)
        if self.err or n > len(self.b) - self.p:
            self.err = True
            return None
        v = struct.unpack_from(fmt, self.b, self.p)[0]; self.p += n
        return v

    def u8(self): return self._take('<B')
    def i8(self): return self._take('<b')
    def u16(self): return self._take('<H')
    def i16(self): return self._take('<h')
    def u32(self): return self._take('<I')
    def f32(self): return self._take('<f')

    def raw(self, n):
        if self.err or n > len(self.b) - self.p:
            self.err = True
            return None
        v = self.b[self.p:self.p + n]; self.p += n
        return v

    def block(self):
        """The next record as a sub-reader. Fewer than 2 bytes left, or this
        reader already failed: an empty reader, and this one is unchanged. A
        length past the end is clamped (the record reads short)."""
        if self.err or len(self.b) - self.p < 2:
            return R(b'')
        n = min(self.u16(), len(self.b) - self.p)
        sub = R(self.b[self.p:self.p + n]); self.p += n
        return sub

    def fill(self, d, fields):
        """Read fields (as W.put) into d; a field the record lacks keeps d's value."""
        for k, t in fields:
            v = self.u8() if t == 'b1' else getattr(self, t)()
            if v is not None:
                d[k] = (v != 0) if t == 'b1' else v
        return d

    def fill_list(self, lst, t):
        """Read len(lst) values of type t into lst in place."""
        for i in range(len(lst)):
            v = getattr(self, t)()
            if v is not None:
                lst[i] = v
        return lst

    def done(self): return self.p >= len(self.b)


def _record(fn, *args):
    """Body bytes of one record written by fn(w, *args)."""
    w = W(); fn(w, *args)
    return bytes(w.b)


# note_div_t order (note_div.h), by label; the index is what the file stores for
# every rate and the echo note.
ECHO_DIV = {k: i for i, k in enumerate(('4BAR', '2BAR', '1/1', '1/2', '1/4', '1/8', '1/16', '1/32',
                                        '1/2T', '1/4T', '1/8T', '1/16T', '1/32T',
                                        '1/2D', '1/4D', '1/8D', '1/16D', '1/32D',
                                        '8BAR', '16BAR'))}


# ── defaults ────────────────────────────────────────────────────────────────

def default_env():
    return dict(attack_ms=0, decay_ms=0, sustain_pct=0, release_ms=0, eg_type=0)


def default_filter():
    return dict(filter_type=0, cutoff_hz=0.0, resonance=0.0, enabled=False, feedback=0.0,
                eg_depth=[[0.0] * SEQ_EGT_COUNT for _ in range(2)], ks_duty_ofs=0.0, key_track=0.0)


def default_lfo():
    return dict(enabled=False, mode=0, wave=0, rate=ECHO_DIV['1/8'], depth=0, targets=0,
                wob_rate=ECHO_DIV['1/8'], wob_depth=0, wob_reach=0, flt_oct_q=0)


def default_lfo2():
    """LFO2's seed in voice_params_init_defaults(): sine, 1/1, depth 50, off, no targets."""
    return dict(default_lfo(), rate=ECHO_DIV['1/1'], depth=50)


def default_dist():
    return dict(type=0, drive=2, bits=8, rate=8, mix=100)


def default_vp():
    """voice_params_init_defaults(): zeroed, unity amp_trim, LFO rates 1/8, audible dist spare."""
    return dict(env=default_env(), env1=default_env(), filter=default_filter(), lfo=default_lfo(),
                lfo2=default_lfo2(),
                env_authored=False, env1_authored=False, filter_authored=False, lfo_authored=False,
                lfo2_authored=False, amp_trim=1.0, dist=default_dist(), dist_authored=False)


def grid(fill=0):
    return [[fill] * SEQ_MAX_STEPS for _ in range(SEQ_TRACKS)]


def default_layer(kind):
    """Neutral per-step values as sequencer_core_add_layer leaves them (an
    authoring baseline, see the module docstring)."""
    return dict(type=kind, num_steps=16, patch=0, synth_flags=0, num_voices=1, chord_mode=False,
                chord_root=0, chord_type=0, swing_pct=0,
                tracks=[dict(base_note=60, patch=0, pcm_preset=0, pcm_mode=0,
                             uni_count=1, uni_detune=12, uni_spread=50, uni_blend=100,
                             repeat_rate=1, mute=False, solo=False, follow=0, wt_frame=0, vp=default_vp())
                        for _ in range(SEQ_TRACKS)],
                grid=grid(0), step_note=grid(60), step_pitch_ofs=grid(0), step_prob=grid(100),
                step_ratchet=grid(1), step_every=grid(1), step_prev=grid(0), step_transform=grid(0),
                step_quant_bypass=grid(0), step_nudge=grid(0), step_frame=grid(0), step_velocity_adj=grid(0),
                step_ratchet_taper=grid(0),
                gate_pct=92, portamento_ms=0, groove_pct=100, fm_algo_override=0xFF,
                vp_src=[0] * SEQ_TRACKS, vp_layer=default_vp(), patch_scope=0, num_tracks=4)


def default_fx():
    """One FX bus (an authoring baseline, see the module docstring)."""
    return dict(eq_low_db=0, eq_mid_db=0, eq_high_db=0, echo_level=0, chorus_level=0, reverb_level=0,
                echo_delay_ms=FX_PARAM_UNSET, echo_feedback=FX_PARAM_UNSET, echo_tone=FX_PARAM_UNSET,
                echo_sync=1, echo_div=ECHO_DIV['1/8D'],
                reverb_liveness=FX_PARAM_UNSET, reverb_damping=FX_PARAM_UNSET,
                reverb_xover_hz=FX_PARAM_UNSET, chorus_rate=FX_PARAM_UNSET,
                chorus_depth=FX_PARAM_UNSET, bus_dist_type=0, bus_dist_drive=2, bus_dist_bits=8,
                bus_dist_rate=8, bus_dist_mix=100, level=100, chorus_delay=FX_PARAM_UNSET)


def _zero(fields):
    """Zero seed for a record without a default_* dict."""
    return {k: (False if t == 'b1' else 0.0 if t == 'f32' else 0) for k, t in fields}


# ── record codecs (ser_env / ser_filter / ser_lfo / ser_dist / ser_vp) ──────
# Each w_<record>(w, x) writes one block into w; each r_<record>(r, seed)
# opens the next block of r and fills seed in place.

ENV_FIELDS = [('attack_ms', 'u32'), ('decay_ms', 'u32'), ('sustain_pct', 'u8'),
              ('release_ms', 'u32'), ('eg_type', 'u8')]


def w_env(w, e):
    w.block(_record(W.put, e, ENV_FIELDS))


def r_env(r, e):
    return r.block().fill(e, ENV_FIELDS)


FILTER_HEAD = [('filter_type', 'u8'), ('cutoff_hz', 'f32'), ('resonance', 'f32'), ('enabled', 'b1'),
               ('feedback', 'f32')]
FILTER_TAIL = [('ks_duty_ofs', 'f32'), ('key_track', 'f32')]


def w_filter(w, f):
    def body(b):
        b.put(f, FILTER_HEAD)
        for eg in range(2):
            for t in range(SEQ_EGT_COUNT):
                b.f32(f['eg_depth'][eg][t])
        b.put(f, FILTER_TAIL)
    w.block(_record(body))


def r_filter(r, f):
    b = r.block()
    b.fill(f, FILTER_HEAD)
    for eg in range(2):
        b.fill_list(f['eg_depth'][eg], 'f32')
    return b.fill(f, FILTER_TAIL)


LFO_FIELDS = [('enabled', 'b1')] + [(k, 'u8') for k in ('mode', 'wave', 'rate', 'depth', 'targets',
                                                        'wob_rate', 'wob_depth', 'wob_reach',
                                                        'flt_oct_q')]


def w_lfo(w, l):
    w.block(_record(W.put, l, LFO_FIELDS))


def r_lfo(r, l):
    return r.block().fill(l, LFO_FIELDS)


DIST_FIELDS = [(k, 'u8') for k in ('type', 'drive', 'bits', 'rate', 'mix')]


def w_dist(w, d):
    w.block(_record(W.put, d, DIST_FIELDS))


def r_dist(r, d):
    return r.block().fill(d, DIST_FIELDS)


VP_AUTHORED = [(k, 'b1') for k in ('env_authored', 'env1_authored', 'filter_authored',
                                   'lfo_authored', 'lfo2_authored')]


def w_vp(w, vp):
    def body(b):
        w_env(b, vp['env']); w_env(b, vp['env1']); w_filter(b, vp['filter']); w_lfo(b, vp['lfo'])
        w_lfo(b, vp['lfo2'])
        b.put(vp, VP_AUTHORED)
        b.f32(vp['amp_trim']); w_dist(b, vp['dist']); b.u8(1 if vp['dist_authored'] else 0)
    w.block(_record(body))


def r_vp(r):
    """Seeds default_vp() whatever the caller holds, as de_vp()."""
    b = r.block()
    vp = default_vp()
    r_env(b, vp['env']); r_env(b, vp['env1']); r_filter(b, vp['filter']); r_lfo(b, vp['lfo'])
    r_lfo(b, vp['lfo2'])
    b.fill(vp, VP_AUTHORED + [('amp_trim', 'f32')])
    r_dist(b, vp['dist'])
    return b.fill(vp, [('dist_authored', 'b1')])


# voice_unison_t (ser_unison): flat uni_* keys of the owning dict.
UNISON_FIELDS = [('uni_count', 'u8'), ('uni_detune', 'u8'), ('uni_spread', 'u8'), ('uni_blend', 'u8')]


def w_unison(w, d):
    w.block(_record(W.put, d, UNISON_FIELDS))


def r_unison(r, d):
    return r.block().fill(d, UNISON_FIELDS)


# ── sections ────────────────────────────────────────────────────────────────

GLOB_FIELDS = [('bpm', 'u16'), ('master_volume', 'f32'), ('quant_enabled', 'b1'), ('quant_root', 'u8'),
               ('quant_scale', 'u8'), ('drum_engine', 'u8'), ('presets_alter_global', 'b1'),
               ('split_flags', 'u8')]
GLOB_FX_FIELDS = [('eq_low_db', 'i8'), ('eq_mid_db', 'i8'), ('eq_high_db', 'i8'),
                  ('echo_level', 'u8'), ('chorus_level', 'u8'), ('reverb_level', 'u8'),
                  ('echo_delay_ms', 'i16'), ('echo_feedback', 'i16'), ('echo_tone', 'i16'),
                  ('echo_sync', 'u8'), ('echo_div', 'u8'),
                  ('reverb_liveness', 'i16'), ('reverb_damping', 'i16'), ('reverb_xover_hz', 'i16'),
                  ('chorus_rate', 'i16'), ('chorus_depth', 'i16'), ('bus_dist_type', 'u8'),
                  ('bus_dist_drive', 'u8'), ('bus_dist_bits', 'u8'), ('bus_dist_rate', 'u8'),
                  ('bus_dist_mix', 'u8'), ('level', 'u8'), ('chorus_delay', 'i16')]


def w_glob(g):
    """The section scalars, then one record per FX bus."""
    w = W()
    w.put(g, GLOB_FIELDS)
    for bus in range(FX_BUS_COUNT):
        w.block(_record(W.put, g['fx'][bus], GLOB_FX_FIELDS))
    return bytes(w.b)


def r_glob(r):
    g = r.fill(_zero(GLOB_FIELDS), GLOB_FIELDS)
    g['fx'] = [r.block().fill(default_fx(), GLOB_FX_FIELDS) for _ in range(FX_BUS_COUNT)]
    return g


LAYR_HEAD = [('type', 'u8'), ('num_steps', 'u8'), ('patch', 'u16'), ('synth_flags', 'u32'),
             ('num_voices', 'u8'), ('chord_mode', 'b1'), ('chord_root', 'u8'), ('chord_type', 'u8'),
             ('swing_pct', 'u8')]
LAYR_ROW_HEAD = [('base_note', 'u8'), ('patch', 'u16'), ('pcm_preset', 'u16'), ('pcm_mode', 'u8')]
LAYR_ROW_TAIL = [('repeat_rate', 'u8'), ('mute', 'b1'), ('solo', 'b1'), ('follow', 'u8'),
                 ('wt_frame', 'u8')]
LAYR_NOTE_FX = [('gate_pct', 'u16'), ('portamento_ms', 'u16'), ('groove_pct', 'u8'),
                ('fm_algo_override', 'u8')]
LAYR_TAIL = [('patch_scope', 'u8'), ('num_tracks', 'u8')]
STEP_ARRAYS = [('grid', 'B'), ('step_note', 'B'), ('step_pitch_ofs', 'b'), ('step_prob', 'B'),
               ('step_ratchet', 'B'), ('step_every', 'B'), ('step_prev', 'B'),
               ('step_transform', 'B'), ('step_quant_bypass', 'B'), ('step_nudge', 'b'),
               ('step_frame', 'B'), ('step_velocity_adj', 'b'), ('step_ratchet_taper', 'b')]


def w_layr(L):
    """Records: the head, one per row (its unison and voice blocks nested),
    the step arrays, the note-FX tail (the layer's voice block nested)."""
    def row(b, T):
        b.put(T, LAYR_ROW_HEAD)
        w_unison(b, T)
        b.put(dict(T, wt_frame=T.get('wt_frame', 0)), LAYR_ROW_TAIL)
        w_vp(b, T['vp'])

    def steps(b):
        for k, fmt in STEP_ARRAYS:
            for t in range(SEQ_TRACKS):
                b.raw(struct.pack('<%d%s' % (SEQ_MAX_STEPS, fmt), *[int(x) for x in L[k][t]]))

    def tail(b):
        b.put(L, LAYR_NOTE_FX)
        for t in range(SEQ_TRACKS):
            b.u8(L['vp_src'][t])
        w_vp(b, L['vp_layer'])
        b.put(L, LAYR_TAIL)

    w = W()
    w.block(_record(W.put, L, LAYR_HEAD))
    for t in range(SEQ_TRACKS):
        w.block(_record(row, L['tracks'][t]))
    w.block(_record(steps))
    w.block(_record(tail))
    return bytes(w.b)


def r_layr(r):
    L = default_layer(0)
    r.block().fill(L, LAYR_HEAD)
    for t in range(SEQ_TRACKS):
        b = r.block()
        b.fill(L['tracks'][t], LAYR_ROW_HEAD)
        r_unison(b, L['tracks'][t])
        b.fill(L['tracks'][t], LAYR_ROW_TAIL)
        L['tracks'][t]['vp'] = r_vp(b)
    b = r.block()
    n = SEQ_TRACKS * SEQ_MAX_STEPS
    for k, fmt in STEP_ARRAYS:
        bs = b.raw(n)
        if bs is not None:
            vals = struct.unpack('<%d%s' % (n, fmt), bs)
            L[k] = [list(vals[t * SEQ_MAX_STEPS:(t + 1) * SEQ_MAX_STEPS]) for t in range(SEQ_TRACKS)]
    b = r.block()
    b.fill(L, LAYR_NOTE_FX)
    b.fill_list(L['vp_src'], 'u8')
    L['vp_layer'] = r_vp(b)
    return b.fill(L, LAYR_TAIL)


ARP_HEAD = [('enabled', 'b1'), ('patch', 'u16'), ('dir', 'u8'), ('octaves', 'u8'), ('rate', 'u8'),
            ('gate_pct', 'u8'), ('scale', 'u8'), ('root', 'u8'), ('portamento_ms', 'u16'),
            ('amp_scale', 'f32')]
ARP_MID = [('lfo2_authored', 'b1'), ('quant_mode', 'u8')]
ARP_TAIL = [('muted', 'b1'), ('solo', 'b1')]


def w_arp(a):
    """One record: scalars, the slots as a raw array, the env/filter/lfo/dist/unison records nested."""
    def body(b):
        b.put(a, ARP_HEAD)
        for i in range(ARP_MAX_SLOTS):
            b.i16(a['slots'][i])
        w_env(b, a['env']); w_env(b, a['env2']); w_filter(b, a['filter']); w_lfo(b, a['lfo'])
        w_lfo(b, a['lfo2'])
        b.put(a, ARP_MID)
        w_dist(b, a['dist'])
        b.put(a, ARP_TAIL)
        w_unison(b, a)
    w = W()
    w.block(_record(body))
    return bytes(w.b)


def r_arp(r):
    b = r.block()
    a = b.fill(_zero(ARP_HEAD + ARP_MID + ARP_TAIL + UNISON_FIELDS), ARP_HEAD)
    a['slots'] = b.fill_list([0] * ARP_MAX_SLOTS, 'i16')
    a['env'] = r_env(b, default_env()); a['env2'] = r_env(b, default_env())
    a['filter'] = r_filter(b, default_filter()); a['lfo'] = r_lfo(b, default_lfo())
    a['lfo2'] = r_lfo(b, default_lfo2())
    b.fill(a, ARP_MID)
    a['dist'] = r_dist(b, default_dist())
    b.fill(a, ARP_TAIL)
    return r_unison(b, a)


DROP_HEAD = [('enabled', 'b1'), ('rate', 'u8'), ('loop_bars', 'u8'), ('octaves', 'u8'), ('seed', 'u32'),
             ('seed_frozen', 'b1'), ('scale', 'u8'), ('root', 'u8'), ('sub_root', 'u8'),
             ('quant_mode', 'u8')]
DROP_VOICE_HEAD = [('patch', 'u16'), ('portamento_ms', 'u16')]
DROP_VOICE_TAIL = [('muted', 'b1'), ('solo', 'b1'), ('gate_pct', 'u8')]


def w_drop(d):
    """The ball drop: the loop record (scalars, the segment records nested,
    the jackpot map raw), then one record per DROP and SUB voice."""
    def loop(b):
        b.put(d, DROP_HEAD)
        b.u8(len(d['segs']))
        for i in range(DROP_SEG_MAX):
            s = d['segs'][i] if i < len(d['segs']) else (0, 0, 0, 0, 0)
            b.block(bytes(v & 0xFF for v in s))
        b.raw(bytes(d['jackpot']).ljust(DROP_JACKPOT_BYTES, b'\0')[:DROP_JACKPOT_BYTES])
        b.u8(d.get('bounce', DROP_BOUNCE_DEFAULT))

    def voice(b, v):
        b.put(v, DROP_VOICE_HEAD)
        w_vp(b, v['vp'])
        w_unison(b, v)
        b.put(v, DROP_VOICE_TAIL)

    w = W()
    w.block(_record(loop))
    for v in d['voices']:
        w.block(_record(voice, v))
    return bytes(w.b)


def r_drop(r):
    b = r.block()
    d = b.fill(_zero(DROP_HEAD), DROP_HEAD)
    n = b.u8() or 0
    segs = [tuple(b.block().fill_list([0] * 5, 'u8')) for _ in range(DROP_SEG_MAX)]
    d['segs'] = segs[:n]
    jp = b.raw(DROP_JACKPOT_BYTES)
    d['jackpot'] = bytes(jp) if jp is not None else bytes(DROP_JACKPOT_BYTES)
    bounce = b.u8()
    d['bounce'] = DROP_BOUNCE_DEFAULT if bounce is None else bounce
    d['voices'] = []
    for _ in range(2):
        vb = r.block()
        v = vb.fill(_zero(DROP_VOICE_HEAD + UNISON_FIELDS + DROP_VOICE_TAIL), DROP_VOICE_HEAD)
        v['vp'] = r_vp(vb)
        r_unison(vb, v)
        d['voices'].append(vb.fill(v, DROP_VOICE_TAIL))
    return d


DRON_FIELDS = [('enabled', 'b1'), ('chord', 'u8'), ('root', 'u8'), ('patch', 'u16'),
               ('amp_peak', 'f32'), ('amp_duck', 'f32'), ('rate', 'u8'),
               ('sub_enabled', 'b1'), ('sub_interval', 'i8'), ('gate_len', 'f32'),
               ('swing', 'u8'), ('blip', 'f32'), ('pattern', 'u8'), ('follow', 'u8')]


DSTD_FIELDS = [('enabled', 'b1'), ('chord', 'u8'), ('root', 'u8'), ('follow', 'u8'),
               ('level', 'f32'), ('patch', 'u16'), ('sub_enabled', 'b1'), ('sub_interval', 'i8')]


def w_fields_vp(fields, d):
    """The drone sections: the scalar record, then the voice_params_t record."""
    w = W()
    w.block(_record(W.put, d, fields))
    w_vp(w, d['vp'])
    return bytes(w.b)


def r_fields_vp(fields, r):
    d = r.block().fill(_zero(fields), fields)
    d['vp'] = r_vp(r)
    return d


def w_dron(d):
    return w_fields_vp(DRON_FIELDS, d)


def r_dron(r):
    return r_fields_vp(DRON_FIELDS, r)


def w_dstd(d):
    return w_fields_vp(DSTD_FIELDS, d)


def r_dstd(r):
    return r_fields_vp(DSTD_FIELDS, r)


def w_prog(p):
    """Records: enabled and the entry count, then one per (root, chord type, bars) entry."""
    w = W()
    w.block(bytes((1 if p['enabled'] else 0, len(p['entries']))))
    for e in p['entries']:
        w.block(bytes(e))
    return bytes(w.b)


def r_prog(r):
    head = r.block().fill(dict(enabled=False, count=0), [('enabled', 'b1'), ('count', 'u8')])
    entries = [tuple(r.block().fill_list([0, 0, 0], 'u8')) for _ in range(head['count'])]
    return dict(enabled=head['enabled'], entries=entries)


def w_chrd(slots):
    """Records: the slot count, then per slot its tone count and SEQ_CHORD_MAX_NOTES notes."""
    w = W()
    w.block(bytes((SEQ_CHORD_SLOTS,)))
    for s in range(SEQ_CHORD_SLOTS):
        notes = list(slots[s]) + [0] * (SEQ_CHORD_MAX_NOTES - len(slots[s]))
        w.block(bytes([sum(1 for n in notes if n)] + notes))
    return bytes(w.b)


def r_chrd(r):
    n = r.block().u8() or 0
    out = [r.block().fill_list([0] * (1 + SEQ_CHORD_MAX_NOTES), 'u8')[1:] for _ in range(n)]
    return [[x for x in s if x] for s in out]


CLIP_HEAD = [('bars', 'u8'), ('stereo', 'b1'), ('tail', 'u8')]


def w_clip(c):
    """One record: the shape, one record per clip slot nested, then after."""
    def body(b):
        b.put(c, CLIP_HEAD)
        b.u8(CLIP_SLOT_COUNT)
        for s in range(CLIP_SLOT_COUNT):
            b.block(bytes((c['level'][s], c['mode'][s])))
        b.u8(c['after'])
    w = W()
    w.block(_record(body))
    return bytes(w.b)


def r_clip(r):
    b = r.block()
    c = b.fill(_zero(CLIP_HEAD), CLIP_HEAD)
    n = b.u8() or 0
    slots = [b.block().fill_list([0, 0], 'u8') for _ in range(n)]
    c['level'] = [s[0] for s in slots]
    c['mode'] = [s[1] for s in slots]
    c['after'] = b.u8() or 0
    return c


def w_mixr(m):
    """Mixer positions (0..100), soft-mute bit masks (layer arrays MAX_LAYERS wide), FADE
    length index. One record, each layer's track_pos row a record nested in it."""
    def body(b):
        for L in range(MAX_LAYERS):
            b.u8(m['layer_pos'][L])
        b.u8(m['layer_mute'])
        for L in range(MAX_LAYERS):
            b.block(bytes(m['track_pos'][L]))
        for L in range(MAX_LAYERS):
            b.u8(m['track_mute'][L])
        for k in range(MIX_MISC_COUNT):
            b.u8(m['misc_pos'][k])
        b.u8(m['misc_mute'])
        b.u8(m['fade_bars_idx'])
    w = W()
    w.block(_record(body))
    return bytes(w.b)


def r_mixr(r):
    b = r.block()
    m = dict(layer_pos=b.fill_list([0] * MAX_LAYERS, 'u8'))
    m['layer_mute'] = b.u8() or 0
    m['track_pos'] = [b.block().fill_list([0] * SEQ_TRACKS, 'u8') for _ in range(MAX_LAYERS)]
    m['track_mute'] = b.fill_list([0] * MAX_LAYERS, 'u8')
    m['misc_pos'] = b.fill_list([0] * MIX_MISC_COUNT, 'u8')
    m['misc_mute'] = b.u8() or 0
    m['fade_bars_idx'] = b.u8() or 0
    return m


PGEN_FIELDS = [(k, 'u8') for k in ('style', 'len', 'bars', 'ext', 'var')] + [('seed', 'u16')]


def w_pgen(g):
    w = W()
    w.block(_record(W.put, g, PGEN_FIELDS))
    return bytes(w.b)


def r_pgen(r):
    return r.block().fill(_zero(PGEN_FIELDS), PGEN_FIELDS)


WTCU_KEYS = ('shape', 'width', 'bright', 'sync', 'peak')


def w_wtcu(c):
    """Custom wavetable parameters: per key a three-item list [A, M, B], then range. One record."""
    def body(b):
        for k in WTCU_KEYS:
            b.u8(c[k][0]); b.u8(c[k][1]); b.u8(c[k][2])
        b.u8(c['range'])
    w = W()
    w.block(_record(body))
    return bytes(w.b)


def r_wtcu(r):
    b = r.block()
    c = {k: b.fill_list([0, 0, 0], 'u8') for k in WTCU_KEYS}
    c['range'] = b.u8() or 0
    return c


FM_NUM_OPS = 6
FM_OP_FIELDS = [('targets', 'u8'), ('ratio', 'f32'), ('fixed_hz', 'f32'), ('level', 'f32')]


def w_fmcu(v):
    """Custom FM voice (fm_voice_t): algorithm, fb_op, then per operator a record
    {targets, ratio, fixed_hz, level, envelope record {rate[4], env_level[4],
    eg_type}}, then
    feedback and the op_fixed mask, all in one record. op_mute is audition
    state and is not stored."""
    assert len(v['ops']) == FM_NUM_OPS

    def op_body(b, op):
        b.put(op, FM_OP_FIELDS)
        b.block(bytes(list(op['rate']) + list(op['env_level']) + [op['eg_type']]))

    def body(b):
        b.u8(v['algorithm']); b.u8(v['fb_op'])
        for op in v['ops']:
            b.block(_record(op_body, op))
        b.f32(v['feedback']); b.u8(v['op_fixed'])
    w = W()
    w.block(_record(body))
    return bytes(w.b)


def r_fmcu(r):
    b = r.block()
    v = b.fill({'algorithm': 0, 'fb_op': 0}, [('algorithm', 'u8'), ('fb_op', 'u8')])
    v['ops'] = []
    for _ in range(FM_NUM_OPS):
        ob = b.block()
        op = ob.fill(_zero(FM_OP_FIELDS), FM_OP_FIELDS)
        eb = ob.block()
        op['rate'] = eb.fill_list([0] * 4, 'u8')
        op['env_level'] = eb.fill_list([0] * 4, 'u8')
        op['eg_type'] = eb.u8() or 0
        v['ops'].append(op)
    v['feedback'] = 0.0
    v['op_fixed'] = 0
    return b.fill(v, [('feedback', 'f32'), ('op_fixed', 'u8')])


# ── whole project ───────────────────────────────────────────────────────────

def encode_payload(p):
    w = W()
    w.section('GLOB', w_glob(p['glob']))
    for L in p['layers']:
        w.section('LAYR', w_layr(L))
    w.section('ARP ', w_arp(p['arp']))
    if 'drop' in p:   # optional: a file without it loads the ball drop disabled
        w.section('DROP', w_drop(p['drop']))
    w.section('DRON', w_dron(p['drone']))
    w.section('DSTD', w_dstd(p['drone_std']))
    w.section('PROG', w_prog(p['prog']))
    w.section('CHRD', w_chrd(p['chords']))
    w.section('CLIP', w_clip(p['clip']))
    if 'mixr' in p:   # optional: a file without it loads every fader at unity
        w.section('MIXR', w_mixr(p['mixr']))
    w.section('PGEN', w_pgen(p['pgen']))
    if 'wtcu' in p:   # optional: a file without it loads the builder defaults
        w.section('WTCU', w_wtcu(p['wtcu']))
    if 'fmcu' in p:   # optional: a file without it loads the default FM voice
        w.section('FMCU', w_fmcu(p['fmcu']))
    return bytes(w.b)


def encode_file(p, name):
    payload = encode_payload(p)
    nm = name.encode()[:NAME_LEN - 1].ljust(NAME_LEN, b'\0')
    hdr = struct.pack('<IHH', MAGIC, FMT_VERSION, 0) + nm + struct.pack('<II', len(payload),
                                                                         zlib.crc32(payload))
    assert len(hdr) == 32
    return hdr + payload


def decode_file(data):
    magic, ver, _res = struct.unpack_from('<IHH', data, 0)
    name = data[8:24].split(b'\0')[0].decode(errors='replace')
    plen, crc = struct.unpack_from('<II', data, 24)
    payload = data[32:32 + plen]
    if magic != MAGIC or ver > FMT_VERSION or zlib.crc32(payload) != crc:
        raise ValueError('bad header/CRC')
    p = dict(name=name, layers=[], skipped=[])
    readers = {'GLOB': ('glob', r_glob), 'ARP ': ('arp', r_arp), 'DROP': ('drop', r_drop),
               'DRON': ('drone', r_dron),
               'DSTD': ('drone_std', r_dstd),
               'PROG': ('prog', r_prog), 'CHRD': ('chords', r_chrd), 'CLIP': ('clip', r_clip),
               'MIXR': ('mixr', r_mixr), 'PGEN': ('pgen', r_pgen), 'WTCU': ('wtcu', r_wtcu), 'FMCU': ('fmcu', r_fmcu)}
    pos = 0
    while pos < len(payload):
        if len(payload) - pos < 9:
            raise ValueError('truncated section header at payload offset %d' % pos)
        tag, ver, blen = struct.unpack_from('<IBI', payload, pos)
        pos += 9
        if blen > len(payload) - pos:
            raise ValueError('section at payload offset %d overruns the payload' % (pos - 9))
        body = R(payload[pos:pos + blen])
        pos += blen
        name4 = struct.pack('<I', tag).decode(errors='replace')
        if name4 not in VER:
            continue                    # unknown section: ignored, as the firmware does
        if ver != VER[name4]:
            if name4 in REQUIRED:
                raise ValueError('%s version %d, codec knows %d' % (name4, ver, VER[name4]))
            p['skipped'].append('%s version %d, codec knows %d: skipped' % (name4, ver, VER[name4]))
            continue
        if name4 == 'LAYR':
            p['layers'].append(r_layr(body))
        else:
            key, fn = readers[name4]
            p[key] = fn(body)
    return p
