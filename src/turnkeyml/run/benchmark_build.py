from typing import Dict, Optional
import multiprocessing
import traceback
import psutil
import turnkeyml.common.build as build
import turnkeyml.common.exceptions as exp
import turnkeyml.common.filesystem as fs
import turnkeyml.common.printing as printing
from turnkeyml.analyze.script import set_status_on_exception
from turnkeyml.run.devices import SUPPORTED_RUNTIMES, apply_default_runtime
import turnkeyml.cli.parser_helpers as parser_helpers

# The licensing for tqdm is confusing. Pending a legal scan,
# the following code provides tqdm to users who have installed
# it already, while being transparent to users who do not
# have tqdm installed.
try:
    from tqdm import tqdm
except ImportError:

    def tqdm(iterable, **kwargs):  # pylint: disable=unused-argument
        return iterable


class SkippedBenchmark(Exception):
    """
    Indicates that a benchmark was skipped
    """


class Process(multiprocessing.Process):
    """
    Standardized way to make it possible to catch exceptions from a
    multiprocessing.Process.
    """

    def __init__(self, *args, **kwargs):
        multiprocessing.Process.__init__(self, *args, **kwargs)
        self._pconn, self._cconn = multiprocessing.Pipe()
        self._exception = None

    def run(self):
        try:
            multiprocessing.Process.run(self)
            self._cconn.send(None)
        except Exception as e:  # pylint: disable=broad-except
            tb = traceback.format_exc()
            self._cconn.send((e, tb))

    @property
    def exception(self):
        if self._pconn.poll():
            self._exception = self._pconn.recv()
        return self._exception


def benchmark_build(
    first: bool,
    cache_dir: str,
    build_name: str,
    runtime: str,
    iterations: int,
    rt_args: Optional[Dict] = None,
):
    """
    Benchmark the build artifact from a successful turnkey build.

    For example, `turnkey linear.py --build-only` would produce a build whose
    resulting artifact is an optimized ONNX file. This function would benchmark
    that optimized ONNX file.

    How it works:
        1. Attempt to load build state from the cache_dir/build_name specified
        2. Pass the build state directly into an instance of BaseRT and
            run the benchmark method
        3. Save stats to the same evaluation entry from the original build

    Args:
        first: whether this is the first benchmark in the job
        cache_dir: same as turnkey
        build_name: same as turnkey
        runtime: same as turnkey
        iterations: same as turnkey
        rt_args: same as turnkey
    """

    state = build.load_state(cache_dir, build_name)

    if state.build_status != build.FunctionStatus.SUCCESSFUL:
        raise SkippedBenchmark(
            "Only successful builds can be benchmarked with this "
            f"function, however selected build at {build_name} "
            f"has state: {state.build_status}"
        )

    selected_runtime = apply_default_runtime(state.config.device, runtime)

    if rt_args is None:
        rt_args_to_use = {}
    else:
        rt_args_to_use = rt_args

    try:
        runtime_info = SUPPORTED_RUNTIMES[selected_runtime]
    except KeyError as e:
        # User should never get this far without hitting an actionable error message,
        # but let's raise an exception just in case.
        raise SkippedBenchmark(
            f"Selected runtime is not supported: {selected_runtime}"
        ) from e

    # Check whether the device and runtime are ready for use prior to
    # running the first benchmark in the job
    # NOTE: we perform this check here, instead of in the outer loop,
    # because this is where we know `runtime_info`
    if first and "requirement_check" in runtime_info:
        runtime_info["requirement_check"]()

    # Load the stats file using the same evaluation ID used in the original build.
    # This allows us to augment those stats with more data instead of starting a new
    # evaluation entry.
    stats = fs.Stats(cache_dir, build_name, state.evaluation_id)

    stats.save_model_eval_stat(
        fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.INCOMPLETE.value
    )

    benchmark_logfile_path = ""
    try:
        # Instantiate BaseRT for the selected runtime
        runtime_handle = runtime_info["RuntimeClass"](
            cache_dir=cache_dir,
            build_name=build_name,
            stats=stats,
            iterations=iterations,
            model=state.results[0],
            # The `inputs` argument to BaseRT is only meant for
            # benchmarking runtimes that have to keep their inputs
            # in memory (e.g., `torch-eager`). We provide None here
            # because this function only works with runtimes that
            # keep their model and inputs on disk.
            inputs=None,
            device_type=state.config.device,
            runtime=selected_runtime,
            **rt_args_to_use,
        )
        benchmark_logfile_path = runtime_handle.logfile_path
        perf = runtime_handle.benchmark()

        for key, value in vars(perf).items():
            stats.save_model_eval_stat(
                key=key,
                value=value,
            )

        # Inform the user of the result
        perf.print()

        stats.save_model_eval_stat(
            fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.SUCCESSFUL.value
        )
    except Exception as e:
        set_status_on_exception(
            runtime_info["build_required"], state, stats, benchmark_logfile_path
        )

        raise e

    # Check whether this benchmark left the device and runtime in a good state
    if "requirement_check" in runtime_info:
        runtime_info["requirement_check"]()


