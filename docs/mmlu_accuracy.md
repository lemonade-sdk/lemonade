
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

Use the syntax provided in the table to run that test subject with the `accuracy-mmlu` tool. For example, To run the "Abstract Algebra" subject, use `accuracy-mmlu --tests abstract_algebra`.

| Test Subject                        | Category          | `--tests` syntax                             |
|-------------------------------------|-------------------|-------------------------------------|
| Abstract Algebra                    | Math              | abstract_algebra                    |
| Anatomy                             | Health            | anatomy                             |
| Astronomy                           | Physics           | astronomy                           |
| Business Ethics                     | Business          | business_ethics                     |
| Clinical Knowledge                  | Health            | clinical_knowledge                  |
| College Biology                     | Biology           | college_biology                     |
| College Chemistry                   | Chemistry         | college_chemistry                   |
| College Computer Science            | Computer Science  | college_computer_science            |
| College Mathematics                 | Math              | college_mathematics                 |
| College Medicine                    | Health            | college_medicine                    |
| College Physics                     | Physics           | college_physics                     |
| Computer Security                   | Computer Science  | computer_security                   |
| Conceptual Physics                  | Physics           | conceptual_physics                  |
| Econometrics                        | Economics         | econometrics                        |
| Electrical Engineering              | Engineering       | electrical_engineering              |
| Elementary Mathematics              | Math              | elementary_mathematics              |
| Formal Logic                        | Philosophy        | formal_logic                        |
| Global Facts                        | Other             | global_facts                        |
| High School Biology                 | Biology           | high_school_biology                 |
| High School Chemistry               | Chemistry         | high_school_chemistry               |
| High School Computer Science        | Computer Science  | high_school_computer_science        |
| High School European History        | History           | high_school_european_history        |
| High School Geography               | Geography         | high_school_geography               |
| High School Government and Politics | Politics          | high_school_government_and_politics |
| High School Macroeconomics          | Economics         | high_school_macroeconomics          |
| High School Mathematics             | Math              | high_school_mathematics             |
| High School Microeconomics          | Economics         | high_school_microeconomics          |
| High School Physics                 | Physics           | high_school_physics                 |
| High School Psychology              | Psychology        | high_school_psychology              |
| High School Statistics              | Math              | high_school_statistics              |
| High School US History              | History           | high_school_us_history              |
| High School World History           | History           | high_school_world_history           |
| Human Aging                         | Health            | human_aging                         |
| Human Sexuality                     | Culture           | human_sexuality                     |
| International Law                   | Law               | international_law                   |
| Jurisprudence                       | Law               | jurisprudence                       |
| Logical Fallacies                   | Philosophy        | logical_fallacies                   |
| Machine Learning                    | Computer Science  | machine_learning                    |
| Management                          | Business          | management                          |
| Marketing                           | Business          | marketing                           |
| Medical Genetics                    | Health            | medical_genetics                    |
| Miscellaneous                       | Other             | miscellaneous                       |
| Moral Disputes                      | Philosophy        | moral_disputes                      |
| Moral Scenarios                     | Philosophy        | moral_scenarios                     |
| Nutrition                           | Health            | nutrition                           |
| Philosophy                          | Philosophy        | philosophy                          |
| Prehistory                          | History           | prehistory                          |
| Professional Accounting             | Other             | professional_accounting             |
| Professional Law                    | Law               | professional_law                    |
| Professional Medicine               | Health            | professional_medicine               |
| Professional Psychology             | Psychology        | professional_psychology             |
| Public Relations                    | Politics          | public_relations                    |
| Security Studies                    | Politics          | security_studies                    |
| Sociology                           | Culture           | sociology                           |
| US Foreign Policy                   | Politics          | us_foreign_policy                   |
| Virology                            | Health            | virology                            |
| World Religions                     | Philosophy        | world_religions                     |
