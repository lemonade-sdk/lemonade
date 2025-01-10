# LLAMA.CPP

Run transformer models using llama.cpp. This integration allows you to:
1. Load and run llama.cpp models
2. Benchmark model performance
3. Use the models with other tools like chat or MMLU accuracy testing

## Prerequisites

You need:
1. A compiled llama.cpp executable (llama-cli or llama-cli.exe)
2. A GGUF model file

### Building llama.cpp (if needed)

#### Linux
```bash
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
make
```

#### Windows
```bash
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
cmake -B build
cmake --build build --config Release
```

The executable will be in `build/bin/Release/llama-cli.exe` on Windows or `llama-cli` in the root directory on Linux.

## Usage

### Loading a Model

Use the `load-llama-cpp` tool to load a model:

```bash
lemonade -i MODEL_NAME load-llama-cpp \
    --executable PATH_TO_EXECUTABLE \
    --model-binary PATH_TO_GGUF_FILE
```

Parameters:
| Parameter     | Required | Default | Description                                           |
|--------------|----------|---------|-------------------------------------------------------|
| executable   | Yes      | -       | Path to llama-cli/llama-cli.exe                      |
| model-binary | Yes      | -       | Path to .gguf model file                             |
| threads      | No       | 1       | Number of threads for generation                      |
| context-size | No       | 512     | Context window size                                  |
| output-tokens| No       | 512     | Maximum number of tokens to generate                 |

### Benchmarking

After loading a model, you can benchmark it using `llama-cpp-bench`:

```bash
lemonade -i MODEL_NAME \
    load-llama-cpp \
        --executable PATH_TO_EXECUTABLE \
        --model-binary PATH_TO_GGUF_FILE \
    llama-cpp-bench
```

Benchmark parameters:
| Parameter         | Default                    | Description                               |
|------------------|----------------------------|-------------------------------------------|
| prompt           | "Hello, I am conscious and"| Input prompt for benchmarking            |
| context-size     | 512                        | Context window size                       |
| output-tokens    | 512                        | Number of tokens to generate              |
| iterations       | 1                          | Number of benchmark iterations            |
| warmup-iterations| 0                          | Number of warmup iterations (not counted) |

The benchmark will measure and report:
- Time to first token (prompt evaluation time)
- Token generation speed (tokens per second)

### Example Commands

#### Windows Example
```bash
# Load and benchmark a model
lemonade -i Qwen/Qwen2.5-0.5B-Instruct-GGUF \
    load-llama-cpp \
        --executable "C:\work\llama.cpp\build\bin\Release\llama-cli.exe" \
        --model-binary "C:\work\llama.cpp\models\qwen2.5-0.5b-instruct-fp16.gguf" \
    llama-cpp-bench \
        --iterations 3 \
        --warmup-iterations 1

# Run MMLU accuracy test
lemonade -i Qwen/Qwen2.5-0.5B-Instruct-GGUF \
    load-llama-cpp \
        --executable "C:\work\llama.cpp\build\bin\Release\llama-cli.exe" \
        --model-binary "C:\work\llama.cpp\models\qwen2.5-0.5b-instruct-fp16.gguf" \
    accuracy-mmlu \
        --tests management \
        --max-evals 2
```

#### Linux Example
```bash
# Load and benchmark a model
lemonade -i Qwen/Qwen2.5-0.5B-Instruct-GGUF \
    load-llama-cpp \
        --executable "./llama-cli" \
        --model-binary "./models/qwen2.5-0.5b-instruct-fp16.gguf" \
    llama-cpp-bench \
        --iterations 3 \
        --warmup-iterations 1
```

## Integration with Other Tools

After loading with `load-llama-cpp`, the model can be used with any tool that supports the ModelAdapter interface, including:
- accuracy-mmlu
- llm-prompt
- accuracy-humaneval
- and more

The integration provides:
- Platform-independent path handling (works on both Windows and Linux)
- Proper error handling with detailed messages
- Performance metrics collection
- Configurable generation parameters (temperature, top_p, top_k)
