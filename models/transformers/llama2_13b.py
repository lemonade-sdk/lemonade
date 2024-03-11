# labels: name::llama2_13b author::transformers task::Generative_AI license::apache-2.0
from turnkeyml.parser import parse
from transformers import LlamaConfig, LlamaForCausalLM
import torch

torch.manual_seed(0)

# Parsing command-line arguments
pretrained, batch_size, max_seq_length, model_path = parse(
    ["pretrained", "batch_size", "max_seq_length", "model_path"]
)

# Model and input configurations
if pretrained:
    if not model_path:
        raise ValueError(
            "TurnkeyML does not include pretrained weights for LLaMA2 "
            "because it has special licensing terms. See for details: "
            "https://huggingface.co/docs/transformers/model_doc/llama2"
        )

    model = LlamaForCausalLM.from_pretrained(model_path)
else:
    config = LlamaConfig(
        architectures=["LlamaForCausalLM"],
        hidden_size=5120,
        intermediate_size=13824,
        max_position_embeddings=4096,
        num_attention_heads=40,
        num_hidden_layers=40,
        num_key_value_heads=40,
        pad_token_id=0,
        vocab_size=32000,
        use_cache=True,
    )
    model = LlamaForCausalLM(config)

inputs = {
    "input_ids": torch.ones(batch_size, max_seq_length, dtype=torch.long),
    "attention_mask": torch.ones(batch_size, max_seq_length, dtype=torch.float),
}

# Call model
# Generate two tokens so that we can instrument both the prefill
#   and token generation stages.
# The token generation stage is the invocation that has "past_key_values"
#   in the input shape.
model.generate(**inputs, max_length=max_seq_length + 2)
