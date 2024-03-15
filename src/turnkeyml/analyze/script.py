import sys
import os
import inspect
import importlib.util
import copy
import time
import shlex
import functools
import dataclasses
import traceback
import hashlib
from datetime import datetime
from typing import Union, List, Dict, Tuple, Optional
from types import FrameType, TracebackType
from enum import Enum
import torch
import git
from turnkeyml.common import printing
import turnkeyml.common.build as build
import turnkeyml.common.exceptions as exp
from turnkeyml.build.stage import Sequence
import turnkeyml.analyze.status as status
import turnkeyml.analyze.model as analyze_model
import turnkeyml.common.tf_helpers as tf_helpers
import turnkeyml.common.labels as labels
from turnkeyml.build_api import build_model
import turnkeyml.common.filesystem as fs
import turnkeyml.run.devices as plugins


class Action(Enum):
    ANALYZE = "analyze"
    BUILD = "build"
    BENCHMARK = "benchmark"


@dataclasses.dataclass
class TracerArgs:
    input: str
    script_args: str
    device: str
    runtime: str
    iterations: int
    actions: List[Action]
    lean_cache: bool
    targets: List[str]
    max_depth: int
    onnx_opset: int
    cache_dir: str
    rebuild: str
    models_found: Dict[str, status.ModelInfo] = dataclasses.field(default_factory=dict)
    script_name: Optional[str] = None
    sequence: Optional[Sequence] = None
    rt_args: Optional[Dict] = None
    verbosity: status.Verbosity = status.Verbosity.DYNAMIC

    @functools.cached_property
    def labels(self) -> Dict[str, str]:
        # Load labels data from python scripts
        # This is not compatible with ONNX files, so we return
        # and empty dictionary in that case
        if self.input.endswith(".py"):
            return labels.load_from_file(self.input)
        else:
            return {}

    @functools.cached_property
    def torch_activations(self) -> List[str]:
        act = tf_helpers.get_classes(torch.nn.modules.activation)
        act += tf_helpers.get_transformers_activations()
        return act

    @property
    def saveable_dict(self) -> Dict:
        """
        Convert TracerArgs data into a dictionary that is safe to save to YAML format.
        All members must be str, List[str], or Dict[str]
        """

        result = {}

        # Get each field from this dataclass, which corresponds
        # all of the turnkey API/CLI args we want to save
        for field in dataclasses.fields(self):
            # Get the value corresponding to each field
            arg_value = getattr(self, field.name)

            # Some of the args have types that are compatible with the YAML
            # format, so we will need to ignore them or process them before saving
            saved_value = None

            if field.name == "models_found":
                # Do not include "models_found" because
                # 1. It spans multiple invocations
                # 2. Includes all the weights of all the models
                continue
            elif isinstance(arg_value, Sequence):
                # `sequence` can be a str or Sequence
                # If we receive an instance of Sequence, we need to convert it
                # to a string to save it to YAML
                saved_value = arg_value.sequence.__class__.__name__
            elif isinstance(arg_value, status.Verbosity):
                saved_value = arg_value.value
            elif isinstance(arg_value, list) and any(
                isinstance(arg_sub_value, Action) for arg_sub_value in arg_value
            ):
                # The --build-only and --analyze-only args are gone by this point in
                # the code and are replaced by a list of Actions. We need to convert each Action
                # enum into a str to save it to YAML
                saved_value = [arg_sub_value.value for arg_sub_value in arg_value]
            else:
                # All other field types can be saved directly
                saved_value = arg_value

            if saved_value:
                result[field.name] = saved_value

        return result

    def __str__(self) -> str:
        result = ""
        for key, value in self.saveable_dict.items():
            result = result + f"{key} {value} "

        return result

    @property
    def hash(self) -> str:
        """
        Returns a unique hash representing the arguments. Useful for distinguishing
        between evaluations of the same model that have different arguments.
        """

        return hashlib.sha256(str(self).encode()).hexdigest()[:8]


