import abc
import sys
import time
import os
import argparse
import textwrap as _textwrap
import re
from typing import Tuple, Dict
from multiprocessing import Process, Queue
import psutil
import turnkeyml.common.printing as printing
import turnkeyml.common.exceptions as exp
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
from turnkeyml.state import State


def _spinner(message, q: Queue):
    """
    Displays a moving "..." indicator so that the user knows that the
    Tool is still working. Tools can optionally use a multiprocessing
    Queue to display the percent progress of the Tool.
    """
    percent_complete = None
    # Get sleep time from environment variable, default to 0.5s if not set
    try:
        sleep_time = float(os.getenv("TURNKEY_BUILD_MONITOR_FREQUENCY", "0.5"))
    except ValueError:
        sleep_time = 0.5

    try:
        parent_process = psutil.Process(pid=os.getppid())
        while parent_process.status() == psutil.STATUS_RUNNING:
            for cursor in ["   ", ".  ", ".. ", "..."]:
                time.sleep(sleep_time)
                while not q.empty():
                    percent_complete = q.get()
                if percent_complete is not None:
                    status = f"      {message} ({percent_complete:.1f}%){cursor}\r"
                else:
                    status = f"      {message}{cursor}         \r"
                sys.stdout.write(status)
                sys.stdout.flush()
    except psutil.NoSuchProcess:
        # If the parent process stopped existing, we can
        # safely assume the spinner no longer needs to spin
        # NOTE: this only seems to be needed on Windows
        pass


def _name_is_file_safe(name: str):
    """
    Make sure the name can be used in a filename
    """

    allowed_in_unique_name = set(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
    )

    if len(name) == 0:
        msg = """
        Tool __init__() was passed a unique_name with no length. A
        uniquely identifying unique_name is required.
        """
        raise ValueError(msg)

    for char in name:
        if char not in allowed_in_unique_name:
            msg = f"""
            Tool __init__() was passed a unique_name:
            {name}
            with illegal characters. The unique_name must be safe to
            use in a filename, meaning it can only use characters: {allowed_in_unique_name}
            """
            raise ValueError(msg)


class NiceHelpFormatter(argparse.RawDescriptionHelpFormatter):
    def __add_whitespace(self, idx, amount, text):
        if idx == 0:
            return text
        return (" " * amount) + text

    def _split_lines(self, text, width):
        textRows = text.splitlines()
        for idx, line in enumerate(textRows):
            search = re.search(r"\s*[0-9\-]{0,}\.?\s*", line)
            if line.strip() == "":
                textRows[idx] = " "
            elif search:
                whitespace_needed = search.end()
                lines = [
                    self.__add_whitespace(i, whitespace_needed, x)
                    for i, x in enumerate(_textwrap.wrap(line, width))
                ]
                textRows[idx] = lines

        return [item for sublist in textRows for item in sublist]


class ToolParser(argparse.ArgumentParser):

    def error(self, message):
        if message.startswith("unrecognized arguments"):
            unrecognized = message.split(": ")[1]
            if not unrecognized.startswith("-"):
                # This was probably a misspelled tool name
                message = message + (
                    f". If `{unrecognized}` was intended to invoke "
                    "a tool, please run `turnkey -h` and check the spelling and "
                    "availability of that tool."
                )
        self.print_usage()
        printing.log_error(message)
        self.exit(2)

    def __init__(
        self, short_description: str, description: str, prog: str, epilog: str, **kwargs
    ):
        super().__init__(
            description=description,
            prog=prog,
            epilog=epilog,
            formatter_class=NiceHelpFormatter,
            **kwargs,
        )

        self.short_description = short_description


