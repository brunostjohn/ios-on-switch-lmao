// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile of a render state (NativeMetalDirect.h): what m2v's translator makes of the state for
//! each of the pack generator's probe draws (drivers/TegraGM20BMetal/raw/pack/probe.mm), with no pipeline state, command
//! buffer or capture. Only the translator's public API is used; the runtime device lends its compile device and its Vulkan
//! device, on which the variants' layouts are made for NVK's stage compile.

use crate::objects::{NmbDevice, NmbRenderPipelineDescriptor, e, get, guard, raw, render_descriptor};
use crate::offline::{NmbOfflineRenderPipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, DescriptorRange, DrawCall, DrawCommands,
    FieldLocation, FragmentTests, ImageSource, NeutralResource, SamplerDescriptor, SamplerSource, StageTable,
    TextureFacts, WriteSource,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::device::{DeviceDescription, VkExtension, VkFeature};
use metal2vulkan::error::Retry;
use metal2vulkan::metal::{
    PixelFormat, SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter, TextureType,
    TextureUsage,
};
use metal2vulkan::pipeline::{
    GraphicsState, PipelineLayout, PushField, RenderPipeline, RenderPipelineDescriptor, SampledType, SlotClass,
    SlotInterface, SlotRef, Stage, StandInView, StorageAttachmentView, TextureSlot,
};
use std::ffi::{CString, c_char, c_void};
use std::sync::{Arc, Mutex};

const PROBE_SLOTS: u32 = 31;
const PROBE_TAGGED: u32 = 16;
const RUN_BYTES: [u32; 4] = [256, 16, 4096, 256];
const BOUND_RUN: usize = 3;
const MAX_WRITES: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectStage {
    pub spirv: *const u32,
    pub spirv_bytes: usize,
    pub entry: *const c_char,
    pub stage: u32,
    pub flags: u32,
    pub subgroup_size: u32,
    pub specialization: *const avk::SpecializationInfo<'static>,
    pub semantic: *const c_void,
    pub portable_context: *const c_void,
    pub portable: Option<unsafe extern "C" fn(*const c_void, *mut *const u32, *mut usize) -> bool>,
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum NmbDirectSource {
    Buffer,
    Bytes,
    Texture,
    Sampler,
    Combined,
    Attachment,
    StorageAttachment,
    Neutral,
    ColorTable,
    Other,
    DepthStandIn,
    StencilStandIn,
    ArgumentTable,
}

pub const NMB_DIRECT_IMMUTABLE: u32 = u32::MAX;
pub const NMB_DIRECT_NEUTRAL: u32 = u32::MAX - 1;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectWrite {
    pub set: u32,
    pub binding: u32,
    pub element: u32,
    pub ty: i32,
    pub source: NmbDirectSource,
    pub stage: u32,
    pub slot: u32,
    pub sampler: u32,
    pub range: u64,
    pub view_type: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbDirectSamplerState {
    pub mag_filter: u32,
    pub min_filter: u32,
    pub mipmap_mode: u32,
    pub address: [u32; 3],
    pub mip_lod_bias: f32,
    pub min_lod: f32,
    pub max_lod: f32,
    pub max_anisotropy: f32,
    pub compare_op: u32,
    pub border_color: u32,
    pub reduction: u32,
    pub border_components: [u32; 4],
    pub anisotropy: bool,
    pub compare: bool,
    pub unnormalized: bool,
}

/// What vk_sampler_state_init reads of `info`, with the reduction mode and border mapping metal2vulkan chains to it
pub(crate) fn sampler_state(
    info: &avk::SamplerCreateInfo<'_>,
    reduction: avk::SamplerReductionMode,
    components: Option<avk::ComponentMapping>,
) -> NmbDirectSamplerState {
    let c = components.unwrap_or_default();
    NmbDirectSamplerState {
        mag_filter: info.mag_filter.as_raw() as u32,
        min_filter: info.min_filter.as_raw() as u32,
        mipmap_mode: info.mipmap_mode.as_raw() as u32,
        address: [info.address_mode_u, info.address_mode_v, info.address_mode_w].map(|a| a.as_raw() as u32),
        mip_lod_bias: info.mip_lod_bias,
        min_lod: info.min_lod,
        max_lod: info.max_lod,
        max_anisotropy: info.max_anisotropy,
        compare_op: if info.compare_enable != 0 { info.compare_op.as_raw() as u32 } else { 0 },
        border_color: info.border_color.as_raw() as u32,
        reduction: reduction.as_raw() as u32,
        border_components: [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
        anisotropy: info.anisotropy_enable != 0,
        compare: info.compare_enable != 0,
        unnormalized: info.unnormalized_coordinates != 0,
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectBinding {
    pub binding: u32,
    pub ty: i32,
    pub count: u32,
    pub flags: u32,
    pub immutable: bool,
    pub sampler: NmbDirectSamplerState,
}

#[repr(C)]
pub struct NmbDirectSet {
    pub bindings: *const NmbDirectBinding,
    pub binding_count: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct NmbDirectPushRange {
    pub stages: u32,
    pub offset: u32,
    pub size: u32,
}

#[repr(C)]
pub struct NmbDirectLayout {
    pub sets: *const NmbDirectSet,
    pub set_count: u32,
    pub push: *const NmbDirectPushRange,
    pub push_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectDraw {
    pub set0: u64,
    pub set0_bindings: *const NmbDirectBinding,
    pub set0_binding_count: u32,
    pub writes: *const NmbDirectWrite,
    pub write_count: u32,
    pub push_mask: u32,
    pub push: [u32; 32],
    pub blend_color_word: u32,
    pub half_blend_color_word: u32,
    pub depth_test_word: u32,
    pub stencil_test_word: u32,
    pub push_holes: u32,
    pub vertex_slot: u32,
    pub vertex_stride: u32,
    pub variant: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectCubicSampler {
    pub slot: u32,
    pub s_address: u32,
    pub t_address: u32,
    pub r_address: u32,
    pub border_color: u32,
    pub normalized: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectTextureFormat {
    pub slot: u32,
    pub format: u64,
}

#[repr(C)]
pub struct NmbDirectBinds {
    pub cubic: *const NmbDirectCubicSampler,
    pub cubic_count: u32,
    pub textures: *const NmbDirectTextureFormat,
    pub texture_count: u32,
    pub fragment_tests: u32,
}

/// What the probe binds besides its tagged textures and samplers: cubic sampler states, and other textures' formats, by slot
#[derive(Default)]
struct Binds {
    cubic: Vec<(u32, SamplerDescriptor)>,
    textures: Vec<(u32, PixelFormat)>,
    fragment_tests: u32,
}

impl Binds {
    /// # Safety
    /// `binds` is null or valid, its arrays of their counts
    unsafe fn of(binds: *const NmbDirectBinds) -> Result<Binds, String> {
        let mut out = Binds::default();
        // SAFETY: the caller's contract.
        let Some(b) = (unsafe { binds.as_ref() }) else {
            return Ok(out);
        };
        if b.fragment_tests > 4 {
            return Err("invalid native fragment-test mode".into());
        }
        out.fragment_tests = b.fragment_tests;
        let cubic: &[NmbDirectCubicSampler] = if b.cubic.is_null() {
            &[]
        } else {
            // SAFETY: `cubic_count` records at `cubic`.
            unsafe { std::slice::from_raw_parts(b.cubic, b.cubic_count as usize) }
        };
        for c in cubic {
            let mut d = SamplerDescriptor::new();
            d.min_filter = SamplerMinMagFilter::Cubic;
            d.mag_filter = SamplerMinMagFilter::Cubic;
            d.s_address_mode = raw::<SamplerAddressMode>(c.s_address.into(), "address mode")?;
            d.t_address_mode = raw::<SamplerAddressMode>(c.t_address.into(), "address mode")?;
            d.r_address_mode = raw::<SamplerAddressMode>(c.r_address.into(), "address mode")?;
            d.border_color = raw::<SamplerBorderColor>(c.border_color.into(), "border color")?;
            d.normalized_coordinates = c.normalized;
            out.cubic.push((c.slot, d));
        }
        let textures: &[NmbDirectTextureFormat] = if b.textures.is_null() {
            &[]
        } else {
            // SAFETY: `texture_count` records at `textures`.
            unsafe { std::slice::from_raw_parts(b.textures, b.texture_count as usize) }
        };
        for t in textures {
            out.textures
                .push((t.slot, raw::<PixelFormat>(t.format, "texture format")?));
        }
        Ok(out)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct NmbDirectStorage {
    pub mask: u32,
    pub writes: u32,
    pub sample_planes: u32,
    pub samples: u32,
    pub formats: [u32; 8],
    pub fragment_tests: u32,
    pub depth_format: u32,
    pub stencil_format: u32,
    pub test_samples: u32,
    pub test_planes: u32,
    pub blend_mask: u32,
    pub blend_model: u32,
}

fn storage_of(graphics: &GraphicsState) -> Result<NmbDirectStorage, String> {
    let mut storage = NmbDirectStorage::default();
    if let Some(tests) = graphics.shader_tests {
        storage.depth_format = tests.depth.format.as_raw() as u32;
        storage.test_samples = tests.depth.samples;
        storage.test_planes = u32::from(!matches!(tests.depth.view, StandInView::Image));
        if let Some(stencil) = tests.stencil {
            if stencil.samples != tests.depth.samples {
                return Err("storage form: depth and stencil sample counts differ".into());
            }
            storage.stencil_format = stencil.format.as_raw() as u32;
            storage.test_planes |= u32::from(!matches!(stencil.view, StandInView::Image)) << 1;
        }
    }
    for attachment in &graphics.storage_attachments {
        let index = attachment.color_index as usize;
        if index >= storage.formats.len() || storage.mask & (1 << index) != 0 {
            return Err("storage form: invalid color attachment index".into());
        }
        storage.mask |= 1 << index;
        // A write the pipeline's mask leaves no channel of stores nothing: the colour is only read, so its draws need
        // no more than a barrier (describeReads' rung, raw_storage_form.h fetchesOnlyUnwritten)
        storage.writes |= u32::from(attachment.stores()) << index;
        storage.sample_planes |=
            u32::from(matches!(attachment.view, StorageAttachmentView::SamplePlanes { .. })) << index;
        storage.samples = graphics.rasterization_samples.as_raw();
        storage.formats[index] = attachment.uint_view.unwrap_or(attachment.format).as_raw() as u32;
        storage.blend_mask |= u32::from(attachment.blend_model != 0) << index;
        storage.blend_model |= u32::from(attachment.blend_model);
    }
    Ok(storage)
}

#[repr(C)]
pub struct NmbDirectState {
    pub device: u64,
    pub layout: u64,
    pub layout_data: *const NmbDirectLayout,
    pub stage_count: u32,
    pub stages: [NmbDirectStage; 2],
    pub sample_shading: bool,
    pub storage_form: bool,
    pub storage: NmbDirectStorage,
    pub blended: u32,
    pub draws: [NmbDirectDraw; 4],
}

/// A translation's failure as the caller's text; one the same request may get past (its deadline, the host's memory) starts
/// "retry: ", which the caller does not keep as the state's refusal
pub(crate) fn compile_error(what: &'static str) -> impl FnOnce(metal2vulkan::error::Error) -> String {
    move |error| match error.retry() {
        Retry::Later => format!("retry: {what}: {error:?}"),
        _ => format!("{what}: {error:?}"),
    }
}

/// The compile device of the probe's barrier form: the runtime's enabled description without the pixel interlock, as the
/// pack's barrier probe ran NVK with it hidden (NVK_DEBUG=no_fsi). One per process, made at its first ask.
static BARRIER: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);
static ORDERED: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);
static COMPUTE: Mutex<Option<([u8; 32], Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);

pub(crate) fn native_compute_device(nmb: &NmbDevice) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = COMPUTE.lock().map_err(|_| "native compute device lock")?;
    let compile = nmb.device().compile_device();
    let fingerprint = compile.profile_fingerprint();
    if let Some((_, device)) = kept.as_ref().filter(|(of, _)| *of == fingerprint) {
        return Ok(device.clone());
    }
    let device = Arc::new(compile.with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53));
    *kept = Some((fingerprint, device.clone()));
    Ok(device)
}

fn specialized_device(nmb: &NmbDevice, barrier: bool) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = if barrier { &BARRIER } else { &ORDERED }
        .lock()
        .map_err(|_| "native compile device lock")?;
    let id = nmb as *const NmbDevice as usize;
    if let Some((of, device)) = kept.as_ref().filter(|(of, _)| *of == id) {
        let _ = of;
        return Ok(device.clone());
    }
    let runtime = nmb.device();
    // SAFETY: the handles are the live runtime device's, used only for this query.
    let handles = unsafe { runtime.raw() };
    let description =
        unsafe { DeviceDescription::query(handles.instance, handles.physical_device, handles.instance_api_version) }
            .map_err(e("device description"))?;
    let mut enabled = description
        .with_enabled(runtime.enabled())
        .map_err(e("enabled description"))?;
    enabled.generated_commands = None;
    let mut policy = *runtime.compile_device().policy();
    let same = metal2vulkan::device::Device::new(enabled.clone(), policy).map_err(e("compile device"))?;
    if same.profile_fingerprint() != runtime.compile_device().profile_fingerprint() {
        return Err("the runtime's compile device is not its enabled description's".into());
    }
    if barrier {
        enabled.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderPixelInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    } else {
        policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::RequireOrdered;
        // The raw stage compiler explicitly interpolates FragCoord.z at the pixel center.
        // This profile uses that contract for sample-depth correction; the runtime profile stays unchanged.
    }
    let device = if barrier {
        metal2vulkan::device::Device::new(enabled, policy)
    } else {
        metal2vulkan::device::Device::new_with_center_fragment_depth(enabled, policy)
    };
    let device = Arc::new(
        device
            .map_err(e("native compile device"))?
            .with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53),
    );
    *kept = Some((id, device.clone()));
    Ok(device)
}

fn stage_number(stage: Stage) -> u32 {
    match stage {
        Stage::Vertex => 1,
        Stage::Fragment => 2,
        _ => 0,
    }
}

/// probe.mm's sampler at slot `s` (of the fragment stage, or a vertex one): linear filters when s&1, a repeating U when
/// s&2, max LOD 1 + s/2
fn probe_sampler(s: u32) -> SamplerDescriptor {
    let mut d = SamplerDescriptor::new();
    let filter = if s & 1 != 0 {
        SamplerMinMagFilter::Linear
    } else {
        SamplerMinMagFilter::Nearest
    };
    d.min_filter = filter;
    d.mag_filter = filter;
    d.mip_filter = SamplerMipFilter::Nearest;
    if s & 2 != 0 {
        d.s_address_mode = SamplerAddressMode::Repeat;
    }
    d.lod_max_clamp = 1.0 + 0.5 * s as f32;
    d
}

/// pack/probe_bind.h probeBindOffset: a uniform slot's residual is the offset itself (16 or more), a storage slot's its low
/// four bits (4, 8 or 12 at the vertex stage, which stays 4-aligned for fixed-function vertex buffers; at the fragment stage
/// one of the twelve that are not multiples of 4)
fn probe_bind_offset(stage: u32, slot: u32) -> u64 {
    const FRAGMENT_CODES: [u64; 12] = [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15];
    let slot = u64::from(slot);
    if stage == 2 {
        16 * (12 + slot / 12) + FRAGMENT_CODES[(slot % 12) as usize]
    } else {
        16 * (1 + slot / 3) + 4 * (1 + slot % 3)
    }
}

/// The texture slots the probe binds by their declarations: each the state's functions declare at the vertex stage or at
/// a fragment slot past the tagged ones, and each tagged fragment one declared other than a single-sample `texture2d` of
/// floats. A texture of the declared type (Core Animation's colour cubes a 3D one, a `texture2d_ms` or a `depth2d_ms` a
/// multisampled one), sampled type and comparison is bound there, so that each gets its own write at its own Metal index
/// where an unbound or mistyped slot would get the declaration's neutral
fn declared_slots(pipeline: &RenderPipeline) -> Vec<(Stage, u32, TextureSlot)> {
    pipeline
        .interface
        .slots
        .iter()
        .filter_map(|(slot, interface)| match interface {
            SlotInterface::Texture(t)
                if slot.class == SlotClass::Texture
                    && (slot.stage == Stage::Vertex
                        || (slot.stage == Stage::Fragment
                            && (slot.index >= PROBE_TAGGED
                                || t.texture_type != TextureType::Type2D
                                || t.sampled_type != SampledType::Float
                                || t.depth_compare))) =>
            {
                Some((slot.stage, slot.index, *t))
            }
            _ => None,
        })
        .collect()
}

/// The vertex sampler slots the state's functions declare: the probe binds its sampler of each index there, as it does at
/// every fragment one
fn vertex_samplers(pipeline: &RenderPipeline) -> Vec<u32> {
    pipeline
        .interface
        .slots
        .iter()
        .filter_map(|(slot, interface)| match interface {
            SlotInterface::Sampler(_) if slot.stage == Stage::Vertex && slot.class == SlotClass::Sampler => {
                Some(slot.index)
            }
            _ => None,
        })
        .collect()
}

/// What the probe binds at a slot declared `t`: its type (multisampled at the state's samples, else 4), and a format of its
/// sampled type or a depth format for a comparison where the state names none
fn declared_facts(t: &TextureSlot, own: Option<PixelFormat>, format: PixelFormat, samples: u32) -> TextureFacts {
    let pixel_format = own.unwrap_or(match t.sampled_type {
        _ if t.depth_compare => PixelFormat::Depth32Float,
        SampledType::Uint => PixelFormat::RGBA8Uint,
        SampledType::Sint => PixelFormat::RGBA8Sint,
        _ => format,
    });
    let mut facts = TextureFacts::new(t.texture_type, pixel_format);
    if matches!(
        t.texture_type,
        TextureType::Type2DMultisample | TextureType::Type2DMultisampleArray
    ) {
        facts.sample_count = if samples > 1 { samples } else { 4 };
        facts.usage |= TextureUsage::RENDER_TARGET;
    }
    facts
}

/// The bind state of probe.mm's draw `run`: every buffer slot of both stages bound (inline bytes, or the bound run's
/// buffers at each slot's own offset), 16 textures of `format` and `samples` and 16 samplers at the fragment stage, a
/// texture of its declaration at each of `declared` and a sampler at each of `vertex_samplers`, `binds`' in their places
fn probe_state(
    run: usize,
    format: PixelFormat,
    samples: u32,
    declared: &[(Stage, u32, TextureSlot)],
    vertex_samplers: &[u32],
    binds: &Binds,
) -> BindState {
    let mut state = BindState::default();
    state.fragment_tests = match binds.fragment_tests {
        1 => FragmentTests::Pass,
        2 => FragmentTests::Drop,
        3 => FragmentTests::Depth { writes: true },
        4 => FragmentTests::Stencil,
        _ => FragmentTests::Any,
    };
    for stage in [Stage::Vertex, Stage::Fragment] {
        let mut table = StageTable::default();
        for slot in 0..PROBE_SLOTS {
            let bound = if run == BOUND_RUN {
                let address = 0x1_0000_0000u64 + (u64::from(stage_number(stage)) << 24) + (u64::from(slot) << 16);
                BoundBuffer::Buffer {
                    length: 256 + u64::from(RUN_BYTES[run]),
                    offset: probe_bind_offset(stage_number(stage), slot),
                    gpu_address: address,
                    device_address: Some(address),
                    attribute_stride: None,
                }
            } else {
                BoundBuffer::Bytes {
                    len: RUN_BYTES[run],
                    attribute_stride: None,
                }
            };
            table.buffers.insert(slot, bound);
        }
        let own_format = |t: u32| {
            binds
                .textures
                .iter()
                .find(|(slot, _)| stage == Stage::Fragment && *slot == t)
                .map(|(_, f)| *f)
        };
        for (_, t, d) in declared
            .iter()
            .filter(|(at, t, _)| *at == stage && (stage == Stage::Vertex || *t >= PROBE_TAGGED))
        {
            table.textures.insert(
                *t,
                BoundTexture::Texture {
                    facts: declared_facts(d, own_format(*t), format, samples),
                    resource_id: None,
                },
            );
        }
        if stage == Stage::Vertex {
            for &s in vertex_samplers {
                table.samplers.insert(s, BoundSampler::new(probe_sampler(s)));
            }
        }
        if stage == Stage::Fragment {
            for t in 0..PROBE_TAGGED {
                let own = own_format(t);
                let mut facts = TextureFacts::new(TextureType::Type2D, own.unwrap_or(format));
                if let Some((_, _, d)) = declared.iter().find(|(at, slot, _)| *at == stage && *slot == t) {
                    facts = declared_facts(d, own, format, samples);
                } else if samples > 1 {
                    facts.texture_type = TextureType::Type2DMultisample;
                    facts.sample_count = samples;
                    facts.usage |= TextureUsage::RENDER_TARGET;
                }
                table.textures.insert(
                    t,
                    BoundTexture::Texture {
                        facts,
                        resource_id: None,
                    },
                );
                let cubic = binds.cubic.iter().find(|(slot, _)| *slot == t).map(|(_, d)| *d);
                table
                    .samplers
                    .insert(t, BoundSampler::new(cubic.unwrap_or_else(|| probe_sampler(t))));
            }
        }
        state.stages.insert(stage, table);
    }
    state
}

const PROBE_DRAW: DrawCall = DrawCall::Primitives {
    vertex_start: 0,
    vertex_count: 4,
    instance_count: 1,
    base_instance: 0,
};

pub(crate) fn slot_parts(slot: &SlotRef) -> (u32, u32) {
    (stage_number(slot.stage), slot.index)
}

fn neutral_view(neutral: &NeutralResource) -> i32 {
    match neutral {
        NeutralResource::Image { view_type, .. } => avk::ImageViewType::from(*view_type).as_raw(),
        _ => -1,
    }
}

pub(crate) fn write_of(w: &metal2vulkan::bind::DescriptorWrite, ranges: &[DescriptorRange]) -> NmbDirectWrite {
    let mut out = NmbDirectWrite {
        set: w.set,
        binding: w.binding,
        element: w.array_element,
        ty: avk::DescriptorType::from(w.ty).as_raw(),
        source: NmbDirectSource::Other,
        stage: 0,
        slot: 0,
        sampler: 0,
        range: 0,
        view_type: -1,
    };
    let sampler_of = |s: &SamplerSource| match s {
        SamplerSource::Slot { slot, .. } => slot.index,
        SamplerSource::Immutable => NMB_DIRECT_IMMUTABLE,
        _ => NMB_DIRECT_NEUTRAL,
    };
    match &w.source {
        WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => {
            out.source = if matches!(w.source, WriteSource::Buffer { .. }) {
                NmbDirectSource::Buffer
            } else {
                NmbDirectSource::Bytes
            };
            (out.stage, out.slot) = slot_parts(slot);
            out.range = ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        WriteSource::Texture { slot, view } => {
            out.source = NmbDirectSource::Texture;
            (out.stage, out.slot) = slot_parts(slot);
            out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
        }
        WriteSource::Sampler { slot, .. } => {
            out.source = NmbDirectSource::Sampler;
            (out.stage, out.slot) = slot_parts(slot);
            out.sampler = slot.index;
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            out.source = NmbDirectSource::Combined;
            match image {
                ImageSource::Texture { slot, view } => {
                    (out.stage, out.slot) = slot_parts(slot);
                    out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
                }
                ImageSource::Neutral(neutral) => {
                    out.slot = NMB_DIRECT_NEUTRAL;
                    out.view_type = neutral_view(neutral);
                }
            }
            out.sampler = sampler_of(sampler);
        }
        WriteSource::Attachment { color_index } => {
            out.source = NmbDirectSource::Attachment;
            out.slot = *color_index;
        }
        WriteSource::TileAttachment { color_index, view, .. } => {
            out.source = NmbDirectSource::StorageAttachment;
            out.stage = 2;
            out.slot = *color_index;
            out.view_type = match view {
                StorageAttachmentView::Attachment => avk::ImageViewType::TYPE_2D.as_raw(),
                StorageAttachmentView::SamplePlanes { .. } => avk::ImageViewType::TYPE_2D_ARRAY.as_raw(),
            };
        }
        WriteSource::DepthStandIn | WriteSource::StencilStandIn => {
            out.source = if matches!(w.source, WriteSource::DepthStandIn) {
                NmbDirectSource::DepthStandIn
            } else {
                NmbDirectSource::StencilStandIn
            };
            out.stage = 2;
            out.view_type = avk::ImageViewType::TYPE_2D.as_raw();
        }
        WriteSource::Neutral(neutral) => {
            out.source = NmbDirectSource::Neutral;
            out.view_type = neutral_view(neutral);
        }
        WriteSource::ColorWriteTable => {
            out.source = NmbDirectSource::ColorTable;
            out.stage = 2;
        }
        WriteSource::ArgumentTable { stage, bytes_len } => {
            out.source = NmbDirectSource::ArgumentTable;
            out.stage = stage_number(*stage);
            out.range = u64::from(*bytes_len);
        }
        _ => {}
    }
    out
}

/// A variant's layout as data, and as the runtime makes it on its device where there is one (pipeline/objects.rs
/// create_set_layout); with no device its handles are 0
pub(crate) struct Layout<'d> {
    device: Option<&'d ash::Device>,
    samplers: Vec<avk::Sampler>,
    sets: Vec<avk::DescriptorSetLayout>,
    pub(crate) pipeline: avk::PipelineLayout,
    /// Set 0's bindings, as data
    pub(crate) set0_bindings: Vec<NmbDirectBinding>,
    set_bindings: Vec<Vec<NmbDirectBinding>>,
    set_records: Vec<NmbDirectSet>,
    push_ranges: Vec<NmbDirectPushRange>,
    pub(crate) data: NmbDirectLayout,
}

impl<'d> Layout<'d> {
    pub(crate) fn new(device: Option<&'d ash::Device>, layout: &PipelineLayout) -> Result<Layout<'d>, String> {
        let mut made = Layout {
            device,
            samplers: Vec::new(),
            sets: Vec::new(),
            pipeline: avk::PipelineLayout::null(),
            set0_bindings: Vec::new(),
            set_bindings: Vec::new(),
            set_records: Vec::new(),
            push_ranges: Vec::new(),
            data: NmbDirectLayout {
                sets: std::ptr::null(),
                set_count: 0,
                push: std::ptr::null(),
                push_count: 0,
            },
        };
        for (set_index, set) in layout.sets.iter().enumerate() {
            let mut immutable: Vec<Vec<avk::Sampler>> = Vec::with_capacity(set.bindings.len());
            let mut data_bindings = Vec::with_capacity(set.bindings.len());
            for binding in &set.bindings {
                let mut data = NmbDirectBinding {
                    binding: binding.binding,
                    ty: avk::DescriptorType::from(binding.ty).as_raw(),
                    count: binding.count,
                    flags: avk::DescriptorBindingFlags::from(binding.flags).as_raw(),
                    immutable: false,
                    sampler: NmbDirectSamplerState::default(),
                };
                let Some(desc) = binding.immutable_sampler else {
                    immutable.push(Vec::new());
                    data_bindings.push(data);
                    continue;
                };
                let mut reduction = avk::SamplerReductionModeCreateInfo::default();
                let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
                let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
                data.immutable = true;
                data.sampler = sampler_state(
                    &info,
                    desc.reduction.into(),
                    desc.border_components.map(avk::ComponentMapping::from),
                );
                data_bindings.push(data);
                let Some(device) = device else {
                    immutable.push(Vec::new());
                    continue;
                };
                // SAFETY: `info` and its chain are live for the call.
                let sampler = unsafe { device.create_sampler(&info, None) }.map_err(e("sampler"))?;
                made.samplers.push(sampler);
                immutable.push(vec![sampler; binding.count as usize]);
            }
            let bindings: Vec<avk::DescriptorSetLayoutBinding<'_>> = set
                .bindings
                .iter()
                .zip(&immutable)
                .map(|(binding, samplers)| {
                    let info = avk::DescriptorSetLayoutBinding::default()
                        .binding(binding.binding)
                        .descriptor_type(binding.ty.into())
                        .descriptor_count(binding.count)
                        .stage_flags(binding.stages.into());
                    if samplers.is_empty() {
                        info
                    } else {
                        info.immutable_samplers(samplers)
                    }
                })
                .collect();
            if set_index == 0 {
                made.set0_bindings = data_bindings.clone();
            }
            made.set_bindings.push(data_bindings);
            let Some(device) = device else {
                continue;
            };
            let flags: Vec<avk::DescriptorBindingFlags> = set.bindings.iter().map(|b| b.flags.into()).collect();
            let mut flags_info = avk::DescriptorSetLayoutBindingFlagsCreateInfo::default().binding_flags(&flags);
            let mut info = avk::DescriptorSetLayoutCreateInfo::default()
                .flags(set.flags.into())
                .bindings(&bindings);
            if flags.iter().any(|f| !f.is_empty()) {
                info = info.push_next(&mut flags_info);
            }
            // SAFETY: `info` and its arrays are live for the call.
            made.sets
                .push(unsafe { device.create_descriptor_set_layout(&info, None) }.map_err(e("set layout"))?);
        }
        made.push_ranges = layout
            .push_constants
            .iter()
            .map(|range| NmbDirectPushRange {
                stages: avk::ShaderStageFlags::from(range.stages).as_raw(),
                offset: range.offset,
                size: range.size,
            })
            .collect();
        made.set_records = layout
            .sets
            .iter()
            .zip(&made.set_bindings)
            .map(|(set, bindings)| NmbDirectSet {
                bindings: bindings.as_ptr(),
                binding_count: bindings.len() as u32,
                flags: avk::DescriptorSetLayoutCreateFlags::from(set.flags).as_raw(),
            })
            .collect();
        made.data = NmbDirectLayout {
            sets: made.set_records.as_ptr(),
            set_count: made.set_records.len() as u32,
            push: made.push_ranges.as_ptr(),
            push_count: made.push_ranges.len() as u32,
        };
        let Some(device) = device else {
            return Ok(made);
        };
        let push: Vec<avk::PushConstantRange> = layout
            .push_constants
            .iter()
            .map(|range| {
                avk::PushConstantRange::default()
                    .stage_flags(range.stages.into())
                    .offset(range.offset)
                    .size(range.size)
            })
            .collect();
        let info = avk::PipelineLayoutCreateInfo::default()
            .set_layouts(&made.sets)
            .push_constant_ranges(&push);
        // SAFETY: as above.
        made.pipeline = unsafe { device.create_pipeline_layout(&info, None) }.map_err(e("pipeline layout"))?;
        Ok(made)
    }

    pub(crate) fn set0(&self) -> u64 {
        self.sets.first().map_or(0, |s| avk::Handle::as_raw(*s))
    }
}

impl Drop for Layout<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        // SAFETY: made here, used by no pipeline or set: the stage compile only reads them.
        unsafe {
            device.destroy_pipeline_layout(self.pipeline, None);
            for &set in &self.sets {
                device.destroy_descriptor_set_layout(set, None);
            }
            for &sampler in &self.samplers {
                device.destroy_sampler(sampler, None);
            }
        }
    }
}

/// One probe draw: the variant it runs on (its index among `variants`), its plan and its commands
struct Run {
    variant: usize,
    commands: DrawCommands,
    plan: BindingPlan,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    variants: &mut Vec<(VariantKey, RenderPipeline)>,
    state: &BindState,
    budget: &Budget,
) -> Result<Run, String> {
    let shape = state.shape();
    let plan = variants[0].1.plan(compile, &shape).map_err(e("plan"))?;
    let (variant, plan) = if plan.variant == variants[0].0 {
        (0, plan)
    } else {
        let at = match variants.iter().position(|(key, _)| *key == plan.variant) {
            Some(at) => at,
            None => {
                let pipeline = compile
                    .compile_render_variant(descriptor, &plan.variant, budget)
                    .map_err(compile_error("compile variant"))?;
                variants.push((plan.variant.clone(), pipeline));
                variants.len() - 1
            }
        };
        (at, variants[at].1.plan(compile, &shape).map_err(e("plan variant"))?)
    };
    let commands = plan.instantiate_draw(state, &PROBE_DRAW).map_err(e("instantiate"))?;
    Ok(Run {
        variant,
        commands,
        plan,
    })
}

fn draw_of(
    run: &Run,
    pipeline: &RenderPipeline,
    layout: &Layout<'_>,
    writes: &[NmbDirectWrite],
    variant: &CString,
) -> Result<NmbDirectDraw, String> {
    let mut draw = NmbDirectDraw {
        set0: layout.set0(),
        set0_bindings: layout.set0_bindings.as_ptr(),
        set0_binding_count: layout.set0_bindings.len() as u32,
        writes: writes.as_ptr(),
        write_count: writes.len() as u32,
        push_mask: 0,
        push: [0; 32],
        blend_color_word: 0,
        half_blend_color_word: 0,
        depth_test_word: 0,
        stencil_test_word: 0,
        push_holes: run.commands.push_holes.len() as u32,
        vertex_slot: u32::MAX,
        vertex_stride: 0,
        variant: variant.as_ptr(),
    };
    for field in &run.plan.push.fields {
        let (word, bytes) = match field.field {
            PushField::BlendColor => (&mut draw.blend_color_word, 32),
            PushField::HalfBlendColor => (&mut draw.half_blend_color_word, 16),
            PushField::ShaderTestsState => (&mut draw.depth_test_word, 16),
            PushField::ShaderStencilState => (&mut draw.stencil_test_word, 16),
            _ => continue,
        };
        let FieldLocation::PushWindow { offset } = field.location else {
            return Err("dynamic state outside the root push window".into());
        };
        if field.size != bytes || offset % 4 != 0 || offset > 128 - bytes {
            return Err("invalid dynamic state push placement".into());
        }
        let encoded = offset / 4 + 1;
        if *word != 0 && *word != encoded {
            return Err("multiple dynamic state push placements".into());
        }
        *word = encoded;
    }
    for write in &run.commands.push_constants {
        for (k, word) in write.bytes.chunks(4).enumerate() {
            let at = write.offset as usize / 4 + k;
            if at < 32 && word.len() == 4 {
                draw.push[at] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                draw.push_mask |= 1 << at;
            }
        }
    }
    if let Some(first) = run.plan.vertex_buffers.iter().min_by_key(|b| b.binding) {
        draw.vertex_slot = first.buffer_index;
        draw.vertex_stride = match pipeline
            .vertex_input
            .bindings
            .iter()
            .find(|b| b.binding == first.binding)
        {
            Some(b) => match b.stride {
                metal2vulkan::pipeline::VertexStride::Static(stride) => stride,
                _ => run
                    .commands
                    .vertex_strides
                    .iter()
                    .find(|(at, _)| *at == first.binding)
                    .map_or(0, |(_, s)| *s),
            },
            None => 0,
        };
    }
    Ok(draw)
}

/// The words used by Metal extended-range blending for `setBlendColorRed:green:blue:alpha:`.
///
/// # Safety
/// `color` and `words` point to four readable floats and eight writable words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn switch_nmb_blend_color_words(color: *const f32, words: *mut u32) {
    // SAFETY: the caller supplies both complete arrays.
    unsafe {
        let color = color.cast::<[f32; 4]>().read();
        words
            .cast::<[u32; 8]>()
            .write(metal2vulkan::bind::extended_blend_color_words(color));
    }
}

/// `texture_format`, `texture_samples`: the probe's tagged textures' MTLPixelFormat (0: BGRA8Unorm) and sample count, as
/// probe.mm binds them for the state; `binds` (null: none): what it binds besides.
///
/// # Safety
/// `descriptor` is a valid record; `binds` is null or valid; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_direct(
    device: *mut NmbDevice,
    descriptor: *const NmbRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        // SAFETY: the caller's contract.
        let binds = unsafe { Binds::of(binds) }?;
        let nmb = get(device)?;
        let descriptor = render_descriptor(descriptor)?;
        // Every raw form of a state (default, storage, ordered, barrier) and every compute kernel lowers half on SM53, so
        // a storage form and its barrier twin round alike. m2v's own twin stays on the Vulkan target (no float16 below SM70).
        let compile = if barrier || binds.fragment_tests != 0 {
            specialized_device(nmb, barrier)?
        } else {
            native_compute_device(nmb)?
        };
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { nmb.device().raw() };
        translate(
            &compile,
            &descriptor,
            Some(handles.device),
            texture_format,
            texture_samples,
            &binds,
            each,
            ctx,
        )
    })
}

/// nmb_render_pipeline_direct's answer for a state of offline functions, made on the compile profile's devices with no
/// Vulkan object: every handle the state hands over is 0.
///
/// # Safety
/// As nmb_render_pipeline_direct's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_offline(
    descriptor: *const NmbOfflineRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        // SAFETY: the caller's contract.
        let binds = unsafe { Binds::of(binds) }?;
        let descriptor = render_descriptor(descriptor)?;
        let compile = offline_devices()?.render(barrier, binds.fragment_tests);
        translate(compile, &descriptor, None, texture_format, texture_samples, &binds, each, ctx)
    })
}

