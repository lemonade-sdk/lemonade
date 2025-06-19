import argparse
import json
import os
import socket
import subprocess
import sys
import time
from typing import Optional

from lemonade.state import State
from lemonade.tools import Tool
import lemonade.common.printing as printing
import lemonade.common.build as build


def is_port_in_use(port, host="localhost"):
    """
    Check if a port is in use
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex((host, port)) == 0


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
        self.server_runner = None

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
            "--server-port", type=int, default=8000, help="Port to use for the server"
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

    def _process_results(self, results_dir, state):
        """Process evaluation results and save to state stats"""
        if not os.path.exists(results_dir) or not os.path.isdir(results_dir):
            printing.log_warning(f"Results directory not found at {results_dir}")
            return

        model_dirs = [
            d
            for d in os.listdir(results_dir)
            if os.path.isdir(os.path.join(results_dir, d))
        ]

        if not model_dirs:
            printing.log_warning(f"No model directories found in {results_dir}")
            return

        model_dir = os.path.join(results_dir, model_dirs[0])
        printing.log_info(f"Found model directory: {model_dir}")

        # Find the results JSON file with timestamp
        results_files = [
            f
            for f in os.listdir(model_dir)
            if f.startswith("results_") and f.endswith(".json")
        ]

        if not results_files:
            printing.log_warning(f"No results files found in {model_dir}")
            return

        # Sort by timestamp
        results_files.sort(reverse=True)
        results_file_path = os.path.join(model_dir, results_files[0])
        printing.log_info(f"Processing results from {results_file_path}")

        # Read and process results
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
                            # Format metric name for stats
                            clean_metric = metric.replace(",", "_")
                            stat_name = f"lm_eval_{task_name}_{clean_metric}"

                            # Save to state stats as percentage
                            state.save_stat(stat_name, float(value) * 100)
                            state.save_stat(f"{stat_name}_units", "%")
                            self.status_stats.append(stat_name)

                            printing.log_info(
                                f"  {metric}: {value:.4f} ({value*100:.2f}%)"
                            )

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
                        state.save_stat(stat_name, float(avg_value) * 100)
                        state.save_stat(f"{stat_name}_units", "%")
                        self.status_stats.append(stat_name)
                        printing.log_info(
                            f"Average {metric}: {avg_value:.4f} ({avg_value*100:.2f}%)"
                        )

        except (IOError, json.JSONDecodeError) as e:
            printing.log_error(f"Error processing results: {e}")

    def run(
        self,
        state: State,
        task: str,
        server_port: int = 8000,
        server_host: str = "localhost",
        num_fewshot: int = 0,
        limit: Optional[int] = None,
        log_samples: bool = False,
        output_path: Optional[str] = None,
    ) -> State:

        import requests
        from lemonade.tools.server.utils.thread import ServerRunner

        model = state.model
        tokenizer = state.tokenizer

        if model is None or tokenizer is None:
            raise ValueError(
                "Model and tokenizer must be loaded in state before running lm-eval-harness"
            )

        # Set up output path
        if output_path is None:
            output_path = os.path.join(
                build.output_dir(state.cache_dir, state.build_name), "lm_eval_results"
            )

        os.makedirs(output_path, exist_ok=True)

        # Check if port is already in use
        if is_port_in_use(server_port, server_host):
            error_msg = (
                f"Port {server_port} is already in use. "
                "Please close all applications using this port and try again."
            )
            printing.log_error(error_msg)
            raise RuntimeError(error_msg)

        # Retroactively determine recipe based on model type to select correct iterator
        # The model is already loaded in server, so we only need recipe for iterator selection
        checkpoint = getattr(state, "checkpoint", "unknown")
        if "OrtGenaiModel" in str(type(model)):
            recipe = "oga-"
        else:
            recipe = "unknown"

        # Start the server thread
        self.server_runner = ServerRunner(
            model=model,
            tokenizer=tokenizer,
            checkpoint=checkpoint,
            recipe=recipe,
            host=server_host,
            port=server_port,
        )
        self.server_runner.start()

        # Wait for server initialization
        printing.log_info("Waiting for server initialization...")

        # Wait for server to start and be responsive
        server_url = f"http://{server_host}:{server_port}"
        max_retries = 30
        retry_delay = 1

        printing.log_info(f"Checking if server is available at {server_url}...")
        for i in range(max_retries):
            try:
                response = requests.get(f"{server_url}/api/v0/health", timeout=2)
                if response.status_code == 200:
                    printing.log_info(f"Server is ready after {i+1} attempts")
                    break
            except requests.exceptions.RequestException:
                if i < max_retries - 1:
                    time.sleep(retry_delay)
                else:
                    printing.log_error(
                        f"Server did not start after {max_retries} attempts"
                    )
                    raise RuntimeError("Failed to start the server")

        # Build API URL
        results_file = os.path.join(output_path, f"{task}_results")

        printing.log_info(f"Running lm-eval-harness on {task}...")

        # Build lm-eval-harness command
        cmd = [
            "lm_eval",
            "--model",
            "local-completions",
            "--tasks",
            task,
            "--model_args",
            (
                f"model={checkpoint},"
                f"base_url={server_url}/api/v0/completions,"
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

            # Process results from the correct location
            results_dir = os.path.join(output_path, f"{task}_results")
            self._process_results(results_dir, state)

        except subprocess.CalledProcessError as e:
            printing.log_error(f"Error running lm-eval-harness: {e}")
            printing.log_error(f"stderr: {e.stderr}")
        except (IOError, ValueError, requests.RequestException) as e:
            printing.log_error(f"Error: {e}")
        finally:
            # Shut down server
            if self.server_runner and self.server_runner.is_alive():
                printing.log_info("Shutting down server runner...")
                self.server_runner.shutdown()

            # Make sure we don't have any lingering references to state's model/tokenizer
            # that could prevent garbage collection
            self.server_runner = None

        return state
