import argparse
import sys
import os
from difflib import get_close_matches
from typing import List, Dict, Tuple, Any
import turnkeyml.common.filesystem as fs
from turnkeyml.sequence import Sequence
from turnkeyml.tools import Tool, FirstTool, NiceHelpFormatter
from turnkeyml.sequence.tool_plugins import get_supported_tools
from turnkeyml.cli.spawn import DEFAULT_TIMEOUT_SECONDS
from turnkeyml.files_api import evaluate_files
import turnkeyml.common.printing as printing
from turnkeyml.tools.management_tools import ManagementTool


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


def _check_extension(
    choices: List[str], file_name: str, error_func: callable, tool_names: List[str]
):
    _, extension = os.path.splitext(file_name.split("::")[0])
    if not extension:
        close_matches = get_close_matches(file_name, tool_names)
        if close_matches:
            # Misspelled tool names can be picked up as input files, so we check
            # for this case here and try to provide a better suggestion
            error_func(
                f"unrecognized argument '{file_name}', did you mean '{close_matches[0]}'?"
            )
        else:
            error_func(
                f"{file_name} was recognized as an argument to `--input-files`, "
                "however it is not a file name (no file extension). If it was "
                "meant to be a tool name, please check whether that tool is "
                "available and correctly spelled in the list of available tools "
                "when calling `turnkey -h`."
            )
    if extension[1:].lower() not in choices:
        error_func(
            f"input_files must end with .py, .onnx, or .txt (got '{file_name}')\n"
        )
    return file_name


def parse_tools(
    parser: argparse.ArgumentParser, supported_tools: List[Tool]
) -> Tuple[Dict[str, Any], Dict[Tool, List[str]], List[str]]:
    """
    Add the help for parsing tools and their args to an ArgumentParser.

    Then, perform the task of parsing a full turnkey CLI command including
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
Available tools that can be sequenced together to perform a build. 

Call `turnkey TOOL -h` to learn more about each tool.

Tools that can start a sequence:
{first_tool_choices}
Tools that go into a sequence:
{eval_tool_choices}
Management tool choices:
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
                    "A single call to turnkey can only invoke each tool once, "
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
            "This call to turnkey invoked both management and "
            "evaluation tools, however each call to turnkey "
            "is only allowed to invoke one or the other. "
            f"Management tools: {management_tools};"
            f"Evaluation tools: {evaluation_tools}."
        )

    if len(management_tools) == 0 and len(evaluation_tools) == 0:
        parser.error(
            "Calls to turnkey are required to call at least "
            "one tool or management tool."
        )

    # Convert tool names into Tool instances
    tool_instances = {tool_classes[cmd](): argv for cmd, argv in tools_invoked.items()}
    evaluation_tools = [tool_classes[cmd] for cmd in evaluation_tools]

    return global_args, tool_instances, evaluation_tools


def main():

    supported_tools = get_supported_tools()

    # Define the argument parser
    parser = CustomArgumentParser(
        description="This utility runs tools in a sequence. "
        "To use it, provide a list of tools and "
        "their arguments. See "
        "https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/tools_user_guide.md "
        "to learn the exact syntax.\n\nExample: turnkey -i my_model.py discover export-pytorch",
        formatter_class=NiceHelpFormatter,
    )

    parser.add_argument(
        "-i",
        "--input-files",
        nargs="+",
        help="One or more inputs that will be evaluated by the tool sequence "
        "(e.g., script (.py), ONNX (.onnx), turnkey build state (state.yaml), "
        "input list (.txt) files)",
        type=lambda file: _check_extension(
            ("py", "onnx", "txt", "yaml"),
            file,
            parser.error,
            {tool.unique_name: tool for tool in supported_tools},
        ),
    )

    parser.add_argument(
        "-d",
        "--cache-dir",
        help="Build cache directory where results will "
        f"be stored (defaults to {fs.DEFAULT_CACHE_DIR})",
        required=False,
        default=fs.DEFAULT_CACHE_DIR,
    )

    parser.add_argument(
        "--lean-cache",
        dest="lean_cache",
        help="Delete all build artifacts (e.g., .onnx files) when the command completes",
        action="store_true",
    )

    parser.add_argument(
        "--labels",
        dest="labels",
        help="Filter the --input-files to only include files that have the provided labels",
        nargs="*",
        default=[],
    )

    slurm_or_processes_group = parser.add_mutually_exclusive_group()

    slurm_or_processes_group.add_argument(
        "--use-slurm",
        dest="use_slurm",
        help="Execute on Slurm instead of using local compute resources",
        action="store_true",
    )

    slurm_or_processes_group.add_argument(
        "--process-isolation",
        dest="process_isolation",
        help="Isolate evaluating each input into a separate process",
        action="store_true",
    )

    parser.add_argument(
        "--timeout",
        type=int,
        default=None,
        help="Build timeout, in seconds, after which a build will be canceled "
        f"(default={DEFAULT_TIMEOUT_SECONDS}). Only "
        "applies when --process-isolation or --use-slurm is also used.",
    )

    global_args, tool_instances, evaluation_tools = parse_tools(parser, supported_tools)

    if len(evaluation_tools) > 0:
        if not issubclass(evaluation_tools[0], FirstTool):
            parser.error(
                "The first tool in the sequence needs to be one "
                "of the 'tools that can start a sequence.' Use "
                "`turnkey -h` to see that list of tools."
            )
        # Run the evaluation tools as a build
        sequence = Sequence(tools=tool_instances)
        evaluate_files(sequence=sequence, **global_args)
    else:
        # Run the management tools
        for management_tool, argv in tool_instances.items():
            # Support "~" in the cache_dir argument
            parsed_cache_dir = os.path.expanduser(global_args[fs.Keys.CACHE_DIR])
            management_tool.parse_and_run(parsed_cache_dir, argv)


if __name__ == "__main__":
    main()
