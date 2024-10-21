"""
Tests focused on the benchmarking functionality of turnkey CLI
"""

import os
import shutil
import glob
import csv
from typing import List, Union
import unittest
from unittest.mock import patch
import sys
import io
from contextlib import redirect_stdout
import platform
import torch
from turnkeyml.cli.cli import main as turnkeycli
import turnkeyml.tools.report as report
import turnkeyml.common.filesystem as fs
import turnkeyml.common.build as build
import turnkeyml.common.exceptions as exceptions
import turnkeyml.common.test_helpers as common
from turnkeyml.common.test_helpers import assert_success_of_builds


def bash(cmd: str) -> List[str]:
    """
    Emulate behavior of bash terminal when listing files
    """
    return glob.glob(cmd)


def flatten(lst: List[Union[str, List[str]]]) -> List[str]:
    """
    Flatten List[Union[str, List[str]]] into a List[str]
    """
    flattened = []
    for element in lst:
        if isinstance(element, list):
            flattened.extend(element)
        else:
            flattened.append(element)
    return flattened


class SmallPytorchModel(torch.nn.Module):
    def __init__(self):
        super(SmallPytorchModel, self).__init__()
        self.fc = torch.nn.Linear(10, 5)

    def forward(self, x):
        output = self.fc(x)
        return output


# Define pytorch model and inputs
pytorch_model = SmallPytorchModel()
inputs = {"x": torch.rand(10)}
inputs_2 = {"x": torch.rand(5)}
input_tensor = torch.rand(10)


