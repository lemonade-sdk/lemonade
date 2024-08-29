import os
import argparse
import torch
from transformers import (
    LlamaForCausalLM,
    LlamaTokenizer,
    AutoTokenizer,
    PreTrainedTokenizerFast,
)
from ryzenai_llm_engine import RyzenAILLMEngine, TransformConfig
from ryzenai_llm_quantizer import QuantConfig, RyzenAILLMQuantizer
from modeling_phi3 import Phi3ForCausalLM
from turnkeyml.state import State
from turnkeyml.tools import FirstTool
from turnkeyml.llm.tools.adapter import ModelAdapter
from turnkeyml.llm.cache import Keys

npu_root_dir = os.path.dirname(__file__)
quantized_models_path = os.path.join(npu_root_dir, "quantized_models")
if not os.path.exists(quantized_models_path):
    os.mkdir(quantized_models_path)


class LlamaModelEval(LlamaForCausalLM):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.model_name = "llama-2-7b-chat"
        self.tokenizer = None

    def forward(self, *args, **kwargs):
        outputs = super().forward(*args, **kwargs)  # pylint: disable=no-member
        return outputs


class Phi3ModelEval(Phi3ForCausalLM):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.model_name = "phi-3-mini-4k-instruct"
        self.tokenizer = None

    def forward(self, *args, **kwargs):
        outputs = super().forward(*args, **kwargs)
        return outputs

    def get_position_embeddings(self):
        raise NotImplementedError(
            f"`get_position_embeddings` is not implemented for {self.__class__}`. "
            f"To implement it, you should overwrite this method in the class {self.__class__} "
            f"in `modeling_{self.__class__.__module__}.py`"
        )

    def resize_position_embeddings(self, new_num_position_embeddings: int):
        raise NotImplementedError(
            f"`resize_position_embeddings` is not implemented for {self.__class__}`."
            f"To implement it, you should overwrite this method in the class {self.__class__} "
            f"in `modeling_{self.__class__.__module__}.py`"
        )


class RyzenAiModel(ModelAdapter):
    """
    RyzenAI NPU models require an attention_mask of all 1's to be passed
    as input to generate. This class exists for the purpose of inserting
    that attention mask.
    """

    def __init__(self, model):
        super().__init__()
        self.model = model

    # pylint: disable=arguments-differ
    def generate(self, input_ids, **kwargs):
        attention_mask = torch.ones(input_ids.shape)
        return self.model.generate(
            input_ids=input_ids, attention_mask=attention_mask, **kwargs
        )

    def __getattr__(self, name):
        """
        Forward all attribute access to self.model.
        """
        return getattr(self.model, name)


