# Using LM-Evaluation-Harness with Lemonade

This guide demonstrates how to use Lemonade with LM-Evaluation-Harness (lm-eval) to evaluate language model performance across a variety of standardized benchmarks. Whether you're comparing different model implementations or validating model capabilities, lm-eval provides a comprehensive framework for model assessment.
Refer to [Lemonade Server](../server_spec.md) to learn more about the server interface used by lm-eval for evaluations.

## What is LM-Evaluation-Harness?

[LM-Evaluation-Harness](https://github.com/EleutherAI/lm-evaluation-harness) (often called `lm-eval`) is an open-source framework for evaluating language models across a wide variety of tasks and benchmarks. Developed by EleutherAI, it has become a standard tool in the AI research community for consistent evaluation of language model capabilities.

The framework supports evaluating models on more than 200 tasks and benchmarks, including popular ones such as:

- MMLU (Massive Multitask Language Understanding)
- GSM8K (Grade School Math)
- HumanEval (Code generation)
- TruthfulQA
- WinoGrande
- HellaSwag
- And many others...

## Advantages of Using lm-eval for Accuracy Measurement

- **Standardization**: Provides a consistent methodology for comparing different models, ensuring fair comparisons across the industry.
- **Community adoption**: Used by major research labs, companies, and the open-source community including Hugging Face, Anthropic, and others.
- **Comprehensive evaluation**: Covers a wide range of capabilities from factual knowledge to reasoning.
- **Open-source**: Transparent methodology that's peer-reviewed by the AI research community.
- **Regular updates**: Continuously updated with new benchmarks and evaluation methods.
- **Reproducibility**: Enables reproducible research results across different models and implementations.
- **Cross-implementation compatibility**: Works with multiple model implementations (llama.cpp, OpenAI API, Hugging Face, etc.) enabling direct comparison of different implementations of the same model.


## Running lm-eval with Lemonade

Lemonade supports integration with lm-eval through its local LLM server. The basic workflow involves:

1. Setting up the environment.
1. Starting the Lemonade server.
1. Loading a model via the API.
1. Running lm-eval tests against the model through the lemonade server.


### Step 1: Environment Setup and Installation

Please refer to the [installation guide](https://lemonade-server.ai/install_options.html), using the PyPI or From Source methods, for environment setup.


### Step 2: Start the Lemonade Server

In a terminal with your environment activated, run the following command:

```powershell
lemonade-server-dev serve
```

This starts a local LLM server on port 8000 by default.

### Step 3: Load a Model

Use the following PowerShell command to load a model into the server:

```powershell
Invoke-RestMethod -Uri "http://localhost:8000/api/v1/load" -Method Post -Headers @{ "Content-Type" = "application/json" } -Body '{ "checkpoint": "meta-llama/Llama-3.2-1B-Instruct", "recipe": "hf-cpu" }'
```

Where:

- `checkpoint` can be changed to use other from Hugging Face (e.g., "meta-llama/Llama-3.2-3B-Instruct")
- `recipe` can be changed to use different backends (e.g., "oga-cpu" for CPU inference on OnnxRuntime GenAI, "oga-hybrid" for AMD Ryzen™ AI acceleration). For more information on Lemonade recipes, see the [Lemonade API ReadMe](../../lemonade_api.md).

### Step 4: Run lm-eval Tests

Now that the model is loaded, open a new PowerShell terminal, activate your environment, and run lm-eval tests using the following command:

```powershell
lm_eval --model local-completions --tasks mmlu_abstract_algebra --model_args model=meta-llama/Llama-3.2-1B-Instruct,base_url=http://localhost:8000/api/v1/completions,num_concurrent=1,max_retries=0,tokenized_requests=False --limit 5
```

Where:
- Change `--tasks` as needed to run other tests (e.g., `--tasks gsm8k`, `--tasks wikitext`, etc.)
For detailed tasks visit [lm-eval](https://github.com/EleutherAI/lm-evaluation-harness/blob/main/lm_eval/tasks/README.md)
- `checkpoint name` should match the model name loaded in step 2

## Types of Tests in lm-eval

The framework implements three primary evaluation methodologies that use different capabilities of language models:

### 1. Log Probability-Based Tests (e.g., MMLU)

These tests evaluate a model's ability to assign probabilities to different possible answers. The model predicts which answer is most likely based on conditional probabilities.

**Example:** In MMLU (Massive Multitask Language Understanding), the model is given a multiple-choice question and must assign probabilities to each answer choice. The model's performance is measured by how often it assigns the highest probability to the correct answer.

#### Commands to Log Probability-Based Tests

**Step 1:** Environment setup and installation - Please refer to the [installation guide](https://lemonade-server.ai/install_options.html), using the PyPI or From Source methods, for environment setup.

**Step 2:** Start the Lemonade Server.

In a terminal with your environment activated, run the following command:

```powershell
lemonade-server-dev serve
```
**Step 3:** Load a Model

```powershell
Invoke-RestMethod -Uri "http://localhost:8000/api/v1/load" -Method Post -Headers @{ "Content-Type" = "application/json" } -Body '{ "checkpoint": "meta-llama/Llama-3.2-1B-Instruct", "recipe": "hf-cpu" }'
```
**Step 4:** Run MMLU Tests

```powershell
lm_eval --model local-completions --tasks mmlu_abstract_algebra --model_args model=meta-llama/Llama-3.2-1B-Instruct,base_url=http://localhost:8000/api/v1/completions,num_concurrent=1,max_retries=0,tokenized_requests=False --limit 5
```
### 2. Rolling Log Probability Tests (e.g., WikiText)

These tests evaluate a model's ability to predict text by measuring the perplexity on held-out data. The model assigns probabilities to each token in a sequence, and performance is measured by how well it predicts the actual next tokens.

**Example:** In perplexity benchmarks like WikiText, the model is evaluated on how well it can predict each token in a document, using a rolling window approach for longer contexts.

#### Commands to Log Probability-Based Tests

**Step 1:** Environment setup and installation - Please refer to the [installation guide](https://lemonade-server.ai/install_options.html), using the PyPI or From Source methods, for environment setup.

**Step 2:** Start the Lemonade Server.

In a terminal with your environment activated, run the following command:

```powershell
lemonade-server-dev serve
```
**Step 3:** Load a Model

```powershell
Invoke-RestMethod -Uri "http://localhost:8000/api/v1/load" -Method Post -Headers @{ "Content-Type" = "application/json" } -Body '{ "checkpoint": "meta-llama/Llama-3.2-1B-Instruct", "recipe": "hf-cpu" }'
```
**Step 4:** Run Wikitext Tests

```powershell
lm_eval --model local-completions --tasks wikitext --model_args model=meta-llama/Llama-3.2-1B-Instruct,base_url=http://localhost:8000/api/v1/completions,num_concurrent=1,max_retries=0,tokenized_requests=False --limit 5
```
### 3. Generation-Based Tests (e.g., GSM8K)

These tests evaluate a model's ability to generate full responses to prompts. The model generates text that is then evaluated against reference answers or using specific metrics.

**Example:** In GSM8K (Grade School Math), the model is given a math problem and must generate a step-by-step solution. Performance is measured by whether the final answer is correct.

#### Commands to Log Probability-Based Tests

**Step 1:** Environment setup and installation - Please refer to the [installation guide](https://lemonade-server.ai/install_options.html), using the PyPI or From Source methods, for environment setup.

**Step 2:** Start the Lemonade Server.

In a terminal with your environment activated, run the following command:

```powershell
lemonade-server-dev serve
```
**Step 3:** Load a Model

```powershell
Invoke-RestMethod -Uri "http://localhost:8000/api/v1/load" -Method Post -Headers @{ "Content-Type" = "application/json" } -Body '{ "checkpoint": "meta-llama/Llama-3.2-1B-Instruct", "recipe": "hf-cpu" }'
```
**Step 4:** Run GSM8k Tests

```powershell
lm_eval --model local-completions --tasks gsm8k --model_args model=meta-llama/Llama-3.2-1B-Instruct,base_url=http://localhost:8000/api/v1/completions,num_concurrent=1,max_retries=0,tokenized_requests=False --limit 5
```
## Interpreting Results

lm-eval provides detailed results for each benchmark, typically including:

- **Accuracy**: Percentage of correct answers.
- **Exact Match**: For generation tasks, whether the generated answer exactly matches the reference.
- **F1 Score**: For certain tasks, measuring the overlap between generated and reference answers.
- **Perplexity**: For language modeling tasks, measuring how well the model predicts text.
- **Group breakdowns**: For some benchmarks, performance across different categories or question types.

Results are provided in a structured format at the end of evaluation, with both detailed and summary statistics.

## Future Work

- **Integrate lm-eval as a Lemonade tool**: Direct integration into the Lemonade CLI ecosystem.

## References

- [LM-Evaluation-Harness GitHub Repository](https://github.com/EleutherAI/lm-evaluation-harness)
- [EleutherAI Documentation](https://www.eleuther.ai/projects/lm-evaluation-harness) 

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->