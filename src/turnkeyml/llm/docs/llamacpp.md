# LLAMA.CPP

Run transformer models using a Llama.cpp binary and checkpoint. This model can then be used with chatting or benchmarks such as MMLU.

## Prerequisites

This flow has been verified with a generic Llama.cpp model.

These instructions are only for linux or Windows with wsl. It may be necessary to be running WSL in an Administrator command prompt.

These instructions also assume that TurnkeyML's llm extensions have been installed (for example with "pip install -e .[llm]")


### Set up Environment (Assumes TurnkeyML is already installed)

Build or obtain the Llama.cpp model and desired checkpoint.
For example (see the [llama.cpp](https://github.com/ggerganov/llama.cpp/blob/master/docs/build.md
) source for more details):
1. cd ~
1. git clone https://github.com/ggerganov/llama.cpp
1. cd llama.cpp
1. make
1. cd models
1. wget https://huggingface.co/TheBloke/Dolphin-Llama2-7B-GGUF/resolve/main/dolphin-llama2-7b.Q5_K_M.gguf


## Usage

The Llama.cpp tool currently supports the following parameters

| Parameter    | Definition                                                                  | Default |
| ---------    | ----------------------------------------------------                        | ------- |
| executable   | Path to the Llama.cpp-generated application binary                          | None    |
| model-binary | Model checkpoint (do not use if --input is passed to lemonade)              | None    |
| threads      | Number of threads to use for computation                                    | 1       |
| context-size | Maximum context length                                                      | 512     |
| temp         | Temperature to use for inference (leave out to use the application default) | None    |

### Example (assuming Llama.cpp built and a checkpoint loaded as above)

```bash
lemonade --input ~/llama.cpp/models/dolphin-llama2-7b.Q5_K_M.gguf load-llama-cpp --executable ~/llama.cpp/llama-cli accuracy-mmlu --ntrain 5
```

On windows, the llama.cpp binary might be in a different location (such as llama.cpp\build\bin\Release\), in which case the command mgiht be something like:
```bash
lemonade --input ~\llama.cpp\models\dolphin-llama2-7b.Q5_K_M.gguf load-llama-cpp --executable ~\llama.cpp\build\bin\Release\llama-cli accuracy-mmlu --ntrain 5
```