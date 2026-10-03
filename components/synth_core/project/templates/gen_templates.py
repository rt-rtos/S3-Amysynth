#!/usr/bin/env python3
"""Build the genre templates into the firmware's template table.

Usage: gen_templates.py --check-src <components dir> --out <project_templates_data.c>

Drift check first, before anything is written: the section versions
project_snapshot.c writes, the store header's magic, format version and name
length (project_store.h), and the firmware constants amp_codec.py and
spec_lib.py copy, each read from the header that defines it. Any mismatch
prints one line per constant and exits 1; a constant that cannot be found
in its source also exits 1.

Then each spec in GENRES order is built through amp_codec and the C file is
written: one array per template holding the full .amp image (32-byte header
+ payload) and the project_templates[] table (project_templates.h), named by
the project name in the image header. Stdlib only; the synth_core build runs
it with the IDF Python.
"""
import argparse
import importlib
import os
import re
import struct
import sys

sys.dont_write_bytecode = True      # runs in the source tree: leave no __pycache__
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import amp_codec as C  # noqa: E402
import spec_lib as S  # noqa: E402

GENRES = ('house', 'techno', 'dub_techno', 'electro', 'boom_bap', 'trap', 'drum_n_bass',
          'synthwave', 'ambient', 'crystal', 'witch_house', 'dreamwave')

# Firmware sources, relative to the components directory.
SNAPSHOT_C = 'synth_core/project/project_snapshot.c'
STORE_H = 'project_store/include/project_store.h'
SEQ_MODEL_H = 'synth_core/sequencer_core/seq_model.h'
SEQ_CONFIG_H = 'synth_core/sequencer_core/seq_core_config.h'
SEQUENCER_H = 'synth_core/include/sequencer_core.h'
FX_BUS_H = 'synth_core/include/fx_bus.h'
AMY_FX_H = 'synth_core/include/amy_fx.h'
ARP_H = 'synth_core/include/arp_core.h'
CHORDS_H = 'synth_core/include/seq_chords.h'
SLOTS_H = 'synth_core/include/synth_slots.h'
DRONE_H = 'synth_core/include/custompatches/drone_core.h'
CHORD_TYPES_H = 'display/chord_types.h'
AMY_H = 'amy/src/amy.h'


class ParseError(Exception):
    pass


