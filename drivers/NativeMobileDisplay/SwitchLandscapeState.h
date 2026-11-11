// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which window carries the overlays now (SwitchLandscapeWindows.h), and the landscape mode's state
// (SwitchLandscapeDisplay.h runs it): in the class body before the overlay headers that read them. Static: the
// instance is at the 16 KiB a kalloc_type class may have.

struct LandscapeState {
    int csc[2] = {-1, -1};  // window A's CSC (matrix, full range) as planeCsc keeps B's and C's
    bool filtersWritten = false;
};

static LandscapeState landscapeState;

// The windows as DC A scans them now: the latched composition's mode (SwitchLandscapeWindows.h).
bool landscapeOn() const
{
    return planeSetOf(planeLatch.active).landscape;
}

static Window planeWindowOf(const PlaneSet &set, unsigned index)
{
    return switch_landscape::windowsOf(set.landscape).overlay[index];
}
