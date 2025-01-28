"""
This script is an example of a tool.py file for Tool Plugin. Such a tool.py 
can be used to redefine the build phase of the turnkey CLI, benchmark_files(),
and build_model() to have any custom behavior.

In this the tool simply passes the build state to the next
tool in the sequence (i.e., this example is a no-op). It also
spends a few seconds updating the monitor's percent progress indicator.

After you install the plugin, you can tell `turnkey` to use this sequence with:

    turnkey -i INPUT_SCRIPT export-pytorch example-plugin-tool 
"""

import argparse
from time import sleep
from turnkeyml.tools import Tool
from turnkeyml.state import State


class ExamplePluginTool(Tool):
    """
    Example of a Tool installed by a plugin. Note that this docstring appears
    in the help menu when `turnkey example-plugin-tool -h` is called.
    """

    unique_name = "example-plugin-tool"

    def __init__(self):
        super().__init__(
            monitor_message="Special step expected by ExamplePluginTool",
        )

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="This is an examples tool from the Tool Plugins example",
            add_help=add_help,
        )

        return parser

    def run(self, state: State):
        self.set_percent_progress(0.0)
        total = 15  # seconds
        for i in range(total):
            sleep(1)
            percent_progress = (i + 1) / float(total) * 100
            self.set_percent_progress(percent_progress)
        return state
