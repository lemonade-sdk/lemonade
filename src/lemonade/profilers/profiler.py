import abc


class Profiler(abc.ABC):

    unique_name: str

    def __init__(self, parser_arg_value=None):
        self.parser_arg_value = parser_arg_value
        # Statistics that will be displayed to the CLI user
        self.status_stats = []

    @staticmethod
    @abc.abstractmethod
    def add_arguments_to_parser(parser):
        """
        Adds the argument parsing for this tool to the parser.
        Uses f"--{self.unique_name}" as the argument.
        """

    @abc.abstractmethod
    def start(self, build_dir):
        """
        This method is called prior to the tool sequence starting.
        This informs the profiler to start gathering data.
        The build directory can be used to store profiling data.
        """

    def tool_starting(self, tool_name):
        """
        This method is called to inform the profiler of the name of the tool that is about to start.
        """

    def tool_stopping(self):
        """
        This method is called to inform the profiler that the tool has finished.
        """

    def stop(self):
        """
        This method is called when the tool sequence has finished.
        This informs the profiler to stop gathering data.
        """

    @abc.abstractmethod
    def generate_results(self, state, timestamp, start_times):
        """
        This method is called so that the profiler can create its output files.
        The state is passed so that build info can be gathered and stats can be written.
        The timestamp can be used for filename in current working directory.
        The start times parameter is a dict with the keys being the tools names and
        the values being the time the tool started.  There is an initial "warmup" key
        that has a start time before the first tool and a "cool down" key that contains the
        time when the last tool ended.
        """


# Copyright (c) 2025 AMD
