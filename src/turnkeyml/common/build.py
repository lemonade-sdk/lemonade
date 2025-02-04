import os
import logging
import sys
import traceback
from typing import Dict, Union
import hashlib
import psutil
import yaml
import torch
import numpy as np
import turnkeyml.common.exceptions as exp

UnionValidModelInstanceTypes = Union[
    None,
    str,
    torch.nn.Module,
    torch.jit.ScriptModule,
]

if os.environ.get("TURNKEY_ONNX_OPSET"):
    DEFAULT_ONNX_OPSET = int(os.environ.get("TURNKEY_ONNX_OPSET"))
else:
    DEFAULT_ONNX_OPSET = 14

MINIMUM_ONNX_OPSET = 11

DEFAULT_REBUILD_POLICY = "if_needed"
REBUILD_OPTIONS = ["if_needed", "always", "never"]


def load_yaml(file_path) -> Dict:
    with open(file_path, "r", encoding="utf8") as stream:
        try:
            return yaml.load(stream, Loader=yaml.FullLoader)
        except yaml.YAMLError as e:
            raise exp.IOError(
                f"Failed while trying to open {file_path}."
                f"The exception that triggered this was:\n{e}"
            )


def builds_dir(cache_dir):
    """
    Each build stores stats, logs, and other files in a build directory.
    All build directories are located at:
        <cache_dir>/builds
    """
    return os.path.join(cache_dir, "builds")


def output_dir(cache_dir, build_name):
    """
    Each build stores stats, logs, and other files in an output directory at:
    All build directories are located at:
        <builds_dir>/<build_name>
    """
    path = os.path.join(builds_dir(cache_dir), build_name)
    return path


def state_file(cache_dir, build_name):
    state_file_name = f"{build_name}_state.yaml"
    path = os.path.join(output_dir(cache_dir, build_name), state_file_name)
    return path


def hash_model(model, hash_params: bool = True):
    # If the model is a path to a file, hash the file
    if isinstance(model, str):
        if model.endswith(".onnx"):
            # TODO: Implement a way of hashing the models but not the parameters
            # of ONNX inputs.
            if not hash_params:
                msg = "hash_params must be True for ONNX files"
                raise ValueError(msg)
            if os.path.isfile(model):
                with open(model, "rb") as f:
                    file_content = f.read()
                return hashlib.sha256(file_content).hexdigest()
            else:
                raise ValueError(
                    "hash_model received str model that doesn't correspond to a file"
                )
        else:
            with open(model, "rb") as f:
                file_content = f.read()
            return hashlib.sha256(file_content).hexdigest()

    if isinstance(model, (torch.nn.Module, torch.jit.ScriptModule)):
        # Convert model parameters and topology to string
        hashable_params = {}
        for name, param in model.named_parameters():
            hashable_params[name] = param.data
        if hash_params:
            hashable_model = (str(model) + str(hashable_params)).encode()
        else:
            hashable_model = str(model).encode()

        # Return hash of topology and parameters
        return hashlib.sha256(hashable_model).hexdigest()

    else:
        msg = f"""
        model type "{type(model)}" unsupported by this hash_model function
        """
        raise ValueError(msg)


class FunctionStatus:
    """
    Status values that are assigned to tools, builds, benchmarks, and other
    functionality to help the user understand whether that function completed
    successfully or not.
    """

    # SUCCESSFUL means the tool/build/benchmark completed successfully.
    SUCCESSFUL = "successful"

    # ERROR means the tool/build/benchmark failed and threw some error that
    # was caught by turnkey. You should proceed by looking at the build
    # logs to see what happened.

    ERROR = "error"

    # TIMEOUT means the tool/build/benchmark failed because it exceeded the timeout
    # set for the turnkey command.
    TIMEOUT = "timeout"

    # KILLED means the build/benchmark failed because the system killed it. This can
    # happen because of an out-of-memory (OOM), system shutdown, etc.
    # You should proceed by re-running the build and keeping an eye on it to observe
    # why it is being killed (e.g., watch the RAM utilization to diagnose an OOM).
    KILLED = "killed"

    # The NOT_STARTED status is applied to all tools/builds/benchmarks at startup.
    # It will be replaced by one of the other status values if the tool/build/benchmark
    # has a chance to start running.
    # A value of NOT_STARTED in the report CSV indicates that the tool/build/benchmark
    # never had a chance to start because turnkey exited before that functionality had
    # a chance to start running.
    NOT_STARTED = "not_started"

    # INCOMPLETE indicates that a tool/build/benchmark started running and did not complete.
    # Each tool, build, and benchmark are marked as INCOMPLETE when they start running.
    # If you open the turnkey_stats.yaml file while the tool/build/benchmark
    # is still running, the status will show as INCOMPLETE. If the tool/build/benchmark
    # is killed without the chance to do any stats cleanup, the status will continue to
    # show as INCOMPLETE in turnkey_stats.yaml.
    # When the report CSV is created, any instance of an INCOMPLETE tool/build/benchmark
    # status will be replaced by KILLED.
    INCOMPLETE = "incomplete"


