import argparse
import copy
import os
import inspect
from typing import Optional, List
import torch
from turnkeyml.tools import FirstTool
import turnkeyml.common.build as build
import turnkeyml.common.exceptions as exp
import turnkeyml.common.filesystem as fs
from turnkeyml.tools.discovery.script import (
    evaluate_script,
    TracerArgs,
)
import turnkeyml.common.printing as printing
from turnkeyml.state import State


default_max_depth = 0


class Discover(FirstTool):
    """
    Discover the PyTorch models and their corresponding inputs in a python script (.py)
    and send one model/inputs pair onwards into the sequence.

    Expected inputs:
     - Input file is a python script (.py file) that invokes at least one PyTorch model

    Outputs:
     - state.results: a PyTorch model instance (torch.nn.Module)
     - state.inputs: a dictionary of example inputs to the model's forward function,
            e.g., model(**inputs)

    You can learn more about how discovery and its arguments work at
    https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/tools_user_guide.md
    """

    unique_name = "discover"

    def __init__(self):
        super().__init__(monitor_message="Discovering PyTorch models")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Discover the PyTorch models in a python script",
            add_help=add_help,
        )

        parser.add_argument(
            "--script-args",
            dest="script_args",
            type=str,
            help="Arguments to pass into the target script(s)",
        )

        parser.add_argument(
            "--max-depth",
            dest="max_depth",
            type=int,
            default=default_max_depth,
            help="Maximum depth to analyze within the model structure of the target script(s)",
        )

        return parser

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        parsed_args = super().parse(state, args, known_only)

        file_path, targets, encoded_input = fs.decode_input_arg(parsed_args.input)

        parsed_args.input = file_path

        if len(targets) > 1:
            raise exp.ArgError(
                "Only one target (number after the ::) is allowed, "
                f"but received {encoded_input}"
            )
        elif len(targets) == 1:
            parsed_args.target = targets[0]
        else:  # len(targets)==0
            parsed_args.target = None

        return parsed_args

    def run(
        self,
        state: State,
        input: str = "",
        target: Optional[List[str]] = None,
        script_args: str = "",
        max_depth: int = default_max_depth,
    ):
        if not input.endswith(".py"):
            raise exp.ArgError(
                "Inputs to the `discover` tool must by python scripts "
                f"(.py files), got {input}",
            )

        if target is None:
            target_to_use = []
        else:
            target_to_use = [target]

        tracer_args = TracerArgs(
            input=input,
            script_args=script_args,
            targets=target_to_use,
            max_depth=max_depth,
        )

        # Discover the models in the python script by executing it with
        # a tracer enabled
        state.models_found = evaluate_script(tracer_args)

        # Count the amount of build-able model invocations discovered
        # If there is only 1, pass it to the next build tool. Otherwise,
        # print all the invocations and suggest that the user select one.
        count = 0
        for model_info in state.models_found.values():
            for (
                invocation_hash,
                invocation_info,
            ) in model_info.unique_invocations.items():
                count += 1

                # Set the same status for all invocations at first
                # The next code block will be responsible for the selected
                # invocation.

                invocation_info.status_message = (
                    "Discovered; select with `-i "
                    f"{os.path.basename(input)}::{invocation_hash}"
                )
                invocation_info.status_message_color = printing.Colors.OKCYAN

        # The potential outcomes of target selection are:
        #   Case 1. Input file has only one model, so we select it and don't
        #       bother the user about target selection
        #   Case 2. Input file has more than one model, and...
        #           a. user didn't select a target, so we auto-select the
        #               least-deep (last discovered) model and let the user
        #               know about target selection
        #           b. user selected a target, so we run with it
        #   Case 3. Exception: Input file contained no models
        #   Case 4. Exception: input file has one or more model, but user
        #               selected an invalid target
        #
        # The purpose of this loop is to identify which of those cases is
        # active.

        if count == 0:
            # Case 3
            raise exp.ToolError(f"No models discovered in input file {input}")

        model_selected = None
        invocation_selected = None
        valid_hashes = []
        case_1 = target is None and count == 1
        case_2a = target is None and count > 1
        for model_info in state.models_found.values():
            for invocation_info in model_info.unique_invocations.values():
                valid_hashes.append(invocation_info.invocation_hash)

                case_2b = (
                    target is not None and invocation_info.invocation_hash == target
                )

                if any([case_1, case_2b]):
                    model_selected = model_info
                    state.invocation_info = invocation_info
                    break
                if case_2a:
                    # Point to the most recent model and invocation identified
                    # We do this so that we can auto-select the last model and invocation
                    # that was discovered, which is typically the least-deep model
                    # because discovery is recursive.
                    model_selected = model_info
                    invocation_selected = invocation_info

            if vars(state).get("invocation_info") is not None:
                # If we have already selected then there is no need to keep iterating
                break

        if model_selected is None:
            # Case 4
            raise exp.ToolError(
                f"Hash {target} was selected, but the only "
                f"valid hashes are {valid_hashes}"
            )

        if case_2a:
            state.invocation_info = invocation_selected
            state.invocation_info.extra_status = (
                "(auto-selected; select manually with "
                f"`-i {os.path.basename(input)}"
                f"::{state.invocation_info.invocation_hash})"
            )

        # Save stats about the model
        state.save_stat(
            fs.Keys.HASH,
            model_selected.hash,
        )
        state.save_stat(
            "selected_invocation_hash",
            state.invocation_info.invocation_hash,
        )
        state.save_stat(
            fs.Keys.MODEL_NAME,
            tracer_args.script_name,
        )
        state.save_stat(
            fs.Keys.PARAMETERS,
            model_selected.params,
        )

        state.save_stat(
            fs.Keys.CLASS,
            type(model_selected.model).__name__,
        )

        # Organize the inputs to python model instances
        args, kwargs = state.invocation_info.inputs
        inputs = {}
        for k in kwargs.keys():
            if torch.is_tensor(kwargs[k]):
                inputs[k] = torch.tensor(kwargs[k].detach().numpy())
            else:
                inputs[k] = copy.deepcopy(kwargs[k])

        # Convert all positional arguments into keyword arguments
        if args != ():

            forward_function = model_info.model.forward
            all_args = list(inspect.signature(forward_function).parameters.keys())
            for i in range(len(args)):
                if torch.is_tensor(args[i]):
                    inputs[all_args[i]] = torch.tensor(args[i].detach().numpy())
                else:
                    inputs[all_args[i]] = args[i]

        # Pass the model and inputs to the next tool
        state.results = model_selected.model
        state.model_hash = build.hash_model(model_selected.model)
        state.expected_input_shapes, state.expected_input_dtypes = (
            build.get_shapes_and_dtypes(inputs)
        )
        state.inputs = inputs

        return state