def _store_traceback(invocation_info: status.UniqueInvocationInfo):
    """
    Store the traceback from an exception into invocation_info so that
    we can print it during the status update.
    """

    exc_type, exc_value, exc_traceback = sys.exc_info()
    invocation_info.traceback = traceback.format_exception(
        exc_type, exc_value, exc_traceback
    )

    # Remove line breaks and sequences of spaces from status message
    invocation_info.status_message = " ".join(invocation_info.status_message.split())


def set_status_on_exception(
    build_required: bool,
    build_state: build.State,
    stats: fs.Stats,
    benchmark_logfile_path: str,
):
    """
    Determine whether an exception was caused by build or benchmark,
    and then record statistics to help with debugging.
    """
    # We get `state` when the build tool succeeds, so we can use that to identify
    # whether the exception was thrown during build or benchmark
    # We also take into account whether a build was requested
    if build_required and not build_state:
        stats.save_model_eval_stat(
            fs.Keys.BUILD_STATUS, build.FunctionStatus.ERROR.value
        )

        # NOTE: The log file for the failed build stage should have
        # already been saved to stats
    else:
        stats.save_model_eval_stat(
            fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.ERROR.value
        )

        # Also save the benchmark log file to the stats
        stats.save_eval_error_log(benchmark_logfile_path)


