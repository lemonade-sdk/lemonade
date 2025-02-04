"""
Tests focused on the analysis capabilities of turnkey CLI
"""

import os
import unittest
import glob
import subprocess
import numpy as np
from contextlib import redirect_stdout
from unittest.mock import patch
import io
import sys
from turnkeyml.cli.cli import main as turnkeycli
from turnkeyml.parser import parse
import turnkeyml.common.filesystem as filesystem
import turnkeyml.common.test_helpers as common
import turnkeyml.common.exceptions as exp
from turnkeyml.common.build import output_dir

try:
    # pylint: disable=unused-import
    import transformers
    import timm
except ImportError as e:
    raise ImportError(
        "The Huggingface transformers and timm libraries are required for running this test. "
        "Install them with `pip install transformers timm`"
    )


# We generate a corpus on to the filesystem during the test
# to get around how weird bake tests are when it comes to
# filesystem access

test_scripts_dot_py = {
    "linear_pytorch.py": """# labels: test_group::selftest license::mit framework::pytorch tags::selftest,small
import torch
import argparse

torch.manual_seed(0)

# Receive command line arg
parser = argparse.ArgumentParser()
parser.add_argument(
    "-m",
    "--my-arg",
)
args = parser.parse_args()
print(f"Received arg {args.my_arg}")

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
unexecuted_model = LinearTestModel(input_features+1, output_features)
inputs = {"x": torch.rand(input_features)}
output = model(**inputs)

""",
    "pipeline.py": """
import os
from transformers import (
    TextClassificationPipeline,
    BertForSequenceClassification,
    BertConfig,
    PreTrainedTokenizerFast,
)

tokenizer_file = os.path.join(os.path.dirname(__file__),"tokenizer.json")
class MyPipeline(TextClassificationPipeline):
    def __init__(self, **kwargs):
        configuration = BertConfig()
        tokenizer = PreTrainedTokenizerFast(tokenizer_file=tokenizer_file)
        super().__init__(
            model=BertForSequenceClassification(configuration), tokenizer=tokenizer
        )


my_pipeline = MyPipeline()
my_pipeline("This restaurant is awesome")
""",
    "activation.py": """
import torch
m = torch.nn.GELU()
input = torch.randn(2)
output = m(input)
""",
    "turnkey_parser.py": """
from turnkeyml.parser import parse

parsed_args = parse(["height", "width", "num_channels"])

print(parsed_args)

""",
    "two_executions.py": """
import torch
import timm

# Creating model and set it to evaluation mode
model = timm.create_model("mobilenetv2_035", pretrained=False)
model.eval()

# Creating inputs
inputs1 = torch.rand((1, 3, 28, 28))
inputs2 = torch.rand((1, 3, 224, 224))

# Calling model
model(inputs1)
model(inputs2)
model(inputs1)
""",
}
minimal_tokenizer = """
{
  "version": "1.0",
  "truncation": null,
  "padding": null,
  "added_tokens": [],
  "normalizer": null,
  "pre_tokenizer": null,
  "post_processor": null,
  "decoder": null,
  "model": {
    "type": "WordPiece",
    "unk_token": "[UNK]",
    "continuing_subword_prefix": "##",
    "max_input_chars_per_word": 100,
    "vocab": {
      "[UNK]": 0
    }
  }
}"""


def cache_is_lean(cache_dir, build_name):
    files = list(glob.glob(f"{cache_dir}/builds/{build_name}/**/*", recursive=True))
    is_lean = len([x for x in files if ".onnx" in x]) == 0
    metadata_found = len([x for x in files if ".txt" in x]) > 0
    return is_lean and metadata_found


def run_cli(args):
    with redirect_stdout(io.StringIO()) as f:
        with patch.object(sys, "argv", args):
            turnkeycli()

            return f.getvalue()


def run_analysis(args):
    output = run_cli(args)
    print(output)

    # Process outputs
    output = output[output.rfind("Discovering PyTorch models") :]
    models_executed = output.count("(executed")
    models_built = output.count("Exporting PyTorch to ONNX")
    return models_executed, 0, models_built


def check_discover_log(build_name: str, expected_content: str):
    log_path = os.path.join(output_dir(cache_dir, build_name), "log_discover.txt")
    with open(log_path, "r", encoding="utf-8") as log_file:
        log_content = log_file.read()
        assert expected_content in log_content, log_content


