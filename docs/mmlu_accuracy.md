
# Using the MMLU accuracy test tools

The Massive Multitask Language Understanding (MMLU) benchmark is a comprehensive evaluation framework designed to assess the capabilities of language models across a wide range of subjects and disciplines. It encompasses a diverse set of questions covering topics from humanities to natural sciences, aiming to measure a model's depth and breadth of knowledge and its ability to generalize across different types of language understanding tasks. For detailed list of subjects tested refer [here](#detailed-list-of-subjects-categories-tested).

This tool provides an automated way to evaluate language models on the MMLU benchmark. It automates the process of downloading the dataset, preparing evaluation prompts, running the model to generate answers, and calculating accuracy metrics across different subjects within the MMLU dataset.

## Dataset
The MMLU dataset can be automatically downloaded by the script to the mmlu_data directory the first time you run the benchmark. The data is sourced from [here](https://people.eecs.berkeley.edu/~hendrycks/data.tar).

## Running the Benchmark

`lemonade -i facebook/opt-125m huggingface-load accuracy-mmlu --ntrain 5 --tests astronomy`

### Optional arguments:

`--ntrain`: The ntrain parameter is designed to specify the number of training examples to be used from a development (dev) set for creating context or background information in the prompts for evaluating language models, especially in tasks like MMLU (default: 5).

In the context of few-shot learning, particularly with language models, "shots" refer to the number of examples provided to the model to help it understand or adapt to the task at hand without explicit training.
By setting `--ntrain` to 5 we achieve 5-shot setting in MMLU.
The model is expected to generate an answer to the test question based on the context provided by the preceding question-answer pairs.

`--data-dir`: The directory where the MMLU data is stored (default: "<lemonade_cache_dir>/data").

`--tests`: Specific tests to run, identified by their subject names. Accepts multiple test names.


## How It Works

1. `Data Preparation:` On the first run, the script downloads the MMLU dataset and extracts it into the specified data directory. It then prepares the data by reading the development and test sets for the specified subjects.

1. `Prompt Generation:` For each subject, the script generates prompts from the development set to provide context for the test questions. This includes a configurable number of training examples (--ntrain) to help the model understand the task.

1. `Model Evaluation:` The specified language model is used to generate answers to each test question. Testing methodology adopted from [here](https://github.com/hendrycks/test).

1. `Accuracy Calculation:` The script compares the model-generated answers against the correct answers to calculate accuracy metrics for each subject.

1. `Saving Results:` Detailed results for each subject, including questions, prompts, correct and generated answers, and overall accuracy, are saved to CSV files in the specified results directory. A summary CSV file compiling accuracy metrics across all evaluated subjects is also generated and available in the cache directory.

## Detailed list of subjects/ categories tested

| Test Subject                     | Category          |
|----------------------------------|-------------------|
| Abstract Algebra                 | Math              |
| Anatomy                          | Health            |
| Astronomy                        | Physics           |
| Business Ethics                  | Business          |
| Clinical Knowledge               | Health            |
| College Biology                  | Biology           |
| College Chemistry                | Chemistry         |
| College Computer Science         | Computer Science  |
| College Mathematics              | Math              |
| College Medicine                 | Health            |
| College Physics                  | Physics           |
| Computer Security                | Computer Science  |
| Conceptual Physics               | Physics           |
| Econometrics                     | Economics         |
| Electrical Engineering           | Engineering       |
| Elementary Mathematics           | Math              |
| Formal Logic                     | Philosophy        |
| Global Facts                     | Other             |
| High School Biology              | Biology           |
| High School Chemistry            | Chemistry         |
| High School Computer Science     | Computer Science  |
| High School European History     | History           |
| High School Geography            | Geography         |
| High School Government and Politics | Politics        |
| High School Macroeconomics       | Economics         |
| High School Mathematics          | Math              |
| High School Microeconomics       | Economics         |
| High School Physics              | Physics           |
| High School Psychology           | Psychology        |
| High School Statistics           | Math              |
| High School US History           | History           |
| High School World History        | History           |
| Human Aging                      | Health            |
| Human Sexuality                  | Culture           |
| International Law                | Law               |
| Jurisprudence                    | Law               |
| Logical Fallacies                | Philosophy        |
| Machine Learning                 | Computer Science  |
| Management                       | Business          |
| Marketing                        | Business          |
| Medical Genetics                 | Health            |
| Miscellaneous                    | Other             |
| Moral Disputes                   | Philosophy        |
| Moral Scenarios                  | Philosophy        |
| Nutrition                        | Health            |
| Philosophy                       | Philosophy        |
| Prehistory                       | History           |
| Professional Accounting          | Other             |
| Professional Law                 | Law               |
| Professional Medicine            | Health            |
| Professional Psychology          | Psychology        |
| Public Relations                 | Politics          |
| Security Studies                 | Politics          |
| Sociology                        | Culture           |
| US Foreign Policy                | Politics          |
| Virology                         | Health            |
| World Religions                  | Philosophy        |
