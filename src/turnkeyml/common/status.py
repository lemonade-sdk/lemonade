import os
import platform
import shutil
import sys
import math
import dataclasses
from typing import Callable, List, Union, Dict, Optional
import textwrap
import psutil
import torch
from turnkeyml.common import printing
from turnkeyml.state import State
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
import turnkeyml.common.analyze_model as analyze_model


def _pretty_print_key(key: str) -> str:
    result = key.split("_")
    result = [word.capitalize() for word in result]
    result = " ".join(result)
    return result


class PrettyFloat(float):
    def __repr__(self):
        return f"{self:0.3f}"


def parameters_to_size(parameters: int, byte_per_parameter: int = 4) -> str:
    size_bytes = parameters * byte_per_parameter
    if size_bytes == 0:
        return "0B"
    size_name = ("B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB")
    i = int(math.floor(math.log(size_bytes, 1024)))
    p = math.pow(1024, i)
    s = round(size_bytes / p, 2)
    return "%s %s" % (s, size_name[i])


@dataclasses.dataclass
class BasicInfo:
    name: str
    script_name: str
    file: str = ""
    line: int = 0
    params: int = 0
    depth: int = 0
    parent_hash: Union[str, None] = None
    model_class: type = None
    # This is the "model hash", not to be confused with the
    # "invocation hash"
    hash: Union[str, None] = None


@dataclasses.dataclass
class SkipFields:
    """
    Keep track of which fields of a model's status should be skipped
    during printout. There are two use cases in mind:
    - For incremental printout: fields that have already been printed.
    - For low-verbosity: fields that should never be printed.
    """

    file_name: bool = False
    model_name: bool = False
    parameters: bool = False
    location: bool = False
    input_shape: bool = False
    build_dir: bool = False
    unique_input_shape: bool = False
    previous_status_message: Optional[str] = None


