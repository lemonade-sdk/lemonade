import os
from turnkeyml.tools import FirstTool, NiceHelpFormatter
import turnkeyml.common.filesystem as fs
import turnkeyml.cli.cli as cli
from turnkeyml.sequence import Sequence
from turnkeyml.tools.management_tools import Cache, Version
from turnkeyml.tools.report import Report
from turnkeyml.state import State

from turnkeyml.llm.tools.huggingface_load import (
    HuggingfaceLoad,
    AdaptHuggingface,
)

from turnkeyml.llm.tools.llamacpp import LoadLlamaCpp

import turnkeyml.llm.cache as cache
from turnkeyml.llm.tools.mmlu import AccuracyMMLU
from turnkeyml.llm.tools.perplexity import AccuracyPerplexity
from turnkeyml.llm.tools.chat import LLMPrompt, Serve


def main():

    # List the available tools
    tools = [
        HuggingfaceLoad,
        LoadLlamaCpp,
        AccuracyMMLU,
        AccuracyPerplexity,
        LLMPrompt,
        AdaptHuggingface,
        Serve,
        # Inherited from TurnkeyML
        Report,
        Cache,
        Version,
    ]

    # Import onnxruntime-genai recipes
    try:
        from turnkeyml.llm.tools.ort_genai.oga import OgaLoad

        tools = tools + [OgaLoad]

    except ModuleNotFoundError:
        pass

    # Import RyzenAI NPU modules only if RyzenAI NPU is installed
    try:
        from turnkeyml.llm.tools.ryzenai_npu.ryzenai_npu import RyzenAINPULoad

        tools = tools + [RyzenAINPULoad]
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

    global_args, tool_instances, evaluation_tools = cli.parse_tools(parser, tools)

    if len(evaluation_tools) > 0:
        if not issubclass(evaluation_tools[0], FirstTool):
            parser.error(
                "The first tool in the sequence needs to be one "
                "of the 'tools that can start a sequence.' Use "
                "`turnkey-llm -h` to see that list of tools."
            )
        # Run the evaluation tools as a build
        sequence = Sequence(tools=tool_instances)

        # Forward the selected input to the first tool in the sequence
        first_tool_args = next(iter(sequence.tools.values()))
        first_tool_args.append("--input")
        first_tool_args.append(global_args["input"])

        state = State(
            cache_dir=os.path.abspath(global_args["cache_dir"]),
            build_name=global_args["input"].replace("/", "_"),
            sequence_info=sequence.info,
        )
        sequence.launch(
            state,
            lean_cache=global_args["lean_cache"],
        )
    else:
        # Run the management tools
        for management_tool, argv in tool_instances.items():
            # Support "~" in the cache_dir argument
            parsed_cache_dir = os.path.expanduser(global_args[fs.Keys.CACHE_DIR])
            management_tool.parse_and_run(parsed_cache_dir, argv)


if __name__ == "__main__":
    main()
