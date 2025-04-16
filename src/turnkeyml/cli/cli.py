import os
from difflib import get_close_matches
from typing import List
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
from turnkeyml.sequence import Sequence
from turnkeyml.tools import FirstTool, NiceHelpFormatter
from turnkeyml.sequence.tool_plugins import get_supported_tools
from turnkeyml.cli.spawn import DEFAULT_TIMEOUT_SECONDS
from turnkeyml.files_api import evaluate_files
from turnkeyml.common.cli_helpers import parse_tools, CustomArgumentParser


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
        f"(e.g., script (.py), ONNX (.onnx), turnkey build state ({build.state_file_name}), "
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