class Tool(abc.ABC):

    unique_name: str

    @classmethod
    def helpful_parser(cls, short_description: str, **kwargs):
        epilog = (
            f"`{cls.unique_name}` is a Tool. It is intended to be invoked as "
            "part of a sequence of Tools, for example: `turnkey -i INPUTS tool-one "
            "tool-two tool-three`. Tools communicate data to each other via State. "
            "You can learn more at "
            "https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/tools_user_guide.md"
        )

        return ToolParser(
            prog=f"turnkey {cls.unique_name}",
            short_description=short_description,
            description=cls.__doc__,
            epilog=epilog,
            **kwargs,
        )

    def status_line(self, successful, verbosity):
        """
        Print a line of status information for this Tool into the monitor.
        """
        if verbosity:
            # Only use special characters when the terminal encoding supports it
            if sys.stdout.encoding == "utf-8":
                success_tick = "✓"
                fail_tick = "×"
            else:
                success_tick = "+"
                fail_tick = "x"

            if self.percent_progress is None:
                progress_indicator = ""
            else:
                progress_indicator = f" ({self.percent_progress:.1f}%)"

            if successful is None:
                # Initialize the message
                printing.logn(f"      {self.monitor_message}   ")
            elif successful:
                # Print success message
                printing.log(f"    {success_tick} ", c=printing.Colors.OKGREEN)
                printing.logn(
                    self.monitor_message + progress_indicator + "            "
                )
            else:
                # successful == False, print failure message
                printing.log(f"    {fail_tick} ", c=printing.Colors.FAIL)
                printing.logn(
                    self.monitor_message + progress_indicator + "            "
                )

    def __init__(
        self,
        monitor_message,
        enable_logger=True,
    ):
        _name_is_file_safe(self.__class__.unique_name)

        self.status_key = f"{fs.Keys.TOOL_STATUS}:{self.__class__.unique_name}"
        self.duration_key = f"{fs.Keys.TOOL_DURATION}:{self.__class__.unique_name}"
        self.memory_key = f"{fs.Keys.TOOL_MEMORY}:{self.__class__.unique_name}"
        self.monitor_message = monitor_message
        self.progress = None
        self.progress_queue = None
        self.percent_progress = None
        self.logfile_path = None
        # Tools can disable build.Logger, which captures all stdout and stderr from
        # the Tool, by setting enable_logger=False
        self.enable_logger = enable_logger
        # Tools can provide a list of keys that can be found in
        # evaluation stats. Those key:value pairs will be presented
        # in the status at the end of the build.
        self.status_stats = []

    @abc.abstractmethod
    def run(self, state: State) -> State:
        """
        Execute the functionality of the Tool by acting on the state.
        """

    @staticmethod
    @abc.abstractmethod
    def parser() -> argparse.ArgumentParser:
        """
        Static method that returns an ArgumentParser that defines the command
        line interface for this Tool.
        """

    def set_percent_progress(self, percent_progress: float):
        """
        Update the progress monitor with a percent progress to let the user
        know how much progress the Tool has made.
        """

        if percent_progress is not None and not isinstance(percent_progress, float):
            raise ValueError(
                f"Input argument must be a float or None, got {percent_progress}"
            )

        if self.progress_queue:
            self.progress_queue.put(percent_progress)
        self.percent_progress = percent_progress

    # pylint: disable=unused-argument
    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        """
        Run the parser and return a Namespace of keyword arguments that the user
        passed to the Tool via the command line.

        Tools should extend this function only if they require specific parsing
        logic, for example decoding the name of a data type into a data type class.

        Args:
            state: the same state passed into the run method of the Tool, useful if
                the parse decoding logic needs to take the state into account.
            args: command line arguments passed from the CLI.
            known_only: this argument allows the CLI framework to
                incrementally parse complex commands.
        """

        if known_only:
            parsed_args = self.__class__.parser().parse_args(args)
        else:
            parsed_args, _ = self.__class__.parser().parse_known_args(args)

        return parsed_args

    def parse_and_run(
        self,
        state: State,
        args,
        monitor: bool = False,
        known_only=True,
    ) -> Dict:
        """
        Helper function to parse CLI arguments into the args expected
        by run(), and then forward them into the run() method.
        """

        parsed_args = self.parse(state, args, known_only)
        return self.run_helper(state, monitor, **parsed_args.__dict__)

    def run_helper(
        self, state: State, monitor: bool = False, **kwargs
    ) -> Tuple[State, int]:
        """
        Wraps the developer-defined .run() method with helper functionality.
        Specifically:
            - Provides a path to a log file
            - Redirects the stdout of the tool to that log file
            - Monitors the progress of the tool on the command line,
                including in the event of an exception
        """

        # Set the build status to INCOMPLETE to indicate that a Tool
        # started running. This allows us to test whether the Tool exited
        # unexpectedly, before it was able to set ERROR
        state.build_status = build.FunctionStatus.INCOMPLETE

        self.logfile_path = os.path.join(
            build.output_dir(state.cache_dir, state.build_name),
            f"log_{self.unique_name}.txt",
        )

        if monitor:
            self.progress_queue = Queue()
            self.progress = Process(
                target=_spinner, args=(self.monitor_message, self.progress_queue)
            )
            self.progress.start()

        try:
            # Execute the build tool

            if self.enable_logger:
                with build.Logger(self.monitor_message, self.logfile_path):
                    state = self.run(state, **kwargs)
            else:
                state = self.run(state, **kwargs)

        except Exception:  # pylint: disable=broad-except
            self.status_line(
                successful=False,
                verbosity=monitor,
            )
            state.build_status = build.FunctionStatus.ERROR
            raise

        else:
            self.status_line(successful=True, verbosity=monitor)

            # Tools should not set build.FunctionStatus.SUCCESSFUL for the whole build,
            # as that is reserved for Sequence.launch()
            if state.build_status == build.FunctionStatus.SUCCESSFUL:
                raise exp.ToolError(
                    "TurnkeyML Tools are not allowed to set "
                    "`state.build_status == build.FunctionStatus.SUCCESSFUL`, "
                    "however that has happened. If you are a plugin developer, "
                    "do not do this. If you are a user, please file an issue at "
                    "https://github.com/onnx/turnkeyml/issues."
                )

        finally:
            if monitor:
                self.progress.terminate()

        return state


class FirstTool(Tool):
    """
    Provides extra features for Tools that are meant to be the first Tool
    in the sequence.

    Specifically:
        - FirstTools should not have any expectations of State.result, since
            they populate State with an initial result.
        - All FirstTools implicitly take an `input` argument that points to
            the input to that Tool, for example an ONNX file or PyTorch script.
    """

    @classmethod
    def helpful_parser(cls, short_description: str, **kwargs):
        parser = super().helpful_parser(short_description, **kwargs)

        # Argument required by TurnkeyML for any tool that starts a sequence
        parser.add_argument("--input", help=argparse.SUPPRESS)

        return parser

    @abc.abstractmethod
    def run(self, state: State, input=None) -> State:
        """
        The run() method of any FirstTool must accept the `input` argument
        """
