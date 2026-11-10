// SPDX-License-Identifier: MIT
// A range replayed into m2v (RawMetalReplay.mm) needs the twin of every attachment of every pass in it. m2v takes a pass with
// a nil attachment and then refuses its draws (AttachmentFormat, NoPipelineState) after the passes before it went out, so a
// range with one missing fails whole, before any of it is encoded.
#pragma once
#include <cstddef>

namespace raw {

// The first object `attachments(at, visit)` names for an operation of [from, to) that `hasTwin` refuses; null when every one
// has a twin. A null attachment is no attachment.
template <typename Attachments, typename HasTwin>
void *missingTwin(size_t from, size_t to, Attachments &&attachments, HasTwin &&hasTwin)
{
    void *missing = nullptr;
    for (size_t at = from; at < to && !missing; ++at)
        attachments(at, [&](void *object) {
            if (!missing && object && !hasTwin(object))
                missing = object;
        });
    return missing;
}

}  // namespace raw