def explore_invocation(
    model_inputs: dict,
    model_info: status.ModelInfo,
    invocation_info: status.UniqueInvocationInfo,
    tracer_args: TracerArgs,
) -> None:
    """
    Calls the turnkey function from within the model forward function
    """

    # Update status to "computing"
    invocation_info.status_message = "Computing..."
    invocation_info.status_message_color = printing.Colors.OKBLUE

    build_name = fs.get_build_name(
        tracer_args.script_name, tracer_args.labels, invocation_info.invocation_hash
    )
    status.update(
        tracer_args.models_found,
        build_name,
        tracer_args.cache_dir,
        invocation_info,
        tracer_args.verbosity,
    )

    # Organize the inputs to python model instances
    # Not necessary for ONNX models
    if model_info.model_type == build.ModelType.ONNX_FILE:
        inputs = model_inputs
    else:
        # Get a copy of the keyword arguments
        args, kwargs = model_inputs
        inputs = {}
        for k in kwargs.keys():
            if torch.is_tensor(kwargs[k]):
                inputs[k] = torch.tensor(kwargs[k].detach().numpy())
            else:
                inputs[k] = copy.deepcopy(kwargs[k])

        # Convert all positional arguments into keyword arguments
        if args != ():
            if model_info.model_type in [
                build.ModelType.PYTORCH,
                build.ModelType.PYTORCH_COMPILED,
            ]:
                forward_function = model_info.model.forward
            elif model_info.model_type == build.ModelType.KERAS:
                forward_function = model_info.model.call
            all_args = list(inspect.signature(forward_function).parameters.keys())
            for i in range(len(args)):
                if torch.is_tensor(args[i]):
                    inputs[all_args[i]] = torch.tensor(args[i].detach().numpy())
                else:
                    inputs[all_args[i]] = args[i]
        invocation_info.inputs = inputs

    # Create a build directory in the cache
    fs.make_build_dir(tracer_args.cache_dir, build_name)

    # If the user has not provided a specific runtime, select the runtime
    # based on the device provided.
    (
        selected_runtime,
        runtime_info,
        sequence_selected,
    ) = plugins.select_runtime_and_sequence(
        tracer_args.device,
        tracer_args.runtime,
        tracer_args.sequence,
    )

    if "status_stats" in runtime_info.keys():
        invocation_info.stats_keys = runtime_info["status_stats"]
    else:
        invocation_info.stats_keys = []

    # Create an ID for the build stats by combining the device and runtime.
    # We don't need more info in the evaluation_id because changes to build_model()
    # arguments (e.g., sequence) will trigger a rebuild, which is intended to replace the
    # build stats so long as the device and runtime have not changed.
    evaluation_id = tracer_args.hash

    stats = fs.Stats(
        tracer_args.cache_dir,
        build_name,
        evaluation_id,
    )
    invocation_info.stats = stats

    # Print the evaluation ID so that we can access it in case this process is killed
    print(
        f"Capturing statistics in turnkey_stats.yaml under evaluation ID: {evaluation_id}"
    )

    # Stats that apply to the model, regardless of build
    stats.save_model_stat(
        fs.Keys.HASH,
        model_info.hash,
    )
    stats.save_model_stat(
        fs.Keys.MODEL_NAME,
        tracer_args.script_name,
    )
    stats.save_model_stat(
        fs.Keys.PARAMETERS,
        model_info.params,
    )
    if model_info.model_type != build.ModelType.ONNX_FILE:
        stats.save_model_stat(fs.Keys.CLASS, type(model_info.model).__name__)
    if fs.Keys.AUTHOR in tracer_args.labels:
        stats.save_model_stat(fs.Keys.AUTHOR, tracer_args.labels[fs.Keys.AUTHOR][0])
    if fs.Keys.TASK in tracer_args.labels:
        stats.save_model_stat(fs.Keys.TASK, tracer_args.labels[fs.Keys.TASK][0])

    # Save the system information used for this evaluation
    system_info = build.get_system_info()
    stats.save_model_stat(
        fs.Keys.SYSTEM_INFO,
        system_info,
    )

    # Save all of the lables in one place
    stats.save_model_stat(fs.Keys.LABELS, tracer_args.labels)

    # If the input script is a built-in TurnkeyML model, make a note of
    # which one
    if os.path.abspath(fs.MODELS_DIR) in os.path.abspath(tracer_args.input):
        try:
            # If this turnkey installation is in a git repo, use the
            # specific git hash
            git_repo = git.Repo(search_parent_directories=True)
            git_hash = git_repo.head.object.hexsha
        except git.exc.InvalidGitRepositoryError:
            # If we aren't in a git repo (e.g., PyPI package), point the user back to main
            git_hash = "main"

        relative_path = tracer_args.input.replace(
            fs.MODELS_DIR,
            f"https://github.com/onnx/turnkeyml/tree/{git_hash}/models",
        ).replace("\\", "/")
        stats.save_model_stat(fs.Keys.MODEL_SCRIPT, relative_path)

    # Evaluation-specific stats

    # Save all of the turnkey arguments into a single key to help
    # with reproducibility
    stats.save_model_eval_stat(
        fs.Keys.EVALUATION_ARGS,
        tracer_args.saveable_dict,
    )

    # Save a timestamp so that we know the order of evaluations within a cache
    stats.save_model_eval_stat(
        fs.Keys.TIMESTAMP,
        datetime.now(),
    )

    # Save specific information into its own key for easier access
    stats.save_model_eval_stat(
        fs.Keys.DEVICE_TYPE,
        tracer_args.device,
    )
    stats.save_model_eval_stat(
        fs.Keys.RUNTIME,
        selected_runtime,
    )
    stats.save_model_eval_stat(
        fs.Keys.ITERATIONS,
        tracer_args.iterations,
    )

    if model_info.model_type == build.ModelType.PYTORCH_COMPILED:
        invocation_info.status_message = (
            "Skipping model compiled using torch.compile(). "
            "turnkey requires models to be in eager mode "
            "(regardless of what runtime you have selected)."
        )
        invocation_info.status_message_color = printing.Colors.WARNING

        return

    # Initialize build and benchmark status to "not started" if
    # that action is part of the evaluation
    if runtime_info["build_required"]:
        stats.save_model_eval_stat(
            fs.Keys.BUILD_STATUS, build.FunctionStatus.NOT_STARTED.value
        )

    if Action.BENCHMARK in tracer_args.actions:
        stats.save_model_eval_stat(
            fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.NOT_STARTED.value
        )

        # Save the device name that will be used for the benchmark
        stats.save_model_eval_stat(
            fs.Keys.DEVICE, runtime_info["RuntimeClass"].device_name()
        )

    build_state = None
    perf = None
    benchmark_logfile_path = ""
    try:
        # Run the build tool (if needed by the runtime)
        if runtime_info["build_required"]:
            # Indicate that the build is running. If the build fails for any reason,
            # we will try to catch the exception and note it in the stats.
            # If a concluded build still has a status of "running", this means
            # there was an uncaught exception.
            stats.save_model_eval_stat(
                fs.Keys.BUILD_STATUS, build.FunctionStatus.INCOMPLETE.value
            )

            build_state = build_model(
                model=model_info.model,
                inputs=inputs,
                evaluation_id=evaluation_id,
                build_name=build_name,
                cache_dir=tracer_args.cache_dir,
                rebuild=tracer_args.rebuild,
                sequence=sequence_selected,
                onnx_opset=tracer_args.onnx_opset,
                device=tracer_args.device,
            )

            stats.save_model_eval_stat(
                fs.Keys.BUILD_STATUS, build.FunctionStatus.SUCCESSFUL.value
            )

            model_to_benchmark = build_state.results[0]

            # Analyze the onnx file (if any) and save statistics
            analyze_model.analyze_onnx(
                build_name=build_name,
                cache_dir=tracer_args.cache_dir,
                stats=stats,
            )
        else:
            model_to_benchmark = model_info.model

        # Run the benchmark tool (if requested by the user)
        if Action.BENCHMARK in tracer_args.actions:
            if tracer_args.rt_args is None:
                rt_args_to_use = {}
            else:
                rt_args_to_use = tracer_args.rt_args

            stats.save_model_eval_stat(
                fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.INCOMPLETE.value
            )

            runtime_handle = runtime_info["RuntimeClass"](
                cache_dir=tracer_args.cache_dir,
                build_name=build_name,
                stats=stats,
                iterations=tracer_args.iterations,
                model=model_to_benchmark,
                inputs=inputs,
                device_type=tracer_args.device,
                runtime=selected_runtime,
                **rt_args_to_use,
            )
            benchmark_logfile_path = runtime_handle.logfile_path
            perf = runtime_handle.benchmark()

            for key, value in vars(perf).items():
                stats.save_model_eval_stat(
                    key=key,
                    value=value,
                )

            stats.save_model_eval_stat(
                fs.Keys.BENCHMARK_STATUS, build.FunctionStatus.SUCCESSFUL.value
            )

            invocation_info.status_message = "Model successfully benchmarked!"
            invocation_info.performance = perf
            invocation_info.status_message_color = printing.Colors.OKGREEN
        else:
            invocation_info.status_message = "Model successfully built!"
            invocation_info.status_message_color = printing.Colors.OKGREEN

    except exp.StageError as e:
        invocation_info.status_message = f"Build Error: {e}"
        invocation_info.status_message_color = printing.Colors.WARNING

        set_status_on_exception(
            runtime_info["build_required"], build_state, stats, benchmark_logfile_path
        )

        _store_traceback(invocation_info)

    except exp.SkipBuild:
        # SkipBuild is an exception that the build_model() API will raise
        # when it is skipping a previously-failed build when rebuild=never is set

        # NOTE: skipping a build should never update build or benchmark status

        invocation_info.status_message = (
            "Build intentionally skipped because rebuild=never"
        )
        invocation_info.status_message_color = printing.Colors.WARNING

    except exp.ArgError as e:
        # ArgError indicates that some argument to build_model() or BaseRT was
        # illegal. In that case we want to halt execution so that users can
        # fix their arguments.

        set_status_on_exception(
            runtime_info["build_required"], build_state, stats, benchmark_logfile_path
        )

        raise e

    except exp.Error as e:
        invocation_info.status_message = f"Error: {e}."
        invocation_info.status_message_color = printing.Colors.WARNING

        set_status_on_exception(
            runtime_info["build_required"], build_state, stats, benchmark_logfile_path
        )

        _store_traceback(invocation_info)

    # This broad exception is ok since enumerating all exceptions is
    # not possible, as the tested software continuously evolves.
    except Exception as e:  # pylint: disable=broad-except
        invocation_info.status_message = f"Unknown turnkey error: {e}"
        invocation_info.status_message_color = printing.Colors.WARNING

        set_status_on_exception(
            runtime_info["build_required"], build_state, stats, benchmark_logfile_path
        )

        _store_traceback(invocation_info)

    finally:
        # Ensure that stdout/stderr is not being forwarded before updating status
        status.stop_logger_forward()

        status.update(
            tracer_args.models_found,
            build_name,
            tracer_args.cache_dir,
            invocation_info,
            tracer_args.verbosity,
        )

        if tracer_args.lean_cache:
            printing.log_info("Removing build artifacts...")
            fs.clean_output_dir(tracer_args.cache_dir, build_name)


