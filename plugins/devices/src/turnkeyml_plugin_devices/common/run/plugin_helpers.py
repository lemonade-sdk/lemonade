import subprocess
import logging
import os
import sys
from typing import List, Optional, Dict

TIMEOUT = 900


class SubprocessError(Exception):
    pass


class CondaError(Exception):
    """
    Triggered when execution within the Conda environment goes wrong
    """


def run_subprocess(cmd):
    """Run a subprocess with the given command and log the output."""
    if isinstance(cmd, str):
        cmd_str = cmd
        shell_flag = True
    else:
        cmd_str = " ".join(cmd)
        shell_flag = False

    try:
        result = subprocess.run(
            cmd,
            check=True,
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
            shell=shell_flag,
        )
        return result
    except subprocess.TimeoutExpired:
        logging.error(f"{cmd_str} timed out after {TIMEOUT} seconds")
        raise SubprocessError("TimeoutExpired")
    except subprocess.CalledProcessError as e:
        logging.error(
            f"Subprocess failed with command: {cmd_str} and error message: {e.stderr}"
        )
        raise SubprocessError("CalledProcessError")
    except (OSError, ValueError) as e:
        logging.error(
            f"Subprocess failed with command: {cmd_str} and error message: {str(e)}"
        )
        raise SubprocessError(str(e))


def get_python_path(conda_env_name):
    try:
        conda_path = os.getenv("CONDA_EXE")
        cmd = [
            conda_path,
            "run",
            "--name",
            conda_env_name,
            "python",
            "-c",
            "import sys; print(sys.executable)",
        ]
        result = subprocess.run(
            cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        python_path = result.stdout.decode().strip()

        return python_path
    except subprocess.CalledProcessError as e:
        raise EnvironmentError(
            f"An error occurred while getting Python path for {conda_env_name} environment"
            f"{e.stderr.decode()}"
        )


def logged_subprocess(
    cmd: List[str],
    cwd: str = os.getcwd(),
    env: Optional[Dict] = None,
    log_file_path: Optional[str] = None,
    log_to_std_streams: bool = True,
    log_to_file: bool = True,
) -> None:
    """
    This function calls a subprocess and sends the logs to either a file, stdout/stderr, or both.

    cmd             Command that will run o a sbprocess
    cwd             Working directory from where the subprocess should run
    env             Evironment to be used by the subprocess (useful for passing env vars)
    log_file_path   Where logs will be stored
    log_to_file     Whether or not to store the subprocess's stdout/stderr into a file
    log_to_std      Whether or not to print subprocess's stdout/stderr to the screen
    """
    if env is None:
        env = os.environ.copy()
    if log_to_file and log_file_path is None:
        raise ValueError("log_file_path must be set when log_to_file is True")

    log_stdout = ""
    log_stderr = ""
    try:
        proc = subprocess.run(
            cmd,
            check=True,
            env=env,
            capture_output=True,
            cwd=cwd,
        )
    except Exception as e:  # pylint: disable=broad-except
        log_stdout = e.stdout.decode(  # pylint: disable=no-member
            "utf-8", errors="replace"
        )
        log_stderr = e.stderr.decode(  # pylint: disable=no-member
            "utf-8", errors="replace"
        )
        raise CondaError(
            f"Exception {e} encountered, \n\nstdout was: "
            f"\n{log_stdout}\n\n and stderr was: \n{log_stderr}"
        )
    else:
        log_stdout = proc.stdout.decode("utf-8", errors="replace")
        log_stderr = proc.stderr.decode("utf-8", errors="replace")
    finally:
        if log_to_std_streams:
            # Print log to stdout
            # This might be useful when this subprocess is being logged externally
            print(log_stdout, file=sys.stdout)
            print(log_stderr, file=sys.stdout)
        if log_to_file:
            log = f"{log_stdout}\n{log_stderr}"
            with open(
                log_file_path,
                "w",
                encoding="utf-8",
            ) as f:
                f.write(log)
