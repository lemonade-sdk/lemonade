# labels: name::phi2 author::transformers task::Generative_AI license::mit
from turnkeyml.parser import parse
from transformers import AutoModelForCausalLM
import torch

torch.manual_seed(0)

# Parsing command-line arguments
pretrained, batch_size, max_seq_length = parse(
    ["pretrained", "batch_size", "max_seq_length"]
)

# Model and input configurations
if pretrained:
    model = AutoModelForCausalLM.from_pretrained("microsoft/phi-2")
else:
    raise ValueError(
        "This model is only supported with pretrained weights, try again with --pretrained"
    )

# Make sure the user's sequence length fits within the model's maximum
assert max_seq_length <= model.config.max_position_embeddings


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