def get_model_hash(
    model: Union[torch.nn.Module, "tf.keras.Model", str], model_type: build.ModelType
):
    return build.hash_model(
        model, model_type, hash_params=model_type == build.ModelType.ONNX_FILE
    )[:8]


def get_invocation_hash(
    model_hash: str, parent_invocation_hash: str, args: Tuple, kwargs: Dict
) -> str:
    """
    Combines the model hash and the input shapes to create the invocation hash
    We also ensure that invocations that come from different parents have different hashes
    """

    # Merge positional and keyword args
    args = {"Positional Arg {}".format(i + 1): arg for i, arg in enumerate(args)}
    kwargs = {**kwargs, **args}

    # Get input shapes and types
    input_shapes, input_dtypes = build.get_shapes_and_dtypes(kwargs)

    hashable_content = (
        f"{model_hash}{parent_invocation_hash}{input_shapes}{input_dtypes}"
    )
    return hashlib.sha256(hashable_content.encode()).hexdigest()[:8], input_shapes


def store_model_info(
    model: Union[torch.nn.Module, "tf.keras.Model"],
    model_name: str,
    model_type: build.ModelType,
    frame: FrameType,
    event: str,
    tracer_args: TracerArgs,
    depth: int,
    parent_hash: str,
):
    # Getting the model hash is only possible after the first inference of Keras models
    model_hash = get_model_hash(model, model_type)

    # File where the model was found
    file = str(frame)[str(frame).find("file ") + 6 : str(frame).find("',")]

    # Line where the model was found
    line = frame.f_lineno if event == "return" else frame.f_lineno - 1

    # Keep track of all models details

    # If we have already found a model, don't add it to models_found again
    # We have to use both the model hash and the script name, since we don't
    # want to ignore a model if it was explicitly called in two different scripts
    identifier = f"{model_hash}_{tracer_args.script_name}"
    model_already_found = False
    for model_info in tracer_args.models_found.values():
        if identifier == f"{model_info.hash}_{model_info.script_name}":
            model_already_found = True

    if not model_already_found:
        build_model = Action.BUILD in tracer_args.actions
        tracer_args.models_found[model_hash] = status.ModelInfo(
            model=model,
            name=model_name,
            file=file,
            line=line,
            depth=depth,
            hash=model_hash,
            parent_hash=parent_hash,
            build_model=build_model,
            model_type=model_type,
            script_name=tracer_args.script_name,
        )


