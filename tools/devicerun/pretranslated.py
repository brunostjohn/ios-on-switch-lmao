"""iPadOS 27's deploy updates to the pretranslated tree: the kernel's one line per update at capture.

kernel/switch/vfs/pretranslated_updates.c gives each file the boot package names either its update ("<path> from the
deploy's update") or no redirect at all ("<path> not updated (<why>); it runs translated at load time"). The second
keeps the boot alive but runs that file without its edit and translated at every load, so any such line fails, as does
a /chosen list the kernel cannot read. "none applied" names why the update folder could not be used; a missing folder
alone is a fresh card's normal state, and the kernel prints it only when an update was needed, which the per-file
lines then fail. The update lines come at capture, so only a boot whose capture line is in the log is judged, and a log
with none is NO_DATA (24 Nov: every one of 22 iPadOS 27 boots had it).
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'perf1-daemons'
SOURCE = 'kernel/switch/vfs/pretranslated_updates.c'
CAPTURE = 'pretranslated.capture'

PRETRANSLATED = [
    Check(
        CAPTURE,
        OWNER,
        (' files from /private/var/Switch/pretranslated (', 'SwitchPretranslated: off ('),
        REPORT,
        process='kernel',
        source='kernel/switch/vfs/pretranslated.c',
        note='the tree was captured, or turned off, at this boot',
    ),
    Check(
        'pretranslated.update',
        OWNER,
        'SwitchPretranslated: /',
        process='kernel',
        needs=CAPTURE,
        boots=CAPTURE,
        regex=r"^SwitchPretranslated: (?!.* from the deploy's update$)(?P<path>/\S+) ?(?P<result>.*)$",
        show=('path', 'result'),
        source=SOURCE,
        note='a file the boot package updates that did not get its update: it runs translated at load time',
    ),
    Check(
        'pretranslated.update-folder',
        OWNER,
        'updates needed, none applied (',
        process='kernel',
        needs=CAPTURE,
        boots=CAPTURE,
        regex=r'none applied \((?P<folder>[^:]+): (?!no such file\)$)(?P<reason>[^)]*)\)$',
        show=('folder', 'reason'),
        source=SOURCE,
        note="Data's update folder or its Switch/ parent could not be used (Switch/ must be root:wheel without group "
        'or other write; //deploy:sd sets it)',
    ),
    Check(
        'pretranslated.update-list',
        OWNER,
        'SwitchPretranslated: no updates (',
        process='kernel',
        needs=CAPTURE,
        boots=CAPTURE,
        source=SOURCE,
        note="the boot package's /chosen update list did not parse (tools/package/adt_edit.py)",
    ),
]
