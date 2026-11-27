"""The IPSW members //install/ipsw's tests read, chosen from ipsw/pins.bzl, and the files each one brings.

Each set is a struct: `data`, the labels, and `env`, its members as `kind role $(rlocationpath file)...` joined by `;`.
"""

load("//ipsw:ipsw.bzl", "ipsw_member_label")
load("//ipsw:pins.bzl", "IPSWS")

# ipsw/ipsw.bzl's _LARGE: //ipsw inflates members above it only on demand.
LARGE = 256 << 20

def _members():
    return [
        (kind, role, pin)
        for kind, ipsw in sorted(IPSWS.items())
        for role, pin in sorted(ipsw.get("members", {}).items())
        if not pin.get("aea_key")
    ]

def _cases(members):
    data = []
    listed = []
    for kind, role, files in members:
        files = [str(f) for f in files]
        data.extend(files)
        listed.append(" ".join([kind, role] + ["$(rlocationpath {})".format(f) for f in files]))
    return struct(data = data, env = ";".join(listed))

def _stored(kind, role, pin):
    if pin["deflated"]:
        return "@ipsw//:stored_{}_{}".format(kind, role)
    return ipsw_member_label(kind, role)

def small_members():
    """The deflated members //ipsw inflates in every build: their stored bytes, then //ipsw's inflated output.

    Returns:
        struct(data, env).
    """
    return _cases([
        (kind, role, [_stored(kind, role, pin), "//ipsw:{}_{}".format(kind, role)])
        for kind, role, pin in _members()
        if pin["deflated"] and pin["stored_size"] <= LARGE
    ])

def large_members():
    """The other members that are not encrypted: their stored bytes, checked against the content pins alone.

    Returns:
        struct(data, env).
    """
    return _cases([
        (kind, role, [_stored(kind, role, pin)])
        for kind, role, pin in _members()
        if not pin["deflated"] or pin["stored_size"] > LARGE
    ])

def stored_images():
    """The members stored without compression or encryption, which is how a UDIF OS image comes.

    Returns:
        struct(data, env).
    """
    return _cases([
        (kind, role, [_stored(kind, role, pin)])
        for kind, role, pin in _members()
        if not pin["deflated"]
    ])
