import os
import inspect
import warnings
import sys
import copy
import argparse
import torch
import torch.onnx.verification
from turnkeyml.tools import Tool
import turnkeyml.common.exceptions as exp
import turnkeyml.common.build as build
import turnkeyml.common.tensor_helpers as tensor_helpers
import turnkeyml.common.onnx_helpers as onnx_helpers
import turnkeyml.common.filesystem as fs
from turnkeyml.state import State


def _warn_to_stdout(message, category, filename, line_number, _, line):
    sys.stdout.write(
        warnings.formatwarning(message, category, filename, line_number, line)
    )


def base_onnx_file(state: State):
    return os.path.join(
        onnx_helpers.onnx_dir(state),
        f"{state.build_name}-op{state.onnx_opset}-base.onnx",
    )


class ExportPytorchModel(Tool):
    """
    Tool that takes a PyTorch model instance, from the state of the previous
    tool in the sequence, and exports it to an ONNX file.

    Expected inputs:
     - state.results: torch.nn.Module or torch.jit.ScriptModule
     - state.inputs: dict that represents valid kwargs to the forward
        function of state.results

    Outputs:
     - state.results: a *-base.onnx file that implements state.results
        given state.inputs
    """

    unique_name = "export-pytorch"

    def __init__(self):
        super().__init__(monitor_message="Exporting PyTorch to ONNX")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Export a PyTorch model to ONNX",
            add_help=add_help,
        )

        parser.add_argument(
            "--opset",
            type=int,
            default=build.DEFAULT_ONNX_OPSET,
            help=f"ONNX opset to export into (default: {build.DEFAULT_ONNX_OPSET})",
        )

        return parser

    def run(self, state: State, opset: int = build.DEFAULT_ONNX_OPSET):
        if not isinstance(state.results, (torch.nn.Module, torch.jit.ScriptModule)):
            msg = f"""
            The current tool (ExportPytorchModel) is only compatible with
            models of type torch.nn.Module or torch.jit.ScriptModule, however
            the tool received a model of type {type(state.results)}.
            """
            raise exp.ToolError(msg)

        state.onnx_opset = opset

        # The `torch.onnx.export()` function accepts a tuple of positional inputs
        # followed by a dictionary with all keyword inputs.
        # The dictionary must be last item in tuple.
        user_provided_args = list(state.inputs.keys())

        if isinstance(state.results, torch.nn.Module):
            # Validate user provided args
            all_args = list(inspect.signature(state.results.forward).parameters.keys())

            for inp in user_provided_args:
                if inp not in all_args:
                    msg = f"""
                    Input name {inp} not found in the model's forward method. Available
                    input names are: {all_args}"
                    """
                    raise ValueError(msg)

            # Most pytorch models have args that are kind = positional_or_keyword.
            # The `torch.onnx.export()` function accepts model args as
            #     (all_positional_args_value,{keyword_arg:value}).
            # To map the input_args correctly and to build an accurate model
            # the order of the input_names must reflect the order of the model args.

            # Collect order of pytorch model args.
            all_args_order_mapping = {arg: idx for idx, arg in enumerate(all_args)}

            # Sort the user provided inputs with respect to model args and store as tuple.
            sorted_user_inputs = sorted(
                user_provided_args, key=lambda x: all_args_order_mapping[x]
            )
            dummy_input_names = tuple(sorted_user_inputs)

            # If a single input is provided torch.onnx.export will
            # not accept a dictionary, so pop the first arg
            user_args = copy.deepcopy(state.inputs)
            first_input = user_args.pop(dummy_input_names[0])

            # Create tuple: (first input, {rest of user_args dict as keyword args})
            dummy_inputs = (first_input, user_args)

        else:  # state.results is a torch.jit.ScriptModule
            dummy_inputs = tuple(state.inputs.values())

            # Collect input names
            dummy_input_names = tuple(state.inputs.keys())

        # Send torch export warnings to stdout (and therefore the log file)
        # so that they don't fill up the command line
        default_warnings = warnings.showwarning
        warnings.showwarning = _warn_to_stdout

        # Export the model to ONNX
        output_path = base_onnx_file(state)
        os.makedirs(onnx_helpers.onnx_dir(state), exist_ok=True)

        torch.onnx.export(
            state.results,
            dummy_inputs,
            output_path,
            input_names=dummy_input_names,
            do_constant_folding=True,
            opset_version=opset,
            verbose=False,
        )

        # Save output names to ensure we are preserving the order of the outputs
        state.expected_output_names = onnx_helpers.get_output_names(output_path)

        # Restore default warnings behavior
        warnings.showwarning = default_warnings

        tensor_helpers.save_inputs(
            [state.inputs],
            onnx_helpers.original_inputs_file(state.cache_dir, state.build_name),
            downcast=False,
        )

        # Check the if the base mode has been exported successfully
        success_msg = "\tSuccess exporting model to ONNX"
        fail_msg = "\tFailed exporting model to ONNX"

        if onnx_helpers.check_model(output_path, success_msg, fail_msg):
            state.results = output_path

            state.save_stat(
                fs.Keys.ONNX_FILE,
                output_path,
            )
        else:
            msg = f"""
            Unable to export model to ONNX using Torch's ONNX exporter.
            We recommend that you modify your model until it is
            compatible with this third party software, then re-run.
            More information may be available in the log file at **{self.logfile_path}**
            """
            raise exp.ToolError(msg)

        return state


