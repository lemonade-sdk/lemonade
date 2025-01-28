import os
import argparse
import pandas as pd
import torch
from datasets import load_dataset
from tqdm import tqdm
from turnkeyml.state import State
from turnkeyml.tools import Tool
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build


class AccuracyPerplexity(Tool):
    """
    Measure perplexity of an LLM using the wikitext dataset.

    Required input state:
        - state.model: instance that provides a __call__() method that returns
        output.logits and supports model.config.max_position_embeddings
        - state.tokenizer: instance of Hugging Face PretrainedTokenizer

    Output state produced: None

    See docs/lemonade/perplexity.md for more details.
    """

    unique_name = "accuracy-perplexity"

    def __init__(self):
        super().__init__(monitor_message="Measuring perplexity")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Measure Perplexity score using Wikitext-2 dataset",
            add_help=add_help,
        )
        return parser

    def run(
        self,
        state: State,
    ) -> State:

        try:
            printing.log_info("Downloading dataset ...")
            dataset = load_dataset("wikitext", "wikitext-2-raw-v1", split="test")
        except Exception as e:  # pylint: disable=broad-except
            printing.log_error(f"Error during dataset load: {e}")
            raise e

        tokenizer = state.tokenizer
        model = state.model
        # Tokenize the entire test dataset text, joining entries with double new lines
        encodings = tokenizer("\n\n".join(dataset["text"]), return_tensors="pt")

        # Retrieve the maximum input length that the model can handle
        try:
            max_length = model.config.max_position_embeddings
        except AttributeError:
            # Some LLMs do not have the config.max_position_embeddings attribute
            # However, most LLMs support at least 2048 context length, so this
            # try-except will allow a few more LLMs to work
            max_length = 2048
        # Set stride to half of the maximum input length for overlapping window processing
        # Refer to docs/perplexity.md for more information on sliding window
        stride = max_length // 2
        # Determine the total sequence length of the tokenized input
        seq_len = encodings.input_ids.size(1)

        negative_log_likelihoods = []
        summary_data = []
        prev_end_location = 0

        model_results_dir = os.path.join(
            build.output_dir(state.cache_dir, state.build_name), "perplexity"
        )

        for begin_location in tqdm(range(0, seq_len, stride)):
            end_location = min(begin_location + max_length, seq_len)
            target_len = end_location - prev_end_location
            input_ids = encodings.input_ids[:, begin_location:end_location]
            target_ids = input_ids.clone()
            target_ids[:, :-target_len] = -100

            # Forward pass the model to get logits
            with torch.no_grad():
                try:
                    outputs = model(input_ids, labels=target_ids)
                    logits = outputs.logits
                except Exception as e:  # pylint: disable=broad-except
                    printing.log_error(
                        f"Error during model forward pass execution: {e}"
                    )

            # Compute loss manually for visualization
            shift_logits = logits[..., :-1, :].contiguous()
            shift_labels = target_ids[..., 1:].contiguous()
            effective_token_count = (target_ids != -100).sum().item()
            negative_log_likelihoods.append(
                (outputs.loss.item(), effective_token_count)
            )

            # Decode predicted and actual next words for the last token position
            predictions = torch.argmax(shift_logits, dim=-1)
            predicted_tokens = predictions[:, -1]
            actual_tokens = shift_labels[:, -1]

            predicted_words = tokenizer.batch_decode(
                predicted_tokens, skip_special_tokens=True
            )
            actual_words = tokenizer.batch_decode(
                actual_tokens, skip_special_tokens=True
            )
            context = tokenizer.decode(input_ids[0, :])

            summary_data.append(
                {
                    "Context": context[-stride:],
                    "Predicted next word": predicted_words,
                    "Actual next word": actual_words,
                    "Loss for this window": outputs.loss.item(),
                }
            )
            prev_end_location = end_location

        # Total loss calculation considering the number of tokens for each segment
        total_loss = sum(loss * count for loss, count in negative_log_likelihoods)
        total_tokens = sum(count for _, count in negative_log_likelihoods)

        # Calculate average negative_log_likelihood and perplexity
        average_negative_log_likelihood = total_loss / total_tokens
        perplexity = torch.exp(torch.tensor(average_negative_log_likelihood))

        # Save accuracy results to stats file
        state.save_stat("perplexity_score", float(perplexity.item()))

        # Save accuracy results to CSV file
        summary_df = pd.DataFrame(summary_data)
        summary_df.to_csv(
            os.path.join(model_results_dir, "summary_results.csv"), index=False
        )
        return state
