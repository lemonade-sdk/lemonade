# pylint: disable=no-member

from typing import Tuple, Dict
from lemonade.state import State
import lemonade.common.printing as printing
import lemonade.cache as cache
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter
from lemonade.common.system_info import (
    get_system_info_dict,
    get_device_info_dict,
    get_system_info as get_system_info_obj,
)


class NotSupported(Exception):
    """
    Indicates that a checkpoint/recipe pair are not supported
    together at this time.
    """

    def __init__(self, msg):
        super().__init__(msg)
        printing.log_error(msg)


def _raise_not_supported(recipe, checkpoint):
    raise NotSupported(
        f"Recipe {recipe} does not have support for checkpoint {checkpoint}"
    )


def _make_state(recipe, checkpoint) -> Dict:
    return State(cache_dir=cache.DEFAULT_CACHE_DIR, build_name=f"{checkpoint}_{recipe}")


def from_pretrained(
    checkpoint: str,
    recipe: str = "hf-cpu",
    do_not_upgrade: bool = True,
) -> Tuple[ModelAdapter, TokenizerAdapter]:
    """
    Load an LLM and the corresponding tokenizer using a lemonade recipe.

    Args:
        - checkpoint: huggingface checkpoint that defines the LLM
        - recipe: defines the implementation and hardware used for the LLM
        - do_not_upgrade: prioritize the local copy of the model, if available,
            even if an upgraded copy is available on the server (note: only applies
            for oga-* recipes)

    Recipe choices:
        - hf-cpu: Huggingface Transformers implementation for CPU with max-perf settings
        - hf-dgpu: Huggingface Transformers implementation on dGPU (via device="cuda")
        - oga-cpu: CPU implementation based on onnxruntime-genai
        - oga-igpu: DirectML implementation for iGPU based on onnxruntime-genai-directml
        - oga-hybird: AMD Ryzen AI Hybrid implementation based on onnxruntime-genai

    Returns:
        - model: LLM instance with a generate() method that invokes the recipe
        - tokenizer: tokenizer instance compatible with the model, which supports
            the encode (call) and decode() methods.
    """

    if recipe == "hf-cpu":
        # Huggingface Transformers recipe for CPU
        # Huggingface supports all checkpoints, so there is nothing to check for

        import torch
        from lemonade.tools.huggingface.load import HuggingfaceLoad

        state = _make_state(recipe, checkpoint)

        state = HuggingfaceLoad().run(
            state,
            input=checkpoint,
            dtype=torch.bfloat16,
        )

        return state.model, state.tokenizer

    elif recipe == "hf-dgpu":
        # Huggingface Transformers recipe for discrete GPU (Nvidia, Instinct, Radeon)

        import torch
        from lemonade.tools.huggingface.load import HuggingfaceLoad

        state = _make_state(recipe, checkpoint)

        state = HuggingfaceLoad().run(
            state,
            input=checkpoint,
            dtype=torch.bfloat16,
            device="cuda",
        )

        return state.model, state.tokenizer

    elif recipe.startswith("oga-"):
        import lemonade.tools.oga.load as oga

        # Make sure the user chose a supported runtime, e.g., oga-cpu
        user_backend = recipe.split("oga-")[1]
        supported_backends = ["cpu", "igpu", "npu", "hybrid"]
        supported_recipes = [f"oga-{backend}" for backend in supported_backends]
        if recipe not in supported_recipes:
            raise NotSupported(
                "Selected OGA recipe is not supported. "
                f"The supported OGA recipes are: {supported_recipes}"
            )

        backend_to_dtype = {
            "cpu": "int4",
            "igpu": "int4",
            "hybrid": "int4",
            "npu": "int4",
        }

        state = _make_state(recipe, checkpoint)

        state = oga.OgaLoad().run(
            state,
            input=checkpoint,
            device=user_backend,
            dtype=backend_to_dtype[user_backend],
            do_not_upgrade=do_not_upgrade,
        )

        return state.model, state.tokenizer

    else:
        _raise_not_supported(recipe, checkpoint)


def get_system_info(verbose: bool = False) -> Dict:
    """
    Get comprehensive system information including hardware details and device information.

    Returns:
        dict: Complete system information including:
            - Basic system info (OS, processor, memory, BIOS, etc.).
            - Device information (CPU, AMD iGPU, AMD dGPU, NPU).
            - Inference engine availability per device.
            - Python package versions (verbose mode only).
    """

    # Get basic system info
    info = get_system_info_dict()

    # Add device information
    info["Devices"] = get_device_info_dict()

    # Filter out verbose-only information if not in verbose mode
    if not verbose:
        essential_keys = ["OS Version", "Processor", "Physical Memory", "Devices"]
        info = {k: v for k, v in info.items() if k in essential_keys}
    else:
        # In verbose mode, add Python packages at the end
        system_info_obj = get_system_info_obj()
        info["Python Packages"] = system_info_obj.get_python_packages()

    return info


def get_device_info() -> Dict:
    """
    Get device information including CPU, AMD iGPU, AMD dGPU, and NPU details.

    Returns:
        dict: Device information including:
            - cpu: CPU details with inference engine availability.
            - amd_igpu: AMD integrated GPU information.
            - amd_dgpu: List of AMD discrete GPU information.
            - npu: NPU information.
    """

    return get_device_info_dict()


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
