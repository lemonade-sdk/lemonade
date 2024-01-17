# labels: name::xmod_base_60_125k author::transformers task::Generative_AI license::apache-2.0
from turnkeyml.parser import parse
from transformers import XmodModel, AutoConfig
import torch

torch.manual_seed(0)

# Parsing command-line arguments
pretrained, batch_size, max_seq_length = parse(
    ["pretrained", "batch_size", "max_seq_length"]
)

# Model and input configurations
if pretrained:
    model = XmodModel.from_pretrained("facebook/xmod-base-60-125k")
else:
    config = AutoConfig.from_pretrained("facebook/xmod-base-60-125k")
    model = XmodModel(config)

model.set_default_language("en_XX")

inputs = {
    "input_ids": torch.ones(batch_size, max_seq_length, dtype=torch.long),
    "attention_mask": torch.ones(batch_size, max_seq_length, dtype=torch.float),
}


# Call model
model(**inputs)
