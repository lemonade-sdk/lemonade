"""
This example shows how to call tools as their own standalone API.

Every Tool class has a run() method that can be used to programmatically
define a build. This is helpful in scenarios where the turnkey CLI is not
flexible enough to run the build(s) you want in the way that you want.

In this specific example, we will call the tools as APIs and set a custom ONNX opset.

You can run this script in your turnkey Conda environment with:
    python onnx_opset.py --onnx-opset YOUR_OPSET
"""

import pathlib
import argparse
import os
from turnkeyml.state import State
from turnkeyml.common.filesystem import Stats, DEFAULT_CACHE_DIR
from turnkeyml.tools.export import ExportPytorchModel
from turnkeyml.tools.discovery import Discover


def main():
    # Define the argument parser
    parser = argparse.ArgumentParser(
        description="Export a PyTorch model with a specified ONNX opset."
    )

    # Add the arguments
    parser.add_argument(
        "--onnx-opset",
        default="16",
        type=int,
        help="ONNX opset to use when creating ONNX files",
    )

    # Parse the arguments
    args = parser.parse_args()

    path_to_hello_world_script = str(
        pathlib.Path(__file__).parent.resolve() / "scripts" / "hello_world.py"
    )

    # Create a State instance
    # The arguments we pass here let the tools know where to store
    # logs and results on disk
    state = State(cache_dir=DEFAULT_CACHE_DIR, build_name="onnx_opset_example")

    # Manually run the Discover tool as an API
    # This will populate state.result with a PyTorch model instance
    # extracted from the target script
    state = Discover().run(state, input=path_to_hello_world_script)

    # Manually run the ExportPytorchModel tool as an API
    # This will export the PyTorch model in state.result into an ONNX file
    state = ExportPytorchModel().run(state, opset=args.onnx_opset)

    # Save the build state to make sure we can access this build later
    state.save()

    # See what we learned about the model during discovery and export
    stats = Stats(state.cache_dir, state.build_name).stats
    print("Parameters:", stats["parameters"])

    # Make sure the ONNX file really exists
    print("Build result file:", state.results, os.path.exists(state.results))


if __name__ == "__main__":
    main()
