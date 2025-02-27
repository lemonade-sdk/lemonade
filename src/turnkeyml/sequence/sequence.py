import sys
import time
import os
import platform
import copy
from datetime import datetime
from typing import List, Dict, Optional
import pytz
import psutil
import turnkeyml.common.printing as printing
import turnkeyml.common.exceptions as exp
import turnkeyml.common.build as build
from turnkeyml.common.system_info import get_system_info_dict
import turnkeyml.common.filesystem as fs
import turnkeyml.common.status as status
from turnkeyml.tools.tool import Tool
from turnkeyml.state import State


def _rewind_stdout(lines: int = 1):
    """
    Helper function for the command line monitor. Moves the cursor up a
    certain number of lines in the terminal, corresponding to the
    status line for a Tool, so that we can update the status of
    that Tool.
    """
    rewind_stdout_one_line = "\033[1A"
    rewind_multiple_lines = rewind_stdout_one_line * lines
    print(rewind_multiple_lines, end="")
    sys.stdout.flush()


class Sequence:
    """
    Helper class to launch and manage build tools.
    """

    def __init__(
        self,
        tools: Dict[Tool, List[str]],
    ):

        self.tools = tools

        # Make sure all the tool names are unique
        self.tool_names = [tool.__class__.unique_name for tool in self.tools.keys()]

        if len(self.tool_names) != len(set(self.tool_names)):
            msg = f"""
            All tools in a Sequence must have unique unique_names, however Sequence
            received duplicates in the list of names: {self.tool_names}
            """
            raise ValueError(msg)

        # Save the process (used to get memory usage)
        self.process = psutil.Process()

    def show_monitor(self, state: State, verbosity: bool):
        """
        Displays the monitor on the terminal. The purpose of the monitor
        is to show the status of each tool (success, failure, not started yet,
        or in-progress).
        """

        if verbosity:
            print()

            printing.logn(
                f'Building "{state.build_name}"',
                c=printing.Colors.BOLD,
            )

            for tool in self.tools:
                tool.status_line(successful=None, verbosity=True)

            _rewind_stdout(len(self.tools))

    def _advance_cursor(self, current_tool_name: str):
        # Advance the cursor below the monitor so
        # we can print a message
        tool_depth_in_sequence = len(self.tool_names) - self.tool_names.index(
            current_tool_name
        )
        stdout_lines_to_advance = tool_depth_in_sequence - 2
        cursor_down = "\n" * stdout_lines_to_advance

        print(cursor_down)

    def _get_mem_usage_str(self) -> str:
        """
        Returns a string with memory usage for the current process
        (non-swapped physical memory).  In Windows OS, the peak memory used in the
        process is also included.

        Example: '1.100 GB (1.638 GB peak)'
        """
        mem_info = self.process.memory_info()
        mem_info_str = f"{mem_info.rss / 1024 ** 3:,.3f} GB"
        if platform.system() == "Windows":
            mem_info_str += f" ({mem_info.peak_wset / 1024 ** 3:,.3f} GB peak)"
        return mem_info_str

    def launch(
        self,
        state: State,
        lean_cache: bool = False,
        monitor: Optional[bool] = None,
        track_memory_interval: Optional[float] = None,
        stats_to_save: Optional[Dict] = None,
    ) -> State:
        """
        Executes the sequence of tools.
        """

        # Allow monitor to be globally disabled by an environment variable
        if monitor is None:
            if os.environ.get("TURNKEY_BUILD_MONITOR") == "False":
                monitor_setting = False
            else:
                monitor_setting = True
        else:
            monitor_setting = monitor

        # Start tracking memory usage
        if track_memory_interval is not None:
            build_dir = build.output_dir(state.cache_dir, state.build_name)
            state.memory_tracker.start(os.getpid(), build_dir, track_memory_interval)

        # Create a build directory in the cache
        fs.make_build_dir(state.cache_dir, state.build_name)

        self.show_monitor(state, monitor_setting)

        if state.build_status == build.FunctionStatus.SUCCESSFUL:
            msg = """
            build_model() is running a build on a model that already built successfully, which
            should not happen because the build should have loaded from cache or rebuilt from scratch.
            If you are using custom tools and Sequences then you have some debugging to do. Otherwise,
            please file an issue at https://github.com/onnx/turnkeyml/issues
            """
            raise exp.Error(msg)

        # Keep a copy of any stats we loaded from disk, in case we need to
        # restore them later
        saved_stats = copy.deepcopy(fs.Stats(state.cache_dir, state.build_name).stats)

        # Save build name to stats so it shows up on reports
        state.save_stat(fs.Keys.BUILD_NAME, state.build_name)

        # Indicate that the build is running. If the build fails for any reason,
        # we will try to catch the exception and note it in the stats.
        # If a concluded build still has a status of "running", this means
        # there was an uncaught exception.
        state.save_stat(fs.Keys.BUILD_STATUS, build.FunctionStatus.INCOMPLETE)

        # Save a timestamp so that we know the order of builds within a cache
        pacific_tz = pytz.timezone("America/Los_Angeles")
        state.save_stat(
            fs.Keys.TIMESTAMP,
            datetime.now(pacific_tz),
        )

        # Save the system information used for this build
        system_info = get_system_info_dict()
        state.save_stat(
            fs.Keys.SYSTEM_INFO,
            system_info,
        )

        # Collect telemetry for the build
        state.save_stat(
            fs.Keys.SELECTED_SEQUENCE_OF_TOOLS,
            self.tool_names,
        )

        # At the beginning of a sequence no tool has started
        for tool in self.tools:
            state.save_stat(tool.status_key, build.FunctionStatus.NOT_STARTED)
            state.save_stat(tool.duration_key, "-")
            state.save_stat(tool.memory_key, "-")

        # Save any additional stats passed in via arguments
        if stats_to_save:
            for stat_key, stat_value in stats_to_save.items():
                state.save_stat(stat_key, stat_value)

        # Save initial memory as a build statistic
        state.save_stat(f"{fs.Keys.TOOL_MEMORY}:__init__", self._get_mem_usage_str())

        # Run the build
        saved_exception = None
        for tool, argv in self.tools.items():

            start_time = time.time()

            # Insert tool name into memory tracker queue before new tool starts
            state.memory_tracker.set_label(tool.unique_name)

            try:

                # Set status as incomplete, since tool just started
                state.save_stat(tool.status_key, build.FunctionStatus.INCOMPLETE)

                # Collect telemetry about the tool
                state.current_build_tool = tool.unique_name

                # Run the tool
                state = tool.parse_and_run(state, argv, monitor_setting)

                # Save the state so that it can be assessed for a cache hit
                state.save()

            except exp.SkipBuild as e:
                # SkipBuild is a special exception, which means that a build
                # was loaded from disk, then we realized we want to skip it.
                # In order to preserve the original stats and state of the build,
                # we need to restore the stats file to what it was at the beginning
                # of this function call. We also need to avoid calling state.save().

                # Restore the prior stats
                fs.save_yaml(
                    saved_stats, fs.Stats(state.cache_dir, state.build_name).file
                )

                # Advance the cursor below the monitor so
                # we can print a message
                self._advance_cursor(tool.unique_name)
                printing.log_warning(str(e))
                return

            # Broad exception is desirable as we want to capture
            # all exceptions (including those we can't anticipate)
            except Exception as e:  # pylint: disable=broad-except

                if os.environ.get("TURNKEY_DEBUG", "").lower() == "true":
                    # It may be useful to raise the exception here, since
                    # if any of the subsequent lines of code raise another
                    # exception it will be very hard to root cause e.
                    raise e

                # Update tool and build status
                state.save_stat(tool.status_key, build.FunctionStatus.ERROR)
                state.save_stat(fs.Keys.BUILD_STATUS, build.FunctionStatus.ERROR)

                # Save the log file for the failed tool to stats for easy reference
                stats = fs.Stats(state.cache_dir, state.build_name)
                stats.save_eval_error_log(tool.logfile_path)

                # Advance the cursor below the monitor so
                # we can print a message
                self._advance_cursor(tool.unique_name)

                if vars(state).get("invocation_info"):
                    state.invocation_info.status_message = f"Error: {e}"
                    state.invocation_info.status_message_color = printing.Colors.WARNING
                else:
                    printing.log_error(e)

                # We will raise this exception after we capture as many statistics
                # about the build as possible
                saved_exception = e

                # Don't run any more tools
                break

            else:
                # Update tool Status
                state.save_stat(tool.status_key, build.FunctionStatus.SUCCESSFUL)
                state.current_build_tool = None

            finally:
                # Store tool duration
                execution_time = time.time() - start_time
                state.save_stat(tool.duration_key, execution_time)

                # Store current memory and peak working memory
                state.save_stat(tool.memory_key, self._get_mem_usage_str())

                # sample each tool at least once
                state.memory_tracker.sample()

        # Stop tracking memory
        state.memory_tracker.stop()

        if not saved_exception:
            state.build_status = build.FunctionStatus.SUCCESSFUL
            state.save_stat(fs.Keys.BUILD_STATUS, build.FunctionStatus.SUCCESSFUL)
            if vars(state).get("invocation_info"):
                state.invocation_info.status_message = (
                    f"Successful build! {state.invocation_info.extra_status}"
                )
                state.invocation_info.status_message_color = printing.Colors.OKGREEN

        plot_path = state.memory_tracker.create_plot(state.build_name)
        if plot_path is not None:
            printing.log_info(f"Saved plot of memory usage to {plot_path}")
            state.save_stat(fs.Keys.MEMORY_USAGE_PLOT, plot_path)
        elif track_memory_interval is not None:
            printing.log_info("Error in memory usage tracking, no plot generated")
            state.save_stat(fs.Keys.MEMORY_USAGE_PLOT, "NONE")

        if vars(state).get("models_found") and vars(state).get("invocation_info"):

            # Present status statistics from the tools
            for tool in self.tools:
                state.invocation_info.stats_keys += tool.status_stats
            if track_memory_interval is not None:
                state.invocation_info.stats_keys += [fs.Keys.MEMORY_USAGE_PLOT]

            print()

            status.recursive_print(
                models_found=state.models_found,
                build_name=state.build_name,
                cache_dir=state.cache_dir,
                parent_model_hash=None,
                parent_invocation_hash=None,
                script_names_visited=[],
            )

        if lean_cache:
            printing.log_info("Removing build artifacts...")
            fs.clean_output_dir(state.cache_dir, state.build_name)

        state.save()

        if saved_exception:
            raise saved_exception

        printing.log_success(
            f"\n    Saved to **{build.output_dir(state.cache_dir, state.build_name)}**"
        )

        return state

    def status_line(self, verbosity):
        """
        Print a status line in the monitor for every tool in the sequence
        """
        for tool in self.tools:
            tool.status_line(successful=None, verbosity=verbosity)

    @property
    def info(self) -> Dict[str, Dict]:
        """
        Return a dictionary of tool_name:argv for the sequence
        """

        return {tool.__class__.unique_name: argv for tool, argv in self.tools.items()}
