import argparse
import os
import sys
import copy
from difflib import get_close_matches
import turnkeyml.common.build as build
import turnkeyml.common.exceptions as exceptions
import turnkeyml.common.filesystem as filesystem
import turnkeyml.cli.report as report
import turnkeyml.cli.parser_helpers as parser_helpers
from turnkeyml.files_api import benchmark_files
from turnkeyml.version import __version__ as turnkey_version
from turnkeyml.run.devices import SUPPORTED_DEVICES, SUPPORTED_RUNTIMES
from turnkeyml.build.sequences import SUPPORTED_SEQUENCES
from turnkeyml.cli.spawn import DEFAULT_TIMEOUT_SECONDS
from turnkeyml.run.benchmark_build import benchmark_cache_cli
from turnkeyml.analyze.status import Verbosity


class MyParser(argparse.ArgumentParser):
    def error(self, message):
        sys.stderr.write(f"error: {message}\n\n")
        sys.stderr.write(f"Run '{self.prog} --help' for more information\n\n")
        self.print_usage(sys.stderr)
        sys.exit(2)


def print_version(_):
    """
    Print the package version number
    """
    print(turnkey_version)


def print_stats(args):
    state_path = build.state_file(args.cache_dir, args.build_name)
    filesystem.print_yaml_file(state_path, "build state")

    filesystem.print_yaml_file(
        filesystem.Stats(args.cache_dir, args.build_name).file, "stats"
    )


def benchmark_command(args):
    """
    Map the argparse args into benchmark_files() arguments

    Assumes the following rules:
    -   All args passed to a "benchmark" command should be forwarded to the benchmark_files()
        API, except as explicitly handled below.
    -   The "dest" names of all CLI args must exactly match the names of the corresponding API arg
    """

    api_args = copy.deepcopy(vars(args))

    # Remove the function ID because it was only used to get us into this method
    api_args.pop("func")

    # Decode CLI arguments before calling the API
    api_args["rt_args"] = parser_helpers.decode_args(api_args["rt_args"])

    benchmark_files(**api_args)