def benchmark_cache_cli(args):
    """
    Wrapper function for benchmark_cache() that passes in the CLI arguments
    """

    rt_args = parser_helpers.decode_args(args.rt_args)

    benchmark_cache(
        cache_dir=args.cache_dir,
        build_name=args.build_name,
        benchmark_all=args.benchmark_all,
        skip_policy=args.skip_policy,
        runtime=args.runtime,
        iterations=args.iterations,
        timeout=args.timeout,
        rt_args=rt_args,
    )


def benchmark_cache(
    cache_dir: str,
    build_name: str,
    benchmark_all: bool,
    skip_policy: str,
    runtime: str,
    iterations: int = 100,
    timeout: Optional[int] = None,
    rt_args: Optional[Dict] = None,
):
    """
    Benchmark one or more builds in a cache using the benchmark_build()
    function.

    These benchmarks always run in process isolation mode because the purpose
    of this function is to quickly iterate over many builds.
    """

    printing.log_warning(
        "This is an experimental feature. Our plan is to deprecate it "
        "in favor of a new command, `turnkey benchmark cache/*`, ASAP. "
        "Please see https://github.com/onnx/turnkeyml/issues/115 "
        "for more info.\n\n"
    )

    if benchmark_all:
        builds = fs.get_available_builds(cache_dir)
    else:
        builds = [build_name]

    # Keep track of whether this is the first build we are benchmarking
    first = True

    # Iterate over all of the selected builds and benchmark them
    for build_name in tqdm(builds):
        if not fs.is_build_dir(cache_dir, build_name):
            raise exp.CacheError(
                f"No build found with name: {build_name}. "
                "Try running `turnkey cache list` to see the builds in your build cache."
            )

        state = build.load_state(cache_dir, build_name)
        stats = fs.Stats(cache_dir, build_name, state.evaluation_id)

        # Apply the skip policy by skipping over this iteration of the
        # loop if the evaluation's pre-existing benchmark status doesn't
        # meet certain criteria
        eval_stats = stats.evaluation_stats
        if (
            fs.Keys.BENCHMARK_STATUS in eval_stats
            and eval_stats[fs.Keys.BENCHMARK_STATUS]
            != build.FunctionStatus.NOT_STARTED.value
        ):
            if skip_policy == "attempted":
                printing.log_warning(
                    f"Skipping because it was previously attempted: {build_name}"
                )
                continue
            elif (
                skip_policy == "successful"
                and eval_stats[fs.Keys.BENCHMARK_STATUS]
                == build.FunctionStatus.SUCCESSFUL.value
            ):
                printing.log_warning(
                    f"Skipping because it was already successfully benchmarked: {build_name}"
                )
                continue
            elif (
                skip_policy == "failed"
                and eval_stats[fs.Keys.BENCHMARK_STATUS]
                != build.FunctionStatus.SUCCESSFUL.value
            ):
                printing.log_warning(
                    f"Skipping because it was previously attempted and failed: {build_name}"
                )
                continue
            elif skip_policy == "none":
                # Skip policy of "none" means we should never skip over a build
                pass

        printing.log_info(f"Attempting to benchmark: {build_name}")

        p = Process(
            target=benchmark_build,
            args=[first, cache_dir, build_name, runtime, iterations, rt_args],
        )
        p.start()
        p.join(timeout=timeout)

        if p.is_alive():
            # Handle the timeout, which is needed if the process is still alive after
            # waiting `timeout` seconds
            parent = psutil.Process(p.pid)
            for child in parent.children(recursive=True):
                child.kill()
            parent.kill()
            stats.save_model_eval_stat(
                fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.TIMEOUT.value
            )

            printing.log_warning(
                f"Benchmarking {build_name} canceled because it exceeded the {timeout} "
                "seconds timeout"
            )
        elif p.exception:
            # Handle any exception raised by the child process. In most cases, we should
            # move on to the next benchmark. However, if the exception was a
            # HardwareError that means the underlying runtime or device
            # is not able to conduct any more benchmarking. In this case the program
            # should exit and the user should follow the suggestion in the exception
            # message (e.g., restart their computer).

            if isinstance(p.exception[0], SkippedBenchmark):
                stats.save_model_eval_stat(
                    fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.NOT_STARTED.value
                )
            else:
                stats.save_model_eval_stat(
                    fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.ERROR.value
                )

            if isinstance(p.exception[0], exp.HardwareError):
                stats.save_model_eval_stat(fs.Keys.ERROR_LOG, p.exception[1])
                raise p.exception[0]
            else:
                printing.log_warning("Benchmarking failed with exception:")
                print(p.exception[1])
        else:
            printing.log_success(f"Done benchmarking: {build_name}")

        first = False
