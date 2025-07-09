
# Perplexity Evaluation


## Overview

Perplexity is a measurement of how well a probability model predicts a sample. A lower perplexity indicates the model is more confident in its predictions. In the context of language models, perplexity measures the likelihood of the sequence according to the model, given as:

`Perplexity (P) = exp(Average Negative Log-Likelihood)`

`Where Average Negative Log-Likelihood = (1/N) * Sum[-log p(x_i) from i=1 to N]`


## Script Functionality

### Key Components

- **`max_length`**: The maximum input length the model can handle at once (set by the model's configuration).
- **`stride`**: The step size for the window, set to half of `max_length` to ensure some overlap and preserve context.
- **`seq_len`**: The total length of the tokenized input.

### Detailed Steps

1. **Load Model and Tokenizer**: Receive the model and tokenizer with specified configurations.
2. **Load and Prepare Data**: Loads the "wikitext-2-raw-v1" dataset and concatenates texts with double newlines. The data is then tokenized.
3. **Sliding Window Perplexity Calculation**: The script uses a sliding window approach (with a stride of half the window size) to calculate the perplexity for subsets of the data, adjusting for the maximum input length of the model:
    - For each window, input data is processed, and the corresponding labels are adjusted to mask out irrelevant parts (using `-100`).
    - The model computes the logits and loss for each window.
    - Predicted and actual words at the end of each window are logged for analysis.
4. **Logging to CSV**: Summarizes the context window, predicted and actual next words, and loss for each window into a CSV file for further analysis.
5. **Perplexity Calculation**: Calculates the total negative log-likelihood adjusted by the effective token count for each window, then computes the average across all tokens to determine the perplexity.

### Example Outputs

The script outputs a CSV file named `summary_results.csv` with the following columns:

- **Context (Partial context displayed for Brevity)**
- **Predicted next word**
- **Actual next word**
- **Loss for this window**

These entries help in understanding how the model is performing at each step of the text.

## How to Interpret Perplexity Results

Understanding Perplexity
Definition: Perplexity is defined as the exponential of the average negative log-likelihood of a model on a given test set. 

Lower Values are Better: A lower perplexity score indicates that the model has a higher probability of correctly predicting the sample, suggesting better performance. A lower perplexity means the model is more certain about its predictions.

### Interpretation:

**High Perplexity:** Indicates confusion or a high level of uncertainty in the model’s predictions. A high perplexity can suggest that the model's language understanding is poor or that the model is not well-tuned for the given data.

**Low Perplexity:** Suggests that the model predictions are more accurate and that it assigns higher probabilities to the actual observed outcomes. This is indicative of a model that has a good grasp of the language patterns seen in the test set.
Practical Implications

**Model Comparison:** Perplexity is particularly useful for comparing different versions of the same model (e.g., before and after quantization, fine-tuning or training on additional data). The model with the lower perplexity is generally considered better at modeling the language of the test corpus.

**Model Selection for Applications:** For applications involving language generation (like machine translation, text summarization, or chatbots), selecting a model with lower perplexity might result in more fluent, coherent, and contextually appropriate text output.

**Diagnosing Model Fit:** High perplexity could indicate underfitting, where the model is too simple to capture the complexity of the language data. It can also help in diagnosing whether the model is well-suited for the specific domain of the text being modeled.


### Caveats in Interpretation

**Dependency on Test Set:** Perplexity is highly dependent on the test set used. A model can show very different perplexity scores on different datasets. Therefore, it's important to consider the nature and domain of the test set when evaluating perplexity.

**Not a Complete Measure:** While perplexity provides a measure of how uncertain a model is about its predictions, it does not directly measure how coherent or contextually appropriate generated texts are. Other qualitative assessments and metrics might be necessary to fully evaluate a language model's output.

**Comparison Across Different Data:** Comparing perplexity scores across models trained or tested on different datasets can be misleading because the intrinsic difficulty of the datasets can affect the perplexity.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->