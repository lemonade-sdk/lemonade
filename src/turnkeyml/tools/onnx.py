import os
import shutil
import warnings
import sys
import argparse
import numpy as np
import onnxruntime
import onnxmltools
import onnx
from turnkeyml.tools import Tool, FirstTool
import turnkeyml.common.exceptions as exp
import turnkeyml.common.build as build
import turnkeyml.common.tensor_helpers as tensor_helpers
import turnkeyml.common.onnx_helpers as onnx_helpers
import turnkeyml.common.filesystem as fs
import turnkeyml.common.status as status
from turnkeyml.state import State


def _warn_to_stdout(message, category, filename, line_number, _, line):
    sys.stdout.write(
        warnings.formatwarning(message, category, filename, line_number, line)
    )


def loaded_onnx_file(state: State):
    return os.path.join(
        onnx_helpers.onnx_dir(state),
        f"{state.build_name}-op{state.onnx_opset}-loaded.onnx",
    )


def opt_onnx_file(state: State):
    return os.path.join(
        onnx_helpers.onnx_dir(state),
        f"{state.build_name}-op{state.onnx_opset}-opt.onnx",
    )


def converted_onnx_file(state: State):
    return os.path.join(
        onnx_helpers.onnx_dir(state),
        f"{state.build_name}-op{state.onnx_opset}-opt-f16.onnx",
    )


class LoadOnnx(FirstTool):
    """
    Tool that takes an ONNX model as input and passes it to the following
    tools.

    Expected inputs:
     - Input: a .onnx file

    Outputs:
     - state.result: a .onnx file that has been copied to the turnkey cache
     - state.inputs: valid inputs to that .onnx file
    """

    unique_name = "load-onnx"

    def __init__(self):
        super().__init__(monitor_message="Loading ONNX Model")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load an ONNX model",
            add_help=add_help,
        )

        return parser

    def run(self, state: State, input: str = ""):

        onnx_file = input
        state.model_hash = build.hash_model(onnx_file)

        if not onnx_file.endswith(".onnx"):
            msg = f"""
            The current tool (ReceiveOnnxModel) expects a path to ONNX
            model, however the tool received {onnx_file}.
            """
            raise exp.ToolError(msg)

        state.inputs = onnx_helpers.dummy_inputs(onnx_file)
        dummy_inputs = tuple(state.inputs.values())
        dummy_input_names = tuple(state.inputs.keys())
        state.inputs = dict(zip(dummy_input_names, dummy_inputs))

        model = onnx.load(onnx_file)
        opset = onnx_helpers.get_opset(model)
        state.onnx_opset = opset
        input_shapes = [
            [d.dim_value for d in _input.type.tensor_type.shape.dim]
            for _input in model.graph.input  # pylint: disable=no-member
        ]

        # Save output node names
        state.expected_output_names = onnx_helpers.get_output_names(model)

        # Check for Dynamic shapes in the model. They can be represented as 0, -1, "unk__".
        for input in input_shapes:
            for dimension in input:
                if dimension < 1 or not isinstance(dimension, int):
                    msg = f"""
                    The received model has dynamic input dimensions. Please freeze the model with static
                    input dimensions.
                    More information may be available in the log file at **{self.logfile_path}**
                    """
                    raise exp.ToolError(msg)

        if opset < build.DEFAULT_ONNX_OPSET and opset >= build.MINIMUM_ONNX_OPSET:
            print(
                f" \n The received model has an opset {opset}. Though this opset is supported \
                we recommend upgrading the model to opset {build.MINIMUM_ONNX_OPSET}"
            )
        elif opset < build.MINIMUM_ONNX_OPSET:
            msg = f"""
            The received model has an opset {opset}. Opset < {build.MINIMUM_ONNX_OPSET} 
            is not supported. Please try upgrading the model to opset {build.MINIMUM_ONNX_OPSET}.
            More information may be available in the log file at **{self.logfile_path}**
            """
            raise exp.ToolError(msg)

        output_path = loaded_onnx_file(state)
        os.makedirs(onnx_helpers.onnx_dir(state), exist_ok=True)
        shutil.copy(onnx_file, output_path)

        tensor_helpers.save_inputs(
            [state.inputs],
            onnx_helpers.original_inputs_file(state.cache_dir, state.build_name),
            downcast=False,
        )

        # Check the if the base mode has been exported successfully
        success_msg = "\tSuccess receiving ONNX Model"
        fail_msg = "\tFailed receiving ONNX Model"

        if onnx_helpers.check_model(output_path, success_msg, fail_msg):
            state.results = output_path

            state.save_stat(
                fs.Keys.ONNX_FILE,
                output_path,
            )
        else:
            msg = f"""
            Unable to process ONNX Model. We recommend that you verify the source of the model.
            Any optimizations performed on the model could result in an error.
            More information may be available in the log file at **{self.logfile_path}**
            """
            raise exp.ToolError(msg)

        # Create a UniqueInvocationInfo and ModelInfo so that we can display status
        # at the end of the sequence
        status.add_to_state(
            state=state,
            name=onnx_file,
            model=onnx_file,
            extension=".onnx",
            input_shapes={key: value.shape for key, value in state.inputs.items()},
        )

        return state


