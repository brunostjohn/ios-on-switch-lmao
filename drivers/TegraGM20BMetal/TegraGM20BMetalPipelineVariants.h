// SPDX-License-Identifier: MIT
// A state's render variants (render_variant.h): each name is made the first time a draw selects it, on the placer's thread,
// never on the thread that draws
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "render_variant.h"
#include "variant.h"

#include <memory>

// The variant `variant` of `p` named by `forms`, Pending: placed from the pack's record of its name, else asked of the
// shader compiler. A queue head waiting for it lends the placer its priority while the placer works on it.
void gm20bPlaceVariant(SwitchMetalPipeline *p, const std::shared_ptr<gm20b::Variant<gm20b::Pipeline>> &variant,
                       const gm20b::RenderVariant &forms);

// The placer's serial queue, whose threads follow the committers (follow.h)
dispatch_queue_t gm20bVariantPlacer();
