"""Compares tree_list.py's listing of a source subtree with its copy's, both read through macOS's kernel.

    python3 tree_compare.py <source listing> <copy listing>

Every column must be equal, except where a copy is meant to differ, and each of those is checked by its own rule:
- inode numbers are new, so hard links are compared as groups of paths that share an inode;
- a file's link count is its names inside the subtree, since names outside it aren't copied;
- a directory's link count comes from the volume's own counting, which follows its features: the copy (Data, with
  expanded records) counts subdirectories, the source (System) counts every child;
- the subtree's own root isn't compared: the copy's root is the volume root, with its own owner and mode.

Exits 1 on any other difference.
"""

import collections
import sys

COLUMNS = [
    'path',
    'kind',
    'ino',
    'owner',
    'mode',
    'size',
    'flags',
    'nlink',
    'birth',
    'mtime',
    'ctime',
    'atime',
    'xattrs',
    'content',
]
EQUAL = ['kind', 'owner', 'mode', 'size', 'flags', 'birth', 'mtime', 'ctime', 'atime', 'xattrs', 'content']


Row = dict[str, str]
Rows = dict[str, Row]
Problems = dict[str, list[str]]


def load(path: str) -> Rows:
    rows = {}
    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        for line in f:
            fields = line.rstrip('\n').split('\t')
            if len(fields) != len(COLUMNS):
                raise SystemExit(f'{path}: a line of {len(fields)} columns: {line!r}')
            row = {c: fields[i] for i, c in enumerate(COLUMNS)}
            rows[row['path']] = row
    rows.pop('/', None)
    return rows


def parent(path: str) -> str:
    return path.rsplit('/', 1)[0] or '/'


def groups(rows: Rows) -> dict[str, tuple[str, ...]]:
    by_ino = collections.defaultdict(list)
    for p, r in rows.items():
        if r['kind'] != 'd':
            by_ino[r['ino']].append(p)
    return {p: tuple(sorted(ps)) for ps in by_ino.values() for p in ps}


def children(rows: Rows) -> tuple[collections.Counter, collections.Counter]:
    count = collections.Counter()
    dirs = collections.Counter()
    for p, r in rows.items():
        count[parent(p)] += 1
        if r['kind'] == 'd':
            dirs[parent(p)] += 1
    return count, dirs


def directory(p: str, s: Row, d: Row, counts: tuple[int, int], problems: Problems) -> None:
    src_children, dst_subdirs = counts
    if int(s['nlink']) != src_children + 2:
        problems['source directory nlink'].append(f'{p}: {s["nlink"]}, {src_children} children')
    if int(d['nlink']) != dst_subdirs + 2:
        problems['copy directory nlink'].append(f'{p}: {d["nlink"]}, {dst_subdirs} subdirectories')


def has_names_outside(
    p: str, s: Row, d: Row, group: tuple[tuple[str, ...], tuple[str, ...]], problems: Problems
) -> bool:
    src_group, dst_group = group
    if src_group != dst_group:
        problems['hard-link group'].append(f'{p}: {src_group} -> {dst_group}')
    names = len(src_group)
    if int(d['nlink']) != names:
        problems['copy nlink'].append(f'{p}: {d["nlink"]}, {names} names in the subtree')
    if int(s['nlink']) > names:
        return True
    if int(s['nlink']) != names:
        problems['source nlink'].append(f'{p}: {s["nlink"]}, {names} names')
    return False


def compare(src: Rows, dst: Rows, problems: Problems) -> int:
    for p in sorted(set(src) ^ set(dst)):
        problems['only in ' + ('source' if p in src else 'copy')].append(p)
    src_groups, dst_groups = groups(src), groups(dst)
    src_children, _ = children(src)
    _, dst_subdirs = children(dst)
    outside = 0
    for p in sorted(set(src) & set(dst)):
        s, d = src[p], dst[p]
        for c in (c for c in EQUAL if s[c] != d[c]):
            problems[c].append(f'{p}: {s[c]} -> {d[c]}')
        if s['kind'] == 'd':
            directory(p, s, d, (src_children[p], dst_subdirs[p]), problems)
        else:
            outside += has_names_outside(p, s, d, (src_groups[p], dst_groups[p]), problems)
    return outside


def main() -> None:
    src, dst = load(sys.argv[1]), load(sys.argv[2])
    problems = collections.defaultdict(list)
    outside = compare(src, dst, problems)
    kinds = collections.Counter(r['kind'] for r in dst.values())
    linked = sum(1 for g in set(groups(dst).values()) if len(g) > 1)
    print(
        f'{len(src)} source entries, {len(dst)} copied: {kinds["d"]} directories, {kinds["f"]} files, '
        f'{kinds["l"]} symlinks; {linked} hard-link groups; {outside} names whose inode has links outside the '
        f'subtree'
    )
    for what, items in sorted(problems.items()):
        print(f'DIFFERENT {what}: {len(items)}')
        for item in items[:10]:
            print(f'  {item}')
    if problems:
        sys.exit(1)
    print('identical, apart from the rules above')


if __name__ == '__main__':
    main()
