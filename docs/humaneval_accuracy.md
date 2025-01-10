# Using the HumanEval accuracy test tools

The HumanEval benchmark is a code generation and functional correctness evaluation framework designed to assess language models' ability to generate Python code. It consists of 164 handwritten programming problems, each containing a function signature, docstring, body, and several unit tests. This benchmark focuses on evaluating a model's capability to generate functionally correct code that passes the test cases, making it particularly useful for assessing code generation capabilities.

This tool provides an automated way to evaluate language models on the HumanEval benchmark. It handles the process of downloading the dataset, generating code completions, executing them in a secure environment, and calculating pass@k metrics.

## Dataset

The HumanEval dataset is automatically downloaded from [OpenAI's human-eval repository](https://github.com/openai/human-eval) when you first run the benchmark. The dataset contains programming problems that test various aspects of Python programming, including:

- Basic programming operations
- String manipulation
- Mathematical computations
- List operations
- Algorithm implementation
- Data structure manipulation

## Running the Benchmark

```bash
lemonade -i meta-llama/Llama-3.2-1B oga-load --device igpu --dtype int4 accuracy-humaneval --k-samples 1 --first-n-samples 5 --timeout 30.0
```

### Optional arguments:

`--k-samples`: Number of completions to generate per prompt (default: 1). This parameter determines the k in pass@k metrics. For example:
- `--k-samples 1`: Calculates pass@1 (single attempt per problem)
- `--k-samples 10`: Calculates pass@10 (ten attempts per problem)
- `--k-samples 100`: Calculates pass@100 (hundred attempts per problem)

Higher k values provide more robust evaluation but take longer to run.

`--first-n-samples`: Evaluate only the first N problems from the dataset (default: entire dataset). Useful for quick testing or when you want to evaluate a subset of problems.

`--timeout`: Maximum time in seconds allowed for each test case execution (default: 30.0). This prevents infinite loops or long-running code from blocking the evaluation.

`--data-dir`: Custom directory for storing the HumanEval dataset (default: "<lemonade_cache_dir>/data/humaneval").

## How It Works

1. **Dataset Preparation:**
   - On first run, the tool downloads the HumanEval dataset (HumanEval.jsonl.gz)
   - The dataset contains function signatures, docstrings, and test cases
   - Each problem is structured to test specific programming capabilities
   - You can evaluate only the first N problems using `--first-n-samples`

2. **Code Generation:**
   - For each programming problem, the model is provided with a prompt containing:
     - Function signature (e.g., `def sort_numbers(numbers):`)
     - Docstring describing the function's purpose and requirements
   - The model generates k code completions for the function body (controlled by `--k-samples`)
   - These k samples are used to calculate the pass@k metric

3. **Secure Execution:**
   - Generated code is executed in a secure sandbox environment maintained by OpenAI's human-eval library. For your awareness, OpenAI's policy is to disable code execution by default, however lemonade enables code execution by default by automatically setting the environment variable `HF_ALLOW_CODE_EVAL=1`. OpenAI provides the following code execution protections:
     - **Process Isolation**: Each code sample runs in a separate process to prevent interference
     - **Resource Limits**:
       - CPU time limit (controlled by `--timeout`)
       - Memory usage restrictions
       - Maximum output size restrictions
     - **Restricted Access**:
       - No network access
       - No file system access outside test directory
       - No subprocess creation
       - No system calls
     - **Module Restrictions**:
       - Only allows importing standard Python libraries needed for testing
       - Blocks potentially dangerous modules (os, sys, subprocess, etc.)
   These security measures are implemented through:
   - Python's built-in `resource` module for resource limits
   - AST (Abstract Syntax Tree) analysis for code validation
   - Process-level isolation using `multiprocessing`
   - Custom import hooks to restrict module access

4. **Evaluation Metrics:**
   - **pass@k**: Percentage of problems solved with k attempts
     - pass@1: Success rate with single attempt
     - pass@10: Success rate within 10 attempts
     - pass@100: Success rate within 100 attempts
   - A problem is considered solved if all test cases pass
   - Results are normalized to percentages

5. **Output Files:**
   The tool generates several output files in the results directory:
   - `evaluation_results.csv`: Contains prompts, completions, and expected answers
   - `humaneval_predictions.jsonl`: Raw model predictions in JSONL format
   - `humaneval_predictions.jsonl_results.jsonl`: Detailed evaluation results

## Example Results Format

The evaluation produces metrics in the following format:
```json
{
    "pass@1": 0.25,    // 25% success rate with 1 attempt
    "pass@10": 0.45,   // 45% success rate within 10 attempts
    "pass@100": 0.65   // 65% success rate within 100 attempts
}
```

## Limitations

1. **Resource Requirements**: Generating multiple samples per problem (high k values) can be computationally intensive and time-consuming.
2. **Memory Usage**: Large language models may require significant memory, especially when generating multiple samples.

## References

1. [Evaluating Large Language Models Trained on Code](https://arxiv.org/abs/2107.03374)
2. [OpenAI HumanEval Repository](https://github.com/openai/human-eval) 