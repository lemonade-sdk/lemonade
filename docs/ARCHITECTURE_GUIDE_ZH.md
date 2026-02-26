# Lemonade Server 架构详解学习指南

> 本文档旨在帮助开发者深入理解 Lemonade Server 的架构设计和代码实现

## 目录

1. [项目概述](#1-项目概述)
2. [目录结构](#2-目录结构)
3. [核心架构](#3-核心架构)
4. [启动流程](#4-启动流程)
5. [后端系统](#5-后端系统)
6. [模型管理](#6-模型管理)
7. [配置系统](#7-配置系统)
8. [请求处理流程](#8-请求处理流程)
9. [关键代码解析](#9-关键代码解析)
10. [扩展开发指南](#10-扩展开发指南)

---

## 1. 项目概述

### 1.1 什么是 Lemonade Server？

Lemonade Server 是一个**本地 AI 推理服务器**，支持在用户的 GPU 和 NPU 上运行：
- **LLM（大语言模型）** - 文本生成、对话
- **图像生成** - Stable Diffusion
- **语音识别** - Whisper
- **语音合成** - Kokoro TTS

### 1.2 技术栈

| 组件 | 技术 |
|------|------|
| 服务器核心 | C++ 17 |
| HTTP 框架 | cpp-httplib |
| JSON 处理 | nlohmann/json |
| CLI 解析 | CLI11 |
| 前端应用 | Electron + React |
| 推理后端 | llama.cpp, whisper.cpp, sd.cpp, FLM 等 |

### 1.3 支持的推理后端（Recipes）

| Recipe | 设备 | 用途 |
|--------|------|------|
| `llamacpp` | GPU (Vulkan/ROCm/Metal) / CPU | LLM 推理 |
| `flm` | NPU (XDNA2) | LLM 推理（AMD NPU） |
| `ryzenai-llm` | NPU (XDNA2) | LLM 推理（RyzenAI） |
| `whispercpp` | NPU / CPU | 语音识别 |
| `sd-cpp` | GPU (ROCm) / CPU | 图像生成 |
| `kokoro` | CPU | 语音合成 |

---

## 2. 目录结构

```
lemonade/
├── src/
│   ├── cpp/                      # C++ 服务器核心
│   │   ├── include/lemon/        # 头文件
│   │   │   ├── backends/         # 后端接口定义
│   │   │   ├── utils/            # 工具类
│   │   │   ├── server.h          # 主服务器类
│   │   │   ├── router.h          # 请求路由器
│   │   │   ├── model_manager.h   # 模型管理器
│   │   │   ├── wrapped_server.h  # 后端基类
│   │   │   └── ...
│   │   ├── server/               # 服务器实现
│   │   │   ├── main.cpp          # 程序入口
│   │   │   ├── server.cpp        # HTTP 服务器
│   │   │   ├── router.cpp        # 路由实现
│   │   │   ├── model_manager.cpp # 模型管理
│   │   │   ├── backends/         # 后端实现
│   │   │   │   ├── llamacpp_server.cpp
│   │   │   │   ├── fastflowlm_server.cpp
│   │   │   │   ├── whisper_server.cpp
│   │   │   │   └── ...
│   │   │   └── utils/            # 工具实现
│   │   ├── tray/                 # 系统托盘应用
│   │   └── resources/            # 配置资源
│   │       ├── server_models.json    # 内置模型定义
│   │       └── backend_versions.json # 后端版本配置
│   ├── app/                      # Electron 桌面应用
│   └── web-app/                  # Web 界面
├── docs/                         # 文档
├── test/                         # 测试
└── examples/                     # 示例代码
```

---

## 3. 核心架构

### 3.1 架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        CLI / Tray App                           │
│                    (cli_parser.cpp / tray/)                     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Server                                  │
│                        (server.cpp)                              │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              HTTP Server (cpp-httplib)                   │    │
│  │    /v1/chat/completions  /v1/models  /api/pull  ...     │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Router                                  │
│                        (router.cpp)                              │
│   - 模型路由 (根据请求中的 model 字段)                           │
│   - 多模型管理 (LRU 缓存, NPU 独占)                              │
│   - 负载追踪 (busy 状态)                                         │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     WrappedServer (基类)                         │
│                   (wrapped_server.h/cpp)                         │
│   - 子进程管理                                                   │
│   - 请求转发                                                     │
│   - 遥测数据收集                                                 │
└─────────────────────────────────────────────────────────────────┘
           │              │              │              │
           ▼              ▼              ▼              ▼
┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
│  LlamaCpp      │ │  FastFlowLM    │ │  Whisper       │ │  SD.cpp        │
│  Server        │ │  Server        │ │  Server        │ │  Server        │
│ (GPU/CPU LLM)  │ │ (NPU LLM)      │ │ (语音识别)     │ │ (图像生成)     │
└────────────────┘ └────────────────┘ └────────────────┘ └────────────────┘
         │                 │                 │                 │
         ▼                 ▼                 ▼                 ▼
┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
│  llama-server  │ │  flm serve     │ │  whisper       │ │  sd.cpp        │
│  (子进程)      │ │  (子进程)      │ │  (子进程)      │ │  (子进程)      │
└────────────────┘ └────────────────┘ └────────────────┘ └────────────────┘
```

### 3.2 核心类关系

```
ICapability (接口)
    │
    ├── ICompletionServer     # chat_completion(), completion()
    ├── IEmbeddingsServer     # embeddings()
    ├── IRerankingServer      # reranking()
    ├── IAudioServer          # audio_transcriptions()
    ├── ITextToSpeechServer   # audio_speech()
    └── IImageServer          # image_generations(), image_edits()

WrappedServer (基类，实现 ICompletionServer)
    │
    ├── LlamaCppServer        # llama.cpp 后端
    ├── FastFlowLMServer      # FLM NPU 后端
    ├── RyzenAIServer         # RyzenAI NPU 后端
    ├── WhisperServer         # Whisper 语音识别 (实现 IAudioServer)
    ├── KokoroServer          # Kokoro TTS (实现 ITextToSpeechServer)
    └── SDServer              # Stable Diffusion (实现 IImageServer)
```

---

## 4. 启动流程

### 4.1 程序入口 (main.cpp)

```cpp
int main(int argc, char** argv) {
    // 1. 单实例检查
    if (SingleInstance::IsAnotherInstanceRunning("Router")) {
        return 1;  // 已有实例运行
    }

    // 2. 解析命令行参数
    CLIParser parser;
    parser.parse(argc, argv);
    auto config = parser.get_config();

    // 3. 创建服务器实例
    Server server(
        config.port,              // 端口 (默认 8000)
        config.host,              // 主机 (默认 localhost)
        config.log_level,         // 日志级别
        config.recipe_options,    // 配置选项 (包含 ctx_size 等)
        config.max_loaded_models, // 最大加载模型数
        config.extra_models_dir,  // 额外模型目录
        config.no_broadcast       // 是否禁用 UDP 广播
    );

    // 4. 启动服务器
    server.run();
    return 0;
}
```

### 4.2 服务器初始化 (server.cpp)

```cpp
Server::Server(...) {
    // 1. 初始化模型管理器
    model_manager_ = std::make_unique<ModelManager>();
    model_manager_->set_extra_models_dir(extra_models_dir);

    // 2. 初始化后端管理器
    backend_manager_ = std::make_unique<BackendManager>();

    // 3. 初始化路由器
    router_ = std::make_unique<Router>(
        default_options,     // 默认配置（包含 ctx_size）
        log_level,
        model_manager_.get(),
        max_loaded_models,
        backend_manager_.get()
    );

    // 4. 创建 HTTP 服务器
    http_server_ = std::make_unique<httplib::Server>();
    setup_routes(*http_server_);
    setup_cors(*http_server_);
}
```

### 4.3 配置选项传递链

```
命令行参数:  lemonade-server serve --ctx-size 8192
                    │
                    ▼
CLIParser::parse() → ServerConfig.recipe_options["ctx_size"] = 8192
                    │
                    ▼
Server 构造函数 → default_options_ = config.recipe_options
                    │
                    ▼
Router 构造函数 → default_options_ = default_options
                    │
                    ▼
Router::load_model() → RecipeOptions effective_options =
                         options.inherit(model_info.recipe_options.inherit(default_opt))
                    │
                    ▼
FastFlowLMServer::load() → int ctx_size = options.get_option("ctx_size")
                    │
                    ▼
启动子进程: flm serve model --ctx-len 8192 --port 8001
```

---

## 5. 后端系统

### 5.1 WrappedServer 基类

`WrappedServer` 是所有后端的基类，提供：

```cpp
class WrappedServer : public ICompletionServer {
protected:
    std::string server_name_;      // 后端名称
    int port_;                     // 后端端口
    ProcessHandle process_handle_; // 子进程句柄
    ModelManager* model_manager_;  // 模型管理器引用

    // 多模型支持
    std::string model_name_;       // 当前模型名
    ModelType model_type_;         // 模型类型 (LLM/AUDIO/IMAGE...)
    DeviceType device_type_;       // 设备类型 (CPU/GPU/NPU)
    RecipeOptions recipe_options_; // 配置选项

public:
    // 纯虚函数 - 子类必须实现
    virtual void load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade = false) = 0;
    virtual void unload() = 0;
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;

protected:
    // 通用方法
    int choose_port();                    // 选择可用端口
    bool wait_for_ready(...);             // 等待后端就绪
    json forward_request(...);            // 转发 HTTP 请求
    void forward_streaming_request(...);  // 转发流式请求
};
```

### 5.2 FastFlowLM 后端实现（FLM/NPU）

```cpp
// fastflowlm_server.cpp

void FastFlowLMServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    // 1. 从配置中获取 ctx_size
    int ctx_size = options.get_option("ctx_size");  // 默认 4096

    // 2. 检查并安装 FLM
    auto install_result = FastFlowLMServer::install_if_needed();

    // 3. 下载模型（如果需要）
    download_model(model_info.checkpoint(), do_not_upgrade);

    // 4. 选择端口
    port_ = choose_port();

    // 5. 构建命令行参数
    std::vector<std::string> args = {
        "serve",
        model_info.checkpoint(),
        "--ctx-len", std::to_string(ctx_size),  // ← ctx_size 传递给 flm
        "--port", std::to_string(port_),
        "--host", "127.0.0.1"
    };

    // 6. 启动 flm 子进程
    std::string flm_path = get_flm_path();
    process_handle_ = utils::ProcessManager::start_process(flm_path, args);

    // 7. 等待后端就绪
    bool ready = wait_for_ready();
    if (!ready) {
        throw std::runtime_error("flm-server failed to start");
    }

    is_loaded_ = true;
}

json FastFlowLMServer::chat_completion(const json& request) {
    // FLM 需要在请求中使用 checkpoint 名称（如 "gemma3:4b"）
    json modified_request = request;
    modified_request["model"] = checkpoint_;
    return forward_request("/v1/chat/completions", modified_request);
}
```

### 5.3 LlamaCpp 后端实现

```cpp
// llamacpp_server.cpp

void LlamaCppServer::load(...) {
    // 从配置获取选项
    int ctx_size = options.get_option("ctx_size");
    std::string backend = options.get_option("llamacpp_backend");  // vulkan/rocm/cpu
    std::string custom_args = options.get_option("llamacpp_args");

    // 获取 llama-server 路径
    std::string llama_path = get_llama_server_path(backend);

    // 构建参数
    std::vector<std::string> args = {
        "--model", model_info.resolved_path(),
        "--ctx-size", std::to_string(ctx_size),
        "--port", std::to_string(port_),
        "--host", "127.0.0.1"
    };

    // 视觉模型添加 mmproj
    if (!model_info.mmproj().empty()) {
        args.push_back("--mmproj");
        args.push_back(model_info.resolved_path("mmproj"));
    }

    // 启动进程
    process_handle_ = utils::ProcessManager::start_process(llama_path, args);
}
```

---

## 6. 模型管理

### 6.1 ModelManager 类

```cpp
class ModelManager {
public:
    // 获取所有支持的模型
    std::map<std::string, ModelInfo> get_supported_models();

    // 获取已下载的模型
    std::map<std::string, ModelInfo> get_downloaded_models();

    // 下载模型
    void download_model(const std::string& model_name, const json& model_data);

    // 注册用户模型
    void register_user_model(const std::string& model_name, const json& model_data);

    // 删除模型
    void delete_model(const std::string& model_name);

private:
    json server_models_;   // 内置模型定义 (server_models.json)
    json user_models_;     // 用户自定义模型
    std::map<std::string, ModelInfo> models_cache_;  // 模型缓存
};
```

### 6.2 ModelInfo 结构

```cpp
struct ModelInfo {
    std::string model_name;                        // 模型名称
    std::map<std::string, std::string> checkpoints; // HuggingFace checkpoint
    std::map<std::string, std::string> resolved_paths; // 本地路径
    std::string recipe;                            // 推理 recipe
    std::vector<std::string> labels;               // 标签 (reasoning, vision...)
    bool downloaded = false;                       // 是否已下载
    double size = 0.0;                             // 模型大小 (GB)
    RecipeOptions recipe_options;                  // 模型特定配置

    ModelType type = ModelType::LLM;   // 模型类型
    DeviceType device = DEVICE_NONE;   // 目标设备

    // 工具方法
    std::string checkpoint(const std::string& type = "main") const;
    std::string resolved_path(const std::string& type = "main") const;
};
```

### 6.3 模型定义示例 (server_models.json)

```json
{
  "Gemma-3-4b-it-GGUF": {
    "checkpoints": {
      "main": "google/gemma-3-4b-it-GGUF:gemma-3-4b-it-Q4_K_M.gguf"
    },
    "recipe": "llamacpp",
    "size": 2.8,
    "labels": ["suggested", "hybrid"],
    "suggested": true
  },
  "Gemma3-4b-it-FLM": {
    "checkpoints": {
      "main": "gemma3:4b"
    },
    "recipe": "flm",
    "size": 4.3,
    "labels": ["npu"]
  },
  "Whisper-Large-v3-Turbo": {
    "checkpoints": {
      "main": "openai/whisper-large-v3-turbo"
    },
    "recipe": "whispercpp",
    "labels": ["audio"],
    "size": 1.6
  }
}
```

---

## 7. 配置系统

### 7.1 RecipeOptions 类

```cpp
class RecipeOptions {
public:
    // 构造函数 - 从 recipe 名称和 JSON 选项创建
    RecipeOptions(const std::string& recipe, const json& options);

    // 获取选项值（如果不存在则返回默认值）
    json get_option(const std::string& opt) const;

    // 继承另一个 RecipeOptions 的值
    RecipeOptions inherit(const RecipeOptions& options) const;

    // 添加 CLI 选项
    static void add_cli_options(CLI::App& app, json& storage);

private:
    std::string recipe_;   // recipe 名称
    json options_;         // 选项值
};
```

### 7.2 默认配置值

```cpp
static const json DEFAULTS = {
    {"ctx_size", 4096},          // 上下文大小
    {"llamacpp_backend", "vulkan"},  // llama.cpp 后端
    {"llamacpp_args", ""},       // 自定义参数
    {"sd-cpp_backend", "cpu"},   // SD 后端
    {"whispercpp_backend", "npu"}, // Whisper 后端
    {"steps", 20},               // 图像生成步数
    {"cfg_scale", 7.0},          // CFG 比例
    {"width", 512},              // 图像宽度
    {"height", 512}              // 图像高度
};
```

### 7.3 各 Recipe 支持的配置

```cpp
static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") {
        return {"ctx_size", "llamacpp_backend", "llamacpp_args"};
    } else if (recipe == "whispercpp") {
        return {"whispercpp_backend"};
    } else if (recipe == "ryzenai-llm" || recipe == "flm") {
        return {"ctx_size"};  // FLM 只支持 ctx_size
    } else if (recipe == "sd-cpp") {
        return {"sd-cpp_backend", "steps", "cfg_scale", "width", "height"};
    }
    return {};
}
```

### 7.4 配置优先级（继承机制）

```
最高优先级
    │
    ├── 1. API 请求中的参数
    │
    ├── 2. lemonade-server run Model --ctx-size 8192
    │
    ├── 3. 模型定义中的 recipe_options
    │
    ├── 4. lemonade-server serve --ctx-size 4096
    │
    └── 5. 默认值 (DEFAULTS)
最低优先级
```

代码实现：
```cpp
void Router::load_model(..., RecipeOptions options, ...) {
    // 创建默认配置
    RecipeOptions default_opt = RecipeOptions(model_info.recipe, default_options_);

    // 继承合并：请求参数 > 模型配置 > 全局配置
    RecipeOptions effective_options = options.inherit(
        model_info.recipe_options.inherit(default_opt)
    );

    // 使用最终配置
    new_server->load(model_name, model_info, effective_options);
}
```

---

## 8. 请求处理流程

### 8.1 HTTP 路由设置

```cpp
void Server::setup_routes(httplib::Server& web_server) {
    // OpenAI 兼容端点
    web_server.Post("/v1/chat/completions",
        [this](auto& req, auto& res) { handle_chat_completions(req, res); });
    web_server.Post("/v1/completions", ...);
    web_server.Post("/v1/embeddings", ...);
    web_server.Get("/v1/models", ...);

    // 管理端点
    web_server.Post("/api/pull", ...);    // 下载模型
    web_server.Post("/api/load", ...);    // 加载模型
    web_server.Post("/api/unload", ...);  // 卸载模型
    web_server.Delete("/api/models/:id", ...);  // 删除模型

    // 系统端点
    web_server.Get("/api/health", ...);
    web_server.Get("/api/system-info", ...);
}
```

### 8.2 Chat Completion 请求流程

```
客户端请求:
POST /v1/chat/completions
{
  "model": "Gemma-3-4b-it-GGUF",
  "messages": [{"role": "user", "content": "Hello"}],
  "stream": false
}
        │
        ▼
Server::handle_chat_completions()
        │
        ├── 1. 解析请求 JSON
        ├── 2. 提取 model 名称
        ├── 3. 自动加载模型（如果未加载）
        │       └── auto_load_model_if_needed(model_name)
        │               └── router_->load_model(...)
        │
        ▼
Router::chat_completion(request)
        │
        ├── 1. 查找已加载的模型服务器
        │       server = find_server_by_model_name(model_name)
        ├── 2. 标记为忙碌状态
        │       server->set_busy(true)
        ├── 3. 调用后端的 chat_completion
        │
        ▼
LlamaCppServer::chat_completion(request)
        │
        ├── 1. 转发请求到 llama-server 子进程
        │       forward_request("/v1/chat/completions", request)
        │
        ▼
llama-server (localhost:8001)
        │
        ├── 执行实际推理
        │
        ▼
返回响应给客户端
```

### 8.3 流式响应处理

```cpp
void Router::chat_completion_stream(const std::string& request_body,
                                    httplib::DataSink& sink) {
    execute_streaming(request_body, sink, [&](WrappedServer* server) {
        server->forward_streaming_request("/v1/chat/completions",
                                          request_body, sink);
    });
}

void WrappedServer::forward_streaming_request(const std::string& endpoint,
                                              const std::string& request_body,
                                              httplib::DataSink& sink,
                                              bool sse) {
    httplib::Client client(get_base_url());

    // 发送请求并逐块转发响应
    auto res = client.Post(endpoint, request_body, "application/json",
        [&sink](const char* data, size_t len) {
            // 实时转发到客户端
            sink.write(data, len);
            return true;
        });
}
```

---

## 9. 关键代码解析

### 9.1 多模型 LRU 缓存管理

```cpp
void Router::load_model(...) {
    // NPU 独占检查 - NPU 一次只能运行一个模型
    if (device_type & DEVICE_NPU) {
        WrappedServer* npu_server = find_npu_server();
        if (npu_server) {
            evict_server(npu_server);  // 驱逐现有 NPU 模型
        }
    }

    // LRU 驱逐检查
    int current_count = count_servers_by_type(model_type);
    if (max_models != -1 && current_count >= max_models) {
        WrappedServer* lru = find_lru_server_by_type(model_type);
        if (lru) {
            evict_server(lru);  // 驱逐最久未使用的模型
        }
    }

    // 创建并加载新后端
    auto new_server = create_backend_server(model_info);
    new_server->load(model_name, model_info, effective_options);
    loaded_servers_.push_back(std::move(new_server));
}
```

### 9.2 后端进程管理

```cpp
// utils/process_manager.h

struct ProcessHandle {
    void* handle;      // 平台特定的进程句柄
    int pid;           // 进程 ID
};

class ProcessManager {
public:
    // 启动进程
    static ProcessHandle start_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir = "",
        bool show_output = false,
        bool filter_health_checks = false
    );

    // 检查进程是否运行
    static bool is_running(const ProcessHandle& handle);

    // 停止进程
    static void stop_process(ProcessHandle& handle);

    // 获取退出码
    static int get_exit_code(const ProcessHandle& handle);
};
```

### 9.3 端口选择

```cpp
int WrappedServer::choose_port() {
    // 从 8001 开始查找可用端口
    for (int port = 8001; port < 9000; ++port) {
        httplib::Server test_server;
        if (test_server.bind_to_port("127.0.0.1", port)) {
            return port;
        }
    }
    throw std::runtime_error("No available port found");
}
```

---

## 10. 扩展开发指南

### 10.1 添加新的推理后端

**步骤 1: 创建头文件**

```cpp
// include/lemon/backends/my_server.h
#pragma once

#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

class MyServer : public WrappedServer {
public:
    MyServer(const std::string& log_level,
             ModelManager* model_manager,
             BackendManager* backend_manager);
    ~MyServer();

    // 必须实现的方法
    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;
    void unload() override;
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

private:
    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
```

**步骤 2: 实现后端**

```cpp
// server/backends/my_server.cpp
#include "lemon/backends/my_server.h"

namespace lemon {
namespace backends {

MyServer::MyServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager)
    : WrappedServer("MyServer", log_level, model_manager, backend_manager) {
}

void MyServer::load(const std::string& model_name,
                   const ModelInfo& model_info,
                   const RecipeOptions& options,
                   bool do_not_upgrade) {
    // 1. 获取配置
    int ctx_size = options.get_option("ctx_size");

    // 2. 选择端口
    port_ = choose_port();

    // 3. 构建命令行参数
    std::vector<std::string> args = {
        "--model", model_info.resolved_path(),
        "--ctx-size", std::to_string(ctx_size),
        "--port", std::to_string(port_)
    };

    // 4. 启动子进程
    std::string exe_path = "/path/to/my-inference-server";
    process_handle_ = utils::ProcessManager::start_process(exe_path, args);

    // 5. 等待就绪
    if (!wait_for_ready("/health")) {
        throw std::runtime_error("Server failed to start");
    }

    is_loaded_ = true;
}

void MyServer::unload() {
    if (is_loaded_ && process_handle_.handle) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        is_loaded_ = false;
    }
}

json MyServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json MyServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

} // namespace backends
} // namespace lemon
```

**步骤 3: 注册后端到 Router**

```cpp
// router.cpp - create_backend_server 方法

std::unique_ptr<WrappedServer> Router::create_backend_server(const ModelInfo& model_info) {
    // ... 现有代码 ...

    } else if (model_info.recipe == "my-recipe") {
        std::cout << "[Router] Creating MyServer backend" << std::endl;
        return std::make_unique<backends::MyServer>(log_level_, model_manager_, backend_manager_);
    }

    // ... 现有代码 ...
}
```

**步骤 4: 添加配置支持**

```cpp
// recipe_options.cpp

static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    // ... 现有代码 ...

    } else if (recipe == "my-recipe") {
        return {"ctx_size", "my_custom_option"};
    }

    // ... 现有代码 ...
}
```

### 10.2 添加新的能力接口

如果你的后端支持特殊功能（如图像生成），需要实现相应接口：

```cpp
// 支持图像生成的后端示例
class MyImageServer : public WrappedServer, public IImageServer {
public:
    // WrappedServer 方法
    void load(...) override;
    void unload() override;
    json chat_completion(...) override;
    json completion(...) override;

    // IImageServer 方法
    json image_generations(const json& request) override {
        return forward_request("/v1/images/generations", request);
    }

    json image_edits(const json& request) override {
        return forward_request("/v1/images/edits", request);
    }

    json image_variations(const json& request) override {
        return forward_request("/v1/images/variations", request);
    }
};
```

### 10.3 添加新模型

**方法 1: 修改 server_models.json（内置模型）**

```json
{
  "My-New-Model": {
    "checkpoints": {
      "main": "organization/model-name:variant.gguf"
    },
    "recipe": "llamacpp",
    "size": 4.0,
    "labels": ["suggested"],
    "suggested": true
  }
}
```

**方法 2: 使用 CLI 添加用户模型**

```bash
# 从 HuggingFace 导入
lemonade-server pull user.MyModel \
    --checkpoint meta-llama/Llama-3.2-1B-Instruct-GGUF:Q4_K_M \
    --recipe llamacpp

# 从本地目录导入
lemonade-server pull user.LocalModel \
    --checkpoint /path/to/model \
    --recipe llamacpp
```

**方法 3: 通过 API 注册**

```python
import requests

response = requests.post("http://localhost:8000/api/pull", json={
    "model": "user.MyModel",
    "checkpoint": "organization/model:variant",
    "recipe": "llamacpp"
})
```

---

## 附录 A: 常见问题

### Q1: 如何修改 FLM 模型的上下文大小？

```bash
# 方法 1: 启动时指定
lemonade-server serve --ctx-size 8192

# 方法 2: 运行单个模型时指定
lemonade-server run Gemma3-4b-it-FLM --ctx-size 8192

# 方法 3: 环境变量
set LEMONADE_CTX_SIZE=8192
lemonade-server serve
```

### Q2: 如何查看支持的后端？

```bash
lemonade-server recipes
```

### Q3: 模型文件存储在哪里？

- **HuggingFace 模型**: `~/.cache/huggingface/hub/`
- **FLM 模型**: FLM 自己的缓存目录
- **用户模型配置**: `~/.config/lemonade/user_models.json` (Linux) 或 `%APPDATA%\lemonade\` (Windows)

### Q4: 如何调试后端启动问题？

```bash
# 启用详细日志
lemonade-server serve --log-level debug

# 手动测试 FLM
flm serve gemma3:4b --ctx-len 4096 --port 8001

# 手动测试 llama.cpp
llama-server --model /path/to/model.gguf --port 8001
```

---

## 附录 B: 关键文件速查

| 文件 | 用途 |
|------|------|
| `main.cpp` | 程序入口 |
| `cli_parser.cpp` | 命令行解析 |
| `server.cpp` | HTTP 服务器和路由设置 |
| `router.cpp` | 请求路由和多模型管理 |
| `wrapped_server.cpp` | 后端基类实现 |
| `fastflowlm_server.cpp` | FLM NPU 后端 |
| `llamacpp_server.cpp` | llama.cpp GPU/CPU 后端 |
| `model_manager.cpp` | 模型下载和管理 |
| `recipe_options.cpp` | 配置选项处理 |
| `server_models.json` | 内置模型定义 |
| `backend_versions.json` | 后端版本配置 |

---

## 附录 C: API 端点参考

### OpenAI 兼容端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/v1/chat/completions` | POST | 聊天补全 |
| `/v1/completions` | POST | 文本补全 |
| `/v1/embeddings` | POST | 文本嵌入 |
| `/v1/models` | GET | 列出模型 |
| `/v1/audio/transcriptions` | POST | 语音识别 |
| `/v1/audio/speech` | POST | 语音合成 |
| `/v1/images/generations` | POST | 图像生成 |

### 管理端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/pull` | POST | 下载模型 |
| `/api/load` | POST | 加载模型 |
| `/api/unload` | POST | 卸载模型 |
| `/api/models/:id` | DELETE | 删除模型 |
| `/api/health` | GET | 健康检查 |
| `/api/system-info` | GET | 系统信息 |
| `/api/stats` | GET | 推理统计 |

---

*文档版本: 1.0*
*最后更新: 2025年2月*
