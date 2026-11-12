// SPDX-License-Identifier: MIT
//! The direct compile's translator devices with no m2v runtime: made from the compile profile (the enabled description
//! and policy of the runtime's device, compile_profile.json) instead of a Vulkan query. The pack generator holds a
//! live runtime to it once per process (nmb_compile_profile_check).

use crate::objects::{NmbDevice, get, guard};
use metal2vulkan::device::{
    Device, DeviceDescription, DevicePolicy, FramebufferFetchPolicy, NativeShaderTarget, VkExtension, VkFeature,
};
use std::ffi::c_char;
use std::sync::OnceLock;

const PROFILE: &str = include_str!("compile_profile.json");

#[derive(serde::Serialize, serde::Deserialize)]
struct Profile {
    enabled: DeviceDescription,
    policy: DevicePolicy,
}

/// The direct compile's three devices: compute and default render forms, the barrier form (no pixel interlock), the
/// ordered form (`RequireOrdered`, centre fragment depth)
pub(crate) struct Devices {
    pub(crate) native: Device,
    pub(crate) barrier: Device,
    pub(crate) ordered: Device,
}

fn devices(p: &Profile) -> Result<Devices, String> {
    let made = |d: Result<Device, metal2vulkan::error::Error>| {
        d.map(|d| d.with_native_target(NativeShaderTarget::Sm53))
            .map_err(|e| format!("compile device: {e:?}"))
    };
    let mut barrier = p.enabled.clone();
    barrier.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
    barrier.features.remove(&VkFeature::FragmentShaderPixelInterlock);
    barrier.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    let mut ordered = p.policy;
    ordered.framebuffer_fetch = FramebufferFetchPolicy::RequireOrdered;
    Ok(Devices {
        native: made(Device::new(p.enabled.clone(), p.policy))?,
        barrier: made(Device::new(barrier, p.policy))?,
        ordered: made(Device::new_with_center_fragment_depth(p.enabled.clone(), ordered))?,
    })
}

/// The profile's devices, made once per process
pub(crate) fn offline_devices() -> Result<&'static Devices, String> {
    static MADE: OnceLock<Result<Devices, String>> = OnceLock::new();
    MADE.get_or_init(|| {
        let p: Profile = serde_json::from_str(PROFILE).map_err(|e| format!("compile profile: {e}"))?;
        devices(&p)
    })
    .as_ref()
    .map_err(Clone::clone)
}

/// The profile of `nmb`'s runtime: its queried description with what it enabled, as `specialized_device` makes it
fn live_profile(nmb: &NmbDevice) -> Result<Profile, String> {
    let runtime = nmb.device();
    // SAFETY: the handles are the live runtime device's, used only for this query.
    let handles = unsafe { runtime.raw() };
    let description = unsafe {
        DeviceDescription::query(handles.instance, handles.physical_device, handles.instance_api_version)
    }
    .map_err(|e| format!("device description: {e:?}"))?;
    let mut enabled = description
        .with_enabled(runtime.enabled())
        .map_err(|e| format!("enabled description: {e:?}"))?;
    enabled.generated_commands = None;
    Ok(Profile {
        enabled,
        policy: *runtime.compile_device().policy(),
    })
}

/// The compile profile of `device` as JSON into `out` (`len` bytes); its length, or 0 on failure
///
/// # Safety
/// `out` is null or `len` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_profile_freeze(device: *mut NmbDevice, out: *mut c_char, len: usize) -> usize {
    guard(0, || {
        let json = serde_json::to_string(&live_profile(get(device)?)?).map_err(|e| e.to_string())?;
        if !out.is_null() && len >= json.len() {
            // SAFETY: the caller's contract.
            unsafe { std::ptr::copy_nonoverlapping(json.as_ptr(), out.cast(), json.len()) };
        }
        Ok(json.len())
    })
}

/// Whether the profile's three devices are `device`'s runtime's, by profile fingerprint
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_profile_check(device: *mut NmbDevice) -> bool {
    guard(false, || {
        let live = devices(&live_profile(get(device)?)?)?;
        let frozen = offline_devices()?;
        Ok(live.native.profile_fingerprint() == frozen.native.profile_fingerprint()
            && live.barrier.profile_fingerprint() == frozen.barrier.profile_fingerprint()
            && live.ordered.profile_fingerprint() == frozen.ordered.profile_fingerprint())
    })
}