def explore_frame(
    frame,
    event,
    local_var_name,
    local_var,
    tracer_args: TracerArgs,
    depth: int = 0,
    parent_hash: Union[str, None] = None,
):
    """
    This function checks whether local_var is a torch or keras model.
    If it is, we will modify its forward function to know when it
    is called.
    """

    # Exit frame exploration if Python is shutting down
    if not bool(sys.modules):
        return

    # Skip all variables that are not a subclass of torch.nn.Module/tf.keras.Model
    # Note: try block used since dead weakreferences fail when checking subclass
    try:
        if issubclass(type(local_var), torch.nn.Module):
            if type(local_var) in tracer_args.torch_activations:
                return
            if "dynamo_ctx" in local_var.__dict__:
                model_type = build.ModelType.PYTORCH_COMPILED
            else:
                model_type = build.ModelType.PYTORCH
        elif tf_helpers.is_keras_subclass(type(local_var)):
            model_type = build.ModelType.KERAS
        else:
            return
    except AttributeError:
        return

    # Skip self variable and variable names commonly used by child models
    if (
        local_var_name == "self"
        or local_var_name == "instance"
        or local_var_name == "child"
        or local_var_name == "layer"
        or local_var_name == "module"
    ):
        return

    # Check if we are inside of a subclass of torch.nn.Module or tf.keras.model
    inside_class = False
    inside_nn_subclass = False
    if "self" in frame.f_locals:
        self_var = frame.f_locals["self"]
        inside_class = type(self_var)
        inside_nn_subclass = issubclass(
            inside_class, torch.nn.Module
        ) or tf_helpers.is_keras_subclass(inside_class)

    if not inside_nn_subclass:
        if hasattr(local_var, "forward_instrumented"):
            # A previously-found model might have been compiled
            # Update that information if needed
            if model_type == build.ModelType.PYTORCH_COMPILED:
                tracer_args.models_found[local_var.turnkey_hash].model_type = (
                    build.ModelType.PYTORCH_COMPILED
                )

            # Starting in version 2.2.0, torch dynamo added wrappers to callbacks
            # while tracing frames, which conflicts with TurnkeML's analysis. Here,
            # we suppress errors caused by those callback wrappers and only raise an
            # error if the compiled model actually tries to execute within TurnkeyML.
            td = torch._dynamo  # pylint: disable=protected-access
            td.config.suppress_errors = True
            if hasattr(td.eval_frame, "guarded_backend_cache"):
                td.eval_frame.guarded_backend_cache.skip_backend_check_for_run_only_mode = (
                    True
                )

            return

        if model_type == build.ModelType.PYTORCH:
            # Avoid instrumenting models before they have been fully loaded
            if analyze_model.count_parameters(local_var, model_type) == 0:
                return

            # Mark this model as instrumented
            local_var.forward_instrumented = True

            # Create a copy of the old forward function
            old_forward = local_var.forward

            # Recursively look for sub-models within the found model
            # This is only possible on Pytorch, since each layer of a torch.nn.module
            # is also a torch.nn.module.
            model_hash = get_model_hash(local_var, model_type)
            local_var.turnkey_hash = model_hash
            if depth < tracer_args.max_depth:
                recursive_search(
                    frame, event, local_var, depth, model_hash, tracer_args
                )

            # We can keep track of Pytorch models even before they are executed
            store_model_info(
                local_var,
                local_var_name,
                model_type,
                frame,
                event,
                tracer_args,
                depth,
                parent_hash,
            )
        elif model_type == build.ModelType.KERAS:
            # Mark this model as instrumented
            local_var.forward_instrumented = True

            # Create a copy of the old forward function
            old_forward = local_var.call

            # Raise exception if user tries to use max_depth!=0 for a keras model
            if tracer_args.max_depth != 0:
                raise exp.Error("max_depth is not supported for Keras models")
        local_var.old_forward = old_forward

        def forward_spy(*args, **kwargs):
            tracer = sys.getprofile()
            if tracer is not None:
                # Turn tracing off while the model is being executed for speed
                sys.setprofile(None)
            elif depth == 0:
                # If this model is being executed and the tracing is already off
                # we are calling a module within a parent module. We only run
                # on child models if the user has explicitly asked us to
                # do so by setting the max_depth flag.
                return old_forward(*args, **kwargs)

            # We can only keep track of keras models once they have been executed
            if model_type == build.ModelType.KERAS:
                store_model_info(
                    local_var,
                    local_var_name,
                    model_type,
                    frame,
                    event,
                    tracer_args,
                    depth,
                    parent_hash,
                )

            # Get parent invocation hash
            parent_invocation_hash = None
            if parent_hash:
                parent_invocation_hash = tracer_args.models_found[
                    parent_hash
                ].last_unique_invocation_executed

            model_hash = get_model_hash(local_var, model_type)
            invocation_hash, input_shapes = get_invocation_hash(
                model_hash, parent_invocation_hash, args, kwargs
            )
            model_info = tracer_args.models_found[model_hash]

            if invocation_hash not in model_info.unique_invocations:
                model_info.unique_invocations[invocation_hash] = (
                    status.UniqueInvocationInfo(
                        name=model_info.name,
                        script_name=model_info.script_name,
                        file=model_info.file,
                        line=model_info.line,
                        params=model_info.params,
                        depth=model_info.depth,
                        build_model=model_info.build_model,
                        model_type=model_info.model_type,
                        model_class=type(model_info.model),
                        invocation_hash=invocation_hash,
                        hash=model_info.hash,
                        is_target=invocation_hash in tracer_args.targets
                        or len(tracer_args.targets) == 0,
                        input_shapes=input_shapes,
                        parent_hash=parent_invocation_hash,
                    )
                )
            model_info.last_unique_invocation_executed = invocation_hash

            # Keep track of execution time
            start_time = time.time()
            outputs = old_forward(*args, **kwargs)
            end_time = time.time()

            invocation_info = model_info.unique_invocations[invocation_hash]
            invocation_info.exec_time = (
                invocation_info.exec_time + end_time - start_time
            )
            invocation_info.executed = invocation_info.executed + 1

            # Call explore_invocation() if this is the first time the model is being executed
            # and this model has been selected by the user
            if (
                invocation_info.executed == 1
                and invocation_info.is_target
                and (model_info.build_model)
            ):
                # Disable all modifications while we evaluate the model
                # This is needed in case a tool called during evaluation wants to
                # trace the model. There are some scenarios (e.g., ipex.quantization.prepare),
                # that raise an exception when they encounter forward_spy()
                local_var.forward = old_forward

                explore_invocation(
                    model_inputs=[args, kwargs],
                    model_info=model_info,
                    invocation_info=invocation_info,
                    tracer_args=tracer_args,
                )

                # Re-enable modifications
                local_var.forward = forward_spy

            build_name = fs.get_build_name(
                tracer_args.script_name,
                tracer_args.labels,
                invocation_info.invocation_hash,
            )
            status.update(
                tracer_args.models_found,
                build_name,
                tracer_args.cache_dir,
                invocation_info,
                tracer_args.verbosity,
            )

            # Turn tracing on again after computing the outputs
            sys.setprofile(tracer)

            return outputs

        # The inspect module offers the ability to actually copy the signature of the wrapped
        # function. This allows other functions to see the correct parameters instead of the
        # enigmatic *args, **kwargs. This is especially important for Keras, since it heavily
        # relies on inspections to the call function.
        forward_spy.__signature__ = inspect.signature(old_forward)

        # Use modified forward/call function
        if model_type == build.ModelType.PYTORCH:
            local_var.forward = forward_spy
        elif model_type == build.ModelType.KERAS:
            local_var.call = forward_spy


