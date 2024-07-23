import sys
import os
import inspect
import importlib.util
import time
import shlex
import functools
import dataclasses
import traceback
import hashlib
from typing import Union, List, Dict, Tuple, Optional
from types import FrameType, TracebackType
import torch
import turnkeyml.common.build as build
import turnkeyml.common.status as status
import turnkeyml.common.analyze_model as analyze_model
import turnkeyml.common.filesystem as fs


def _get_classes(module) -> List[str]:
    """
    Returns all classes within a module.
    """
    return [y for x, y in inspect.getmembers(module, inspect.isclass)]


def _get_transformers_activations() -> List:
    """
    We need this helper because transformers is not a required depenence for
    this project, however if we are analyzing a transformers model then we need
    to inspect its activations.
    """
    if "transformers" in sys.modules:
        return _get_classes(sys.modules["transformers"].activations)
    else:
        return []


@dataclasses.dataclass
class TracerArgs:
    input: str
    script_args: str
    targets: List[str]
    max_depth: int
    models_found: Dict[str, status.ModelInfo] = dataclasses.field(default_factory=dict)
    script_name: Optional[str] = None

    @functools.cached_property
    def torch_activations(self) -> List[str]:
        act = _get_classes(torch.nn.modules.activation)
        act += _get_transformers_activations()
        return act

    @property
    def hash(self) -> str:
        """
        Returns a unique hash representing the arguments. Useful for distinguishing
        between evaluations of the same model that have different arguments.
        """

        return hashlib.sha256(str(self).encode()).hexdigest()[:8]


def get_model_hash(model: Union[torch.nn.Module, str]):
    if isinstance(model, str) and model.endswith(".onnx"):
        hash_params = True
    else:
        hash_params = False
    return build.hash_model(model, hash_params=hash_params)[:8]


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
    model: torch.nn.Module,
    model_name: str,
    frame: FrameType,
    event: str,
    tracer_args: TracerArgs,
    depth: int,
    parent_hash: str,
):
    model_hash = get_model_hash(model)

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
        tracer_args.models_found[model_hash] = status.ModelInfo(
            model=model,
            name=model_name,
            file=file,
            line=line,
            depth=depth,
            hash=model_hash,
            parent_hash=parent_hash,
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
    This function checks whether local_var is a torch model.
    If it is, we will modify its forward function to know when it
    is called.
    """

    # Exit frame exploration if Python is shutting down
    if not bool(sys.modules):
        return

    # Skip all variables that are not a subclass of torch.nn.Module
    # Note: try block used since dead weakreferences fail when checking subclass
    try:
        if issubclass(type(local_var), torch.nn.Module):
            if type(local_var) in tracer_args.torch_activations:
                return
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

    # Check if we are inside of a subclass of torch.nn.Module
    inside_class = False
    inside_nn_subclass = False
    if "self" in frame.f_locals:
        self_var = frame.f_locals["self"]
        inside_class = type(self_var)
        inside_nn_subclass = issubclass(inside_class, torch.nn.Module)

    if not inside_nn_subclass:
        if hasattr(local_var, "forward_instrumented"):

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

        # Avoid instrumenting models before they have been fully loaded
        if analyze_model.count_parameters(local_var) == 0:
            return

        # Mark this model as instrumented
        local_var.forward_instrumented = True

        # Create a copy of the old forward function
        old_forward = local_var.forward

        # Recursively look for sub-models within the found model
        # This is only possible on Pytorch, since each layer of a torch.nn.module
        # is also a torch.nn.module.
        model_hash = get_model_hash(local_var)
        local_var.turnkey_hash = model_hash
        if depth < tracer_args.max_depth:
            recursive_search(frame, event, local_var, depth, model_hash, tracer_args)

        # We can keep track of Pytorch models even before they are executed
        store_model_info(
            local_var,
            local_var_name,
            frame,
            event,
            tracer_args,
            depth,
            parent_hash,
        )

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

            # Get parent invocation hash
            parent_invocation_hash = None
            if parent_hash:
                parent_invocation_hash = tracer_args.models_found[
                    parent_hash
                ].last_unique_invocation_executed

            model_hash = get_model_hash(local_var)
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
                        model_class=type(model_info.model),
                        invocation_hash=invocation_hash,
                        hash=model_info.hash,
                        is_target=invocation_hash in tracer_args.targets
                        or len(tracer_args.targets) == 0,
                        input_shapes=input_shapes,
                        parent_hash=parent_invocation_hash,
                        inputs=[args, kwargs],
                        extension=f".{tracer_args.input.split('.')[-1]}",
                        forward_function_pointer=local_var.forward,
                        original_forward_function=old_forward,
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

            # Turn tracing on again after computing the outputs
            sys.setprofile(tracer)

            return outputs

        # The inspect module offers the ability to actually copy the signature of the wrapped
        # function. This allows other functions to see the correct parameters instead of the
        # enigmatic *args, **kwargs.
        forward_spy.__signature__ = inspect.signature(old_forward)

        # Use modified forward/call function
        local_var.forward = forward_spy


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
    model: torch.nn.Module,
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

    # Restore the original forward function for all models
    for model_info in tracer_args.models_found.values():
        for invocation_info in model_info.unique_invocations.values():
            invocation_info.forward_function_pointer = (
                invocation_info.original_forward_function
            )

    return tracer_args.models_found