class OptimizeOnnxModel(Tool):
    """
    Tool that takes a .onnx file and uses ONNX Runtime to optimize it by
    performing constant folding, redundant node eliminations,
    semantics-preserving node fusions, etc.

    Expected inputs:
     - state.results: a .onnx file

    Outputs:
     - state.results: a *-opt.onnx file
    """

    unique_name = "optimize-ort"

    def __init__(self):
        super().__init__(monitor_message="Optimizing ONNX file")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Use OnnxRuntime to optimize an ONNX model",
            add_help=add_help,
        )

        return parser

    def run(self, state: State):
        input_onnx = state.results
        output_path = opt_onnx_file(state)

        # Perform some basic optimizations on the model to remove shape related
        # information inserted for dynamic shape inference.
        # Given that we're compiling against a fixed sequence length the dynamic
        # shape information is not necessary
        session_options = onnxruntime.SessionOptions()

        # Set graph optimization level
        session_options.graph_optimization_level = (
            onnxruntime.GraphOptimizationLevel.ORT_ENABLE_BASIC
        )

        # To enable model serialization after graph optimization set this
        session_options.optimized_model_filepath = output_path

        # Optimize graph
        onnxruntime.InferenceSession(input_onnx, session_options)

        # Check that the converted model is still valid
        success_msg = "\tSuccess optimizing ONNX model"
        fail_msg = "\tFailed optimizing ONNX model"

        if onnx_helpers.check_model(output_path, success_msg, fail_msg):
            state.results = output_path

            state.save_stat(
                fs.Keys.ONNX_FILE,
                output_path,
            )
        else:
            msg = f"""
            Unable to optimize ONNX file using ONNX runtime.
            We recommend that you modify your model until it is
            compatible with this third party software, then re-run.
            More information may be available in the log file at **{self.logfile_path}**
            """
            raise exp.ToolError(msg)

        return state


class ConvertOnnxToFp16(Tool):
    """
    Tool that takes an ONNX file and converts its trained parameters
    to fp16.

    Expected inputs:
     - state.results: a .onnx file

    Outputs:
     - state.results: a *-f16.onnx file with FP16 trained parameters
    """

    unique_name = "convert-fp16"

    def __init__(self):
        super().__init__(
            monitor_message="Converting to FP16",
        )

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Use OnnxMLTools to convert an ONNX model to fp16",
            add_help=add_help,
        )

        return parser

    def run(self, state: State):
        input_onnx = state.results

        # Convert the model to FP16
        # Some ops will not be converted to fp16 because they are in a block list
        # The latest list can be found here. It is not necessarily the list that
        # our version of onnxmltools sees
        # https://github.com/microsoft/onnxconverter-common/blob/master/onnxconverter_common/float16.py#L82

        # Send onnxmltools warnings to stdout (and therefore the log file)
        # so that they don't fill up the command line
        default_warnings = warnings.showwarning
        warnings.showwarning = _warn_to_stdout

        # Legalize ops are ops that have been or are currently in the block list
        # that we explicitly want removed
        legalize_ops = ["InstanceNormalization", "Resize", "Max"]
        op_block_list = onnxmltools.utils.float16_converter.DEFAULT_OP_BLOCK_LIST.copy()
        for op in legalize_ops:
            # Check to see that they are not in the block list before we remove them
            # Necessary because the block list may be updated, and not in the state we expect
            if op in op_block_list:
                op_block_list.remove(op)

        # Infer shapes before converting to FP16 to enable models with >2GB
        onnx.shape_inference.infer_shapes_path(input_onnx)

        fp32_model = onnx.load_model(input_onnx)
        fp16_model = onnxmltools.utils.float16_converter.convert_float_to_float16(
            fp32_model, op_block_list=op_block_list, disable_shape_infer=True
        )

        # Load inputs and convert to fp16
        inputs_file = onnx_helpers.original_inputs_file(
            state.cache_dir, state.build_name
        )
        if os.path.isfile(inputs_file):
            inputs = np.load(inputs_file, allow_pickle=True)
            inputs_converted = tensor_helpers.save_inputs(
                inputs, inputs_file, downcast=True
            )
        else:
            raise exp.ToolError(
                "Attempted to convert inputs to FP16, however inputs file was not found."
            )

        # Overwrite expected dtypes
        _, state.expected_input_dtypes = build.get_shapes_and_dtypes(
            inputs_converted[0]
        )

        # Indicate that inputs must be downcasted during inference
        state.downcast_applied = True

        # Save FP16 model (use external data format if needed)
        output_path = converted_onnx_file(state)
        try:
            onnxmltools.utils.save_model(fp16_model, output_path)
        except ValueError:
            onnx.save_model(fp16_model, output_path, save_as_external_data=True)

        # Restore default warnings behavior
        warnings.showwarning = default_warnings

        # Check that the converted model is still valid
        success_msg = "\tSuccess converting ONNX model to fp16"
        fail_msg = "\tFailed converting ONNX model to fp16"

        if onnx_helpers.check_model(output_path, success_msg, fail_msg):
            state.results = output_path

            state.save_stat(
                fs.Keys.ONNX_FILE,
                output_path,
            )
        else:
            msg = f"""
            Attempted to use onnxmltools, a third party library, to convert your
            model to the float16 datatype, however this operation was not successful.
            More information may be available in the log file at **{self.logfile_path}**
            """
            raise exp.ToolError(msg)

        return state
