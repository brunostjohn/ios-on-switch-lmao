// SPDX-License-Identifier: MIT
extern "C" void switch_gpu_bridge_completed() {}

extern "C" void switch_gpu_events_completed() {}

extern "C" void switch_gpu_copy_completed(void) {}

extern "C" void switch_gpu_admission_changed(void) {}

int switch_gpu_test_ready_waiting;

extern "C" int switch_gpu_ready_waiting(void) { return switch_gpu_test_ready_waiting; }

// The path counts (SwitchGpuPathStats.h) are the kernel's; a test that reads them defines its own.
extern "C" __attribute__((weak)) void switch_gpu_path_taken(int, unsigned) {}
