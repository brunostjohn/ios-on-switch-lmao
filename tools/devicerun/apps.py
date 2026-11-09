"""The overlay's apps in /Applications stay registered with LaunchServices on every boot.

lsd's first-boot work (_LSServerCleanEphemeralMobileContainersForFirstBoot, 21H461 and 24B119) removes, after each
reboot, every container that is not the root, system data, Preboot or user data volume, with the bundles filed under it.
Until the kernel named the root volume for the overlay's /Applications union (kernel/switch/vfs/identity.c), the
overlay's apps were filed under the union's own volume: SwitchHWTest disappeared on every boot after the one that
registered it (boot 16b: 267 apps against 269, SpringBoard's recents naming it 'not installed').
"""

from tools.devicerun.rules import Check

OWNER = 'r123-appreg'
LSD = 'lsd'
FIRST_BOOT = 'apps.lsd-first-boot'
CLEANUP = 'CoreServices _LSServerCleanEphemeralMobileContainersForFirstBoot'

APPS = [
    Check(
        FIRST_BOOT,
        OWNER,
        'Reboot detected, performing first boot work.',
        'boots >= 1',
        process=LSD,
        source=CLEANUP,
    ),
    Check(
        'apps.overlay-container-removed',
        OWNER,
        'Marking container at /Applications/ for removal',
        process=LSD,
        needs=FIRST_BOOT,
        source=CLEANUP,
        note="the overlay's apps were filed under the union's own volume; one line on the first boot after a store "
        'that predates the fix is that store',
    ),
    Check(
        'apps.overlay-not-installed',
        OWNER,
        'one of the referenced apps is not installed',
        process='SpringBoard',
        regex=r'primary: (?P<app>com\.brunostjohn\.switch\.[\w.]+)',
        show=('app',),
        source='SpringBoard.framework -[SBRecentAppLayouts _validateAndUpdateRecents:]',
        note='its "signature version was no longer supported" line is the same nil app',
    ),
]
