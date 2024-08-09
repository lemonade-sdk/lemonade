"""
Utilities for spawning new turnkey calls, in both local processes and on Slurm
"""

import os
import subprocess
import pathlib
import time
import shlex
import platform
from threading import Event, Lock, Thread
from time import monotonic
import getpass
from typing import List, Optional, Dict, Union
import psutil
import turnkeyml.common.filesystem as filesystem
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build
from turnkeyml.cli.parser_helpers import encode_args
from turnkeyml.sequence import Sequence


class WatchdogTimer(Thread):
    """
    Kill process in *timeout* seconds unless the timer is restarted.

    This is needed because Popen natively supports streaming output to the terminal,
    checking that output, and timeouts--but not all 3 at the same time.

    We use this function to provide a timeout while leveraging Popen's native ability
    to stream and check output.
    """

    def __init__(self, timeout, pid, timer=monotonic, **kwargs):
        super().__init__(**kwargs)
        self.timeout = timeout
        self.pid = pid
        self.timer = timer
        self.cancelled = Event()
        self.blocked = Lock()
        self.timeout_reached = False
        self.deadline = None

    def run(self):
        # Don't start the timer until `.start()` is called
        self.restart()
        # Wait until timeout happens or the timer is canceled
        while not self.cancelled.wait(self.deadline - self.timer()):
            with self.blocked:
                if self.deadline <= self.timer() and not self.cancelled.is_set():
                    self.timeout_reached = True
                    return self.kill_process_tree()

    def restart(self):
        self.deadline = self.timer() + self.timeout

    def cancel(self):
        self.cancelled.set()

    def kill_process_tree(self):
        parent = psutil.Process(self.pid)
        for child in parent.children(recursive=True):
            child.kill()
        parent.kill()


def parse_evaluation_id(line: str, current_value: str) -> Optional[str]:
    """
    Parse the evaluation ID from a line of turnkey process output.
    Used to clean up after a turnkey subprocess is killed.
    """

    if "Capturing statistics in turnkey_stats.yaml" in line:
        # This indicates a stats file was created for this evaluation
        # Expected phrase: "Capturing statistics in turnkey_stats.yaml
        #   under evaluation ID: {evaluation_id}"
        return line.replace(
            "Capturing statistics in turnkey_stats.yaml under evaluation ID: ", ""
        ).rstrip()
    else:
        # Don't replace a previously-parsed value with None
        # if we have already found one
        if current_value is not None:
            return current_value
        return None


def parse_build_name(line: str, current_value: str) -> Optional[str]:
    """
    Parse the build directory from a line of turnkey process output.
    Used to clean up after a turnkey subprocess is killed.
    """

    if "Build dir:" in line:
        # This declares the name of the build directory
        # 'Build' directories are created for any evaluation, even
        # if there is not actually a build (e.g., torch-eager runtime benchmark),
        # which is why we use this line to find the build directory.
        # Expected phrase:
        #   "Build dir:      {cache_dir}/{build_name}"
        return os.path.basename(
            os.path.normpath(line.replace("Build dir:", "").rstrip())
        )
    else:
        # Don't replace a previously-parsed value with None
        # if we have already found one
        if current_value is not None:
            return current_value
        return None


if os.environ.get("TURNKEY_TIMEOUT_SECONDS"):
    timeout_env_var = os.environ.get("TURNKEY_TIMEOUT_SECONDS")
    SECONDS_IN_A_DAY = 60 * 60 * 24
    if timeout_env_var > SECONDS_IN_A_DAY:
        raise ValueError(
            f"Value of TURNKEY_TIMEOUT_SECONDS must be less than 1 day, got {timeout_env_var}"
        )
    DEFAULT_TIMEOUT_SECONDS = int(timeout_env_var)
else:
    DEFAULT_TIMEOUT_SECONDS = 3600


def slurm_jobs_in_queue(job_name=None) -> List[str]:
    """Return the set of slurm jobs that are currently pending/running"""
    user = getpass.getuser()
    if job_name is None:
        output = subprocess.check_output(["squeue", "-u", user])
    else:
        output = subprocess.check_output(["squeue", "-u", user, "--name", job_name])
    output = output.split(b"\n")
    output = [s.decode("utf").split() for s in output]

    # Remove headers
    output.pop(0)

    # Remove empty line at the end
    output.pop(-1)

    # Get just the job names
    if len(output) > 0:
        name_index_in_squeue = 2
        output = [s[name_index_in_squeue] for s in output]

    return output


def arg_format(name: str):
    name_underscores = name.replace("_", "-")
    return f"--{name_underscores}"


def list_arg(key: str, values: List):
    if values is not None:
        result = " ".join(values)
        return f"{key} {result}"
    else:
        return ""


def value_arg(key: str, value: Union[str, int]):
    if value is not None:
        return f'{key}="{value}"'
    else:
        return ""


def bool_arg(key: str, value: bool):
    if value:
        return f"{key}"
    else:
        return ""


def dict_arg(key: str, value: Dict):
    if value:
        return f"{key} {' '.join(encode_args(value))}"
    else:
        return ""


def sequence_arg(value: Sequence) -> Dict[str, Dict[str, str]]:
    result = ""
    for tool, args in value.info.items():
        result = result + f"{tool} {' '.join(args)} "

    return result