@dataclasses.dataclass
class UniqueInvocationInfo(BasicInfo):
    """
    Refers to unique static model invocations
    (i.e. models executed with unique input shapes)
    """

    invocation_hash: Union[str, None] = None
    traceback: List[str] = None
    inputs: Union[dict, None] = None
    input_shapes: Union[dict, None] = None
    executed: int = 0
    exec_time: float = 0.0
    status_message: str = ""
    extra_status: Optional[str] = ""
    is_target: bool = False
    auto_selected: bool = False
    status_message_color: printing.Colors = printing.Colors.ENDC
    traceback_message_color: printing.Colors = printing.Colors.FAIL
    stats_keys: List[str] = dataclasses.field(default_factory=list)
    forward_function_pointer: callable = None
    original_forward_function: callable = None
    # Fields specific to printing status
    skip: SkipFields = None
    extension: str = None
    indent: str = None

    def __post_init__(self):
        self.skip = SkipFields()

    def _print_heading(
        self,
        exec_time_formatted: str,
        print_file_name: bool,
        model_visited: bool,
        multiple_unique_invocations: bool,
    ):
        if self.skip.file_name or self.skip.model_name:
            return

        if print_file_name:
            print(f"{self.script_name}{self.extension}:")

        # Print invocation about the model (only applies to scripts, not ONNX files or
        # LLMs, which have no extension)
        if not (
            self.extension == ".onnx"
            or self.extension == "_state.yaml"
            or self.extension == ""
        ):
            if self.depth == 0 and multiple_unique_invocations:
                if not model_visited:
                    printing.logn(f"{self.indent}{self.name}")
            else:
                printing.log(f"{self.indent}{self.name}")
                printing.logn(
                    f" (executed {self.executed}x{exec_time_formatted})",
                    c=printing.Colors.OKGREEN,
                )

        self.skip.file_name = True
        self.skip.model_name = True

    def _print_location(self):
        if self.skip.location or self.file == "":
            return

        if self.depth == 0:
            print(f"{self.indent}\tLocation:\t{self.file}", end="")
            if self.extension == ".onnx":
                print()
            else:
                print(f", line {self.line}")
            self.skip.location = True

    def _print_parameters(self):
        if self.skip.parameters or self.params is None:
            return

        # Display number of parameters and size
        parameters_size = parameters_to_size(self.params)
        print(
            f"{self.indent}\tParameters:\t{'{:,}'.format(self.params)} ({parameters_size})"
        )

        self.skip.parameters = True

    def _print_unique_input_shape(
        self,
        exec_time_formatted: str,
        invocation_idx: int,
        multiple_unique_invocations: bool,
    ):
        if self.skip.unique_input_shape:
            return

        if self.depth == 0 and multiple_unique_invocations:
            printing.logn(
                f"\n{self.indent}\tWith input shape {invocation_idx+1} "
                f"(executed {self.executed}x{exec_time_formatted})",
                c=printing.Colors.OKGREEN,
            )

        self.skip.unique_input_shape = True

    def _print_input_shape(self):
        if self.skip.input_shape or self.input_shapes is None:
            return

        # Prepare input shape to be printed
        input_shape = dict(self.input_shapes)
        input_shape = {key: value for key, value in input_shape.items() if value != ()}
        input_shape = str(input_shape).replace("{", "").replace("}", "")

        print(f"{self.indent}\tInput Shape:\t{input_shape}")

        self.skip.input_shape = True

    def _print_build_dir(self, cache_dir: str, build_name: str):
        if self.skip.build_dir or not self.is_target:
            return

        print(f"{self.indent}\tBuild dir:\t{build.output_dir(cache_dir, build_name)}")

        self.skip.build_dir = True

    def _print_peak_memory(self):
        if platform.system() == "Windows":
            print(
                f"{self.indent}\tPeak memory:\t"
                f"{psutil.Process().memory_info().peak_wset / 1024**3:,.3f} GB"
            )

    def _print_status(self, cache_dir: str, build_name: str):
        stats = fs.Stats(cache_dir, build_name)
        if self.skip.previous_status_message:
            if self.skip.previous_status_message == self.status_message:
                # This is a special case for skipping: we only want to skip
                # printing the outcome if we have already printed that
                # exact message already.
                return
            else:
                # Print some whitespace to help the status stand out
                print()

        printing.log(f"{self.indent}\tStatus:\t\t")
        printing.logn(
            f"{self.status_message}",
            c=self.status_message_color,
        )
        if self.is_target:

            # Get the maximum key length to figure out the number
            # of tabs needed to align the values
            max_key_len = 0
            for key in self.stats_keys:
                max_key_len = max(len(_pretty_print_key(key)), max_key_len)

            screen_width = shutil.get_terminal_size().columns
            wrap_screen_width = screen_width - 2

            for key in self.stats_keys:
                nice_key = _pretty_print_key(key)
                try:
                    value = stats.stats[key]
                    if isinstance(value, float):
                        value = PrettyFloat(value)
                    elif isinstance(value, list):
                        value = [
                            PrettyFloat(v) if isinstance(v, float) else v for v in value
                        ]
                    # Tools may provide a unit of measurement for their status
                    # stats, whose key name should follow the format
                    # "STATUS_STATS_KEY_units"
                    units_key = key + "_units"
                    units = stats.stats.get(units_key)
                    units = units if units is not None else ""
                    if self.extension == "":
                        value_tabs = " " * (
                            (max_key_len - len(_pretty_print_key(key))) + 1
                        )
                        hanging_indent = (
                            len(self.indent) + 8 + len(nice_key) + 1 + len(value_tabs)
                        )
                        hanging_indent_str = " " * hanging_indent
                        if (
                            isinstance(value, list)
                            and len(value) > 0
                            and all(isinstance(item, str) for item in value)
                        ):
                            # Value is a list of strings, so output each one starting
                            # on its own line
                            printing.logn(f"{self.indent}\t{nice_key}:{value_tabs}[")
                            for line_counter, text in enumerate(value):
                                lines = textwrap.wrap(
                                    "'" + text + "'",
                                    width=wrap_screen_width,
                                    initial_indent=hanging_indent_str,
                                    subsequent_indent=hanging_indent_str,
                                )
                                if line_counter + 1 < len(value):
                                    # Not the last text item in the list, so add a comma
                                    lines[-1] = lines[-1] + ","
                                for line in lines:
                                    printing.logn(line)
                            printing.logn(f"{' ' * hanging_indent}] {units}")
                        else:
                            # Wrap value as needed
                            status_str = (
                                f"{self.indent}\t{nice_key}:{value_tabs}{value} {units}"
                            )
                            lines = textwrap.wrap(
                                status_str,
                                width=wrap_screen_width,
                                subsequent_indent=hanging_indent_str,
                            )
                            for line in lines:
                                printing.logn(line)
                    else:
                        printing.logn(
                            f"{self.indent}\t\t\t{nice_key}:\t{value} {units}"
                        )
                except KeyError:
                    # Ignore any keys that are missing because that means the
                    # evaluation did not produce them
                    pass

            if self.traceback is not None:
                if os.environ.get("TURNKEY_TRACEBACK") != "False":
                    for line in self.traceback:
                        for subline in line.split("\n")[:-1]:
                            print(f"{self.indent}\t{subline}")

                else:
                    printing.logn(
                        f"{self.indent}\t\t\tTo see the full stack trace, "
                        "rerun with `export TURNKEY_TRACEBACK=True`.\n",
                        c=self.status_message_color,
                    )
            else:
                print()

        self.skip.previous_status_message = self.status_message

    def print(
        self,
        build_name: str,
        cache_dir: str,
        print_file_name: bool = False,
        invocation_idx: int = 0,
        model_visited: bool = False,
        multiple_unique_invocations: bool = False,
    ):
        """
        Print information about a given model or submodel.
        """

        if self.extension == ".onnx" or self.extension == "":
            self.indent = "\t" * (2 * self.depth)
        else:
            self.indent = "\t" * (2 * self.depth + 1)

        if self.exec_time == 0:
            exec_time_formatted = ""
        else:
            exec_time_formatted = f" - {self.exec_time:.2f}s"

        self._print_heading(
            exec_time_formatted,
            print_file_name,
            model_visited,
            multiple_unique_invocations,
        )
        if (self.depth == 0 and not model_visited) or (self.depth != 0):
            # Print this information only once per model
            self._print_location()
            self._print_parameters()
        self._print_unique_input_shape(
            exec_time_formatted, invocation_idx, multiple_unique_invocations
        )
        self._print_input_shape()
        self._print_build_dir(cache_dir=cache_dir, build_name=build_name)
        self._print_peak_memory()
        self._print_status(cache_dir=cache_dir, build_name=build_name)

        print()