class Firmware:
    """Reads #define values and enumerators out of the C sources by regex."""

    _COMMENT = re.compile(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*', re.S)

    def __init__(self, root):
        self.root = root
        self._text = {}

    def text(self, rel):
        if rel not in self._text:
            path = os.path.join(self.root, rel)
            try:
                with open(path, encoding='utf-8', errors='replace') as f:
                    src = f.read()
            except OSError as e:
                raise ParseError('%s: %s' % (rel, e.strerror))
            self._text[rel] = self._COMMENT.sub(
                lambda m: m.group(0) if m.group(0).startswith('"') else ' ', src)
        return self._text[rel]

    @staticmethod
    def c_int(expr, rel, what):
        """Integer value of a constant C expression: literals with suffixes,
        integer casts, INT16_MIN, + - * << |."""
        e = re.sub(r'\(\s*u?int(8|16|32)_t\s*\)', '', expr)
        e = re.sub(r'\bINT16_MIN\b', '(-32768)', e)
        e = re.sub(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\b', r'\1', e)
        if not re.fullmatch(r'[0-9a-fA-FxX()\s+\-*<|]+', e):
            raise ParseError('%s: %s = "%s" is not a plain integer' % (rel, what, expr.strip()))
        try:
            return int(eval(e, {'__builtins__': {}}))
        except Exception:
            raise ParseError('%s: %s = "%s" is not a plain integer' % (rel, what, expr.strip()))

    def define(self, rel, name):
        vals = {self.c_int(m.group(1), rel, name) for m in re.finditer(
            r'^[ \t]*#[ \t]*define[ \t]+%s[ \t]+(.+?)[ \t]*$' % re.escape(name),
            self.text(rel), re.M)}
        if not vals:
            raise ParseError('%s: #define %s not found' % (rel, name))
        if len(vals) > 1:
            raise ParseError('%s: #define %s has several values %s' % (rel, name, sorted(vals)))
        return vals.pop()

    def _enum_with(self, rel, name):
        """(names, values) of the enum that declares `name`."""
        for m in re.finditer(r'\benum\b[^{;]*\{(.*?)\}', self.text(rel), re.S):
            names, vals, nxt = [], [], 0
            for entry in m.group(1).split(','):
                entry = entry.strip()
                if not entry:
                    continue
                em = re.fullmatch(r'([A-Za-z_]\w*)\s*(?:=\s*(.+))?', entry, re.S)
                if not em:
                    break
                if em.group(2) is not None:
                    expr = em.group(2)
                    for n, v in zip(names, vals):
                        expr = re.sub(r'\b%s\b' % n, '(%d)' % v, expr)
                    nxt = self.c_int(expr, rel, em.group(1))
                names.append(em.group(1)); vals.append(nxt)
                nxt += 1
            if name in names:
                return names, vals
        raise ParseError('%s: enumerator %s not found' % (rel, name))

    def enum(self, rel, name):
        names, vals = self._enum_with(rel, name)
        return vals[names.index(name)]

    def enum_count(self, rel, name):
        """Entries declared before `name` (the trailing _COUNT idiom)."""
        names, _ = self._enum_with(rel, name)
        return names.index(name)

    def enum_bit(self, rel, name):
        return 1 << self.enum(rel, name)

    def sections(self):
        """{codec section name: version} as project_snapshot.c writes them."""
        src = self.text(SNAPSHOT_C)
        tags = {}
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(TAG_\w+)[ \t]+(0[xX][0-9a-fA-F]+)[uU]?',
                             src, re.M):
            tags[m.group(1)] = struct.pack('<I', int(m.group(2), 16)).decode('ascii', 'replace')
        out = {}
        for m in re.finditer(r'\btlv_begin_section\s*\(\s*\w+\s*,\s*(TAG_\w+)\s*,\s*(\w+)\s*\)', src):
            tag, ver = m.group(1), m.group(2)
            if tag not in tags:
                raise ParseError('%s: %s used but not defined' % (SNAPSHOT_C, tag))
            out[tags[tag]] = (int(ver, 0) if ver[0].isdigit()
                              else self.define(SNAPSHOT_C, ver))
        if not out:
            raise ParseError('%s: no tlv_begin_section() calls found' % SNAPSHOT_C)
        return out


def constant_checks(fw):
    """(name in the codec or spec_lib, its value, firmware file, resolver, firmware symbol)."""
    d, e, n, b = fw.define, fw.enum, fw.enum_count, fw.enum_bit
    checks = [
        ('MAGIC', C.MAGIC, STORE_H, d, 'PROJECT_MAGIC'),
        ('FMT_VERSION', C.FMT_VERSION, STORE_H, d, 'PROJECT_FMT_VERSION'),
        ('NAME_LEN', C.NAME_LEN, STORE_H, d, 'PROJECT_NAME_LEN'),
        ('SEQ_EGT_COUNT', C.SEQ_EGT_COUNT, SEQ_MODEL_H, n, 'SEQ_EGT_COUNT'),
        ('SEQ_TRACKS', C.SEQ_TRACKS, SEQ_MODEL_H, d, 'SEQ_TRACKS'),
        ('SEQ_MAX_STEPS', C.SEQ_MAX_STEPS, SEQ_MODEL_H, d, 'SEQ_MAX_STEPS'),
        ('FX_BUS_COUNT', C.FX_BUS_COUNT, FX_BUS_H, d, 'FX_BUS_COUNT'),
        ('ARP_MAX_SLOTS', C.ARP_MAX_SLOTS, ARP_H, d, 'ARP_MAX_SLOTS'),
        ('SEQ_CHORD_SLOTS', C.SEQ_CHORD_SLOTS, CHORDS_H, d, 'SEQ_CHORD_SLOTS'),
        ('SEQ_CHORD_MAX_NOTES', C.SEQ_CHORD_MAX_NOTES, CHORDS_H, d, 'SEQ_CHORD_MAX_NOTES'),
        ('CLIP_SLOT_COUNT', C.CLIP_SLOT_COUNT, SLOTS_H, d, 'CLIP_SLOT_COUNT'),
        ('FX_PARAM_UNSET', C.FX_PARAM_UNSET, AMY_FX_H, d, 'FX_PARAM_UNSET'),
        ('SEQ_CHORD_BASE', S.SEQ_CHORD_BASE, CHORDS_H, d, 'SEQ_CHORD_BASE'),
        ('GATE_HOLD', S.GATE_HOLD, SEQ_CONFIG_H, d, 'SEQ_GATE_HOLD'),
        ('ARP_REST', S.ARP_REST, ARP_H, d, 'ARP_REST'),
    ]
    for k, v in C.ECHO_DIV.items():                 # '1/8D' -> FX_ECHO_DIV_8D
        checks.append(("ECHO_DIV['%s']" % k, v, AMY_FX_H, e, 'FX_ECHO_DIV_' + k[2:]))
    for sym in ('MAJ', 'MIN', 'MAJ7', 'MIN7', 'DOM7', 'SUS2', 'SUS4', 'DIM', 'AUG', 'MIN9',
                'MAJ9', 'MAJ6', 'MIN6', 'DOM9'):
        checks.append(('CHORD_' + sym, getattr(S, 'CHORD_' + sym), CHORD_TYPES_H, e, 'CHORD_' + sym))
    for sym in ('LPF', 'BPF', 'HPF', 'LPF24'):
        checks.append(('FILTER_' + sym, getattr(S, 'FILTER_' + sym), SEQ_MODEL_H, d,
                       'SEQ_FILTER_' + sym))
    for sym, fsym in (('NORMAL', 'NORMAL'), ('LINEAR', 'LINEAR'), ('DX7', 'DX7'),
                      ('TRUE_EXP', 'TRUE_EXPONENTIAL')):
        checks.append(('ENV_' + sym, getattr(S, 'ENV_' + sym), AMY_H, d, 'ENVELOPE_' + fsym))
    for sym in ('PITCH', 'CUTOFF', 'DRIVE', 'MIX'):
        checks.append(('EGT_' + sym, getattr(S, 'EGT_' + sym), SEQ_MODEL_H, e, 'SEQ_EGT_' + sym))
    for sym, fsym in (('SINE', 'SINE'), ('TRI', 'TRIANGLE'), ('SAW_UP', 'SAW_UP'),
                      ('SAW_DOWN', 'SAW_DOWN'), ('SQUARE', 'SQUARE'), ('RANDOM', 'RANDOM')):
        checks.append(('LFO_' + sym, getattr(S, 'LFO_' + sym), SEQ_MODEL_H, e, 'LFO_WAVE_' + fsym))
    for sym in ('FILTER', 'AMP', 'PITCH', 'PAN', 'SCAN'):
        checks.append(('LFO_TGT_' + sym, getattr(S, 'LFO_TGT_' + sym), SEQ_MODEL_H, b,
                       'LFO_TARGET_' + sym))
    for sym in ('1_8', '1_4', '1_2', '1BAR', '2BAR', '4BAR', '1_16'):
        checks.append(('LFO_' + sym, getattr(S, 'LFO_' + sym), SEQ_MODEL_H, e, 'LFO_RATE_' + sym))
    for sym in ('CLIP', 'FOLD', 'CRUSH'):
        checks.append(('DIST_' + sym, getattr(S, 'DIST_' + sym), AMY_H, d, 'DIST_' + sym))
    for sym in ('DRUM', 'MELODIC'):
        checks.append(('SEQ_LAYER_' + sym, getattr(S, 'SEQ_LAYER_' + sym), SEQ_MODEL_H, e,
                       'SEQ_LAYER_' + sym))
    for sym in ('LAYER', 'TRACK'):
        checks.append(('SCOPE_' + sym, getattr(S, 'SCOPE_' + sym), SEQ_MODEL_H, e,
                       'SEQ_PATCH_SCOPE_' + sym))
    for sym in ('CHORD', 'ROOT', 'OFF'):
        checks.append(('FOLLOW_' + sym, getattr(S, 'FOLLOW_' + sym), SEQ_MODEL_H, e,
                       'SEQ_FOLLOW_' + sym))
    for sym in ('1_1', '1_4', '1_8', '1_16', '1_32'):
        checks.append(('ARP_RATE_' + sym, getattr(S, 'ARP_RATE_' + sym), ARP_H, e, 'ARP_RATE_' + sym))
    for sym in ('UP', 'DOWN', 'SLOT'):
        checks.append(('ARP_' + sym, getattr(S, 'ARP_' + sym), ARP_H, e, 'ARP_' + sym))
    for sym in ('OWN', 'GLOBAL', 'CHORD'):
        checks.append(('ARP_QUANT_' + sym, getattr(S, 'ARP_QUANT_' + sym), ARP_H, e,
                       'ARP_QUANT_' + sym))
    for sym in ('WAVE', 'PATCH'):
        checks.append(('DRONE_SRC_' + sym, getattr(S, 'DRONE_SRC_' + sym), DRONE_H, e,
                       'DRONE_SRC_' + sym))
    for sym in ('1_4', '1_8', '1_16', '1_32', '1_1'):
        checks.append(('DRONE_RATE_' + sym, getattr(S, 'DRONE_RATE_' + sym), DRONE_H, e,
                       'DRONE_RATE_' + sym))
    for sym in ('FULL', 'FOUR', 'OFFBEAT', 'GALLOP', 'DUB'):
        checks.append(('DRONE_PAT_' + sym, getattr(S, 'DRONE_PAT_' + sym), DRONE_H, e,
                       'DRONE_PAT_' + sym))
    for sym in ('OFF', 'ROOT', 'CHORD'):
        checks.append(('DRONE_FOLLOW_' + sym, getattr(S, 'DRONE_FOLLOW_' + sym), DRONE_H, e,
                       'DRONE_FOLLOW_' + sym))
    for sym in ('SINE', 'PULSE', 'SAW_DOWN', 'SAW_UP', 'TRIANGLE'):
        checks.append(('WAVE_' + sym, getattr(S, 'WAVE_' + sym), AMY_H, d, sym))
    for sym in ('DRUMS', 'DRONES', 'CLIPS'):
        checks.append(('SPLIT_' + sym, getattr(S, 'SPLIT_' + sym), FX_BUS_H, b, 'FX_GROUP_' + sym))
    for sym, fsym in (('SINE', 'SINE'), ('SAW_DOWN', 'SAW_DOWN'), ('SAW_UP', 'SAW_UP'),
                      ('PULSE', 'PULSE'), ('TRIANGLE', 'TRIANGLE'), ('NOISE', 'NOISE'),
                      ('KS', 'KS'), ('BASS_SUB_DETUNE', 'BASS_1'), ('BASS_ACID', 'BASS_2'),
                      ('BASS_DX7', 'BASS_3'), ('WT_PPG', 'WAVETABLE_2')):
        checks.append(('P_' + sym, getattr(S, 'P_' + sym), SEQUENCER_H, d, 'SEQ_PATCH_' + fsym))
    return checks


def drift(fw):
    """One line per codec/firmware mismatch; empty when they agree."""
    out = []
    secs = fw.sections()
    for name in sorted(set(secs) | set(C.VER)):
        cv, fv = C.VER.get(name), secs.get(name)
        if cv != fv:
            out.append("VER['%s']: codec %s, firmware %s (%s)"
                       % (name, 'missing' if cv is None else cv,
                          'missing' if fv is None else fv, SNAPSHOT_C))
    for name, cv, rel, resolve, sym in constant_checks(fw):
        fv = resolve(rel, sym)
        if cv != fv:
            out.append('%s: codec %s, firmware %s (%s)' % (name, cv, fv, rel))
    return out


def c_source(images):
    lines = ['/* Generated by gen_templates.py from the template specs beside it - do not edit. */',
             '#include <stddef.h>', '#include <stdint.h>', '#include "project_templates.h"', '']
    for g, name, data in images:
        lines.append('static const uint8_t tpl_%s[] = {' % g)
        for i in range(0, len(data), 16):
            lines.append('    ' + ' '.join('0x%02x,' % x for x in data[i:i + 16]))
        lines += ['};', '']
    lines.append('const project_template_t project_templates[] = {')
    for g, name, data in images:
        lines.append('    {"%s", tpl_%s, sizeof tpl_%s},'
                     % (name.replace('\\', '\\\\').replace('"', '\\"'), g, g))
    lines += ['};', '',
              'const size_t project_templates_n = sizeof project_templates / sizeof project_templates[0];',
              '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check-src', required=True, help='the repository components directory')
    ap.add_argument('--out', required=True, help='C file to write')
    a = ap.parse_args()

    try:
        bad = drift(Firmware(a.check_src))
    except ParseError as e:
        print('gen_templates: drift check cannot read the firmware: %s' % e, file=sys.stderr)
        return 1
    if bad:
        print('gen_templates: template codec does not match the firmware:', file=sys.stderr)
        for line in bad:
            print('  ' + line, file=sys.stderr)
        return 1

    images = []
    for g in GENRES:
        spec = importlib.import_module(g)
        data = C.encode_file(spec.build(), spec.NAME)
        images.append((g, data[8:8 + C.NAME_LEN].split(b'\0')[0].decode('ascii'), data))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + '.tmp'
    with open(tmp, 'w') as f:
        f.write(c_source(images))
    os.replace(tmp, a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
