// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraSMMURamView.h.
#include <IOKit/IOLib.h>
#include "TegraSMMU.h"
#include "TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

TegraSMMUIdentityTable *TegraSMMURamViewEnv::makeTable()
{
    switch_dma_buffer page = {};
    auto *table = IONew(TegraSMMUIdentityTable, 1);
    auto *users = IONew(uint16_t, TegraSMMUDomain::kEntries);
    if (!table || !users || !switch_dma_alloc(&page, TegraSMMUDomain::kPage)) {
        if (table)
            IODelete(table, TegraSMMUIdentityTable, 1);
        if (users)
            IODelete(users, uint16_t, TegraSMMUDomain::kEntries);
        return nullptr;
    }
    bzero(users, TegraSMMUDomain::kEntries * sizeof(uint16_t));
    *table = {reinterpret_cast<uint32_t *>(page.va), page.pa, 0, users, 0, 0, nullptr};
    return table;  // kept for good with its page: a view's tables outlive every hold
}

void TegraSMMURamViewEnv::clean(const void *va, size_t bytes)
{
    switch_dcache_clean(va, bytes);
}

void TegraSMMURamViewEnv::sync()
{
    switch_dcache_sync();
}

void TegraSMMURamViewEnv::flushAsid(uint32_t asid)
{
    tegra_smmu_flush(asid);
}

void TegraSMMURamViewEnv::flushEntries(uint32_t asid, uint64_t pa, uint64_t bytes, uint64_t iova, uint64_t iovaBytes)
{
    switch_smmu_flush_range(asid, pa, bytes, iova, iovaBytes);
}

bool TegraSMMURamView::init(uint32_t asidLow, uint32_t asidHigh)
{
    if (lock_)
        return true;
    IOLock *lock = IOLockAlloc();
    if (!lock || !switch_dma_alloc(&low_, TegraSMMUDomain::kPage) ||
        !switch_dma_alloc(&high_, TegraSMMUDomain::kPage) ||
        !core_.init(&env_, asidLow, reinterpret_cast<uint32_t *>(low_.va), low_.pa, asidHigh,
                    reinterpret_cast<uint32_t *>(high_.va), high_.pa)) {
        switch_dma_free(&low_);
        switch_dma_free(&high_);
        if (lock)
            IOLockFree(lock);
        return false;
    }
    lock_ = lock;
    return true;
}

int TegraSMMURamView::attach(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, bool alwaysTranslated,
                             uint32_t asid2, uint32_t ptb2, char *line, size_t size)
{
    const uint32_t fault = tegra_smmu_fault_ptb();
    if (!lock_ || !fault) {
        snprintf(line, size, "no memory for the view's directories or the fault directory");
        return -1;
    }
    const uint32_t asids[4] = {core_.halves[0].asid, core_.halves[1].asid, asid2 ? asid2 : TEGRA_SMMU_ASID_FAULT,
                               TEGRA_SMMU_ASID_FAULT};
    const uint32_t ptbs[4] = {core_.ptb(0), core_.ptb(1), asid2 ? ptb2 : fault, fault};
    return tegra_smmu_attach_split(asids, ptbs, asidReg, translationReg, translationBits, alwaysTranslated, line, size);
}

int TegraSMMURamView::attachNarrow(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, char *line,
                                   size_t size)
{
    if (!lock_) {
        snprintf(line, size, "no memory for the view's directories");
        return -1;
    }
    return tegra_smmu_attach(core_.halves[0].asid, core_.ptb(0), asidReg, translationReg, translationBits, line, size);
}

bool TegraSMMURamView::reserveTables()
{
    IOLockLock(lock_);
    const bool made = core_.makeTables();
    core_.commit();
    IOLockUnlock(lock_);
    return made;
}

bool TegraSMMURamView::hold(uint64_t pa, uint64_t bytes)
{
    IOLockLock(lock_);
    const bool held = core_.hold(pa, bytes);
    IOLockUnlock(lock_);
    return held;
}

void TegraSMMURamView::release(uint64_t pa, uint64_t bytes)
{
    IOLockLock(lock_);
    const bool released = core_.release(pa, bytes);
    IOLockUnlock(lock_);
    if (!released)
        panic("TegraSMMU: ASID %u: release of %#llx + %#llx, a page of which has no holder", core_.halves[0].asid,
              static_cast<unsigned long long>(pa), static_cast<unsigned long long>(bytes));
}

void TegraSMMURamView::commit()
{
    IOLockLock(lock_);
    core_.commit();
    IOLockUnlock(lock_);
}

bool TegraSMMURamView::holdPages(const uint64_t *pages, uint32_t count)
{
    IOLockLock(lock_);
    uint32_t held = 0;
    while (held < count && core_.hold(pages[held], TegraSMMUDomain::kPage))
        ++held;
    if (held < count) {
        for (uint32_t i = 0; i < held; ++i)
            (void)core_.release(pages[i], TegraSMMUDomain::kPage);
    }
    core_.commit();
    IOLockUnlock(lock_);
    return held == count;
}

void TegraSMMURamView::releasePages(const uint64_t *pages, uint32_t count)
{
    IOLockLock(lock_);
    bool released = true;
    for (uint32_t i = 0; i < count; ++i)
        released &= core_.release(pages[i], TegraSMMUDomain::kPage);
    core_.commit();
    IOLockUnlock(lock_);
    if (!released)
        panic("TegraSMMU: ASID %u: release of %u pages from %#llx, one of which has no holder", core_.halves[0].asid,
              count, static_cast<unsigned long long>(count ? pages[0] : 0));
}
