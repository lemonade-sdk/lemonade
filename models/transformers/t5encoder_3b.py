# labels: name::t5encoder_3b author::transformers task::Generative_AI license::apache-2.0
from turnkeyml.parser import parse
from transformers import T5EncoderModel, AutoConfig
import torch

torch.manual_seed(0)

# Parsing command-line arguments
pretrained, batch_size, max_seq_length = parse(
    ["pretrained", "batch_size", "max_seq_length"]
)

# Model and input configurations
if pretrained:
    model = T5EncoderModel.from_pretrained("t5-3b")
else:
    config = AutoConfig.from_pretrained("t5-3b")
    model = T5EncoderModel(config)


inputs = {
    "input_ids": torch.ones(batch_size, max_seq_length, dtype=torch.long),
    "attention_mask": torch.ones(batch_size, max_seq_length, dtype=torch.float),
}


# Call model
model(**inputs)
