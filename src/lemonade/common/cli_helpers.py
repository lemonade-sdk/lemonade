import argparse
import sys
from typing import List, Dict, Tuple, Any
from lemonade.tools import Tool, FirstTool
import lemonade.common.printing as printing
from lemonade.tools.management_tools import ManagementTool


class CustomArgumentParser(argparse.ArgumentParser):

    def error(self, message):
        self.print_usage()
        printing.log_error(message)
        self.exit(2)


def _tool_list_help(tools: List[Tool], subclass, exclude=None) -> str:
    help = ""

    for tool_class in tools:
        if exclude and issubclass(tool_class, exclude):
            continue
        if issubclass(tool_class, subclass):
            help = (
                help
                + f" * {tool_class.unique_name}: {tool_class.parser().short_description}\n"
            )

    return help


def parse_tools(
    parser: argparse.ArgumentParser, supported_tools: List[Tool], cli_name="lemonade"
) -> Tuple[Dict[str, Any], Dict[Tool, List[str]], List[str]]:
    """
    Add the help for parsing tools and their args to an ArgumentParser.

    Then, perform the task of parsing a full CLI command including
    teasing apart the global arguments and separate tool invocations.
    """

    tool_parsers = {tool.unique_name: tool.parser() for tool in supported_tools}
    tool_classes = {tool.unique_name: tool for tool in supported_tools}

    # Sort tools into categories and format for the help menu
    first_tool_choices = _tool_list_help(supported_tools, FirstTool)
    eval_tool_choices = _tool_list_help(supported_tools, Tool, exclude=FirstTool)
    mgmt_tool_choices = _tool_list_help(supported_tools, ManagementTool)

    tools_action = parser.add_argument(
        "tools",
        metavar="tool --tool-args [tool --tool-args...]",
        nargs="?",
        help=f"""\
Run `{cli_name} TOOL -h` to learn more about each tool.

Tools that can start a sequence:
{first_tool_choices}
Tools that go into a sequence:
{eval_tool_choices}
Management tools:
{mgmt_tool_choices}""",
        choices=tool_parsers.keys(),
    )

    # run as if "-h" was passed if no parameters are passed
    if len(sys.argv) == 1:
        sys.argv.append("-h")

    # Break sys.argv into categories based on which tools were invoked
    # Arguments that are passed prior to invoking a tool are categorized as
    # global arguments that should be used to initialize the state.
    current_tool = "globals"
    tools_invoked = {current_tool: []}
    cmd = sys.argv[1:]
    while len(cmd):
        if cmd[0] in tool_parsers.keys():
            # Make sure each tool was only called once
            if cmd[0] in tools_invoked.keys():
                parser.error(
                    "A single call to lemonade can only invoke each tool once, "
                    f"however this call invokes tool {cmd[0]} multiple times."
                )
            current_tool = cmd.pop(0)
            tools_invoked[current_tool] = []
        else:
            tools_invoked[current_tool].append(cmd.pop(0))

    # Trick argparse into thinking tools was not a positional argument
    # this helps to avoid an error where an incorrect arg/value pair
    # can be misinterpreted as the tools positional argument
    tools_action.option_strings = ["--tools"]

    # Do one pass of parsing to figure out if -h was used
    global_args = vars(parser.parse_args(tools_invoked["globals"]))

    # Remove "tools" from global args because it was just there
    # as a placeholder
    global_args.pop("tools")

    # Remove globals from the list since its already been parsed
    tools_invoked.pop("globals")
    evaluation_tools = []
    management_tools = []
    for cmd, argv in tools_invoked.items():
        tool_parsers[cmd].parse_args(argv)

        # Keep track of whether the tools are ManagementTool or not,
        # since ManagementTools are mutually exclusive with evaluation
        # tools
        if issubclass(tool_classes[cmd], ManagementTool):
            management_tools.append(cmd)
        else:
            evaluation_tools.append(cmd)

    if len(management_tools) > 0 and len(evaluation_tools) > 0:
        parser.error(
            "This call to lemonade invoked both management and "
            "evaluation tools, however each call to lemonade "
            "is only allowed to invoke one or the other. "
            f"Management tools: {management_tools};"
            f"Evaluation tools: {evaluation_tools}."
        )

    if len(management_tools) == 0 and len(evaluation_tools) == 0:
        parser.error(
            "Calls to lemonade are required to call at least "
            "one tool or management tool."
        )

    # Convert tool names into Tool instances
    tool_instances = {tool_classes[cmd](): argv for cmd, argv in tools_invoked.items()}
    evaluation_tools = [tool_classes[cmd] for cmd in evaluation_tools]

    return global_args, tool_instances, evaluation_tools


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
