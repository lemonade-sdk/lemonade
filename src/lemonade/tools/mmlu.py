import argparse
import os
import tarfile
from pathlib import Path
from typing import List, Optional
import subprocess
import tqdm
import numpy as np
import pandas as pd
import requests
from turnkeyml.state import State
from turnkeyml.tools import Tool
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs

# Constants
choices = ["A", "B", "C", "D"]
dataset_url = "https://people.eecs.berkeley.edu/~hendrycks/data.tar"


def min_handle_none(*args: int):
    """
    Returns the minimum of the arguments. If one of the arguments is none,
    it doesn't count towards the min.
    """

    filter_out_none = (value for value in args if value is not None)
    return min(filter_out_none)


class AccuracyMMLU(Tool):
    """
    See docs/lemonade/mmlu_accuracy.md for more details
    """

    unique_name = "accuracy-mmlu"

    def __init__(self):
        super().__init__(monitor_message="Measuring accuracy with MMLU")
        self.status_stats = []

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Run accuracy benchmark using Massive Multitask "
            "Language Understanding (MMLU) test",
            add_help=add_help,
        )

        parser.add_argument(
            "--ntrain",
            type=int,
            default=5,
            help="Number of training examples to use. Default set to 5 for `5 Shot`",
        )
        parser.add_argument(
            "--max-evals",
            type=int,
            default=None,
            help="Maximum evaluations to run per test",
        )
        parser.add_argument(
            "--data-dir",
            type=str,
            required=False,
            help="Directory containing test and dev data (default: lemonade cache).",
        )
        parser.add_argument(
            "--tests",
            nargs="+",
            help=(
                "Specific tests to run. For a single quick test, we suggest 'management'."
                + "Default: run all tests."
            ),
        )
        return parser

    def run(
        self,
        state: State,
        ntrain: int = 5,
        max_evals: int = None,
        data_dir: Optional[str] = None,
        tests: List[str] = None,
    ) -> State:

        if data_dir:
            data_dir_to_use = data_dir
        else:
            data_dir_to_use = os.path.join(state.cache_dir, "data", "mmlu")

        # Setup MMLU dataset
        dataset_dir = download_and_extract_dataset(data_dir_to_use, dataset_url)

        model_results_dir = os.path.join(
            build.output_dir(state.cache_dir, state.build_name), "mmlu"
        )
        os.makedirs(model_results_dir, exist_ok=True)

        tests_to_run = [
            f.replace("_test.csv", "")
            for f in sorted(os.listdir(os.path.join(dataset_dir, "test")))
            if f.endswith("_test.csv")
        ]
        if tests is not None:
            unsupported_tests = set(tests) - set(tests_to_run)
            if unsupported_tests:
                raise ValueError(
                    f"Invalid test names provided: {', '.join(unsupported_tests)}. "
                    f"Valid tests are: {', '.join(tests_to_run)}"
                )
            tests_to_run = [test for test in tests if test in tests_to_run]

        tokenizer = state.tokenizer
        model = state.model

        # Update Tool progress monitor
        self.set_percent_progress(0.0)
        number_of_questions = float(
            sum(
                [
                    min_handle_none(
                        len(
                            _safe_read_csv(
                                os.path.join(dataset_dir, "test", f"{subject}_test.csv")
                            )
                        ),
                        max_evals,
                    )
                    for subject in tests_to_run
                ]
            )
        )

        questions_completed = 0

        summary_data = []
        for subject in tqdm.tqdm(tests_to_run):
            dev_df = _safe_read_csv(
                os.path.join(dataset_dir, "dev", f"{subject}_dev.csv")
            )[:ntrain]
            test_df = _safe_read_csv(
                os.path.join(dataset_dir, "test", f"{subject}_test.csv")
            )

            # Evaluate the model on the test data for a given subject
            detailed_results = []

            for i in range(min_handle_none(test_df.shape[0], max_evals)):
                prompt = _gen_prompt(dev_df, subject, ntrain) + _format_example(
                    test_df, i, include_answer=False
                )
                input_ids = tokenizer(prompt, return_tensors="pt").input_ids

                response_text = _generate_response(tokenizer, model, input_ids)
                try:
                    pred_label = response_text[-1].upper()
                # Handle models generating empty outputs
                except IndexError:
                    pred_label = "-"

                label = test_df.iloc[i, -1].strip().upper()
                detailed_results.append(
                    {
                        "Question": test_df.iloc[i, 0],
                        "Prompt": prompt,
                        "Correct Answer": label,
                        "Generated Answer": pred_label,
                        "Correct": pred_label == label,
                    }
                )

                # Update progress monitor
                questions_completed = questions_completed + 1
                percent_completed = questions_completed / number_of_questions * 100
                self.set_percent_progress(percent_completed)

            acc = np.mean([res["Correct"] for res in detailed_results])

            subject_results_df = pd.DataFrame(detailed_results)
            subject_csv_path = os.path.join(
                model_results_dir, f"{subject}_detailed_results.csv"
            )
            subject_results_df.to_csv(subject_csv_path, index=False)

            # Update summary_data with total questions and correct answers
            correct_answers_count = sum(
                result["Correct"] for result in detailed_results
            )

            summary_data.append(
                {
                    "Subject": subject,
                    "Accuracy": acc,
                    "Total Questions": len(test_df),
                    "Evaluated Questions": (
                        max_evals
                        if max_evals is not None and max_evals < len(test_df)
                        else len(test_df)
                    ),
                    "Correct Answers": correct_answers_count,
                }
            )

            # Save accuracy results to stats file
            # And display in the CLI
            stat_name = f"mmlu_{subject}_accuracy"
            stat_units_name = f"{stat_name}_units"
            state.save_stat(stat_name, float(acc) * 100)
            state.save_stat(stat_units_name, "%")
            self.status_stats.append(stat_name)

        # Calculate average of mmlu accuracy and display in the CLI
        acc_avg = np.mean([accuracy_data["Accuracy"] for accuracy_data in summary_data])
        state.save_stat(fs.Keys.AVERAGE_MMLU_ACCURACY, float(acc_avg) * 100)
        state.save_stat(f"{fs.Keys.AVERAGE_MMLU_ACCURACY}_units", "%")
        self.status_stats.append(fs.Keys.AVERAGE_MMLU_ACCURACY)

        # Save accuracy results to CSV file
        summary_df = pd.DataFrame(summary_data)
        summary_df.to_csv(
            os.path.join(model_results_dir, "summary_results.csv"), index=False
        )
        return state