class RyzenAINPULoad(FirstTool):
    """
    Tool that loads an LLM checkpoint on to a RyzenAI NPU.

    Input: the name or path to a checkpoint. Supported options:
        "TheBloke/Llama-2-7b-Chat-fp16"
        "meta-llama/Llama-2-7b-chat-hf"
        "microsoft/Phi-3-mini-4k-instruct"
        "meta-llama/Meta-Llama-3-8B-Instruct"
        "meta-llama/Meta-Llama-3-8B"

    Output:
        state.model: handle to a Huggingface-style LLM loaded on NPU
        state.tokenizer = Huggingface-style LLM tokenizer instance
        state.dtype = data type of the model on NPU

    Note: This tool expects the ryzenai-transformers library to be pre-installed.
            If that library is not installed, this tool will not load.
    """

    unique_name = "ryzenai-npu-load"

    def __init__(self):
        super().__init__(monitor_message="Loading LLM on RyzenAI NPU")

        self.status_stats = [Keys.DTYPE]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Quantize and transform a model using AWQ \
                in int4 format in RyzenAI NPU",
            add_help=add_help,
        )

        parser.add_argument("--device", required=True, choices=["phx", "stx"])

        return parser

    # pylint: disable=C0103
    def run(self, state: State, input: str = "", device=None) -> State:

        checkpoint = input

        w_bit = 4
        group_size = 128

        if (
            checkpoint == "TheBloke/Llama-2-7b-Chat-fp16"
            or checkpoint == "meta-llama/Llama-2-7b-chat-hf"
        ):
            model_name = "llama-2-7b-chat"
            algorithm = "awqplus"
            flash_attention_plus = False
            trust_remote_code = False
            CausalLMModel = LlamaModelEval
            LMTokenizer = LlamaTokenizer
            quantized_model_path = os.path.join(
                quantized_models_path,
                f"quantized_llama-2-7b-chat_w{w_bit}_g{group_size}_{algorithm}.pth",
            )

        elif (
            checkpoint == "meta-llama/Meta-Llama-3-8B-Instruct"
            or checkpoint == "meta-llama/Meta-Llama-3-8B"
        ):
            model_name = checkpoint.replace("meta-llama/", "")
            algorithm = "awqplus"
            flash_attention_plus = False
            trust_remote_code = False
            CausalLMModel = LlamaModelEval
            LMTokenizer = PreTrainedTokenizerFast
            quantized_model_path = os.path.join(
                quantized_models_path,
                f"quantized_{model_name}_w{w_bit}_g{group_size}_{algorithm}.pth",
            )

        elif checkpoint == "microsoft/Phi-3-mini-4k-instruct":
            model_name = "phi-3-mini-4k-instruct"
            algorithm = "pergrp"
            flash_attention_plus = False
            trust_remote_code = True
            CausalLMModel = Phi3ModelEval
            LMTokenizer = AutoTokenizer

            quantized_model_path = os.path.join(
                quantized_models_path,
                f"quantized_Phi-3-mini-4k-instruct_w{w_bit}_g{group_size}_{algorithm}.pth",
            )

        else:
            raise ValueError(f"Model {checkpoint} is not a supported model.")

        if not os.path.exists(quantized_model_path):

            model = CausalLMModel.from_pretrained(
                checkpoint,
                torch_dtype=torch.bfloat16,
                trust_remote_code=trust_remote_code,
                attn_implementation="eager",
            )

            model.tokenizer = LMTokenizer.from_pretrained(
                checkpoint, trust_remote_code=trust_remote_code
            )

            quant_config = QuantConfig(
                quant_mode=algorithm,
                model_name=checkpoint,
                dataset="raw",
                w_bit=w_bit,
                group_size=group_size,
                use_qscales=True,
            )

            model = RyzenAILLMQuantizer.quantize(model, quant_config=quant_config)
            torch.save(model, quantized_model_path)
        else:
            model = torch.load(quantized_model_path)

        if device == "phx":
            fast_attention = False
        elif device == "stx":
            fast_attention = True
        else:
            raise Exception(f"Use a supported device instead of {device}")

        # Different library versions support different flags
        # We maintain a safe set of flags and a cutting-edge set of flags,
        # and attempt each
        try:
            transform_config = TransformConfig(
                flash_attention_plus=flash_attention_plus,
                fast_attention=fast_attention,
                fast_mlp=device != "phx",
                fast_norm=device != "phx",
                precision="w4abf16",
                model_name=model_name,
                target="aie",
                w_bit=w_bit,
                group_size=group_size,
                profilegemm=False,
            )
        except TypeError:
            transform_config = TransformConfig(
                flash_attention_plus=False,
                fast_attention=False,
                fast_mlp=False,
                precision="w4abf16",
                model_name=model_name,
                target="aie",
                w_bit=w_bit,
                group_size=group_size,
                profilegemm=False,
            )

        model = RyzenAILLMEngine.transform(model, transform_config)
        model = model.to(torch.bfloat16)
        model.eval()

        state.model = RyzenAiModel(model)
        state.tokenizer = model.tokenizer
        state.dtype = "int4"

        state.save_stat(Keys.CHECKPOINT, checkpoint)
        state.save_stat(Keys.DEVICE, "ryzenai-npu")
        state.save_stat(Keys.DTYPE, "int4")

        return state
