# SwitchDisplayPort

The USB-C DisplayPort path is linked into the kernel by default. A native external
`IOMobileFramebufferLegacy` service exists from boot and reports disconnected until
an EDID is available. iPadOS chooses the output mode and presentation behavior.

The BM92T36 negotiates alternate mode while the power contract is stable. Pin D
keeps two DisplayPort lanes and the USB SuperSpeed pair; pin C uses all four lanes.
The partner's advertised assignments decide which can be used. Source-powered
adapters can enter alternate mode when the Switch has the host data role.

The driver publishes progressive RGB8 timings within the link, PLL and display
controller limits. Display controller B scans the compositor's shared IOSurfaces
through its own SMMU domain. Cached mappings survive recurring frames; completed
producer fences allow a direct flip. AUX completion, frame retirement and blocked
GPU admission use interrupts or completion notifications. Suspend tears down the
link and resume probes the sink again.

```sh
bazel test //drivers/SwitchDisplayPort/test:all
bazel build //:boot
```

The host tests cover PD negotiation, EDID parsing, link training policies and the
native IOAV timing layout from both pinned kernelcaches. Hardware acceptance still
requires the OLED dock and a direct USB-C display: test both plug orientations,
HDMI and dock reconnects, charging under USB load, and sleep/wake with output active.
Confirm USB SuperSpeed only when the negotiated pin assignment leaves its pair free.

SOR audio format, packet enable and ELD hooks are present. The HDA controller and
CoreAudio output integration are being completed separately.