# Create a unique ID from this run by hashing pid + process start time
def unique_id():
    pid = os.getpid()
    p = psutil.Process(pid)
    start_time = p.create_time()
    return hashlib.sha256(f"{pid}{start_time}".encode()).hexdigest()


def get_shapes_and_dtypes(inputs: dict):
    """
    Return the shape and data type of each value in the inputs dict
    """
    shapes = {}
    dtypes = {}
    for key in sorted(inputs):
        value = inputs[key]
        if isinstance(
            value,
            (list, tuple),
        ):
            for v, i in zip(value, range(len(value))):
                if isinstance(v, (list, tuple)):
                    # Handle nested lists/tuples, for example past_key_values
                    # in an LLM that has KV-caching enabled
                    for v2, i2 in zip(v, range(len(v))):
                        subsubkey = f"{key}[{i}][{i2}]"
                        shapes[subsubkey] = np.array(v2).shape
                        dtypes[subsubkey] = np.array(v2).dtype.name
                else:
                    # Handle single list/tuple
                    subkey = f"{key}[{i}]"
                    shapes[subkey] = np.array(v).shape
                    dtypes[subkey] = np.array(v).dtype.name
        elif torch.is_tensor(value):
            shapes[key] = np.array(value.detach()).shape
            dtypes[key] = np.array(value.detach()).dtype.name
        elif isinstance(value, np.ndarray):
            shapes[key] = value.shape
            dtypes[key] = value.dtype.name
        elif isinstance(value, (bool, int, float)):
            shapes[key] = (1,)
            dtypes[key] = type(value).__name__
        elif value is None:
            pass
        else:
            raise exp.Error(
                "One of the provided inputs contains the unsupported "
                f' type {type(value)} at key "{key}".'
            )

    return shapes, dtypes


class Logger:
    """
    Redirects stdout to file (and console if needed)
    """

    def __init__(
        self,
        initial_message: str,
        log_path: str = None,
    ):
        self.debug = os.environ.get("TURNKEY_BUILD_DEBUG") == "True"
        self.terminal = sys.stdout
        self.terminal_err = sys.stderr
        self.log_path = log_path

        # Create the empty logfile
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(f"{initial_message}\n")

        # Disable any existing loggers so that we can capture all
        # outputs to a logfile
        self.root_logger = logging.getLogger()
        self.handlers = [handler for handler in self.root_logger.handlers]
        for handler in self.handlers:
            self.root_logger.removeHandler(handler)

        # Send any logger outputs to the logfile
        if not self.debug:
            self.file_handler = logging.FileHandler(filename=log_path)
            self.file_handler.setLevel(logging.INFO)
            formatter = logging.Formatter(
                "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
            )
            self.file_handler.setFormatter(formatter)
            self.root_logger.addHandler(self.file_handler)

    def __enter__(self):
        sys.stdout = self
        sys.stderr = self

    def __exit__(self, _exc_type, _exc_value, _exc_tb):
        # Ensure we also capture the traceback as part of the logger when exceptions happen
        if _exc_type:
            traceback.print_exception(_exc_type, _exc_value, _exc_tb)

        # Stop redirecting stdout/stderr
        sys.stdout = self.terminal
        sys.stderr = self.terminal_err

        # Remove the logfile logging handler
        if not self.debug:
            self.file_handler.close()
            self.root_logger.removeHandler(self.file_handler)

            # Restore any pre-existing loggers
            for handler in self.handlers:
                self.root_logger.addHandler(handler)

    def write(self, message):
        if self.log_path is not None:
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(message)
        if self.debug or self.log_path is None:
            self.terminal.write(message)
            self.terminal.flush()
            self.terminal_err.write(message)
            self.terminal_err.flush()

    def flush(self):
        # needed for python 3 compatibility.
        pass
