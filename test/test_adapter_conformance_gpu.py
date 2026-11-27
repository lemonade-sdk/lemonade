"""GPU memory reading in tools/adapter_conformance.py: all-smi first, nvidia-smi as the fallback.

python3 -m unittest test/test_adapter_conformance_gpu.py
"""

import json
import os
import subprocess
import sys
import unittest

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
)
import adapter_conformance as ac  # noqa: E402

MIB = 1024 * 1024

SNAPSHOT = {
    "schema": 1,
    "gpus": [
        {
            "device_type": "GPU",
            "name": "Tesla P100-PCIE-16GB",
            "uuid": "GPU-eff8bb7c",
            "total_memory": 16 * 1024 * MIB,
            "used_memory": 406 * MIB,
        },
        {
            "device_type": "GPU",
            "name": "AMD Radeon Pro WX 5100 Graphics",
            "uuid": "GPU-0000:21:00.0",
            "total_memory": 8 * 1024 * MIB,
            "used_memory": 8192 * MIB,
        },
    ],
}
NVIDIA_CSV = "0, 259\n1, 2183\n"


def runner(outputs):
    """A stand-in for ac._run: maps the program name to its output, or to an exception to raise."""
    calls = []

    def run(cmd):
        calls.append(cmd)
        out = outputs.get(os.path.basename(cmd[0]))
        if isinstance(out, BaseException):
            raise out
        if out is None:
            raise FileNotFoundError(cmd[0])
        return out

    run.calls = calls
    return run


class GpuMib(unittest.TestCase):
    def setUp(self):
        self._run = ac._run
        self._env = os.environ.pop("ALL_SMI", None)

    def tearDown(self):
        ac._run = self._run
        if self._env is not None:
            os.environ["ALL_SMI"] = self._env

    def test_all_smi_snapshot_is_used_first(self):
        ac._run = runner({"all-smi": json.dumps(SNAPSHOT), "nvidia-smi": NVIDIA_CSV})
        self.assertEqual(ac.gpu_mib(), {0: 406, 1: 8192})
        self.assertEqual(ac._run.calls[0][1:], ["snapshot"])
        self.assertEqual(
            len(ac._run.calls), 1, "nvidia-smi must not run when all-smi answered"
        )

    def test_amd_cards_are_counted(self):
        ac._run = runner({"all-smi": json.dumps(SNAPSHOT)})
        self.assertEqual(ac.gpu_mib()[1], 8192)

    def test_entries_that_are_not_gpus_are_skipped(self):
        snap = {
            "gpus": [{"device_type": "NPU", "used_memory": 5 * MIB}] + SNAPSHOT["gpus"]
        }
        ac._run = runner({"all-smi": json.dumps(snap)})
        self.assertEqual(ac.gpu_mib(), {0: 406, 1: 8192})

    def test_missing_all_smi_falls_back_to_nvidia_smi(self):
        ac._run = runner({"nvidia-smi": NVIDIA_CSV})
        self.assertEqual(ac.gpu_mib(), {0: 259, 1: 2183})

    def test_bad_all_smi_output_falls_back_to_nvidia_smi(self):
        ac._run = runner({"all-smi": "not json", "nvidia-smi": NVIDIA_CSV})
        self.assertEqual(ac.gpu_mib(), {0: 259, 1: 2183})

    def test_all_smi_timeout_falls_back_to_nvidia_smi(self):
        ac._run = runner(
            {
                "all-smi": subprocess.TimeoutExpired("all-smi", 30),
                "nvidia-smi": NVIDIA_CSV,
            }
        )
        self.assertEqual(ac.gpu_mib(), {0: 259, 1: 2183})

    def test_blank_and_malformed_nvidia_smi_lines_are_skipped(self):
        ac._run = runner(
            {"nvidia-smi": "0, 259\n\nNo devices were found on bus 3\n1, 2183\n"}
        )
        self.assertEqual(ac.gpu_mib(), {0: 259, 1: 2183})

    def test_no_tool_gives_an_empty_reading(self):
        ac._run = runner({})
        self.assertEqual(ac.gpu_mib(), {})

    def test_all_smi_path_comes_from_the_environment(self):
        os.environ["ALL_SMI"] = "/opt/tools/all-smi"
        ac._run = runner({"all-smi": json.dumps(SNAPSHOT)})
        ac.gpu_mib()
        self.assertEqual(ac._run.calls[0][0], "/opt/tools/all-smi")


if __name__ == "__main__":
    unittest.main()