@dataclasses.dataclass
class ModelInfo(BasicInfo):
    model: torch.nn.Module = None
    old_forward: Union[Callable, None] = None
    unique_invocations: Union[Dict[str, UniqueInvocationInfo], None] = (
        dataclasses.field(default_factory=dict)
    )
    last_unique_invocation_executed: Union[str, None] = None

    def __post_init__(self):
        self.params = analyze_model.count_parameters(self.model)


def recursive_print(
    models_found: Dict[str, ModelInfo],
    build_name: str,
    cache_dir: str,
    parent_model_hash: Union[str, None] = None,
    parent_invocation_hash: Union[str, None] = None,
    script_names_visited: List[str] = False,
) -> None:
    script_names_visited = []

    for model_hash in models_found.keys():
        model_visited = False
        model_info = models_found[model_hash]
        invocation_idx = 0
        for invocation_hash in model_info.unique_invocations.keys():
            unique_invocation = model_info.unique_invocations[invocation_hash]

            if (
                parent_model_hash == model_info.parent_hash
                and unique_invocation.executed > 0
                and (
                    model_info.unique_invocations[invocation_hash].parent_hash
                    == parent_invocation_hash
                )
            ):
                print_file_name = False
                if model_info.script_name not in script_names_visited:
                    script_names_visited.append(model_info.script_name)
                    if model_info.depth == 0:
                        print_file_name = True

                # In this verbosity mode we want to print all of the information
                # every time, so reset SkipFields
                # NOTE: to introduce a new lower-verbosity mode, set some members
                # of SkipFields to False to skip them
                unique_invocation.skip = SkipFields()

                unique_invocation.print(
                    build_name=build_name,
                    cache_dir=cache_dir,
                    print_file_name=print_file_name,
                    invocation_idx=invocation_idx,
                    model_visited=model_visited,
                    multiple_unique_invocations=len(model_info.unique_invocations) > 1,
                )
                model_visited = True
                invocation_idx += 1

                if print_file_name:
                    script_names_visited.append(model_info.script_name)

                recursive_print(
                    models_found,
                    build_name,
                    cache_dir,
                    parent_model_hash=model_hash,
                    parent_invocation_hash=invocation_hash,
                    script_names_visited=script_names_visited,
                )


def stop_logger_forward() -> None:
    """
    Stop forwarding stdout and stderr to file
    """
    if hasattr(sys.stdout, "terminal"):
        sys.stdout = sys.stdout.terminal
    if hasattr(sys.stderr, "terminal_err"):
        sys.stderr = sys.stderr.terminal_err


def add_to_state(
    state: State,
    name: str,
    model: Union[str, torch.nn.Module],
    extension: str = "",
    input_shapes: Optional[Dict] = None,
):
    if vars(state).get("model_hash"):
        model_hash = state.model_hash
    else:
        model_hash = 0

    if os.path.exists(name):
        file_name = fs.clean_file_name(name)
        file = name
    else:
        file_name = name
        file = ""

    state.invocation_info = UniqueInvocationInfo(
        name=input,
        script_name=file_name,
        file=file,
        input_shapes=input_shapes,
        hash=model_hash,
        is_target=True,
        extension=extension,
        executed=1,
    )
    state.models_found = {
        "the_model": ModelInfo(
            model=model,
            name=input,
            script_name=input,
            file=input,
            unique_invocations={model_hash: state.invocation_info},
            hash=model_hash,
        )
    }
    state.invocation_info.params = state.models_found["the_model"].params
