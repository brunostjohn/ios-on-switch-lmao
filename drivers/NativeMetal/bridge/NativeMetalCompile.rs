// SPDX-License-Identifier: MIT
//! The system shader compiler's compile context (NativeMetalCompile.h): the direct compile (NativeMetalDirect.rs,
//! NativeMetalDirectCompute.rs) with no runtime device. It holds what a direct compile takes of a runtime device and nothing
//! else: NVK's instance and physical device, m2v's compile device and enable set made from them as a runtime device would
//! make them, and a Vulkan device of no queues, on which NAK compiles the stages and NVK makes the variants' layouts and
//! samplers. A device of no queues opens no kernel device, rings, heaps or descriptor tables, and it enables only the
//! features its stage compile reads (NVK's robustness state); m2v's own creation enables more, but nothing else of its
//! device reaches a record.

use crate::direct::{DirectHost, NmbDirectBinds, NmbDirectState, Vulkan, render_direct, specialized_device};
use crate::direct_compute::{NmbDirectDispatch, NmbDirectKernel, pipeline_direct};
use crate::functions::LibraryOf;
use crate::objects::{
    NmbComputePipelineDescriptor, NmbDevice, NmbLibrary, NmbRenderPipelineDescriptor, device_policy, e, get, guard,
    library_handle,
};
use ash::vk as avk;
use metal2vulkan::device::{DeviceDescription, DevicePolicy, EnableSet, VkExtension, VkFeature};
use metal2vulkan::vk::ApiVersion;
use std::ffi::{CStr, c_char, c_void};
use std::os::unix::ffi::OsStrExt;

/// The instance version a runtime device's instance is made at (DeviceOptions::instance_api_version unset), which its
/// description is queried at
const API: ApiVersion = ApiVersion::V1_2;

pub struct NmbCompileContext {
    _entry: ash::Entry,
    instance: ash::Instance,
    physical: avk::PhysicalDevice,
    device: ash::Device,
    compile: metal2vulkan::device::Device,
    enabled: EnableSet,
}

impl DirectHost for NmbCompileContext {
    fn compile_device(&self) -> &metal2vulkan::device::Device {
        &self.compile
    }

    fn enabled(&self) -> &EnableSet {
        &self.enabled
    }

    fn vulkan(&self) -> Vulkan<'_> {
        Vulkan {
            instance: &self.instance,
            physical: self.physical,
            api: API,
            device: &self.device,
        }
    }
}

/// What a runtime device of `description` enables under `policy`, and whether it enables device-generated commands: the
/// runtime's device/create.rs enable_set and generated_commands under nmb_device_open's options (nothing withheld)
fn runtime_enables(description: &DeviceDescription, policy: &DevicePolicy) -> (EnableSet, bool) {
    let mut set = description.recommended_enables(policy);
    if description.extensions.contains(&VkExtension::ExtExternalMemoryHost) {
        set.extensions.insert(VkExtension::ExtExternalMemoryHost);
    }
    let generated = description.generated_commands.is_some()
        && set.features.contains(&VkFeature::BufferDeviceAddress)
        && description.extensions.contains(&VkExtension::KhrMaintenance5)
        && description.features.contains(&VkFeature::Maintenance5);
    if generated {
        set.extensions.insert(VkExtension::KhrMaintenance5);
        set.features.insert(VkFeature::Maintenance5);
    }
    (set, generated)
}

/// NVK's device of no queues with the robustness features `set` enables: what vk_device_init makes its robustness state of,
/// the one part of a device NVK's stage compile reads
fn compile_only_device(
    instance: &ash::Instance,
    physical: avk::PhysicalDevice,
    set: &EnableSet,
) -> Result<ash::Device, String> {
    let on = |feature| set.features.contains(&feature);
    let mut features = avk::PhysicalDeviceFeatures2::default()
        .features(avk::PhysicalDeviceFeatures::default().robust_buffer_access(on(VkFeature::RobustBufferAccess)));
    let mut robustness2 = avk::PhysicalDeviceRobustness2FeaturesEXT::default()
        .robust_buffer_access2(on(VkFeature::RobustBufferAccess2))
        .robust_image_access2(on(VkFeature::RobustImageAccess2))
        .null_descriptor(on(VkFeature::NullDescriptor));
    let mut image =
        avk::PhysicalDeviceImageRobustnessFeatures::default().robust_image_access(on(VkFeature::RobustImageAccess));
    let mut extensions = Vec::new();
    if set.extensions.contains(&VkExtension::ExtRobustness2) {
        extensions.push(ash::ext::robustness2::NAME.as_ptr());
    }
    if set.extensions.contains(&VkExtension::ExtImageRobustness) {
        extensions.push(ash::ext::image_robustness::NAME.as_ptr());
    }
    let mut info = avk::DeviceCreateInfo::default()
        .enabled_extension_names(&extensions)
        .push_next(&mut features);
    if robustness2.robust_buffer_access2 | robustness2.robust_image_access2 | robustness2.null_descriptor != 0 {
        info = info.push_next(&mut robustness2);
    }
    if image.robust_image_access != 0 {
        info = info.push_next(&mut image);
    }
    // SAFETY: `physical` is the instance's, and `info` and its chain live for the call.
    unsafe { instance.create_device(physical, &info, None) }.map_err(e("compile device"))
}

