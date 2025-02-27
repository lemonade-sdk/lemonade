# pylint: disable=no-member

from typing import Tuple, Dict
from turnkeyml.state import State
import turnkeyml.common.printing as printing
import lemonade.cache as cache
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter


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
) -> Tuple[ModelAdapter, TokenizerAdapter]:
    """
    Load an LLM and the corresponding tokenizer using a lemonade recipe.

    Args:
        - checkpoint: huggingface checkpoint that defines the LLM
        - recipe: defines the implementation and hardware used for the LLM

    Recipe choices:
        - hf-cpu: Huggingface Transformers implementation for CPU with max-perf settings
        - hf-dgpu: Huggingface Transformers implementation on dGPU (via device="cuda")
        - oga-cpu: CPU implementation based on onnxruntime-genai
        - oga-dml: DirectML implementation for iGPU based on onnxruntime-genai-directml
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
        from lemonade.tools.huggingface_load import HuggingfaceLoad

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
        from lemonade.tools.huggingface_load import HuggingfaceLoad

        state = _make_state(recipe, checkpoint)

        state = HuggingfaceLoad().run(
            state,
            input=checkpoint,
            dtype=torch.bfloat16,
            device="cuda",
        )

        return state.model, state.tokenizer

    elif recipe.startswith("oga-"):
        import lemonade.tools.ort_genai.oga as oga

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
            "cpu": "fp32",
            "igpu": "fp16",
            "hybrid": "int4",
            "npu": "int4",
        }

        state = _make_state(recipe, checkpoint)

        state = oga.OgaLoad().run(
            state,
            input=checkpoint,
            device=user_backend,
            dtype=backend_to_dtype[user_backend],
        )

        return state.model, state.tokenizer

    else:
        _raise_not_supported(recipe, checkpoint)