class Testing(unittest.TestCase):
    def setUp(self) -> None:
        fs.rmdir(cache_dir)
        fs.rmdir(new_cache_dir)

        return super().setUp()

    # TODO: Investigate why this test is failing only on Windows CI failing
    @unittest.skipIf(platform.system() == "Windows", "Windows CI only failure")
    def test_001_cli_benchmark(self):
        # Test the first model in the corpus
        test_script = list(common.test_scripts_dot_py.keys())[0]

        testargs = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, test_script),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "optimize-ort",
            "benchmark",
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        assert_success_of_builds([test_script], cache_dir, check_perf=True)

    def test_002_runtimes(self):
        # Attempt to benchmark using an invalid runtime
        with self.assertRaises(exceptions.ArgError):
            testargs = [
                "turnkey",
                "-i",
                bash(f"{corpus_dir}/linear.py"),
                "--cache-dir",
                cache_dir,
                "discover",
                "export-pytorch",
                "optimize-ort",
                "benchmark",
                "--device",
                "x86",
                "--runtime",
                "trt",
            ]
            with patch.object(sys, "argv", flatten(testargs)):
                turnkeycli()

        # Benchmark with Pytorch
        testargs = [
            "turnkey",
            "-i",
            bash(f"{corpus_dir}/linear.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "benchmark",
            "--device",
            "x86",
            "--runtime",
            "torch-eager",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Benchmark with Onnx Runtime
        testargs = [
            "turnkey",
            "-i",
            bash(f"{corpus_dir}/linear.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "optimize-ort",
            "benchmark",
            "--device",
            "x86",
            "--runtime",
            "ort",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

    def test_003_cli_iteration_count(self):
        # Test the first model in the corpus
        test_script = list(common.test_scripts_dot_py.keys())[0]

        test_iterations = 123
        testargs = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, test_script),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "optimize-ort",
            "benchmark",
            "--iterations",
            str(test_iterations),
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        assert_success_of_builds(
            [test_script],
            cache_dir,
            check_perf=True,
            check_iteration_count=test_iterations,
        )

    def test_004_cli_process_isolation(self):
        # Test the first model in the corpus
        test_script = list(common.test_scripts_dot_py.keys())[0]

        with redirect_stdout(io.StringIO()) as f:
            testargs = [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, test_script),
                "--cache-dir",
                cache_dir,
                "--process-isolation",
                "discover",
                "export-pytorch",
                "--opset",
                "17",
                "optimize-ort",
                "benchmark",
            ]
            with patch.object(sys, "argv", testargs):
                turnkeycli()

            assert_success_of_builds([test_script], cache_dir, check_perf=True)

    def test_005_cli_export_only(self):
        # Test the first model in the corpus
        test_script = list(common.test_scripts_dot_py.keys())[0]

        testargs = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, test_script),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "benchmark",
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        assert_success_of_builds([test_script], cache_dir, check_onnx_file_count=1)

    def test_006_cli_onnx_model(self):
        """
        Manually export an ONNX file, then feed it into the CLI
        """
        build_name = "receive_onnx"
        onnx_file = os.path.join(corpus_dir, f"{build_name}.onnx")

        # Create ONNX file
        torch.onnx.export(
            pytorch_model,
            input_tensor,
            onnx_file,
            opset_version=build.DEFAULT_ONNX_OPSET,
            input_names=["input"],
            output_names=["output"],
        )

        testargs = [
            "turnkey",
            "-i",
            onnx_file,
            "--cache-dir",
            cache_dir,
            "load-onnx",
            "benchmark",
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        assert_success_of_builds([build_name], cache_dir)

    def test_007_cli_onnx_model_opset(self):
        """
        Manually export an ONNX file with a non-defualt opset, then feed it into the CLI
        """
        build_name = "receive_onnx_opset"
        onnx_file = os.path.join(corpus_dir, f"{build_name}.onnx")
        user_opset = build.MINIMUM_ONNX_OPSET

        # Make sure we are using an non-default ONNX opset
        assert user_opset != build.DEFAULT_ONNX_OPSET

        # Create ONNX file
        torch.onnx.export(
            pytorch_model,
            input_tensor,
            onnx_file,
            opset_version=user_opset,
            input_names=["input"],
            output_names=["output"],
        )

        testargs = [
            "turnkey",
            "-i",
            onnx_file,
            "--cache-dir",
            cache_dir,
            "load-onnx",
            "benchmark",
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        assert_success_of_builds([build_name], cache_dir)

    def test_008_cli_timeout(self):
        """
        Make sure that the --timeout option and its associated reporting features work.

        timeout.py is designed to take 20s to discover, which gives us the
        opportunity to kill it with a timeout.
        """

        testargs = [
            "turnkey",
            "-i",
            os.path.join(extras_dir, "timeout.py"),
            "--cache-dir",
            cache_dir,
            "--process-isolation",
            "--timeout",
            "10",
            "discover",
            "export-pytorch",
            "optimize-ort",
            "benchmark",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        testargs = [
            "turnkey",
            "report",
            "--input-caches",
            cache_dir,
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        # Read generated CSV file and make sure the build was killed by the timeout
        summary_csv_path = report.get_report_name()
        with open(summary_csv_path, "r", encoding="utf8") as summary_csv:
            summary = list(csv.DictReader(summary_csv))

        # Check the summary for "killed", but also accept the edge case that
        # the build timed out before the stats.yaml was created
        try:
            timeout_summary = summary[0]

            assert timeout_summary["build_status"] == "timeout", timeout_summary[
                "build_status"
            ]
        except IndexError:
            # Edge case where the CSV is empty because the build timed out before
            # the stats.yaml was created, which in turn means the CSV is empty
            pass
        except KeyError:
            # Edge case where the CSV only contains a key for "error_log"
            assert "timeout" in timeout_summary["error_log"]

    def test_009_cli_report(self):
        # NOTE: this is not a unit test, it relies on other command
        # If this test is failing, make sure the following tests are passing:
        # - test_cli_corpus

        test_scripts = common.test_scripts_dot_py.keys()

        # Benchmark the test corpus so we have builds to report
        testargs = [
            "turnkey",
            "-i",
            bash(f"{corpus_dir}/*.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "optimize-ort",
            "benchmark",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        testargs = [
            "turnkey",
            "report",
            "--input-caches",
            cache_dir,
        ]
        with patch.object(sys, "argv", testargs):
            turnkeycli()

        # Read generated CSV file
        summary_csv_path = report.get_report_name()
        with open(summary_csv_path, "r", encoding="utf8") as summary_csv:
            summary = list(csv.DictReader(summary_csv))

        # Check if csv file contains all expected rows and columns
        expected_cols = [
            "model_name",
            "author",
            "class",
            "parameters",
            "hash",
            "runtime",
            "device_type",
            "device",
            "mean_latency",
            "throughput",
            "selected_sequence_of_tools",
        ]
        linear_summary = summary[1]
        assert len(summary) == len(test_scripts)
        for elem in expected_cols:
            assert (
                elem in linear_summary
            ), f"Couldn't find expected key {elem} in results spreadsheet"

        # Check whether all rows we expect to be populated are actually populated
        assert (
            linear_summary["model_name"] == "linear2"
        ), f"Wrong model name found {linear_summary['model_name']}"
        assert (
            linear_summary["author"] == "turnkey"
        ), f"Wrong author name found {linear_summary['author']}"
        assert (
            linear_summary["class"] == "TwoLayerModel"
        ), f"Wrong class found {linear_summary['model_class']}"
        assert (
            linear_summary["hash"] == "80b93950"
        ), f"Wrong hash found {linear_summary['hash']}"
        assert (
            linear_summary["runtime"] == "ort"
        ), f"Wrong runtime found {linear_summary['runtime']}"
        assert (
            linear_summary["device_type"] == "x86"
        ), f"Wrong device type found {linear_summary['device_type']}"
        assert (
            float(linear_summary["mean_latency"]) > 0
        ), f"latency must be >0, got {linear_summary['x86_latency']}"
        assert (
            float(linear_summary["throughput"]) > 100
        ), f"throughput must be >100, got {linear_summary['throughput']}"

        # Make sure the report.get_dict() API works
        result_dict = report.get_dict(
            summary_csv_path,
            [
                "selected_sequence_of_tools",
                "tool_duration:discover",
                "tool_duration:export-pytorch",
                "tool_duration:optimize-ort",
                "tool_status:discover",
                "tool_status:export-pytorch",
                "tool_status:optimize-ort",
            ],
        )
        for result in result_dict.values():
            # All of the models should have exported to ONNX and optimized the ONNX model
            for tool in ["export-pytorch", "optimize-ort"]:
                assert tool in result["selected_sequence_of_tools"]
                duration = result[f"tool_duration:{tool}"]
                status = result[f"tool_status:{tool}"]
                assert (
                    status == "successful"
                ), f"Unexpected status {status} for tool '{tool}'"
                try:
                    assert (
                        float(duration) > 0
                    ), f"Tool {tool} has invalid duration '{duration}'"
                except ValueError:
                    # Catch the case where the value is not numeric
                    assert False, f"Tool {tool} has invalid duration {duration}"

    def test_010_cli_cache_benchmark(self):

        test_scripts = common.test_scripts_dot_py.keys()

        # Build the test corpus so we have builds to benchmark
        testargs = [
            "turnkey",
            "-i",
            bash(f"{corpus_dir}/*.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
            "optimize-ort",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Benchmark the single model from cache directory
        selected_build = fs.get_available_builds(cache_dir)[-1]
        state_file_path = os.path.join(
            cache_dir, selected_build, f"{selected_build}_state.yaml"
        )

        testargs = [
            "turnkey",
            "--cache-dir",
            cache_dir,
            "-i",
            state_file_path,
            "load-build",
            "benchmark",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Make sure the benchmark happened
        test_script = selected_build + ".py"
        assert_success_of_builds([test_script], cache_dir, check_perf=True)

        # Benchmark the cache directory
        testargs = [
            "turnkey",
            "--cache-dir",
            cache_dir,
            "-i",
            os.path.join(cache_dir, "*", "*_state.yaml"),
            "load-build",
            "benchmark",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Make sure the benchmarks happened
        assert_success_of_builds(test_scripts, cache_dir, check_perf=True)

    def test_011_cli_cache_move(self):

        test_script = list(common.test_scripts_dot_py.keys())[0]

        # Build a model into the default cache location
        testargs = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, test_script),
            "--cache-dir",
            cache_dir,
            "discover",
            "export-pytorch",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Move the cache to a new location
        shutil.move(cache_dir, new_cache_dir)

        # Get the build state file in its new location
        selected_build = fs.get_available_builds(new_cache_dir)[-1]
        state_file_path = os.path.join(
            new_cache_dir, selected_build, f"{selected_build}_state.yaml"
        )

        # Benchmark the cached build in its new location
        testargs = [
            "turnkey",
            "-i",
            state_file_path,
            "load-build",
            "benchmark",
        ]
        with patch.object(sys, "argv", flatten(testargs)):
            turnkeycli()

        # Make sure the benchmark happened
        test_script = selected_build + ".py"
        assert_success_of_builds([test_script], new_cache_dir, check_perf=True)


if __name__ == "__main__":
    # Create a cache directory a directory with test models
    cache_dir, corpus_dir = common.create_test_dir("benchmark")
    new_cache_dir = f"{cache_dir}2"

    extras_dir = os.path.join(corpus_dir, "extras")
    os.makedirs(extras_dir, exist_ok=True)

    for key, value in common.extras_python(corpus_dir).items():
        file_path = os.path.join(extras_dir, key)

        with open(file_path, "w", encoding="utf") as f:
            f.write(value)

    unittest.main()