/// The state `descriptor` describes as the translator makes it on `compile` for the probe's draws, its layouts made on
/// `vulkan` where there is one, handed to `each`
#[allow(clippy::too_many_arguments)]
fn translate(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    vulkan: Option<&ash::Device>,
    texture_format: u64,
    texture_samples: u32,
    binds: &Binds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let format = match texture_format {
        0 => PixelFormat::BGRA8Unorm,
        f => raw::<PixelFormat>(f, "texture format")?,
    };
    let budget = Budget::standard();
    let base = compile
        .compile_render_pipeline(descriptor, &budget)
        .map_err(compile_error("compile"))?;
    let declared = declared_slots(&base);
    let samplers = vertex_samplers(&base);
    let mut variants = vec![(VariantKey::new(), base)];
    let mut runs = Vec::with_capacity(RUN_BYTES.len());
    for run in 0..RUN_BYTES.len() {
        runs.push(settle(
            compile,
            descriptor,
            &mut variants,
            &probe_state(run, format, texture_samples.max(1), &declared, &samplers, binds),
            &budget,
        )?);
    }
    let layouts = variants
        .iter()
        .map(|(_, pipeline)| Layout::new(vulkan, &pipeline.layout))
        .collect::<Result<Vec<_>, String>>()?;
    let writes: Vec<Vec<NmbDirectWrite>> = runs
        .iter()
        .map(|run| {
            run.plan
                .descriptors
                .iter()
                .take(MAX_WRITES)
                .map(|w| write_of(w, &run.commands.descriptor_ranges))
                .collect()
        })
        .collect();
    let names: Vec<CString> = runs
        .iter()
        .map(|run| CString::new(format!("{:?}", run.plan.variant)).unwrap_or_default())
        .collect();
    let draws: Vec<NmbDirectDraw> = runs
        .iter()
        .enumerate()
        .map(|(k, run)| {
            draw_of(
                run,
                &variants[run.variant].1,
                &layouts[run.variant],
                &writes[k],
                &names[k],
            )
        })
        .collect::<Result<Vec<_>, String>>()?;

    let first = &runs[0];
    let pipeline = &variants[first.variant].1;
    let mut storage = storage_of(&pipeline.graphics)?;
    for run in &runs {
        if storage_of(&variants[run.variant].1.graphics)? != storage {
            return Err("storage form: probe variants change the attachment contract".into());
        }
    }
    if storage.mask != 0 {
        storage.fragment_tests = binds.fragment_tests;
    }
    let specialization = &first.plan.specialization;
    let entries: Vec<CString> = pipeline
        .stages
        .iter()
        .map(|s| CString::new(s.entry_point.as_str()).unwrap_or_default())
        .collect();
    let maps: Vec<Vec<avk::SpecializationMapEntry>> = pipeline
        .stages
        .iter()
        .map(|stage| {
            specialization
                .entries
                .iter()
                .filter(|entry| stage.specialization.constants.iter().any(|c| c.id == entry.constant_id))
                .map(|&entry| entry.into())
                .collect()
        })
        .collect();
    let infos: Vec<avk::SpecializationInfo<'_>> = maps
        .iter()
        .map(|map| {
            avk::SpecializationInfo::default()
                .map_entries(map)
                .data(&specialization.data)
        })
        .collect();
    if pipeline.stages.len() > 2 {
        return Err(format!("{} stages", pipeline.stages.len()));
    }
    let mut state = NmbDirectState {
        device: vulkan.map_or(0, |d| avk::Handle::as_raw(d.handle())),
        layout: avk::Handle::as_raw(layouts[first.variant].pipeline),
        layout_data: &layouts[first.variant].data,
        stage_count: pipeline.stages.len() as u32,
        stages: [NmbDirectStage {
            spirv: std::ptr::null(),
            spirv_bytes: 0,
            entry: std::ptr::null(),
            stage: 0,
            flags: 0,
            subgroup_size: 0,
            specialization: std::ptr::null(),
            semantic: std::ptr::null(),
            portable_context: std::ptr::null(),
            portable: None,
        }; 2],
        sample_shading: pipeline.graphics.sample_shading,
        storage_form: storage.mask != 0,
        storage,
        blended: pipeline
            .graphics
            .color_attachments
            .iter()
            .enumerate()
            .filter(|(_, c)| c.as_ref().is_some_and(|c| c.blend.is_some()))
            .fold(0, |mask, (j, _)| mask | 1 << j),
        draws: [draws[0], draws[1], draws[2], draws[3]],
    };
    for (k, stage) in pipeline.stages.iter().enumerate() {
        state.stages[k] = NmbDirectStage {
            spirv: stage.spirv.as_ptr(),
            spirv_bytes: stage.spirv.len() * 4,
            entry: entries[k].as_ptr(),
            stage: avk::ShaderStageFlags::from(stage.stage).as_raw(),
            flags: avk::PipelineShaderStageCreateFlags::from(stage.stage_flags).as_raw(),
            subgroup_size: stage.required_subgroup_size.unwrap_or(0),
            specialization: if maps[k].is_empty() {
                std::ptr::null()
            } else {
                (&infos[k] as *const avk::SpecializationInfo<'_>).cast()
            },
            semantic: std::ptr::null(),
            portable_context: std::ptr::null(),
            portable: None,
        };
    }
    // SAFETY: the caller's contract; everything `state` points at lives until after the call.
    Ok(unsafe { each(&state, ctx) })
}
