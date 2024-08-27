import argparse
from typing import Optional
import turnkeyml.common.exceptions as exp
import turnkeyml.common.filesystem as fs
from turnkeyml.tools import Tool
from turnkeyml.state import State
import turnkeyml.cli.parser_helpers as parser_helpers
from turnkeyml_plugin_devices.common.run.devices import (
    runtime_plugins,
    apply_default_runtime,
)
from turnkeyml_plugin_devices.common.run.performance import Device, parse_device

default_iterations = 100
benchmark_default_device = "x86"


class Benchmark(Tool):
    """
    Tool that benchmarks a model based on the selected device and runtime.

    Expected inputs:
     - state.results is a model to be benchmarked

    Outputs: None
    """

    unique_name = "benchmark"

    def __init__(self):
        super().__init__(monitor_message="Benchmarking model")

        self.status_stats = ["throughput", "mean_latency"]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        supported_devices, supported_runtimes, _ = runtime_plugins()

        parser = __class__.helpful_parser(
            short_description="Benchmark a model",
            add_help=add_help,
        )

        parser.add_argument(
            "--device",
            choices=supported_devices,
            dest="device",
            help="Type of hardware device to be used for the benchmark "
            f'(defaults to "{benchmark_default_device}")',
            required=False,
        )

        parser.add_argument(
            "--runtime",
            choices=supported_runtimes.keys(),
            dest="runtime",
            help="Software runtime that will be used to collect the benchmark. "
            "Must be compatible with the selected device. "
            "Automatically selects a sequence if `--sequence` is not used. "
            "If this argument is not set, the default runtime of the selected device will be used.",
            required=False,
            default=None,
        )

        parser.add_argument(
            "--iterations",
            dest="iterations",
            type=int,
            default=default_iterations,
            help="Number of execution iterations of the model to capture\
                the benchmarking performance (e.g., mean latency)",
        )

        parser.add_argument(
            "--rt-args",
            dest="rt_args",
            type=str,
            nargs="*",
            help="Optional arguments provided to the runtime being used",
        )

        return parser

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        parsed_args = super().parse(state, args, known_only)

        # Inherit the device from the tool of a prior tool, if available
        parse_device(
            state, parsed_args, benchmark_default_device, self.__class__.__name__
        )

        parsed_args.rt_args = parser_helpers.decode_args(parsed_args.rt_args)

        return parsed_args

    def run(
        self,
        state: State,
        device: str = benchmark_default_device,
        runtime: str = None,
        iterations: int = default_iterations,
        rt_args: Optional[str] = None,
    ):

        _, supported_runtimes, _ = runtime_plugins()

        selected_runtime = apply_default_runtime(device, runtime)

        # Get the default part and config by providing the Device class with
        # the supported devices by the runtime
        runtime_supported_devices = supported_runtimes[selected_runtime][
            "supported_devices"
        ]
        specific_device = str(Device(device, runtime_supported_devices))

        if rt_args is None:
            rt_args_to_use = {}
        else:
            rt_args_to_use = rt_args

        try:
            runtime_info = supported_runtimes[selected_runtime]
        except KeyError as e:
            # User should never get this far without hitting an actionable error message,
            # but let's raise an exception just in case.
            raise exp.ToolError(
                f"Selected runtime is not supported: {selected_runtime}"
            ) from e

        # Save the device name that will be used for the benchmark
        state.save_stat(fs.Keys.DEVICE, runtime_info["RuntimeClass"].device_name())

        # Save specific information into its own key for easier access
        state.save_stat(
            fs.Keys.DEVICE_TYPE,
            specific_device,
        )
        state.save_stat(
            fs.Keys.RUNTIME,
            runtime,
        )

        state.save_stat(
            fs.Keys.ITERATIONS,
            iterations,
        )

        # Check whether the device and runtime are ready for use prior to
        # running the benchmark
        if "requirement_check" in runtime_info:
            runtime_info["requirement_check"]()

        # Each runtimes can contribute its own status stats
        if runtime_info.get("status_stats"):
            self.status_stats += runtime_info.get("status_stats")

        # Instantiate BaseRT for the selected runtime
        runtime_handle = runtime_info["RuntimeClass"](
            cache_dir=state.cache_dir,
            build_name=state.build_name,
            stats=fs.Stats(state.cache_dir, state.build_name),
            iterations=iterations,
            model=state.results,
            inputs=vars(state).get(fs.Keys.INPUTS),
            device_type=specific_device,
            runtime=selected_runtime,
            **rt_args_to_use,
        )
        perf = runtime_handle.benchmark()

        for key, value in vars(perf).items():
            state.save_stat(
                key=key,
                value=value,
            )

        # Inform the user of the result
        perf.print()

        state.perf = perf

        return state
