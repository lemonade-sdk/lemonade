"""
This example shows how to use the LoadBuild tool as an API for working
with builds you have saved to your cache.

As a prerequisite, first run the onnx_opset.py API example to make sure
that you have a build ready for loading.
"""

import os
import numpy as np
import onnxruntime as ort
from turnkeyml.common.filesystem import get_available_builds, DEFAULT_CACHE_DIR
from turnkeyml.state import State
from turnkeyml.common.build import output_dir
from turnkeyml.tools.load_build import LoadBuild
from turnkeyml.tools.onnx import ConvertOnnxToFp16


def main():
    # This is the build name specified in the previous example
    prerequisite_build = "onnx_opset_example"

    # We use the _state.yaml file in the build directory when loading a build
    prior_state_file = os.path.join(
        output_dir(DEFAULT_CACHE_DIR, prerequisite_build),
        f"{prerequisite_build}_state.yaml",
    )

    # Make sure the "onnx_opset_example" build from the last
    # example was actually created
    builds = get_available_builds(DEFAULT_CACHE_DIR)
    print("All builds available in the cache:", builds)

    if prerequisite_build in builds:
        print(f"{prerequisite_build} exists!")
    else:
        raise Exception(
            f"{prerequisite_build} not found in cache. Make sure to "
            "run the onnx_opset.py API example before this example."
        )

    # Create a placeholder State instance that we will later populate by loading the build
    state = State(cache_dir=DEFAULT_CACHE_DIR, build_name=prerequisite_build)

    # Load the prior build state into our State instance
    # Set a skip_policy of "none" so that this example will work if you run
    # it multiple times
    state = LoadBuild().run(state, input=prior_state_file, skip_policy="none")

    # Convert the model to fp16 using turnkey's onnx conversion tool
    state = ConvertOnnxToFp16().run(state)

    # Load the ONNX model as an InferenceSession and execute one inference
    sess_options = ort.SessionOptions()
    sess_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    onnx_session = ort.InferenceSession(state.results, sess_options)

    # x is the example input tensor from the onnx_opset_example
    input_size = 9
    x = np.random.rand(input_size).astype(np.float16)

    # Run inference
    out = onnx_session.run([onnx_session.get_outputs()[0].name], {"x": x})
    print("Output:", out[0])


if __name__ == "__main__":
    main()
