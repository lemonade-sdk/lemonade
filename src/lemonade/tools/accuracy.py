import argparse
import json
import os
import subprocess
import sys
from typing import Optional

import requests

from lemonade.state import State
from lemonade.tools import Tool
from lemonade.tools.server_load import ServerAdapter
import lemonade.common.printing as printing
import lemonade.common.build as build


class LMEvalHarness(Tool):
    """
    Tool for evaluating LLMs using lm-eval-harness on industry standard benchmarks
    like MMLU, GSM8k, and more. See docs/lemonade/lm_eval.md for more details.
    """

    unique_name = "lm-eval-harness"

    def __init__(self):
        super().__init__(
            monitor_message="Evaluate model accuracy using ElutherAI's lm-eval-harness"
        )
        self.status_stats = []

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Evaluate model using lm-eval-harness",
            add_help=add_help,
        )

        parser.add_argument(
            "--task",
            type=str,
            required=True,
            help="Task(s) to evaluate on (e.g., gsm8k, mmlu)",
        )

        parser.add_argument(
            "--num-fewshot",
            type=int,
            default=0,
            help="Number of examples in few-shot prompts",
        )

        parser.add_argument(
            "--limit",
            type=int,
            default=None,
            help="Limit the number of examples per task",
        )

        parser.add_argument(
            "--log-samples",
            action="store_true",
            help="Log samples for each task to log file",
        )

        parser.add_argument(
            "--output-path",
            type=str,
            default=None,
            help="Path to save evaluation results",
        )

        return parser

    def _scale_metric(self, metric_name, value):
        """
        Scale metric value appropriately based on type and range

        Args:
            metric_name: Name of the metric (e.g., "acc,none", "ppl")
            value: Numeric value of the metric

        Returns:
            tuple: (scaled_value, units, display_string)
        """
        fraction_metrics = {
            "acc",
            "accuracy",
            "f1",
            "exact_match",
            "em",
            "win_rate",
            "recall",
            "precision",
            "rouge",
            "bleu",
            "meteor",
            "bertscore",
            "match",
            "correct",
            "pass",
            "success_rate",
        }

        metric_base = metric_name.split(",")[0].lower()
        is_fraction = any(
            frac_metric in metric_base for frac_metric in fraction_metrics
        )
        is_in_unit_range = 0 <= value <= 1

        if is_fraction and is_in_unit_range:
            scaled_value = float(value) * 100
            units = "%"
            display_str = f"{value:.4f} ({scaled_value:.2f}%)"
        else:
            scaled_value = float(value)
            units = "raw"
            display_str = f"{value:.4f}"

        return scaled_value, units, display_str

    def _process_results(self, results_path, state):
        """
        Process evaluation results and save to state stats

        Args:
            results_path: Can be either a direct JSON file path or a directory path
            state: State object to save metrics to
        """
        results_file_path = None

        # Determine if this is a file or directory and find the JSON file
        if os.path.isfile(results_path) and results_path.endswith(".json"):
            # Direct JSON file path (modern format)
            results_file_path = results_path
        elif os.path.isdir(results_path):
            # Look for model subdirectories
            model_dirs = [
                d
                for d in os.listdir(results_path)
                if os.path.isdir(os.path.join(results_path, d))
            ]

            if model_dirs:
                # Format: results_dir/model_name/results_*.json
                model_dir = os.path.join(results_path, model_dirs[0])
                printing.log_info(f"Found model directory: {model_dir}")

                results_files = [
                    f
                    for f in os.listdir(model_dir)
                    if f.startswith("results_") and f.endswith(".json")
                ]

                if results_files:
                    results_files.sort(reverse=True)
                    results_file_path = os.path.join(model_dir, results_files[0])
                else:
                    printing.log_warning(f"No results files found in {model_dir}")
                    return
            else:
                printing.log_warning(f"No model directories found in {results_path}")
                return
        else:
            # Handle case where lm-eval adds timestamp to expected filename
            results_dir = os.path.dirname(results_path)
            if os.path.exists(results_dir):
                json_files = [f for f in os.listdir(results_dir) if f.endswith(".json")]
                if json_files:
                    results_file_path = os.path.join(results_dir, json_files[0])
                    printing.log_info(f"Found results file: {results_file_path}")
                else:
                    printing.log_warning(f"No JSON results file found in {results_dir}")
                    return
            else:
                printing.log_warning(f"Results path not found at {results_path}")
                return

        if not results_file_path or not os.path.exists(results_file_path):
            printing.log_warning(f"Results file not found at {results_file_path}")
            return

        printing.log_info(f"Processing results from {results_file_path}")

        try:
            with open(results_file_path, "r", encoding="utf-8") as f:
                results = json.load(f)

            # Extract and display metrics
            if "results" in results:
                for task_name, metrics in results["results"].items():
                    printing.log_info(f"Results for {task_name}:")

                    for metric, value in metrics.items():
                        if isinstance(value, (int, float)) and not metric.startswith(
                            "alias"
                        ):
                            # Format metric name for stats - remove ,none suffix
                            clean_metric = metric.split(",")[0]  # Remove ,none suffix
                            stat_name = f"lm_eval_{task_name}_{clean_metric}"

                            # Scale metric appropriately
                            scaled_value, units, value_str = self._scale_metric(
                                metric, value
                            )
                            display_str = f"  {metric}: {value_str}"

                            state.save_stat(stat_name, scaled_value)
                            state.save_stat(f"{stat_name}_units", units)
                            self.status_stats.append(stat_name)

                            printing.log_info(display_str)

                # Save summary metrics if available
                avg_metrics = {}
                if "higher_is_better" in results:
                    for metric_type in results["higher_is_better"].values():
                        for metric in metric_type.keys():
                            if metric not in avg_metrics:
                                avg_metrics[metric] = []

                for task_metrics in results["results"].values():
                    for metric, value in task_metrics.items():
                        if isinstance(value, (int, float)) and not metric.startswith(
                            "alias"
                        ):
                            base_metric = metric.split(",")[0]
                            if base_metric in avg_metrics:
                                avg_metrics[base_metric].append(value)

                # Calculate and save averages
                for metric, values in avg_metrics.items():
                    if values:
                        avg_value = sum(values) / len(values)
                        stat_name = f"lm_eval_average_{metric}"

                        # Apply same scaling logic as individual metrics
                        scaled_avg, units, value_str = self._scale_metric(
                            metric, avg_value
                        )
                        display_str = f"Average {metric}: {value_str}"

                        state.save_stat(stat_name, scaled_avg)
                        state.save_stat(f"{stat_name}_units", units)
                        self.status_stats.append(stat_name)
                        printing.log_info(display_str)

        except (IOError, json.JSONDecodeError) as e:
            printing.log_error(f"Error processing results: {e}")

    def _get_tokenizer_repo(self, server_url: str, model_name: str) -> str:
        """
        Get the HuggingFace repo that contains the tokenizer for a model.

        For GGUF models, the checkpoint repo often doesn't have tokenizer files,
        so we query HuggingFace API to find the base_model which has the tokenizer.

        Args:
            server_url: URL of the Lemonade Server
            model_name: Name of the model in Lemonade

        Returns:
            HuggingFace repo ID that contains the tokenizer
        """
        # Step 1: Get checkpoint from Lemonade server
        hf_repo = model_name  # Default fallback
        try:
            model_response = requests.get(
                f"{server_url}/api/v1/models/{model_name}", timeout=10
            )
            if model_response.ok:
                model_info = model_response.json()
                checkpoint = model_info.get("checkpoint", "")
                # Extract HF repo from checkpoint (format: "repo/name:variant")
                if checkpoint and "/" in checkpoint:
                    hf_repo = checkpoint.split(":")[0]
        except requests.exceptions.RequestException:
            printing.log_warning(
                f"Could not fetch model info from server: {model_name}"
            )
            return model_name

        # Step 2: Query HuggingFace API for base_model (which has the tokenizer)
        try:
            hf_api_url = f"https://huggingface.co/api/models/{hf_repo}"
            hf_response = requests.get(hf_api_url, timeout=15)
            if hf_response.ok:
                hf_info = hf_response.json()
                # Check cardData.base_model first (most reliable)
                card_data = hf_info.get("cardData", {})
                base_model = card_data.get("base_model")
                if base_model:
                    # base_model can be a string or list
                    if isinstance(base_model, list) and base_model:
                        tokenizer_repo = base_model[0]
                    elif isinstance(base_model, str):
                        tokenizer_repo = base_model
                    else:
                        tokenizer_repo = hf_repo
                    printing.log_info(
                        f"Using tokenizer from base model: {tokenizer_repo}"
                    )
                    return tokenizer_repo

                # Fallback: check tags for base_model:
                tags = hf_info.get("tags", [])
                for tag in tags:
                    if tag.startswith("base_model:") and not tag.startswith(
                        "base_model:quantized:"
                    ):
                        tokenizer_repo = tag.replace("base_model:", "")
                        printing.log_info(
                            f"Using tokenizer from base model tag: {tokenizer_repo}"
                        )
                        return tokenizer_repo

        except requests.exceptions.RequestException as e:
            printing.log_warning(f"Could not query HuggingFace API: {e}")

        # Fallback to the checkpoint repo itself
        printing.log_info(f"Using tokenizer from checkpoint: {hf_repo}")
        return hf_repo

    def run(
        self,
        state: State,
        task: str,
        num_fewshot: int = 0,
        limit: Optional[int] = None,
        log_samples: bool = False,
        output_path: Optional[str] = None,
    ) -> State:
        """
        Run lm-eval-harness against a model loaded on Lemonade Server.

        Requires: Model must be loaded via the `load` tool first, which sets
        state.model to a ServerAdapter and state.server_url to the server URL.

        Args:
            state: State with model loaded via `load` tool
            task: Task(s) to evaluate (e.g., gsm8k, mmlu)
            num_fewshot: Number of few-shot examples
            limit: Limit number of examples per task
            log_samples: Whether to log samples
            output_path: Path to save results
        """
        # Check if lm-eval is available
        try:
            # pylint: disable=unused-import
            import lm_eval
        except ImportError:
            error_msg = (
                "lm-eval-harness is required but not installed. "
                "Please install it using one of the following commands:\n"
                "  pip install lemonade-sdk[dev]\n"
                "  pip install -e .[dev]\n"
            )
            printing.log_error(error_msg)
            raise ImportError(error_msg)

        # Validate that model was loaded via server_load.py
        if not isinstance(state.model, ServerAdapter):
            raise ValueError(
                "lm-eval-harness requires a model loaded via the 'load' tool. "
                "Use: lemonade-eval -i <model-name> load lm-eval-harness --task <task>\n"
                "Make sure Lemonade Server is running with 'lemonade-server serve'."
            )

        # Get server URL from state (set by server_load.py)
        server_url = getattr(state, "server_url", None)
        if not server_url:
            raise ValueError(
                "Server URL not found in state. "
                "The model must be loaded via the 'load' tool."
            )

        model_name = getattr(state, "checkpoint", "unknown")

        # Verify server is still healthy and get model info for tokenizer
        printing.log_info(f"Verifying server at {server_url}...")
        try:
            response = requests.get(f"{server_url}/api/v1/health", timeout=10)
            response.raise_for_status()
            printing.log_info("Server is healthy")
        except requests.exceptions.RequestException as e:
            raise RuntimeError(
                f"Cannot connect to Lemonade Server at {server_url}: {e}\n"
                "Make sure the server is still running."
            )

        # Get the HuggingFace base model to use as tokenizer source
        # lm-eval needs a valid HF repo with tokenizer files (not GGUF repos)
        tokenizer_repo = self._get_tokenizer_repo(server_url, model_name)

        # Set up output path
        if output_path is None:
            output_path = os.path.join(
                build.output_dir(state.cache_dir, state.build_name), "lm_eval_results"
            )

        os.makedirs(output_path, exist_ok=True)

        results_file = os.path.join(output_path, f"{task}_results.json")

        printing.log_info(f"Running lm-eval-harness on {task}...")

        # Build lm-eval-harness command
        # Use sys.executable -m to ensure cross-platform compatibility (Windows)
        cmd = [
            sys.executable,
            "-m",
            "lm_eval",
            "--model",
            "local-completions",
            "--tasks",
            task,
            "--model_args",
            (
                f"model={model_name},"
                f"base_url={server_url}/api/v1/completions,"
                f"tokenizer={tokenizer_repo},"
                f"num_concurrent=1,"
                f"max_retries=5,"
                f"retry_timeout=10,"
                f"tokenized_requests=False"
            ),
            "--num_fewshot",
            str(num_fewshot),
            "--output_path",
            results_file,
        ]

        if limit is not None:
            cmd.extend(["--limit", str(limit)])

        if log_samples:
            cmd.extend(["--log_samples"])

        try:
            # On Windows, set UTF-8 mode to handle Unicode output
            env = os.environ.copy()
            if sys.platform == "win32":
                env["PYTHONIOENCODING"] = "utf-8"

            # Execute lm-eval-harness command
            result = subprocess.run(
                cmd, check=True, text=True, capture_output=True, env=env
            )

            # Log relevant output and skip any parts that might cause encoding issues
            try:
                printing.log_info(result.stdout)
            except UnicodeEncodeError:
                printing.log_info(
                    "Results obtained successfully but couldn't display due to encoding issues"
                )

            # Process results from the JSON file
            self._process_results(results_file, state)

        except subprocess.CalledProcessError as e:
            printing.log_error(f"Error running lm-eval-harness: {e}")
            printing.log_error(f"stderr: {e.stderr}")
            raise
        except (IOError, ValueError) as e:
            printing.log_error(f"Error: {e}")
            raise

        return state