def tracefunc(
    frame: FrameType, event: str, _, tracer_args: TracerArgs
) -> TracebackType:
    """
    This function is used to trace the program as it runs in order
    to keep track of all all instantiated models.
    This function is passed to sys.setprofile() as a callback function.
    It receives three arguments:
        frame (the stack frame from the code being run),
        event (a string naming the type of notification), and
        arg (an event-specific value)

    """

    # Create a copy of f_locals.keys() to avoid errors due to dict changing
    local_names = list(frame.f_locals.keys())

    # Loop over all local variables to check if new models can be found
    for local_var_name in local_names:
        explore_frame(
            frame,
            event,
            local_var_name,
            frame.f_locals[local_var_name],
            tracer_args=tracer_args,
            depth=0,
        )

    return tracefunc


def recursive_search(
    frame: FrameType,
    event: str,
    model: Union[torch.nn.Module, "tf.keras.Model"],
    depth: int,
    parent_hash: Union[str, None],
    tracer_args: TracerArgs,
):
    """
    Recursively check for submodels within found models
    """
    element_names = list(dict(model.named_modules()).keys())[1:]
    for element_name in element_names:
        if hasattr(model, element_name):
            element = getattr(model, element_name)
            if issubclass(type(element), torch.nn.Module):
                explore_frame(
                    frame,
                    event,
                    element_name,
                    element,
                    tracer_args,
                    depth=depth + 1,
                    parent_hash=parent_hash,
                )


