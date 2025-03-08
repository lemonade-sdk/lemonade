import os
from turnkeyml.tools import FirstTool, NiceHelpFormatter
import turnkeyml.common.filesystem as fs
import turnkeyml.cli.cli as cli
from turnkeyml.sequence import Sequence
from turnkeyml.tools.management_tools import Cache, Version, SystemInfo
from turnkeyml.state import State

from lemonade.tools.huggingface_load import (
    HuggingfaceLoad,
    AdaptHuggingface,
)

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
from lemonade.tools.serve import Server


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
        AdaptHuggingface,
        HuggingfaceBench,
        OgaBench,
        QuarkQuantize,
        QuarkLoad,
        LemonadeReport,
        Server,
        # Inherited from TurnkeyML
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

    # Define the argument parser
    parser = cli.CustomArgumentParser(
        description="Turnkey analysis and benchmarking of GenAI models. "
        "This utility is a toolchain. To use it, provide a list of tools and "
        "their arguments.",
        formatter_class=NiceHelpFormatter,
    )

    parser.add_argument(
        "-i",
        "--input",
        help="The input that will be evaluated by the tool sequence "
        "(e.g., huggingface checkpoints)",
    )

    parser.add_argument(
        "-d",
        "--cache-dir",
        help="Cache directory where the results of each tool will "
        f"be stored (defaults to {cache.DEFAULT_CACHE_DIR})",
        required=False,
        default=cache.DEFAULT_CACHE_DIR,
    )

    parser.add_argument(
        "--lean-cache",
        dest="lean_cache",
        help="Delete all build artifacts (e.g., .onnx files) when the command completes",
        action="store_true",
    )

    parser.add_argument(
        "-m",
        "--memory",
        nargs="?",
        metavar="TRACK_INTERVAL",
        type=float,
        default=None,
        const=0.25,
        help="Track physical memory usage during the build and generate a plot when the "
        "command completes. Optionally, specify the tracking interval (sec), "
        "defaults to 0.25 sec.",
    )

    global_args, tool_instances, evaluation_tools = cli.parse_tools(parser, tools)

    if len(evaluation_tools) > 0:
        if not issubclass(evaluation_tools[0], FirstTool):
            parser.error(
                "The first tool in the sequence needs to be one "
                "of the 'tools that can start a sequence.' Use "
                "`lemonade -h` to see that list of tools."
            )
        # Run the evaluation tools as a build
        sequence = Sequence(tools=tool_instances)

        # Forward the selected input to the first tool in the sequence
        first_tool_args = next(iter(sequence.tools.values()))
        first_tool_args.append("--input")
        first_tool_args.append(global_args["input"])

        state = State(
            cache_dir=os.path.abspath(global_args["cache_dir"]),
            build_name=cache.build_name(global_args["input"]),
            sequence_info=sequence.info,
        )
        sequence.launch(
            state,
            lean_cache=global_args["lean_cache"],
            track_memory_interval=global_args["memory"],
        )
    else:
        # Run the management tools
        for management_tool, argv in tool_instances.items():
            # Support "~" in the cache_dir argument
            parsed_cache_dir = os.path.expanduser(global_args[fs.Keys.CACHE_DIR])
            management_tool.parse_and_run(parsed_cache_dir, argv)


if __name__ == "__main__":
    main()
