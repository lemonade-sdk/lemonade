import argparse
from turnkeyml.tools import Tool
from turnkeyml.state import State


class CombinedExampleTool(Tool):
    """
    This is an empty Tool that we include in our example that provides both
    a sequence and a runtime in a single plugin package.
    """

    unique_name = "combined-example-tool"

    def __init__(self):
        super().__init__(
            monitor_message="Special step expected by CombinedExampleRT",
        )

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="This is an examples tool from the combined example",
            add_help=add_help,
        )

        return parser

    def run(self, state: State):
        return state