def _list_tests(data_dir):
    """Lists all available tests based on the files in the test data directory."""
    test_files = [
        f for f in os.listdir(os.path.join(data_dir, "test")) if f.endswith("_test.csv")
    ]
    print(
        "Available tests:",
        *[f.replace("_test.csv", "") for f in sorted(test_files)],
        sep="\n",
    )


def _format_subject(subject):
    """Formats a subject string by replacing underscores with spaces."""
    return " ".join(subject.split("_"))


def _safe_read_csv(path):
    """Safely reads a CSV file and returns a DataFrame."""
    try:
        return pd.read_csv(path, header=None)
    except FileNotFoundError:
        printing.log_error(f"Error: File not found - {path}")
    except Exception as e:  # pylint: disable=broad-except
        printing.log_error(f"An error occurred while reading {path}: {e}")


def _format_example(df, idx, include_answer=True):
    """Formats an example from the dataframe into a prompt string."""
    prompt = df.iloc[idx, 0]
    for j in range(1, df.shape[1] - 1):
        prompt += f"\n{choices[j-1]}. {df.iloc[idx, j]}"
    prompt += "\nAnswer_:"
    if include_answer:
        prompt += f" {df.iloc[idx, -1]}\n\n"
    return prompt


def _gen_prompt(train_df, subject, k=-1):
    """Generates a prompt string from multiple examples."""
    prompt = (
        "The following are multiple choice questions (with answers) about "
        + f"{_format_subject(subject)}.\n\n"
    )
    for i in range(min(k, train_df.shape[0]) if k != -1 else train_df.shape[0]):
        prompt += _format_example(train_df, i)
    return prompt


def _generate_response(tokenizer, model, input_ids):
    """Generates a model response for the given input IDs."""
    try:
        response = model.generate(input_ids, max_new_tokens=1)
        return tokenizer.decode(response[0], skip_special_tokens=True).strip()
    except subprocess.CalledProcessError as e:
        printing.log_warning(
            f"Subprocess failed with command: {e} and error message: {e.stderr}"
        )
    except Exception as e:  # pylint: disable=broad-except
        printing.log_warning(f"Error during model generation: {e}")
    return ""  # Return an empty string on failure


def download_and_extract_dataset(data_cache_dir: str, dataset_url: str):
    """
    Download the dataset from the given URL and extract it into the target directory.
    """

    # Create the directory if it does not exist
    Path(data_cache_dir).mkdir(parents=True, exist_ok=True)

    # Check if the data already exists to avoid re-downloading
    if not os.listdir(data_cache_dir):  # Checks if the directory is empty
        printing.log_info(f"Downloading dataset to {data_cache_dir}")

        # Download the dataset
        response = requests.get(dataset_url, stream=True)
        if response.status_code == 200:
            tar_path = os.path.join(data_cache_dir, "data.tar")
            with open(tar_path, "wb") as f:
                f.write(response.raw.read())

            printing.log_info("Extracting dataset...")
            # Extract the tar file
            with tarfile.open(tar_path) as tar:
                tar.extractall(path=data_cache_dir)
            os.remove(tar_path)
            printing.log_info("Dataset ready.")
        else:
            printing.log_info("Failed to download the dataset.")
    else:
        printing.log_info(
            f"Dataset already exists in {data_cache_dir}, skipping download."
        )

    # MMLU data is stored in data.tar/data
    return os.path.join(data_cache_dir, "data")
