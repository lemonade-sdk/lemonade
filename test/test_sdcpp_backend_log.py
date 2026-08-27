"""Unit tests for the sd-cpp active-backend assertion.

Run with: python -m unittest test.test_sdcpp_backend_log -v

The excerpts below are trimmed from the real validate_sdcpp CI artifacts of the
run reported in #3401, where the rocm-stable leg passed while generating every
image on CPU.
"""

import unittest

from test.utils.sdcpp_backend_log import (
    assert_active_backend,
    find_init_failure_reasons,
    find_initialized_devices,
)

ROCM_FELL_BACK_TO_CPU = """
ggml_cuda_init: failed to initialize ROCm: no ROCm-capable device is detected
[INFO ] ggml_extend_backend.cpp:545  - Found 1 backend devices:
[DEBUG] ggml_extend_backend.cpp:548  - #0: CPU
[DEBUG] ggml_extend_backend.cpp:395  - Initializing backend: CPU
[DEBUG] ggml_extend_backend.cpp:590  - Using CPU backend
"""

VULKAN_ON_GPU = """
ggml_vulkan: Found 1 Vulkan devices:
ggml_vulkan: 0 = AMD Radeon(TM) 8060S Graphics (AMD proprietary driver) | uma: 1
[INFO ] ggml_extend_backend.cpp:545  - Found 2 backend devices:
[DEBUG] ggml_extend_backend.cpp:548  - #0: Vulkan0
[DEBUG] ggml_extend_backend.cpp:548  - #1: CPU
[DEBUG] ggml_extend_backend.cpp:395  - Initializing backend: Vulkan0
"""

ROCM_ON_GPU = """
[INFO ] ggml_extend_backend.cpp:545  - Found 2 backend devices:
[DEBUG] ggml_extend_backend.cpp:548  - #0: ROCm0
[DEBUG] ggml_extend_backend.cpp:548  - #1: CPU
[DEBUG] ggml_extend_backend.cpp:395  - Initializing backend: ROCm0
"""


class FindInitializedDevicesTest(unittest.TestCase):
    def test_reads_the_selected_device(self):
        self.assertEqual(find_initialized_devices(VULKAN_ON_GPU), ["Vulkan0"])

    def test_reports_every_sd_server_instance_in_order(self):
        # A leg loads several models, each restarting sd-server.
        log = ROCM_ON_GPU + ROCM_FELL_BACK_TO_CPU
        self.assertEqual(find_initialized_devices(log), ["ROCm0", "CPU"])

    def test_returns_empty_without_the_marker(self):
        self.assertEqual(find_initialized_devices("no device lines here"), [])


class AssertActiveBackendTest(unittest.TestCase):
    def test_accepts_the_requested_gpu_backend(self):
        self.assertEqual(assert_active_backend("vulkan", VULKAN_ON_GPU), ["Vulkan0"])
        self.assertEqual(assert_active_backend("rocm", ROCM_ON_GPU), ["ROCm0"])

    def test_accepts_cpu_when_cpu_was_requested(self):
        self.assertEqual(assert_active_backend("cpu", ROCM_FELL_BACK_TO_CPU), ["CPU"])

    def test_rejects_the_silent_cpu_fallback(self):
        with self.assertRaises(RuntimeError) as ctx:
            assert_active_backend("rocm", ROCM_FELL_BACK_TO_CPU)
        message = str(ctx.exception)
        self.assertIn("requested backend 'rocm'", message)
        self.assertIn("CPU", message)
        # The job log must name why the backend did not come up.
        self.assertIn("no ROCm-capable device is detected", message)

    def test_rejects_a_fallback_on_any_model_in_the_leg(self):
        with self.assertRaises(RuntimeError):
            assert_active_backend("rocm", ROCM_ON_GPU + ROCM_FELL_BACK_TO_CPU)

    def test_rejects_a_gpu_backend_that_is_not_the_requested_one(self):
        with self.assertRaises(RuntimeError):
            assert_active_backend("rocm", VULKAN_ON_GPU)

    def test_rejects_a_log_without_evidence(self):
        # No marker means the leg was never verified; that must fail, not pass.
        with self.assertRaises(RuntimeError) as ctx:
            assert_active_backend("rocm", "sd-server started\n")
        self.assertIn("Initializing backend:", str(ctx.exception))

    def test_rejects_an_unknown_backend(self):
        with self.assertRaises(RuntimeError):
            assert_active_backend("sycl", ROCM_ON_GPU)


class FindInitFailureReasonsTest(unittest.TestCase):
    def test_extracts_the_hip_failure(self):
        self.assertEqual(
            find_init_failure_reasons(ROCM_FELL_BACK_TO_CPU),
            [
                "ggml_cuda_init: failed to initialize ROCm: no ROCm-capable device is detected"
            ],
        )

    def test_deduplicates_repeated_failures(self):
        log = ROCM_FELL_BACK_TO_CPU + ROCM_FELL_BACK_TO_CPU
        self.assertEqual(len(find_init_failure_reasons(log)), 1)

    def test_is_empty_on_a_healthy_log(self):
        self.assertEqual(find_init_failure_reasons(ROCM_ON_GPU), [])


if __name__ == "__main__":
    unittest.main()
