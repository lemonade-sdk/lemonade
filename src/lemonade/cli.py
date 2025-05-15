import os
from lemonade.version import __version__ as version_number
from lemonade.tools import FirstTool, NiceHelpFormatter
from lemonade.profilers.memory_tracker import MemoryTracker
import lemonade.common.filesystem as fs
import lemonade.common.cli_helpers as cli
from lemonade.sequence import Sequence
from lemonade.tools.management_tools import Cache, Version, SystemInfo
from lemonade.state import State

from lemonade.tools.huggingface_load import HuggingfaceLoad

from lemonade.tools.huggingface_bench import HuggingfaceBench
from lemonade.tools.ort_genai.oga_bench import OgaBench
from lemonade.tools.llamacpp_bench import LlamaCppBench
from lemonade.tools.llamacpp import LoadLlamaCpp

import lemonade.cache as cache
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.perplexity import AccuracyPerplexity
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.quark.quark_load import QuarkLoad
from lemonade.tools.quark.quark_quantize import QuarkQuantize
from lemonade.tools.report.llm_report import LemonadeReport
from lemonade.tools.server.serve import Server


def main():

    # List the available tools
    tools = [
        HuggingfaceLoad,
        LoadLlamaCpp,
        LlamaCppBench,
        AccuracyMMLU,
        AccuracyHumaneval,
        AccuracyPerplexity,
        LLMPrompt,
        HuggingfaceBench,
        OgaBench,
        QuarkQuantize,
        QuarkLoad,
        LemonadeReport,
        Server,
        # Inherited from lemonade
        Cache,
        Version,
        SystemInfo,
    ]

    # Import onnxruntime-genai recipes
    try:
        from lemonade.tools.ort_genai.oga import OgaLoad

        tools = tools + [OgaLoad]

    except ModuleNotFoundError:
        pass

    # List the available profilers
    profilers = [MemoryTracker]

    # Define the argument parser
    parser = cli.CustomArgumentParser(
        description=f"""Tools for evaluating and deploying LLMs (v{version_number}).

Read this to learn the command syntax:
https://github.com/lemonade-sdk/lemonade/blob/main/docs/README.md""",
        formatter_class=NiceHelpFormatter,
    )

    parser.add_argument(
        "-i",
        "--input",
        help="The input that will be evaluated by the starting tool "
        "(e.g., huggingface checkpoint)",
    )

    parser.add_argument(
        "-d",
        "--cache-dir",
        help="Cache directory where tool results are "
        f"stored (default: {cache.DEFAULT_CACHE_DIR})",
        required=False,
        default=cache.DEFAULT_CACHE_DIR,
    )

    for profiler in profilers:
        profiler.add_arguments_to_parser(parser)

    global_args, tool_instances, evaluation_tools = cli.parse_tools(
        parser, tools, cli_name="lemonade"
    )

    profiler_instances = [
        profiler(global_args[profiler.unique_name])
        for profiler in profilers
        if global_args.get(profiler.unique_name, None) is not None
    ]

    if len(evaluation_tools) > 0:
        if not issubclass(evaluation_tools[0], FirstTool):
            parser.error(
                "The first tool in the sequence needs to be one "
                "of the 'tools that can start a sequence.' Use "
                "`lemonade -h` to see that list of tools."
            )
        # Run the evaluation tools as a build
        sequence = Sequence(tools=tool_instances, profilers=profiler_instances)

        # Forward the selected input to the first tool in the sequence
        first_tool_args = next(iter(sequence.tools.values()))
        first_tool_args.append("--input")
        first_tool_args.append(global_args["input"])

        state = State(
            cache_dir=os.path.abspath(global_args["cache_dir"]),
            build_name=cache.build_name(global_args["input"]),
            sequence_info=sequence.info,
        )
        sequence.launch(state)
    else:
        # Run the management tools
        for management_tool, argv in tool_instances.items():
            # Support "~" in the cache_dir argument
            parsed_cache_dir = os.path.expanduser(global_args[fs.Keys.CACHE_DIR])
            management_tool.parse_and_run(parsed_cache_dir, argv)


if __name__ == "__main__":
    main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