@dataclasses.dataclass
class HelpfulHandler:
    # Type of exception to handle
    exc_type: Exception
    # Do not print any traceback after this message is encountered
    traceback_stop_msg: str
    # Message to print that gives context to the traceback
    helpful_msg: str


class AnalysisException(Exception):
    pass


class HelpfulExceptions:
    """
    Catch certain exceptions, defined by `HelpfulHandler`s, and print a more helpful
    error message and traceback than what would ordinarily be printed out. This is
    useful to avoid showing the user a giant traceback that goes all the way through
    our profiling code.
    """

    def __init__(self, exceptions_to_handle: List[HelpfulHandler]):
        self.excs = exceptions_to_handle

    def __enter__(self):
        pass

    def __exit__(self, exc_type, _exc_value, exc_tb):
        for exc_handler in self.excs:
            if exc_type == exc_handler.exc_type:
                # Search the full traceback for the traceback_stop_msg
                tb = traceback.format_tb(exc_tb)

                # This default value of offending_line makes it so we will print
                # the entire traceback if we can't find the traceback_stop_msg
                offending_line = -2
                for i, line in enumerate(tb):
                    if exc_handler.traceback_stop_msg in line:
                        offending_line = i

                # Eliminate the lines of traceback before and after the traceback_stop_msg
                # Typically, the lines that follow will be related to our profiling
                # code and not helpful to the user

                # Start the helpful_traceback after line 3, since the first 3 lines are related
                # to our profiler
                start_line = 3
                helpful_traceback = "\n".join(tb[start_line : offending_line + 1])

                # sys.tracebacklimit = 0 prevents the unwanted traceback from printing
                # when we raise our AnalysisException
                sys.tracebacklimit = 0
                raise AnalysisException(
                    f"{exc_handler.helpful_msg}\n\nTraceback: \n\n: {helpful_traceback}"
                )


