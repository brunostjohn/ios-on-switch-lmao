// SPDX-License-Identifier: MIT
// The 3D engine's macro slots (CALL_MME_MACRO(j)) and MME shadow scratch words as the GM20B's two drivers use them: the
// one table of the raw path's macro ABI v5. NVK's context loads its macros at slots 0..NVK_MME_COUNT-1 in nvk_mme.h's
// order, and the raw path's context replays that load before it loads its own, so the kernel's push profile reads NVK's
// clear and anti-alias macros by NVK's numbers. The raw path's draw macros take NVK's draw macros' slots, so the
// profile counts both drivers' draws as one, and every other raw macro takes a slot past them, which the profile counts
// as no draw. NVK's macros keep their shadow scratch words below SWITCH_GM20B_SCRATCH_NVK_END (nvk_mme.h
// NVK_MME_NUM_SCRATCH); the raw path's ranges lie past it. kits/SwitchGM20BEmit/test/macro_slots_test.py checks the NVK
// numbers against nvk_mme.h.
#ifndef SWITCH_GM20B_MACRO_SLOTS_H
#define SWITCH_GM20B_MACRO_SLOTS_H

enum {
    SWITCH_GM20B_MACRO_NVK_CLEAR = 2,            // NVK_MME_CLEAR
    SWITCH_GM20B_MACRO_NVK_SET_ANTI_ALIAS = 10,  // NVK_MME_SET_ANTI_ALIAS: its argument a pass's sample count
    SWITCH_GM20B_MACRO_DRAWS_FIRST = 11,         // NVK_MME_DRAW
    SWITCH_GM20B_MACRO_DRAWS_LAST = 16,          // NVK_MME_DRAW_INDEXED_INDIRECT_COUNT
    SWITCH_GM20B_MACRO_SLOTS = 128,              // CALL_MME_MACRO(j), LOAD_MME_START_ADDRESS_RAM's entries

    SWITCH_GM20B_MACRO_DRAW = SWITCH_GM20B_MACRO_DRAWS_FIRST,
    SWITCH_GM20B_MACRO_DRAW_INDEXED,
    SWITCH_GM20B_MACRO_INDIRECT,
    SWITCH_GM20B_MACRO_INDEXED_INDIRECT,
    SWITCH_GM20B_MACRO_DRAW_PARAMS,
    SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS,
    SWITCH_GM20B_MACRO_VCM_CONSTANTS,
    // indirect command buffers': an execute's range, a gated draw of each kind, a dispatch command's gate, a command's
    // bound buffers (x1-calls3's kIcbBind), a patch command's control-stage words and draw
    SWITCH_GM20B_MACRO_ICB_RANGE,
    SWITCH_GM20B_MACRO_ICB_DRAW,
    SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED,
    SWITCH_GM20B_MACRO_ICB_GATE,
    SWITCH_GM20B_MACRO_ICB_BIND,
    SWITCH_GM20B_MACRO_ICB_PATCHES,
    // a draw under vertex amplification, of vertices and of indices: draws past the slots the push profile counts
    SWITCH_GM20B_MACRO_AMPLIFY,
    SWITCH_GM20B_MACRO_AMPLIFY_INDEXED,
    SWITCH_GM20B_MACRO_RAW_END,  // one past the raw path's last slot
};

enum {
    SWITCH_GM20B_SCRATCH_WORDS = 256,  // SET_MME_SHADOW_SCRATCH(i)
    SWITCH_GM20B_SCRATCH_NVK_END = 100,