def run_turnkey(
    build_name: str,
    sequence: Sequence,
    file_name: str,
    process_isolation: bool,
    use_slurm: bool,
    cache_dir: str,
    lean_cache: bool,
    timeout: Optional[int] = DEFAULT_TIMEOUT_SECONDS,
    working_dir: str = os.getcwd(),
    ml_cache_dir: Optional[str] = os.environ.get("SLURM_ML_CACHE"),
    max_jobs: int = 50,
):
    """
    Run turnkey on a single input file in a separate process (e.g., Slurm, subprocess).
    Any arguments that should be passed to the new turnkey process must be provided via kwargs.

    kwargs must also match the following format:
      The key must be the snake_case version of the CLI argument (e.g, build_only for --build-only)
    """

    if use_slurm and process_isolation:
        raise ValueError(
            "use_slurm and process_isolation are mutually exclusive, but both are True"
        )

    type_to_formatter = {
        str: value_arg,
        int: value_arg,
        bool: bool_arg,
        list: list_arg,
        dict: dict_arg,
    }

    invocation_args = f"-i {file_name}"

    # Add cache_dir to kwargs so that it gets processed
    # with the other arguments
    kwargs = {"cache_dir": cache_dir, "lean_cache": lean_cache}

    for key, value in kwargs.items():
        if value is not None:
            arg_str = type_to_formatter[type(value)](arg_format(key), value)
            invocation_args = invocation_args + " " + arg_str

    invocation_args = invocation_args + " " + sequence_arg(sequence)

    if use_slurm:
        # Change args into the format expected by Slurm
        slurm_args = " ".join(shlex.split(invocation_args))

        # Remove the .py extension from the build name
        job_name = filesystem.clean_file_name(file_name)

        # Put the timeout into format days-hh:mm:ss
        hh_mm_ss = time.strftime("%H:%M:%S", time.gmtime(timeout))
        slurm_format_timeout = f"00-{hh_mm_ss}"

        while len(slurm_jobs_in_queue()) >= max_jobs:
            print(
                f"Waiting: Your number of jobs running ({len(slurm_jobs_in_queue())}) "
                "matches or exceeds the maximum "
                f"concurrent jobs allowed ({max_jobs}). "
                f"The jobs in queue are: {slurm_jobs_in_queue()}"
            )
            time.sleep(5)

        shell_script = os.path.join(
            pathlib.Path(__file__).parent.resolve(), "run_slurm.sh"
        )

        slurm_command = ["sbatch", "-c", "1"]
        if os.environ.get("TURNKEY_SLURM_USE_DEFAULT_MEMORY") != "True":
            slurm_command.append("--mem=128000")
        slurm_command.extend(
            [
                f"--time={slurm_format_timeout}",
                f"--job-name={job_name}",
                shell_script,
                "turnkey",
                slurm_args,
                working_dir,
            ]
        )
        if ml_cache_dir is not None:
            slurm_command.append(ml_cache_dir)

        print(f"Submitting job {job_name} to Slurm")
        subprocess.check_call(slurm_command)
    else:  # process isolation
        command = "turnkey " + invocation_args
        printing.log_info(f"Starting process with command: {command}")

        # Linux and Windows want to handle some details differently
        if platform.system() != "Windows":
            command = shlex.split(command)

        # Launch a subprocess for turnkey to evaluate the script
        try:
            process_output = []
            with subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
            ) as p:
                # Create our own watchdog timer in a thread
                # This is needed because the `for line in p.stdout` is a blocking
                # call that is incompatible with Popen's native timeout features
                watchdog = WatchdogTimer(timeout, p.pid)
                watchdog.start()

                # Print the subprocess's output to the command line as it comes in,
                # while also storing it in a variable so that we can analyze it
                # in the event that the subprocess is killed
                for line in p.stdout:
                    print(line, end="")
                    process_output.append(line)

                p.wait()
                watchdog.cancel()

                # If the subprocess was killed, raise an exception that provides more
                # detail about why it was killed
                if watchdog.timeout_reached:
                    evaluation_status = build.FunctionStatus.TIMEOUT
                    raise subprocess.TimeoutExpired(p.args, timeout)

                if p.returncode != 0:
                    evaluation_status = build.FunctionStatus.KILLED
                    raise subprocess.CalledProcessError(p.returncode, p.args)

        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            # If the subprocess was killed, we will attempt to figure out what happened,
            # provide helpful error messages and information, and clean up the
            # turnkey cache as much as possible.

            # Print some newlines so that our error messages don't end up in the
            # middle of the status monitor
            print("\n\n")
            printing.log_error(
                "The turnkey subprocess was terminated with the error shown below. "
                f"turnkey will move on to the next input.\n\n{e}"
            )

            # Perform fault handling
            printing.log_info(
                f"Detected failed build {build_name}. "
                "The parent process will attempt to clean up."
            )

            # Cleaning the cache is the last step in evaluation
            # If a "lean cache" evaluation was killed, it is safe to assume we still
            # need to clean the cache
            # It is also harmless to run clean_output_dir() again even if the subprocess
            # did have a chance to run it before the subprocess was killed
            if "--lean-cache" in command:
                printing.log_info("Removing build artifacts...")
                filesystem.clean_output_dir(cache_dir, build_name)

            # Perform fault handling within the stats file if it exists
            if os.path.isfile(filesystem.stats_file(cache_dir, build_name)):
                try:
                    # Amend the stats with a specific function status if possible
                    if isinstance(e, subprocess.TimeoutExpired):
                        evaluation_status = build.FunctionStatus.TIMEOUT
                    else:
                        evaluation_status = build.FunctionStatus.KILLED

                    stats = filesystem.Stats(
                        cache_dir,
                        build_name,
                    )

                    for key in stats.stats.keys():
                        if stats.stats[key] == build.FunctionStatus.INCOMPLETE:
                            stats.save_stat(key, evaluation_status)

                    # Save the exception into the error log stat
                    stats.save_stat(filesystem.Keys.ERROR_LOG, str(e))

                except Exception as stats_exception:  # pylint: disable=broad-except
                    printing.log_info(
                        "Stats file found, but unable to perform cleanup due to "
                        f"exception: {stats_exception}"
                    )
