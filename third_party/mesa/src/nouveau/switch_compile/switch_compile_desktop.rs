// SPDX-License-Identifier: MIT
extern crate nak_rs;
extern crate nil;

use std::ffi::{c_char, c_void};

pub const SWITCH_XR_PLANE_WORDS: usize = 6;
pub const VK_SUCCESS: i32 = 0;
pub const VK_SHADER_STAGE_VERTEX_BIT: u32 = 0x0000_0001;
pub const VK_SHADER_STAGE_FRAGMENT_BIT: u32 = 0x0000_0010;
pub const VK_SHADER_STAGE_COMPUTE_BIT: u32 = 0x0000_0020;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwitchNvkStageInfo {
    pub spirv: *const u32,
    pub spirv_bytes: usize,
    pub entry: *const c_char,
    pub stage: u32,
    pub specialization: *const c_void,
    pub layout: u64,
    pub pipeline_flags: u64,
    pub pipeline_pnext: *const c_void,
    pub stage_pnext: *const c_void,
    pub sample_shading: bool,
    pub split: bool,
    pub half_targets: u32,
    pub const_push_mask: u32,
    pub const_push_words: [u32; 32],
    pub center_frag_depth: u64,
    pub semantic: *const c_void,
    pub portable_context: *const c_void,
    pub portable: Option<unsafe extern "C" fn(*const c_void, *mut *const u32, *mut usize) -> bool>,
    pub texs_f16: bool,
    pub turned: bool,
    pub xr_plane_bindings: [u64; SWITCH_XR_PLANE_WORDS],
    pub unorm_targets: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwitchNvkStage {
    pub header: [u8; 80],
    pub code: *const c_void,
    pub code_bytes: u32,
    pub data: *const c_void,
    pub data_bytes: u32,
    pub cbuf_types: [u8; 16],
    pub cbuf_count: u32,
    pub num_gprs: u8,
    pub early_fragment_tests: bool,
    pub writes_depth: bool,
    pub post_depth_coverage: bool,
    pub split: *const c_void,
    pub owner: *mut c_void,
    pub interlock_control: u32,
    pub uses_sample_shading: u64,
    pub semantic_version: u32,
    pub bound_textures: u8,
    pub num_barriers: u8,
    pub local_size: [u16; 3],
    pub smem_size: u16,
    pub slm_size: u32,
    pub crs_size: u32,
    pub cbuf_sets: [u8; 16],
    pub cbuf_offsets: [u32; 16],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SwitchNvkStageMetrics {
    pub version: u32,
    pub code_bytes: u32,
    pub instructions: u32,
    pub gprs: u32,
    pub max_warps: u32,
    pub scratch_bytes: u32,
    pub memory_spills: u32,
    pub memory_fills: u32,
    pub register_spills: u32,
    pub register_fills: u32,
    pub static_cycles: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwitchNakBinding {
    pub binding: u32,
    pub ty: i32,
    pub count: u32,
    pub flags: u32,
    pub offset: u32,
    pub stride: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwitchNakSet {
    pub bindings: *const SwitchNakBinding,
    pub binding_count: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwitchNakLayout {
    pub sets: *const SwitchNakSet,
    pub set_count: u32,
    pub create_flags: u32,
    pub push_ranges: *const c_void,
    pub push_range_count: u32,
}

unsafe extern "C" {
    pub fn switch_compile_abi(sizes: *mut [u32; 3]);
    pub fn switch_compile_stage(
        frozen: *const c_void,
        info: *const SwitchNvkStageInfo,
        layout: *const SwitchNakLayout,
        out: *mut SwitchNvkStage,
    ) -> i32;
    pub fn switch_compile_stage_finish(stage: *mut SwitchNvkStage);
    pub fn switch_compile_ranked_stage(
        frozen: *const c_void,
        info: *const SwitchNvkStageInfo,
        layout: *const SwitchNakLayout,
        out: *mut SwitchNvkStage,
    ) -> i32;
    pub fn switch_compile_split_parts(split: *const c_void, out: *mut c_void);
    pub fn switch_compile_stage_metrics(
        stage: *const SwitchNvkStage,
        out: *mut SwitchNvkStageMetrics,
        bytes: usize,
    ) -> bool;
}

pub fn abi() -> [u32; 3] {
    let mut sizes = [0u32; 3];
    unsafe {
        switch_compile_abi(&mut sizes);
    }
    sizes
}
