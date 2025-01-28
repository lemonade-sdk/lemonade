import argparse
import os
import csv
from typing import Dict, Optional, Any
import requests
from human_eval.data import write_jsonl, read_problems
from human_eval.evaluation import evaluate_functional_correctness

from turnkeyml.state import State
from turnkeyml.tools import Tool
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build


class AccuracyHumaneval(Tool):
    """
    HumanEval accuracy measurement tool.

    This tool evaluates language models on the HumanEval dataset, which consists of
    Python programming problems. It measures the model's ability to:
    1. Generate functionally correct code completions
    2. Pass unit tests for each programming problem

    Metrics:
    - pass@1: Percentage of problems solved with 1 generation attempt
    - pass@10: Percentage of problems solved within 10 generation attempts
    - pass@100: Percentage of problems solved within 100 generation attempts

    See docs/lemonade/humaneval_accuracy.md for more details
    """

    unique_name = "accuracy-humaneval"
    DATASET = "https://github.com/openai/human-eval/blob/master/data/HumanEval.jsonl.gz?raw=true"
    TOTAL_PROBLEMS = 164  # Total number of problems in the HumanEval dataset

    def __init__(self):
        super().__init__(monitor_message="Measuring accuracy with HumanEval")
        self.status_stats = []
        # Enable code evaluation for HumanEval
        os.environ["HF_ALLOW_CODE_EVAL"] = "1"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Run accuracy benchmark using HumanEval dataset",
            add_help=add_help,
        )
        parser.add_argument(
            "--k-samples",
            type=int,
            default=1,
            help="Number of completions to generate per prompt for pass@k calculation"
            " (default: %(default)s)",
        )
        parser.add_argument(
            "--first-n-samples",
            type=int,
            default=AccuracyHumaneval.TOTAL_PROBLEMS,
            help=f"Evaluate only the first N problems from the dataset (default: "
            f"%(default)s, evaluates all {AccuracyHumaneval.TOTAL_PROBLEMS} problems)",
        )
        parser.add_argument(
            "--timeout",
            type=float,
            default=30.0,
            help="Timeout in seconds for each test case (default: %(default)s)",
        )
        parser.add_argument(
            "--data-dir",
            type=str,
            default=None,
            help="Custom directory for dataset storage (default: %(default)s, "
            "uses <lemonade_cache_dir>/data/humaneval)",
        )
        return parser

    def run(
        self,
        state: State,
        data_dir: Optional[str] = None,
        k_samples: int = 1,
        first_n_samples: Optional[int] = TOTAL_PROBLEMS,
        timeout: float = 30.0,
    ) -> State:
        """
        Run HumanEval evaluation on the model.

        Args:
            state: Current state containing model and tokenizer
            data_dir: Optional custom directory for dataset storage
            k_samples: Number of completions to generate per prompt for pass@k calculation
            first_n_samples: Number of first N problems to evaluate
            timeout: Timeout in seconds for each test case

        Returns:
            Updated state with evaluation results
        """
        # Validate required state components
        if not hasattr(state, "model") or not hasattr(state, "tokenizer"):
            raise ValueError("State must contain both 'model' and 'tokenizer'")

        # Setup directories
        data_dir_to_use = data_dir or os.path.join(state.cache_dir, "data", "humaneval")
        data_path = os.path.join(data_dir_to_use, "HumanEval.jsonl.gz")
        model_results_dir = os.path.join(
            build.output_dir(state.cache_dir, state.build_name), "humaneval"
        )
        os.makedirs(model_results_dir, exist_ok=True)

        # Download dataset if needed
        self._download_dataset(data_path)

        # Run evaluation
        results = self._evaluate_model(
            state.model,
            state.tokenizer,
            data_path,
            k_samples,
            timeout,
            model_results_dir,
            first_n_samples,
        )

        # Save metrics
        self._save_metrics(state, results)

        return state

    def _download_dataset(self, output_path: str) -> None:
        """Download HumanEval dataset if not already present."""
        if os.path.exists(output_path):
            printing.log_info(f"Dataset already exists at: {output_path}")
            return

        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        response = requests.get(self.DATASET, stream=True)

        if response.status_code == 200:
            with open(output_path, "wb") as file:
                for chunk in response.iter_content(chunk_size=8192):
                    file.write(chunk)
            printing.log_info(f"Dataset downloaded successfully to: {output_path}")
        else:
            raise RuntimeError(
                f"Failed to download dataset. Status code: {response.status_code}"
            )

    def _evaluate_model(
        self,
        model: Any,
        tokenizer: Any,
        data_path: str,
        k_samples: int,
        timeout: float,
        results_dir: str,
        first_n_samples: Optional[int] = TOTAL_PROBLEMS,
    ) -> Dict[str, float]:
        """
        Evaluate model on HumanEval dataset.

        Args:
            model: The language model to evaluate
            tokenizer: The tokenizer for the model
            data_path: Path to the HumanEval dataset
            k_samples: Number of completions per prompt for pass@k calculation
            timeout: Test case timeout in seconds
            results_dir: Directory to save results
            first_n_samples: Number of first N problems to evaluate

        Returns:
            Dictionary containing evaluation metrics
        """
        dataset = read_problems(data_path)

        # Limit to first N problems
        dataset_keys = list(dataset.keys())[:first_n_samples]
        ignore_incomplete = True

        samples = []

        # Update Tool progress monitor
        self.set_percent_progress(0.0)
        questions_completed = 0
        number_of_questions = first_n_samples * k_samples

        # Save completions and expected answers
        csv_path = os.path.join(results_dir, "evaluation_results.csv")
        with open(
            csv_path, mode="w", newline="", encoding="utf-8", errors="replace"
        ) as file:
            writer = csv.writer(file)
            writer.writerow(["Prompt", "Completion", "Expected Answer"])

            for task_id in dataset_keys:
                try:
                    for _ in range(k_samples):
                        prompt = dataset[task_id]["prompt"]
                        expected = dataset[task_id]["canonical_solution"]

                        # Generate completion
                        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
                        completion = model.generate(
                            input_ids,
                            max_new_tokens=512,
                            do_sample=False,
                        )
                        completion_text = tokenizer.decode(
                            completion[0], skip_special_tokens=True
                        )

                        # Save results
                        samples.append(
                            {"task_id": task_id, "completion": completion_text}
                        )
                        writer.writerow([prompt, completion_text, expected])

                        # Update progress monitor after completing all samples for a question
                        questions_completed = questions_completed + 1
                        percent_completed = (
                            questions_completed / number_of_questions * 100
                        )
                        self.set_percent_progress(percent_completed)

                # pylint: disable=W0718
                except Exception as e:
                    printing.log_info(f"Error processing task {task_id}: {str(e)}")
                    continue

        # Save predictions and evaluate
        pred_path = os.path.join(results_dir, "humaneval_predictions.jsonl")
        write_jsonl(pred_path, samples)
        printing.log_info(f"Results saved in: {results_dir}")

        # Run functional correctness evaluation
        k_values = [k_samples]
        results = evaluate_functional_correctness(
            pred_path,
            k_values,
            n_workers=1,
            timeout=timeout,
            problem_file=data_path,
            ignore_incomplete=ignore_incomplete,
        )
        return results

    def _save_metrics(self, state: State, results: Dict[str, float]) -> None:
        """Save evaluation metrics to state."""
        for metric, value in results.items():
            metric_name = f"humaneval_{metric}"
            state.save_stat(
                metric_name, float(value) * 100 if value is not None else None
            )
            state.save_stat(f"{metric_name}_units", "%")
            self.status_stats.append(metric_name)