    // an indirect command buffer execute's range: location, length
    SWITCH_GM20B_SCRATCH_ICB_LOCATION = 120,
    SWITCH_GM20B_SCRATCH_ICB_LENGTH = 121,
    // a command's bound buffers: 128 + 3 x index (address low, address high, size), index < 31
    SWITCH_GM20B_SCRATCH_ICB_BUFFERS = 128,
    SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS = 31,
    SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END = SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS,
    // a patch execute's layout of its inherited state: set 0's address and the root table's (high, low), each
    // control-stage buffer's set-0 byte offset, the root byte offset of the control stage's first push word
    SWITCH_GM20B_SCRATCH_ICB_SET_HI = SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END,
    SWITCH_GM20B_SCRATCH_ICB_ROOT_HI = SWITCH_GM20B_SCRATCH_ICB_SET_HI + 2,
    SWITCH_GM20B_SCRATCH_ICB_ENTRY = SWITCH_GM20B_SCRATCH_ICB_ROOT_HI + 2,
    SWITCH_GM20B_SCRATCH_ICB_CONTROL = SWITCH_GM20B_SCRATCH_ICB_ENTRY + 3,
    SWITCH_GM20B_SCRATCH_ICB_PATCHES_END = SWITCH_GM20B_SCRATCH_ICB_CONTROL + 1,
    // vertex amplification's, which the amplify macros read (the raw path's RenderEncoder writes them where the count, the
    // mapping or the bound state changes): the count, the state's root byte offsets of its AmplificationCount,
    // AmplificationId and AmplificationViewportOffset words (0: none), its layer (SWITCH_GM20B_AMP_LAYER_*, else the root
    // byte offset of its AmplificationLayerOffset word), whether its fetches need a pixel barrier between two
    // amplifications (its barrier form's), then each amplification's render target and viewport offsets
    SWITCH_GM20B_SCRATCH_AMP_COUNT = SWITCH_GM20B_SCRATCH_ICB_PATCHES_END,
    SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD,
    SWITCH_GM20B_SCRATCH_AMP_ID_WORD,
    SWITCH_GM20B_SCRATCH_AMP_VIEWPORT_WORD,
    SWITCH_GM20B_SCRATCH_AMP_LAYER,
    SWITCH_GM20B_SCRATCH_AMP_BARRIER,
    SWITCH_GM20B_SCRATCH_AMP_LAYERS,
    SWITCH_GM20B_SCRATCH_AMP_VIEWPORTS = SWITCH_GM20B_SCRATCH_AMP_LAYERS + 8,
    SWITCH_GM20B_SCRATCH_AMP_END = SWITCH_GM20B_SCRATCH_AMP_VIEWPORTS + 8,
};

// What the patch macro's words name: a layout entry of no buffer, the constant buffer sizes it selects (NVK's
// descriptor set slot and root descriptor table), its control-stage buffers and push words
enum {
    SWITCH_GM20B_ICB_NO_ENTRY = 0x3fff,
    SWITCH_GM20B_ICB_SET_BYTES = 0x100,
    SWITCH_GM20B_ICB_ROOT_BYTES = 0x800,
    SWITCH_GM20B_ICB_PATCH_BUFFERS = 3,
    SWITCH_GM20B_ICB_PATCH_CONTROL_WORDS = 6,
};

// SWITCH_GM20B_SCRATCH_AMP_LAYER's modes besides a word: the draws render the pass's first layer, or SET_RT_LAYER takes
// each amplification's render target offset (V_SELECTS_LAYER). No root word lies at either byte offset.
enum {
    SWITCH_GM20B_AMP_LAYER_FIRST = 0,
    SWITCH_GM20B_AMP_LAYER_RT = 1,
    SWITCH_GM20B_AMP_MAX = 8,  // Metal's largest amplification count
};

#ifdef __cplusplus
#define SWITCH_GM20B_SLOTS_ASSERT(c, m) static_assert(c, m)
#else
#define SWITCH_GM20B_SLOTS_ASSERT(c, m) _Static_assert(c, m)
#endif

SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_MACRO_VCM_CONSTANTS > SWITCH_GM20B_MACRO_DRAWS_LAST,
                          "every macro but a draw's takes a slot the push profile counts as no draw");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_MACRO_RAW_END <= SWITCH_GM20B_MACRO_SLOTS, "every raw slot is a CALL_MME_MACRO");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_ICB_LOCATION >= SWITCH_GM20B_SCRATCH_NVK_END &&
                              SWITCH_GM20B_SCRATCH_ICB_LENGTH < SWITCH_GM20B_SCRATCH_ICB_BUFFERS &&
                              SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END <= SWITCH_GM20B_SCRATCH_ICB_SET_HI,
                          "no two scratch ranges overlap, nor one NVK's");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_AMP_END <= SWITCH_GM20B_SCRATCH_WORDS,
                          "every scratch word is a SET_MME_SHADOW_SCRATCH");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_AMP_VIEWPORTS - SWITCH_GM20B_SCRATCH_AMP_LAYERS ==
                                  SWITCH_GM20B_AMP_MAX &&
                              SWITCH_GM20B_SCRATCH_AMP_END - SWITCH_GM20B_SCRATCH_AMP_VIEWPORTS == SWITCH_GM20B_AMP_MAX,
                          "a mapping word for each amplification");

#undef SWITCH_GM20B_SLOTS_ASSERT

#endif
