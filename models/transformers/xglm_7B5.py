# labels: name::xglm_7B5 author::transformers task::Generative_AI license::apache-2.0
from turnkeyml.parser import parse
from transformers import XGLMModel, AutoConfig
import torch

torch.manual_seed(0)

# Parsing command-line arguments
pretrained, batch_size, max_seq_length = parse(
    ["pretrained", "batch_size", "max_seq_length"]
)

# Model and input configurations
if pretrained:
    model = XGLMModel.from_pretrained("facebook/xglm-7.5B")
else:
    config = AutoConfig.from_pretrained("facebook/xglm-7.5B")
    model = XGLMModel(config)

# Make sure the user's sequence length fits within the model's maximum
assert max_seq_length <= model.config.max_position_embeddings


inputs = {
    "input_ids": torch.ones(batch_size, max_seq_length, dtype=torch.long),
    "attention_mask": torch.ones(batch_size, max_seq_length, dtype=torch.float),
}


# Call model
model(**inputs)
