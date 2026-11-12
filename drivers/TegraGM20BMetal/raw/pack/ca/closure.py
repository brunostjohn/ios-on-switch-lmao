"""The closure tier: Core Animation names one field away from the corpus's, manifest's and variants' names.

QuartzCore names a state by its segments (caspec.py), and the names its devices drew show which fields it varies on its
own between states that are otherwise one: the texture function (img beside cim, a filter's blur passes), an image's
function (src, alp, sar, ues ...), an image's coordinate, filter, memoryless and colour-opcode fields, and the fragment
flags (n, s, u). Each such variation the journals hold between two names (an axis) is applied to every seed name the
device asks for (deepcolour.asked), but a generic one (g: CA makes those out of its archive alone): a state of the
seeds' with another value an axis pairs with its own. Not axes: the
pipeline segment (the blend tier gives each fragment every blend CA lifts into the pipeline), and the flags QuartzCore
sets from the device or the target, not the state (c: CADeviceUseCIF10 or an image's format, deepcolour.py; g: the
generic state; x: an extended-range target). Only names this QuartzCore makes a state of are kept.
"""

import collections
from collections.abc import Callable

import caspec
import deepcolour

Fields = list[tuple[str, str]]
NOT_VARIED = set('cgx')  # flags the device or the target sets
# The fields the tier varies. Each adds its own share of states (iOS 27's split: image 20.4k, texture 15.7k, image
# fields 11.5k, flags 5.7k, beside 41.6k in the other tiers); leaving one out leaves out its share.
AXES = ('texture', 'flags', 'image', 'image fields')


def _segment(tables: caspec.Tables, kind: str, text: str) -> tuple[list[tuple[str, str]], str]:
    """Give a segment's lettered fields in order (letter, value) and its flags."""
    rows = {row['letter']: row for row in tables.t['name_tables'][kind]}
    out, at = [], 0
    while at < len(text):
        row = rows.get(text[at])
        at += 1
        if row is None:
            continue
        if row['flag']:
            return out, text[at:]
        width = row['width'] * row['count']
        out.append((row['letter'], text[at : at + width]))
        at += width
    return out, ''


def fields(tables: caspec.Tables, name: str) -> Fields:
    """Give a name as (field, value) pairs.

    The pipeline segment, the fragment's texture function, its other fields and its flags, then each image's function
    and its other fields with its flags.
    """
    pipeline, fragment, *images = name.split('_')
    out = [('pipeline', pipeline)]
    lettered, flags = _segment(tables, 'fragment', fragment)
    out.append(('texture', dict(lettered).get('T', '')))
    out.append(('fragment', ''.join(letter + value for letter, value in lettered if letter != 'T')))
    out.append(('flags', flags))
    for k, image in enumerate(images):
        lettered, flags = _segment(tables, 'image', image)
        out.append((f'image{k}', dict(lettered).get('I', '')))
        rest = ''.join(letter + value for letter, value in lettered if letter != 'I')
        out.append((f'image{k} fields', rest + ('X' + flags if flags else '')))
    return out


def name_of(f: Fields) -> str:
    d = dict(f)
    texture = 'T' + d['texture'] if d['texture'] else ''
    images = []
    while f'image{len(images)}' in d:
        k = len(images)
        images.append('I' + d[f'image{k}'] + d[f'image{k} fields'])
    return '_'.join([d['pipeline'], texture + d['fragment'] + 'X' + d['flags'], *images])


def _axis(field: str) -> str | None:
    axis = field if field in ('texture', 'flags') else None
    if field.startswith('image'):
        axis = 'image fields' if field.endswith(' fields') else 'image'
    return axis if axis in AXES else None


def axes(tables: caspec.Tables, names: list[str]) -> dict[str, dict[str, set[str]]]:
    """Give each axis's values and, for each, the values the names show beside it in an otherwise equal name."""
    alike = collections.defaultdict(set)
    for name in names:
        f = fields(tables, name)
        for k, (field, value) in enumerate(f):
            if _axis(field):
                alike[(field, tuple(f[:k] + f[k + 1 :]))].add(value)
    out: dict[str, dict[str, set[str]]] = collections.defaultdict(lambda: collections.defaultdict(set))
    for (field, _rest), values in alike.items():
        axis = _axis(field)
        for value in values:
            out[axis][value] |= {
                v for v in values - {value} if axis != 'flags' or not (set(v) ^ set(value)) & NOT_VARIED
            }
    return out


def tier(
    tables: caspec.Tables, seeds: list[str], drawn: list[str], cif10: bool | None, makes: Callable[[str], bool]
) -> list[str]:
    """Give the names one axis away from a seed the device asks for that this QuartzCore makes (`makes`).

    The axes are those `drawn`'s names show; `cif10` as caset.labels takes it.
    """
    learned = axes(tables, drawn)
    seen = set(seeds)
    out = []
    for seed in seeds:
        if caspec.decode(tables, seed).flag('g') or seed not in deepcolour.asked(tables, seed, cif10):
            continue
        f = fields(tables, seed)
        for k, (field, value) in enumerate(f):
            axis = _axis(field)
            for other in sorted(learned[axis].get(value, ())) if axis else ():
                name = name_of([*f[:k], (field, other), *f[k + 1 :]])
                if name not in seen and makes(name):
                    seen.add(name)
                    out.append(name)
    return out
