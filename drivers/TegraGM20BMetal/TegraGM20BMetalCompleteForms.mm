// SPDX-License-Identifier: MIT
// Completion's background forms (TegraGM20BMetalComplete.h askForms): what a pack gives a state of its own beside its
// record, asked of the shader compiler for a state it made once that state is placed.
#import "TegraGM20BMetalComplete.h"

#include <pthread/qos.h>
#include <array>
#include "gpu.h"
#include "record.h"

namespace gm20b {
namespace {

// Whether `r`'s full form (RequestForm::Full) adds what directRecord (direct.cpp) adds a pack build's record, as far as
// `r` shows it: a program variant, which programVariants makes only of a uniform range read through a residual push
// word, or its fragment program's split, which it tries only where `r` is of no storage form, set 1 arrays or argument
// tables and was not tried. Each only of a state of a vertex and a fragment stage. False where `r` is a full form
// already: split, or holding a variant beside its turned one.
bool fullFormAdds(const RuntimeRecord &r)
{
    if (r.split)
        return false;
    for (const RuntimeRecord::Variant &v : r.variants)
        if (!(v.assume & AssumeTurned))
            return false;
    const PackPipeline &p = r.pipeline;
    if (!pixelStage(p) || tessellates(p))
        return false;
    if (!r.splitTried && !p.storageMask && !p.heapArrays && !p.spillWords[0] && !p.spillWords[1])
        return true;
    const SetEntries entries = entriesOf(r);
    for (const PackUniformRange &u : p.uniform)
        if (u.entry < entries.size()) {
            const int word = bufferResidualWord(entries[u.entry]);
            if (word >= 0 && word < 32)
                return true;
        }
    return false;
}

Pipeline **twinSlot(Pipeline &made)
{
    return made.storageBlendBits ? &made.blendTwin : &made.barrierTwin;
}

}  // namespace

void Completion::askForms(const Ask &a, const CacheKey &k, Pipeline *made, const RuntimeRecord &record)
{
    if (!made || !a.facade)
        return;
    std::array<RequestForm, 2> forms{};
    uint32_t count = 0;
    if (made->storageBits && !__atomic_load_n(twinSlot(*made), __ATOMIC_ACQUIRE))
        forms[count++] = RequestForm::Barrier;
    if (fullFormAdds(record))
        forms[count++] = RequestForm::Full;
    {
        std::lock_guard<gm20b::Mutex> l(formLock_);
        uint32_t asking = 0;
        for (uint32_t i = 0; i < count; i++)
            if (formsAsked_.insert({made, forms[i]}).second)
                forms[asking++] = forms[i];
        count = asking;
    }
    if (!count)
        return;
    // off the creating thread and the answer queue, whose blocks a form's request would hold up. The descriptors are
    // copied here, on the creating thread: the synchronous creation hands over the app's own, which it may change for its
    // next state while the forms are asked (and Apple's classes are not read safely across threads)
    Ask asked = a;
    asked.facade = [a.facade copy];
    asked.descriptor = [a.descriptor copy];
    const CacheKey key = k;
    dispatch_group_enter(forms_);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        for (uint32_t i = 0; i < count; i++)
            if (!stopping_.load(std::memory_order_acquire))
                formOf(asked, key, made, forms[i]);
        dispatch_group_leave(forms_);
    });
}

void Completion::formOf(const Ask &a, const CacheKey &k, Pipeline *made, RequestForm form)
{
    CompileRequest r;
    StoreKey store;
    if (!renderRequest(a, k, false, r, store, false, RequestKind::Render, form))
        return;
    const std::string packKey = packKeyOf(r);
    RuntimeRecord stored;
    std::string why;
    if (const RecordCache::Found found = fromStore(store, packKey, stored, why); found != RecordCache::Found::None) {
        if (found == RecordCache::Found::Record)
            attachForm(made, stored, a.caTransform, form);
        return;
    }
    if (!remote_->taking() || !renderRequest(a, k, true, r, store, false, RequestKind::Render, form))
        return;
    const bool caTransform = a.caTransform;
    dispatch_group_enter(forms_);
    const bool sent = remote_->compileAsync(
        encodeRequest(r), false, uint32_t(QOS_CLASS_BACKGROUND),
        [this, made, store, packKey, caTransform, form](Remote::Answer answer, const Remote::Answered &) {
            auto record = std::make_shared<RuntimeRecord>();
            std::string refusal;
            if (stopping_.load(std::memory_order_acquire) || answer != Remote::Answer::Made ||
                fromStore(store, packKey, *record, refusal) != RecordCache::Found::Record) {
                dispatch_group_leave(forms_);
                return;
            }
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
                if (!stopping_.load(std::memory_order_acquire))
                    attachForm(made, *record, caTransform, form);
                dispatch_group_leave(forms_);
            });
        });
    if (!sent)
        dispatch_group_leave(forms_);
}

void Completion::attachForm(Pipeline *made, const RuntimeRecord &record, bool caTransform, RequestForm form)
{
    if (form == RequestForm::Full) {
        device_.attachFullForm(record);
        return;
    }
    std::string why;
    bool retry = false;
    Pipeline *twin = device_.addRecord(record, caTransform, why, &retry);
    if (!twin || twin == made || twin->storageBits)
        return;
    Pipeline *none = nullptr;
    __atomic_compare_exchange_n(twinSlot(*made), &none, twin, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

}  // namespace gm20b