def main():
    """
    Parses arguments passed by user and forwards them into a
    command function
    """

    parser = MyParser(
        description="TurnkeyML benchmarking command line interface",
        formatter_class=argparse.RawTextHelpFormatter,
    )

    # We use sub-parsers to keep the help info neatly organized for each command
    # Sub-parses also allow us to set command-specific help on options like --cache-dir
    # that are used in multiple commands

    subparsers = parser.add_subparsers(
        title="command",
        help="Choose one of the following commands:",
        metavar="COMMAND",
        required=True,
    )

    #######################################
    # Parser for the "benchmark" command
    #######################################

    def check_extension(choices, file_name, error_func):
        _, extension = os.path.splitext(file_name.split("::")[0])
        if extension[1:].lower() not in choices:
            error_func(
                f"input_files must end with .py, .onnx, or .txt (got '{file_name}')\n"
            )
        return file_name

    benchmark_parser = subparsers.add_parser(
        "benchmark",
        help="Benchmark the performance of one or more models",
        description="Analyze, build, and then benchmark the model(s) within input file(s).",
    )
    benchmark_parser.set_defaults(func=benchmark_command)

    benchmark_parser.add_argument(
        "input_files",
        nargs="+",
        help="One or more script (.py), ONNX (.onnx), or input list (.txt) files to be benchmarked",
        type=lambda file: check_extension(
            ("py", "onnx", "txt"), file, benchmark_parser.error
        ),
    )

    toolchain_select_group = benchmark_parser.add_argument_group(
        "Select which phase(s) of the toolchain to run "
        "(default is to run analyze, build, and benchmark)"
    )

    toolchain_select_group.add_argument(
        "-a",
        "--analyze-only",
        dest="analyze_only",
        help="Stop this command after the analyze phase",
        action="store_true",
    )

    toolchain_select_group.add_argument(
        "-b",
        "--build-only",
        dest="build_only",
        help="Stop this command after the analyze and build phases",
        action="store_true",
    )

    analyze_group = benchmark_parser.add_argument_group(
        "Options that specifically apply to the `analyze` phase of the toolflow"
    )

    analyze_group.add_argument(
        "--labels",
        dest="labels",
        help="Only benchmark the scripts that have the provided labels",
        nargs="*",
        default=[],
    )

    analyze_group.add_argument(
        "--script-args",
        dest="script_args",
        type=str,
        help="Arguments to pass into the target script(s)",
    )

    analyze_group.add_argument(
        "--max-depth",
        dest="max_depth",
        type=int,
        default=0,
        help="Maximum depth to analyze within the model structure of the target script(s)",
    )

    both_build_benchmark_group = benchmark_parser.add_argument_group(
        "Options that apply to both the `build` and `benchmark` phases of the toolflow"
    )

    benchmark_default_device = "x86"
    both_build_benchmark_group.add_argument(
        "--device",
        choices=SUPPORTED_DEVICES,
        dest="device",
        help="Type of hardware device to be used for the benchmark "
        f'(defaults to "{benchmark_default_device}")',
        required=False,
        default=benchmark_default_device,
    )

    both_build_benchmark_group.add_argument(
        "--runtime",
        choices=SUPPORTED_RUNTIMES.keys(),
        dest="runtime",
        help="Software runtime that will be used to collect the benchmark. "
        "Must be compatible with the selected device. "
        "Automatically selects a sequence if `--sequence` is not used. "
        "If this argument is not set, the default runtime of the selected device will be used.",
        required=False,
        default=None,
    )

    both_build_benchmark_group.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="Build cache directory where the resulting build directories will "
        f"be stored (defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    both_build_benchmark_group.add_argument(
        "--lean-cache",
        dest="lean_cache",
        help="Delete all build artifacts except for log files when the command completes",
        action="store_true",
    )

    build_group = benchmark_parser.add_argument_group(
        "Options that apply specifically to the `build` phase of the toolflow"
    )

    build_group.add_argument(
        "--sequence",
        choices=SUPPORTED_SEQUENCES.keys(),
        dest="sequence",
        help="Name of a build sequence that will define the model-to-model transformations, "
        "used to build the models. Each runtime has a default sequence that it uses.",
        required=False,
        default=None,
    )

    build_group.add_argument(
        "--rebuild",
        choices=build.REBUILD_OPTIONS,
        dest="rebuild",
        help=f"Sets the cache rebuild policy (defaults to {build.DEFAULT_REBUILD_POLICY})",
        required=False,
        default=build.DEFAULT_REBUILD_POLICY,
    )

    build_group.add_argument(
        "--onnx-opset",
        dest="onnx_opset",
        type=int,
        default=None,
        help=f"ONNX opset used when creating ONNX files (default={build.DEFAULT_ONNX_OPSET}). "
        "Not applicable when input model is already a .onnx file.",
    )

    benchmark_group = benchmark_parser.add_argument_group(
        "Options that apply specifically to the `benchmark` phase of the toolflow"
    )

    benchmark_group.add_argument(
        "--iterations",
        dest="iterations",
        type=int,
        default=100,
        help="Number of execution iterations of the model to capture\
              the benchmarking performance (e.g., mean latency)",
    )

    benchmark_group.add_argument(
        "--rt-args",
        dest="rt_args",
        type=str,
        nargs="*",
        help="Optional arguments provided to the runtime being used",
    )

    all_toolflows_group = benchmark_parser.add_argument_group(
        "Options that apply to all toolflows"
    )

    slurm_or_processes_group = all_toolflows_group.add_mutually_exclusive_group()

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

    all_toolflows_group.add_argument(
        "--timeout",
        type=int,
        default=None,
        help="Build timeout, in seconds, after which a build will be canceled "
        f"(default={DEFAULT_TIMEOUT_SECONDS}). Only "
        "applies when --process-isolation or --use-slurm is also used.",
    )

    default_verbosity = Verbosity.AUTO.value
    all_toolflows_group.add_argument(
        "--verbosity",
        choices=[field.value for field in Verbosity],
        default=default_verbosity,
        help="Verbosity of the status updates printed to the command line "
        f"(default={default_verbosity}). '{Verbosity.DYNAMIC.value}': "
        "take over the terminal, updating "
        " it with a summary of all turnkey information. "
        f"'{Verbosity.STATIC.value}': print each evaluation as it takes place and "
        "never clear the terminal.",
    )

    #######################################
    # Subparser for the "cache" command
    #######################################

    cache_parser = subparsers.add_parser(
        "cache",
        help="Commands for managing the build cache",
    )

    cache_subparsers = cache_parser.add_subparsers(
        title="cache",
        help="Commands for managing the build cache",
        required=True,
        dest="cache_cmd",
    )

    #######################################
    # Parser for the "cache report" command
    #######################################

    report_parser = cache_subparsers.add_parser(
        "report", help="Generate reports in CSV format"
    )
    report_parser.set_defaults(func=report.summary_spreadsheets)

    report_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dirs",
        help=(
            "One or more build cache directories to generate the report "
            f"(defaults to {filesystem.DEFAULT_CACHE_DIR})"
        ),
        default=[filesystem.DEFAULT_CACHE_DIR],
        nargs="*",
    )

    report_parser.add_argument(
        "-r",
        "--report-dir",
        dest="report_dir",
        help="Path to folder where report will be saved (defaults to current working directory)",
        required=False,
        default=os.getcwd(),
    )

    #######################################
    # Parser for the "cache list" command
    #######################################

    list_parser = cache_subparsers.add_parser(
        "list", help="List all builds in a target cache"
    )
    list_parser.set_defaults(func=filesystem.print_available_builds)

    list_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="The builds in this build cache directory will printed to the terminal "
        f" (defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    #######################################
    # Parser for the "cache stats" command
    #######################################

    stats_parser = cache_subparsers.add_parser(
        "stats", help="Print stats about a build in a target cache"
    )
    stats_parser.set_defaults(func=print_stats)

    stats_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="The stats of a build in this build cache directory will printed to the terminal "
        f" (defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    stats_parser.add_argument(
        "build_name",
        help="Name of the specific build whose stats are to be printed, within the cache directory",
    )

    #######################################
    # Parser for the "cache delete" command
    #######################################

    delete_parser = cache_subparsers.add_parser(
        "delete", help="Delete one or more builds in a build cache"
    )
    delete_parser.set_defaults(func=filesystem.delete_builds)

    delete_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="Search path for builds " f"(defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    delete_group = delete_parser.add_mutually_exclusive_group(required=True)

    delete_group.add_argument(
        "build_name",
        nargs="?",
        help="Name of the specific build to be deleted, within the cache directory",
    )

    delete_group.add_argument(
        "--all",
        dest="delete_all",
        help="Delete all builds in the cache directory",
        action="store_true",
    )

    #######################################
    # Parser for the "cache clean" command
    #######################################

    clean_parser = cache_subparsers.add_parser(
        "clean",
        help="Remove the build artifacts from one or more builds in a build cache",
    )
    clean_parser.set_defaults(func=filesystem.clean_builds)

    clean_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="Search path for builds " f"(defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    clean_group = clean_parser.add_mutually_exclusive_group(required=True)

    clean_group.add_argument(
        "build_name",
        nargs="?",
        help="Name of the specific build to be cleaned, within the cache directory",
    )

    clean_group.add_argument(
        "--all",
        dest="clean_all",
        help="Clean all builds in the cache directory",
        action="store_true",
    )

    #######################################
    # Parser for the "cache location" command
    #######################################

    cache_location_parser = cache_subparsers.add_parser(
        "location",
        help="Print the location of the default build cache directory",
    )
    cache_location_parser.set_defaults(func=filesystem.print_cache_dir)

    #######################################
    # Parser for the "cache benchmark" command
    #######################################

    cache_benchmark_parser = cache_subparsers.add_parser(
        "benchmark",
        help="Benchmark one or more builds in a build cache",
    )
    cache_benchmark_parser.set_defaults(func=benchmark_cache_cli)

    cache_benchmark_parser.add_argument(
        "-d",
        "--cache-dir",
        dest="cache_dir",
        help="Search path for builds " f"(defaults to {filesystem.DEFAULT_CACHE_DIR})",
        required=False,
        default=filesystem.DEFAULT_CACHE_DIR,
    )

    cache_benchmark_group = cache_benchmark_parser.add_mutually_exclusive_group(
        required=True
    )

    cache_benchmark_group.add_argument(
        "build_name",
        nargs="?",
        help="Name of the specific build to be benchmarked, within the cache directory",
    )

    cache_benchmark_group.add_argument(
        "--all",
        dest="benchmark_all",
        help="Benchmark all builds in the cache directory",
        action="store_true",
    )

    skip_policy_default = "attempted"
    cache_benchmark_parser.add_argument(
        "--skip",
        choices=[skip_policy_default, "failed", "successful", "none"],
        dest="skip_policy",
        help=f"Sets the policy for skipping benchmark attempts (defaults to {skip_policy_default})."
        "`attempted` means to skip any previously-attempted benchmark, "
        "whether it succeeded or failed."
        "`failed` skips benchmarks that have already failed once."
        "`successful` skips benchmarks that have already succeeded."
        "`none` will attempt all benchmarks, regardless of whether they were previously attempted.",
        required=False,
        default=skip_policy_default,
    )

    cache_benchmark_parser.add_argument(
        "--timeout",
        type=int,
        default=1800,
        help="Benchmark timeout, in seconds, after which each benchmark will be canceled "
        "(default: 30min).",
    )

    cache_benchmark_parser.add_argument(
        "--runtime",
        choices=SUPPORTED_RUNTIMES.keys(),
        dest="runtime",
        help="Software runtime that will be used to collect the benchmark. "
        "Must be compatible with the device chosen for the build. "
        "If this argument is not set, the default runtime of the selected device will be used.",
        required=False,
        default=None,
    )

    cache_benchmark_parser.add_argument(
        "--iterations",
        dest="iterations",
        type=int,
        default=100,
        help="Number of execution iterations of the model to capture\
              the benchmarking performance (e.g., mean latency)",
    )

    cache_benchmark_parser.add_argument(
        "--rt-args",
        dest="rt_args",
        type=str,
        nargs="*",
        help="Optional arguments provided to the runtime being used",
    )

    #######################################
    # Subparser for the "models" command
    #######################################

    models_parser = subparsers.add_parser(
        "models",
        help="Commands for managing the models",
    )

    models_subparsers = models_parser.add_subparsers(
        title="models",
        help="Commands for managing the models",
        required=True,
        dest="models_cmd",
    )

    models_location_parser = models_subparsers.add_parser(
        "location",
        help="Print the location of the models directory",
    )
    models_location_parser.set_defaults(func=filesystem.print_models_dir)

    models_location_parser.add_argument(
        "--quiet",
        dest="verbose",
        help="Command output will only include the directory path",
        required=False,
        action="store_false",
    )

    #######################################
    # Parser for the "version" command
    #######################################

    version_parser = subparsers.add_parser(
        "version",
        help="Print the package version number",
    )
    version_parser.set_defaults(func=print_version)

    #######################################
    # Execute the command
    #######################################

    # The default behavior of this CLI is to run the build command
    # on a target script. If the user doesn't provide a command,
    # we alter argv to insert the command for them.

    # Special characters that indicate a string is a filename, not a command
    file_chars = [".", "/", "\\", "*"]

    if len(sys.argv) > 1:
        first_arg = sys.argv[1]
        if first_arg not in subparsers.choices.keys() and "-h" not in first_arg:
            if any(char_to_check in first_arg for char_to_check in file_chars):
                # User has provided a file as the first positional arg
                sys.argv.insert(1, "benchmark")
            else:
                # User has provided a command as the first positional arg
                # Check how close we are from each of the valid options
                # NOTE: if we are not close to a valid option, we will let
                # argparse detect and raise the error
                valid_options = list(subparsers.choices.keys())
                close_matches = get_close_matches(first_arg, valid_options)

                if close_matches:
                    raise exceptions.ArgError(
                        f"Unexpected command `turnkey {first_arg}`. "
                        f"Did you mean `turnkey {close_matches[0]}`?"
                    )

    args = parser.parse_args()

    args.func(args)


if __name__ == "__main__":
    main()