class VerifyOnnxExporter(Tool):
    """
    Tool that runs a parity test on an input PyTorch model and an ONNX
    file derived from that model.

    Note that the derived ONNX file is discarded by the verification API,
    so we can't use it in downstream Tools. To use this tool in the same sequence
    as other build tools, we recommend:
        discover -> verify-exporter -> export-pytorch -> other tools

    Expected inputs:
     - state.results: torch.nn.Module or torch.jit.ScriptModule
     - state.inputs: dict that represents valid kwargs to the forward
        function of state.results

    Outputs: No change to state
    """

    unique_name = "verify-exporter"

    def __init__(self):
        super().__init__(monitor_message="Verifying ONNX exporter")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Verify if model can be exported to ONNX without major "
            "numerical discrepancies",
            add_help=add_help,
        )

        return parser

    def run(self, state: State):

        # Verify if the exported model matches the input torch model
        try:
            # Tolerance levels for the torch export are recommended by Pytorch here:
            # https://pytorch.org/docs/stable/testing.html#module-torch.testing
            fp32_tolerance = torch.onnx.verification.VerificationOptions(
                rtol=1.3e-6, atol=1e-5
            )

            # The `torch.onnx.verification.find_mismatch()` takes input arguments to the
            # model as `input_args (Tuple[Any, ...])`
            export_verification = torch.onnx.verification.find_mismatch(
                state.results,
                tuple(state.inputs.values()),
                opset_version=state.onnx_opset,
                options=fp32_tolerance,
            )

            # `export_verification.has_mismatch()` returns True if a mismatch is found and
            # False otherwise. If no mismatch is found,# `is_export_valid` is set to "Valid",
            # indicating successful verification.
            # If a mismatch is found, `is_export_valid` is set to "Invalid", indicating
            # the verification failed.
            if not export_verification.has_mismatch():
                is_export_valid = "valid"
            else:
                is_export_valid = "invalid"

        # The except block catches any type of exception that might occur during the
        # verification process. If any exception occurs,`is_export_valid` is set to
        # "Unverified", indicating that the verification process could not be completed,
        # and therefore the model's export status is unverified.
        except Exception:  # pylint: disable=broad-except
            is_export_valid = "unverified"

        state.save_stat(
            fs.Keys.TORCH_ONNX_EXPORT_VALIDITY,
            is_export_valid,
        )

        return state