fn open(loader: &CStr) -> Result<NmbCompileContext, String> {
    // SAFETY: NVK's image, which exports vkGetInstanceProcAddr, opened as a runtime device opens its loader.
    let entry =
        unsafe { ash::Entry::load_from(std::ffi::OsStr::from_bytes(loader.to_bytes())) }.map_err(e("loader"))?;
    let application = avk::ApplicationInfo::default()
        .engine_name(c"metal2vulkan")
        .api_version(API.packed());
    let info = avk::InstanceCreateInfo::default().application_info(&application);
    // SAFETY: `info` lives for the call.
    let instance = unsafe { entry.create_instance(&info, None) }.map_err(e("instance"))?;
    // SAFETY: the instance is live.
    let physical = unsafe { instance.enumerate_physical_devices() }
        .map_err(e("physical devices"))?
        .into_iter()
        .next()
        .ok_or("no physical device")?;
    // SAFETY: `physical` was enumerated from the instance, made at API.
    let description = unsafe { DeviceDescription::query(&instance, physical, API) }.map_err(e("device description"))?;
    let policy = device_policy();
    let (enabled, generated) = runtime_enables(&description, &policy);
    let mut stated = description.with_enabled(&enabled).map_err(e("enabled description"))?;
    if !generated {
        stated.generated_commands = None;
    }
    let compile = metal2vulkan::device::Device::new(stated, policy).map_err(e("compile device"))?;
    let device = compile_only_device(&instance, physical, &enabled)?;
    Ok(NmbCompileContext {
        _entry: entry,
        instance,
        physical,
        device,
        compile,
        enabled,
    })
}

/// # Safety
/// `loader` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_context_open(loader: *const c_char) -> *mut NmbCompileContext {
    guard(std::ptr::null_mut(), || {
        if loader.is_null() {
            return Err("a loader is required".into());
        }
        // SAFETY: the caller's contract.
        Ok(Box::into_raw(Box::new(open(unsafe { CStr::from_ptr(loader) })?)))
    })
}

/// A library of a metallib's AIR slice (`air`: of bare AIR) made without a device, as nmb_library_new makes one
///
/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_library_new(bytes: *const c_void, length: usize, air: bool) -> *mut NmbLibrary {
    guard(std::ptr::null_mut(), || {
        if bytes.is_null() || length == 0 {
            return Err("no library bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length) };
        let library = if air {
            metal2vulkan::library::Library::from_air(bytes).map_err(e("AIR"))?
        } else {
            metal2vulkan::library::Library::from_metallib(
                crate::air_slice(bytes).ok_or("metallib without an AIR64 slice")?,
            )
            .map_err(e("metallib"))?
        };
        Ok(library_handle(LibraryOf::Compile(library)))
    })
}

/// nmb_render_pipeline_direct on the context
///
/// # Safety
/// As nmb_render_pipeline_direct's.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn nmb_compile_render_direct(
    context: *const NmbCompileContext,
    descriptor: *const NmbRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    // SAFETY: the caller's contract.
    unsafe {
        render_direct(
            || Ok(get(context)? as &dyn DirectHost),
            descriptor,
            barrier,
            texture_format,
            texture_samples,
            binds,
            each,
            ctx,
        )
    }
}

/// nmb_compute_pipeline_direct on the context
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_compute_direct(
    context: *const NmbCompileContext,
    descriptor: *const NmbComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        pipeline_direct(get(context)?, descriptor, dispatch, each, ctx)
    })
}

/// Whether the context compiles as `device` does: the same enable set, and the same profile of each compile device a
/// direct compile picks (the runtime's, its barrier and ordered forms; the native target's follows the runtime's)
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_context_matches(context: *const NmbCompileContext, device: *const NmbDevice) -> bool {
    guard(false, || {
        let (context, device) = (get(context)?, get(device)?);
        let mut same = context.enabled == *DirectHost::enabled(device)
            && context.compile.profile_fingerprint() == DirectHost::compile_device(device).profile_fingerprint();
        for barrier in [false, true] {
            same &= specialized_device(context, barrier)?.profile_fingerprint()
                == specialized_device(device, barrier)?.profile_fingerprint();
        }
        Ok(same)
    })
}
