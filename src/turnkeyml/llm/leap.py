# pylint: disable=no-member

from typing import Tuple, Dict
from turnkeyml.state import State
import turnkeyml.common.printing as printing
import turnkeyml.llm.cache as cache
from turnkeyml.llm.tools.adapter import ModelAdapter, TokenizerAdapter


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


class HuggingfaceCudaTokenizer(TokenizerAdapter):
    """
    Wrap the Huggingface tokenizer class by sending the encoded
    tokenizer inputs to the dGPU.

    This allows LEAP recipes to be fungible by saving the user the
    additional step of managing the input's device location.
    """

    def __init__(self, tokenizer):
        self.tokenizer = tokenizer

    def __call__(self, prompt, **kwargs):
        return self.tokenizer(prompt, **kwargs).to(device="cuda")

    def decode(self, response, **kwargs):
        return self.tokenizer.decode(response, **kwargs)


def from_pretrained(
    checkpoint: str,
    recipe: str = "hf-cpu",
) -> Tuple[ModelAdapter, TokenizerAdapter]:
    """
    Load an LLM and the corresponding tokenizer using a bespoke lemonade recipe.

    Not all recipes are available with all checkpoints. A leap.NotSupported exception
    will be raised in these cases.

    Args:
        - checkpoint: huggingface checkpoint that defines the LLM
        - recipe: defines the implementation and hardware used for the LLM

    Recipe choices:
        - hf-cpu: Huggingface Transformers implementation for CPU with max-perf settings
        - hf-dgpu: Huggingface Transformers implementation on dGPU (via device="cuda")
        - dml-og-igpu: DirectML implementation for iGPU based on onnxruntime-genai
        - ryzenai-npu: RyzenAI implementation of huggingface transformers PyTorch model

    Returns:
        - model: LLM instance with a generate() method that invokes the recipe
        - tokenizer: tokenizer instance compatible with the model, which supports
            the encode (call) and decode() methods.
    """

    if recipe == "hf-cpu":
        # Huggingface Transformers recipe for CPU
        # Huggingface supports all checkpoints, so there is nothing to check for

        import torch
        from turnkeyml.llm.tools.huggingface_load import HuggingfaceLoad

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
        from turnkeyml.llm.tools.huggingface_load import HuggingfaceLoad

        state = _make_state(recipe, checkpoint)

        state = HuggingfaceLoad().run(
            state,
            input=checkpoint,
            dtype=torch.bfloat16,
            device="cuda",
        )

        # Wrap the tokenizer to ensure that inputs are placed on the dGPU device
        tokenizer = HuggingfaceCudaTokenizer(state.tokenizer)

        return state.model, tokenizer

    elif recipe == "oga-dml-igpu":
        import turnkeyml.llm.tools.ort_genai.oga as oga

        state = _make_state(recipe, checkpoint)

        state = oga.OgaLoad().run(
            state,
            input=checkpoint,
            device="igpu",
            dtype="int4",
        )

        return state.model, state.tokenizer

    elif recipe == "ryzenai-npu":
        if (
            checkpoint != "TheBloke/Llama-2-7b-Chat-fp16"
            and checkpoint != "meta-llama/Llama-2-7b-chat-hf"
            and checkpoint != "microsoft/Phi-3-mini-4k-instruct"
            and checkpoint != "meta-llama/Meta-Llama-3-8B-Instruct"
            and checkpoint != "meta-llama/Meta-Llama-3-8B"
        ):
            _raise_not_supported(recipe, checkpoint)

        import turnkeyml.llm.tools.ryzenai_npu.ryzenai_npu as ryzenai_npu

        state = _make_state(recipe, checkpoint)

        state = ryzenai_npu.RyzenAINPULoad().run(state, checkpoint, device="phx")

        return state.model, state.tokenizer

    else:
        _raise_not_supported(recipe, checkpoint)
