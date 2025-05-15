import os
import shutil
from typing import List, Dict, Optional
import onnx
import turnkeyml.common.filesystem as fs
import turnkeyml.common.build as build
import turnkeyml.common.onnx_helpers as onnx_helpers
from turnkeyml.state import load_state


# We generate a corpus on to the filesystem during the test
# to get around how weird bake tests are when it comes to
# filesystem access
test_scripts_dot_py = {
    "linear.py": """# labels: name::linear author::turnkey license::mit test_group::a task::test
import torch

torch.manual_seed(0)


class LinearTestModel(torch.nn.Module):
    def __init__(self, input_features, output_features):
        super(LinearTestModel, self).__init__()
        self.fc = torch.nn.Linear(input_features, output_features)

    def forward(self, x):
        output = self.fc(x)
        return output


input_features = 10
output_features = 10

# Model and input configurations
model = LinearTestModel(input_features, output_features)
inputs = {"x": torch.rand(input_features)}

output = model(**inputs)

""",
    "linear2.py": """# labels: name::linear2 author::turnkey license::mit test_group::b task::test
import torch

torch.manual_seed(0)

# Define model class
class TwoLayerModel(torch.nn.Module):
    def __init__(self, input_size, output_size):
        super(TwoLayerModel, self).__init__()
        self.fc = torch.nn.Linear(input_size, output_size)
        self.fc2 = torch.nn.Linear(output_size, output_size)

    def forward(self, x):
        output = self.fc(x)
        output = self.fc2(output)
        return output


# Instantiate model and generate inputs
input_size = 10
output_size = 5
pytorch_model = TwoLayerModel(input_size, output_size)
inputs = {"x": torch.rand(input_size)}

pytorch_outputs = pytorch_model(**inputs)

# Print results
print(f"Pytorch_outputs: {pytorch_outputs}")
""",
    "crash.py": """# labels: name::crash author::turnkey license::mit task::test
import torch
import sys

torch.manual_seed(0)

# The purpose of this script is to intentionally crash
# so that we can test --resume
# Any test that doesn't supply the crash signal will treat this
# as a normal input script that runs a small model
if len(sys.argv) > 1:
    if sys.argv[1] == "crash!":
        assert False

class LinearTestModel(torch.nn.Module):
    def __init__(self, input_features, output_features):
        super(LinearTestModel, self).__init__()
        self.fc = torch.nn.Linear(input_features, output_features)

    def forward(self, x):
        output = self.fc(x)
        return output


input_features = 5
output_features = 5

# Model and input configurations
model = LinearTestModel(input_features, output_features)
inputs = {"x": torch.rand(input_features)}

output = model(**inputs)
""",
}


def extras_python(corpus_dir: str):
    return {
        "compiled.py": """
# labels: name::linear author::selftest test_group::selftest task::test
import torch

torch.manual_seed(0)


class LinearTestModel(torch.nn.Module):
    def __init__(self, input_features, output_features):
        super(LinearTestModel, self).__init__()
        self.fc = torch.nn.Linear(input_features, output_features)

    def forward(self, x):
        output = self.fc(x)
        return output


input_features = 10
output_features = 10

# Compiled model
model = LinearTestModel(input_features, output_features)
model = torch.compile(model)
inputs = {"x": torch.rand(input_features)}
model(**inputs)

# Non-compiled model
model2 = LinearTestModel(input_features * 2, output_features)
inputs2 = {"x": torch.rand(input_features * 2)}
model2(**inputs2)
    """,
        "selected_models.txt": f"""
    {os.path.join(corpus_dir,"linear.py")}
    {os.path.join(corpus_dir,"linear2.py")}
    """,
        "timeout.py": """
# labels: name::timeout author::turnkey license::mit test_group::a task::test
import torch
import time
torch.manual_seed(0)


class LinearTestModel(torch.nn.Module):
    def __init__(self, input_features, output_features):
        super(LinearTestModel, self).__init__()
        time.sleep(20)
        self.fc = torch.nn.Linear(input_features, output_features)

    def forward(self, x):
        output = self.fc(x)
        return output


input_features = 50
output_features = 10

# Model and input configurations
model = LinearTestModel(input_features, output_features)
inputs = {"x": torch.rand(input_features)}

output = model(**inputs)

    """,
    }


def create_test_dir(
    key: str,
    test_scripts: Dict = None,
    base_dir: str = os.path.dirname(os.path.abspath(__file__)),
):
    # Define paths to be used
    cache_dir = os.path.join(base_dir, "generated", f"{key}_cache_dir")
    corpus_dir = os.path.join(base_dir, "generated", "test_corpus")

    # Delete folders if they exist and
    if os.path.isdir(cache_dir):
        shutil.rmtree(cache_dir)
    if os.path.isdir(corpus_dir):
        shutil.rmtree(corpus_dir)
    os.makedirs(corpus_dir, exist_ok=True)

    # Populate corpus dir
    if test_scripts is None:
        test_scripts = test_scripts_dot_py
    for key, value in test_scripts.items():
        model_path = os.path.join(corpus_dir, key)
        with open(model_path, "w", encoding="utf") as f:
            f.write(value)

    return cache_dir, corpus_dir


def strip_dot_py(test_script_file: str) -> str:
    return test_script_file.split(".")[0]


def get_stats_and_state(
    test_script: str,
    cache_dir: str,
) -> int:
    # Figure out the build name by surveying the build cache
    builds = fs.get_all(cache_dir)
    test_script_name = strip_dot_py(test_script)

    for build_state_file in builds:
        if test_script_name in build_state_file:
            build_state = load_state(state_path=build_state_file)
            stats = fs.Stats(
                build_state.cache_dir,
                build_state.build_name,
            )
            return stats.stats, build_state

    raise Exception(f"Stats not found for {test_script}")


def assert_success_of_builds(
    test_script_files: List[str],
    cache_dir: str,
    check_perf: bool = False,
    check_opset: Optional[int] = None,
    check_iteration_count: Optional[int] = None,
    check_onnx_file_count: Optional[int] = None,
) -> int:
    # Figure out the build name by surveying the build cache
    # for a build that includes test_script_name in the name
    builds = fs.get_all(cache_dir)
    builds_found = 0

    for test_script in test_script_files:
        test_script_name = strip_dot_py(test_script)
        script_build_found = False

        for build_state_file in builds:
            if test_script_name in build_state_file:
                build_state = load_state(state_path=build_state_file)
                stats = fs.Stats(
                    build_state.cache_dir,
                    build_state.build_name,
                )
                assert build_state.build_status == build.FunctionStatus.SUCCESSFUL
                script_build_found = True
                builds_found += 1

                if check_perf:
                    assert stats.stats["mean_latency"] > 0
                    assert stats.stats["throughput"] > 0

                if check_iteration_count:
                    iterations = stats.stats["iterations"]
                    assert iterations == check_iteration_count

                if check_opset:
                    onnx_model = onnx.load(build_state.results)
                    model_opset = getattr(onnx_model.opset_import[0], "version", None)
                    assert model_opset == check_opset

                if check_onnx_file_count:
                    onnx_dir = onnx_helpers.onnx_dir(build_state)
                    assert len(os.listdir(onnx_dir)) == check_onnx_file_count

        assert script_build_found

    # Returns the total number of builds found
    return builds_found