class Testing(unittest.TestCase):
    def setUp(self) -> None:
        filesystem.rmdir(cache_dir)
        return super().setUp()

    def test_01_basic(self):
        pytorch_output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py"),
                "--cache-dir",
                cache_dir,
                "discover",
            ]
        )
        assert np.array_equal(pytorch_output, (1, 0, 0))

    def test_03_depth(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
            ]
        )
        assert np.array_equal(output, (2, 0, 0))

    def test_04_build(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py::76af2f62"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
                "export-pytorch",
            ]
        )
        assert np.array_equal(output, (2, 0, 1))

    def test_05_cache(self):
        model_hash = "76af2f62"
        run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, f"linear_pytorch.py::{model_hash}"),
                "--cache-dir",
                cache_dir,
                "--lean-cache",
                "discover",
                "--max-depth",
                "1",
                "export-pytorch",
            ]
        )
        build_name = f"linear_pytorch_{model_hash}"
        labels_found = filesystem.Stats(cache_dir, build_name).stats[
            filesystem.Keys.LABELS
        ]
        assert cache_is_lean(cache_dir, build_name) and labels_found != {}, labels_found

    def test_06_generic_args(self):
        test_arg = "test_arg"
        run_cli(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
                "--script-args",
                f"--my-arg {test_arg}",
            ]
        )
        check_discover_log("linear_pytorch", f"Received arg {test_arg}")

    def test_07_valid_turnkey_args(self):
        height, width, num_channels = parse(["height", "width", "num_channels"])
        cmd = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, "turnkey_parser.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "--script-args",
            f"--num_channels {num_channels+1}",
        ]
        subprocess.run(cmd)
        expected_output = str([height, width, num_channels + 1])
        check_discover_log("turnkey_parser", expected_output)

    def test_08_invalid_turnkey_args(self):
        cmd = [
            "turnkey",
            "-i",
            os.path.join(corpus_dir, "turnkey_parser.py"),
            "--cache-dir",
            cache_dir,
            "discover",
            "--script-args",
            "--invalid_arg 123",
        ]

        subprocess.run(cmd)
        check_discover_log("turnkey_parser", "error: unrecognized argument")

    def test_09_pipeline(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "pipeline.py"),
                "--cache-dir",
                cache_dir,
                "discover",
            ]
        )
        assert np.array_equal(output, (1, 0, 0))

    def test_10_activation(self):
        with self.assertRaises(exp.ToolError):
            run_analysis(
                [
                    "turnkey",
                    "-i",
                    os.path.join(corpus_dir, "activation.py"),
                    "--cache-dir",
                    cache_dir,
                    "discover",
                ]
            )

    def test_11_analyze_only(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py"),
                "--cache-dir",
                cache_dir,
                "discover",
            ]
        )
        assert np.array_equal(output, (1, 0, 0))

    def test_12_turnkey_hashes(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "linear_pytorch.py::76af2f62"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
                "export-pytorch",
            ]
        )
        assert np.array_equal(output, (2, 0, 1))

    def test_13_clean_cache(self):
        model_hash = "76af2f62"
        run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, f"linear_pytorch.py::{model_hash}"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
                "export-pytorch",
            ]
        )
        build_name = f"linear_pytorch_{model_hash}"

        cmd = [
            "turnkey",
            "--cache-dir",
            cache_dir,
            "cache",
            "--clean",
            "--build-names",
            build_name,
        ]
        subprocess.run(cmd, check=True)

        assert cache_is_lean(cache_dir, build_name)

    def test_14_same_model_different_input_shapes(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "two_executions.py"),
                "--cache-dir",
                cache_dir,
                "discover",
            ]
        )
        assert np.array_equal(output, (2, 0, 0))

    def test_15_same_model_different_input_shapes_maxdepth(self):
        output = run_analysis(
            [
                "turnkey",
                "-i",
                os.path.join(corpus_dir, "two_executions.py"),
                "--cache-dir",
                cache_dir,
                "discover",
                "--max-depth",
                "1",
            ]
        )
        assert np.array_equal(output, (6, 0, 0))


if __name__ == "__main__":
    # Create a test directory
    cache_dir, corpus_dir = common.create_test_dir("analysis", test_scripts_dot_py)

    with open(os.path.join(corpus_dir, "tokenizer.json"), "w", encoding="utf") as f:
        f.write(minimal_tokenizer)

    unittest.main()
