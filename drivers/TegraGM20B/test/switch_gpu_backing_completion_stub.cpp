// SPDX-License-Identifier: MIT
extern "C" void switch_gpu_bridge_completed() {}

extern "C" void switch_gpu_events_completed() {}

extern "C" void switch_gpu_copy_completed(void) {}

extern "C" void switch_gpu_admission_changed(void) {}

int switch_gpu_test_ready_waiting;

extern "C" int switch_gpu_ready_waiting(void) { return switch_gpu_test_ready_waiting; }