def evaluate_script(tracer_args: TracerArgs) -> Dict[str, status.ModelInfo]:
    tracer_args.script_name = fs.clean_file_name(tracer_args.input)

    # Get a pointer to the script's python module
    spec = importlib.util.spec_from_file_location("__main__", tracer_args.input)
    module = importlib.util.module_from_spec(spec)

    # Overwriting argv to import input script using "input-args"
    if tracer_args.script_args is None:
        tracer_args.script_args = []
    else:
        tracer_args.script_args = shlex.split(tracer_args.script_args)
    sys.argv = [tracer_args.input] + tracer_args.script_args
    sys.path.append(os.getcwd())

    # Create a tracer object that bundles a callback function with some args
    tracer = functools.partial(tracefunc, tracer_args=tracer_args)

    # Enabling analysis via setprofile
    sys.setprofile(tracer)

    # Import input script. Each executed frame of the input script will
    # trigger the tracefunc() callback function (defined above)
    with HelpfulExceptions(
        [
            HelpfulHandler(
                torch.jit.frontend.NotSupportedError,
                "torch.jit.script(",
                "torch.jit.script() is not supported by turnkey CLI and benchmark_files() API, "
                "however torch.jit.script() is being called in your script."
                "You can try passing your model instance into the build_model() API instead. ",
            )
        ]
    ):
        spec.loader.exec_module(module)

    # Stop profiling when we're done executing the module
    sys.setprofile(None)

    return tracer_args.models_found
