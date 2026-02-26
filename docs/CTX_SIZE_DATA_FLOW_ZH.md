# `--ctx-size` 参数数据流详解

本文档详细解释了 `--ctx-size` 参数从命令行输入到最终传递给推理后端的完整代码流程。

## 概述

当用户执行以下命令时：
```bash
lemonade-server serve --ctx-size 8192
```

参数 `8192` 会经过以下代码路径：

```
CLI → CLIParser → ServerConfig → Server → Router → FastFlowLMServer → flm子进程
```

---

## 第一步：CLI11 解析命令行参数

### 文件: `cli_parser.cpp`

```cpp
// RecipeOptions::add_cli_options() 被调用来注册 --ctx-size 选项
static void add_serve_options(CLI::App* serve, ServerConfig& config) {
    // ... 其他选项 ...

    // 关键：这里调用 RecipeOptions 的静态方法来添加所有 recipe 相关的选项
    RecipeOptions::add_cli_options(*serve, config.recipe_options);
}
```

### 文件: `recipe_options.cpp`

```cpp
// CLI_OPTIONS 定义了 --ctx-size 的元数据
static const json CLI_OPTIONS = {
    {"--ctx-size", {
        {"option_name", "ctx_size"},        // 内部存储的 key 名
        {"type_name", "SIZE"},              // 帮助信息中显示的类型
        {"envname", "LEMONADE_CTX_SIZE"},   // 环境变量名
        {"help", "Context size for the model"}
    }},
    // ... 其他选项 ...
};

// 默认值定义
static const json DEFAULTS = {
    {"ctx_size", 4096},   // ← 默认值 4096
    // ...
};

void RecipeOptions::add_cli_options(CLI::App& app, json& storage) {
    for (auto& [key, opt] : CLI_OPTIONS.items()) {
        const std::string opt_name = opt["option_name"];  // "ctx_size"
        json defval = DEFAULTS[opt_name];  // 4096

        // 因为 defval 是整数，所以进入这个分支
        if (defval.is_number_integer()) {
            // 创建一个 CLI 选项，当用户提供值时，存储到 storage["ctx_size"]
            o = app.add_option_function<int>(
                key,  // "--ctx-size"
                [opt_name, &storage = storage](int val) {
                    storage[opt_name] = val;  // storage["ctx_size"] = 8192
                },
                opt["help"]
            );
            o->default_val((int) defval);  // 设置默认值 4096
        }

        // 设置环境变量名，允许通过 LEMONADE_CTX_SIZE=8192 设置
        o->envname(opt["envname"]);  // "LEMONADE_CTX_SIZE"
    }
}
```

### 数据存储位置

```cpp
// 在 cli_parser.h 中定义
struct ServerConfig {
    int port = 8000;
    std::string host = "localhost";
    std::string log_level = "info";
    json recipe_options = json::object();  // ← ctx_size 存储在这里！
    // ...
};
```

解析后的结果：
```json
// config.recipe_options 的内容:
{
    "ctx_size": 8192
}
```

---

## 第二步：CLIParser 返回配置

### 文件: `cli_parser.h`

```cpp
class CLIParser {
public:
    // 解析命令行参数
    int parse(int argc, char** argv);

    // 获取配置 - 返回包含 recipe_options 的 ServerConfig
    ServerConfig get_config() const { return config_; }

private:
    ServerConfig config_;  // 存储解析结果
};
```

---

## 第三步：main.cpp 获取配置并创建 Server

### 文件: `main.cpp`

```cpp
int main(int argc, char** argv) {
    CLIParser parser;
    parser.parse(argc, argv);

    // 获取配置，此时 config.recipe_options["ctx_size"] = 8192
    auto config = parser.get_config();

    // 创建服务器，传入 recipe_options
    Server server(
        config.port,
        config.host,
        config.log_level,
        config.recipe_options,    // ← 传递包含 ctx_size 的 JSON
        config.max_loaded_models,
        config.extra_models_dir,
        config.no_broadcast
    );

    server.run();
}
```

---

## 第四步：Server 保存配置并传递给 Router

### 文件: `server.cpp`

```cpp
Server::Server(int port, const std::string& host, const std::string& log_level,
               const json& default_options,  // ← 这就是 recipe_options
               int max_loaded_models,
               const std::string& extra_models_dir, bool no_broadcast)
    : port_(port), host_(host), log_level_(log_level),
      default_options_(default_options),  // ← 保存到成员变量
      no_broadcast_(no_broadcast), running_(false), udp_beacon_() {

    // ...初始化其他组件...

    // 创建 Router，传入 default_options
    router_ = std::make_unique<Router>(
        default_options_,     // ← 传递给 Router
        log_level,
        model_manager_.get(),
        max_loaded_models,
        backend_manager_.get()
    );
}
```

---

## 第五步：Router 保存默认配置

### 文件: `router.cpp`

```cpp
Router::Router(const json& default_options, const std::string& log_level,
               ModelManager* model_manager, int max_loaded_models,
               BackendManager* backend_manager)
    : default_options_(default_options),  // ← 保存默认配置
      log_level_(log_level),
      model_manager_(model_manager),
      max_loaded_models_(max_loaded_models),
      backend_manager_(backend_manager) {

    // 此时 default_options_["ctx_size"] = 8192
}
```

---

## 第六步：Router 加载模型时合并配置

### 文件: `router.cpp`

```cpp
void Router::load_model(const std::string& model_name,
                       const ModelInfo& model_info,
                       RecipeOptions options,         // 请求级别的配置
                       bool do_not_upgrade) {

    // 创建默认配置对象
    RecipeOptions default_opt = RecipeOptions(model_info.recipe, default_options_);

    // 配置继承链: 请求参数 > 模型配置 > 全局配置
    RecipeOptions effective_options = options.inherit(
        model_info.recipe_options.inherit(default_opt)
    );

    // 打印最终生效的配置
    std::cout << "[Router] Effective settings: "
              << effective_options.to_log_string() << std::endl;
    // 输出: "[Router] Effective settings: ctx_size=8192"

    // ... 创建后端 ...

    // 调用后端的 load 方法
    new_server->load(model_name, model_info, effective_options, do_not_upgrade);
}
```

---

## 第七步：FastFlowLMServer 使用配置启动 flm 进程

### 文件: `fastflowlm_server.cpp`

```cpp
void FastFlowLMServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {

    std::cout << "[FastFlowLM] Loading model: " << model_name << std::endl;

    // ★★★ 关键：从 options 中获取 ctx_size ★★★
    int ctx_size = options.get_option("ctx_size");  // 返回 8192

    // ... 安装和下载检查 ...

    // 选择端口
    port_ = choose_port();  // 例如 8001

    // 获取 flm 可执行文件路径
    std::string flm_path = get_flm_path();  // 例如 "C:\Program Files\flm\flm.exe"

    // ★★★ 构建命令行参数 ★★★
    std::vector<std::string> args = {
        "serve",
        model_info.checkpoint(),        // 例如 "gemma3:4b"
        "--ctx-len", std::to_string(ctx_size),  // "--ctx-len", "8192"
        "--port", std::to_string(port_),        // "--port", "8001"
        "--host", "127.0.0.1"
    };

    // 打印完整命令
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    // 输出: [ProcessManager] Starting process: "C:\Program Files\flm\flm.exe" "serve" "gemma3:4b" "--ctx-len" "8192" "--port" "8001" "--host" "127.0.0.1"

    // ★★★ 启动 flm 子进程 ★★★
    process_handle_ = utils::ProcessManager::start_process(
        flm_path,    // 可执行文件路径
        args,        // 命令行参数
        "",          // 工作目录
        is_debug(),  // 是否显示输出
        true         // 过滤健康检查日志
    );

    // 等待后端就绪
    bool ready = wait_for_ready();
    if (!ready) {
        throw std::runtime_error("flm-server failed to start");
    }

    is_loaded_ = true;
}
```

---

## RecipeOptions::get_option() 的实现

### 文件: `recipe_options.cpp`

```cpp
json RecipeOptions::get_option(const std::string& opt) const {
    // 如果 options_ 中有这个选项，返回它
    // 否则返回默认值
    return options_.contains(opt) ? options_[opt] : DEFAULTS[opt];
}
```

---

## 配置继承机制详解

### RecipeOptions::inherit() 的实现

```cpp
RecipeOptions RecipeOptions::inherit(const RecipeOptions& options) const {
    json merged = options_;  // 从当前配置开始

    // 合并父配置（只添加不存在的键）
    for (auto it = options.options_.begin(); it != options.options_.end(); ++it) {
        if (!merged.contains(it.key()) && !is_empty_option(it.value())) {
            merged[it.key()] = it.value();
        }
    }

    return RecipeOptions(recipe_, merged);
}
```

### 继承示例

假设有以下配置：

```
CLI:           --ctx-size 8192              → default_options = {"ctx_size": 8192}
模型定义:      recipe_options = {}           → 模型没有自定义配置
请求参数:      options = {}                  → API 请求没有指定
```

合并过程：
```cpp
// 1. 创建默认配置
RecipeOptions default_opt("flm", {"ctx_size": 8192});

// 2. 模型配置继承默认配置
// model_info.recipe_options = {}（空的）
// model_info.recipe_options.inherit(default_opt) = {"ctx_size": 8192}

// 3. 请求配置继承模型配置
// options = {}（空的）
// options.inherit({"ctx_size": 8192}) = {"ctx_size": 8192}

// 最终: effective_options = {"ctx_size": 8192}
```

---

## 完整数据流图

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 命令行: lemonade-server serve --ctx-size 8192                           │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ CLI11 解析 (recipe_options.cpp)                                         │
│                                                                         │
│   add_option_function<int>("--ctx-size", callback, help)                │
│   callback: storage["ctx_size"] = 8192                                  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ ServerConfig (cli_parser.h)                                             │
│                                                                         │
│   struct ServerConfig {                                                 │
│       json recipe_options = {"ctx_size": 8192};  // ← 存储在这里        │
│   };                                                                    │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ main.cpp                                                                │
│                                                                         │
│   auto config = parser.get_config();                                    │
│   Server server(..., config.recipe_options, ...);                       │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Server 构造函数 (server.cpp)                                            │
│                                                                         │
│   default_options_ = default_options;  // 保存 {"ctx_size": 8192}       │
│   router_ = make_unique<Router>(default_options_, ...);                 │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Router 构造函数 (router.cpp)                                            │
│                                                                         │
│   default_options_ = default_options;  // 保存 {"ctx_size": 8192}       │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                        (当收到推理请求或 load 请求时)
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Router::load_model() (router.cpp)                                       │
│                                                                         │
│   // 配置继承合并                                                       │
│   RecipeOptions default_opt("flm", default_options_);                   │
│   RecipeOptions effective_options = options.inherit(                    │
│       model_info.recipe_options.inherit(default_opt)                    │
│   );                                                                    │
│   // effective_options = {"ctx_size": 8192}                             │
│                                                                         │
│   new_server->load(model_name, model_info, effective_options);          │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ FastFlowLMServer::load() (fastflowlm_server.cpp)                        │
│                                                                         │
│   int ctx_size = options.get_option("ctx_size");  // 获取 8192          │
│                                                                         │
│   std::vector<std::string> args = {                                     │
│       "serve", "gemma3:4b",                                             │
│       "--ctx-len", "8192",  // ← ctx_size 转换为字符串                  │
│       "--port", "8001",                                                 │
│       "--host", "127.0.0.1"                                             │
│   };                                                                    │
│                                                                         │
│   process_handle_ = ProcessManager::start_process(flm_path, args);      │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 子进程                                                                  │
│                                                                         │
│   flm.exe serve gemma3:4b --ctx-len 8192 --port 8001 --host 127.0.0.1  │
│                                                                         │
│   (FLM 使用 8192 作为模型的上下文长度)                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 关键代码总结

| 文件 | 关键代码 | 作用 |
|------|---------|------|
| `recipe_options.cpp` | `DEFAULTS["ctx_size"] = 4096` | 定义默认值 |
| `recipe_options.cpp` | `storage[opt_name] = val` | CLI 参数解析回调 |
| `cli_parser.h` | `json recipe_options` | 存储解析结果 |
| `server.cpp` | `default_options_(default_options)` | Server 保存配置 |
| `router.cpp` | `default_options_(default_options)` | Router 保存配置 |
| `router.cpp` | `options.inherit(...)` | 配置继承合并 |
| `fastflowlm_server.cpp` | `options.get_option("ctx_size")` | 获取配置值 |
| `fastflowlm_server.cpp` | `"--ctx-len", std::to_string(ctx_size)` | 传递给子进程 |

---

## 如何在运行时修改 ctx-size

### 方法 1: 命令行参数（启动时）
```bash
lemonade-server serve --ctx-size 8192
```

### 方法 2: 环境变量
```bash
# Windows
set LEMONADE_CTX_SIZE=8192
lemonade-server serve

# Linux/macOS
LEMONADE_CTX_SIZE=8192 lemonade-server serve
```

### 方法 3: 使用 run 命令时指定
```bash
lemonade-server run Gemma3-4b-it-FLM --ctx-size 8192
```

### 方法 4: 通过 API 加载时指定
```bash
curl -X POST http://localhost:8000/api/v1/load \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Gemma3-4b-it-FLM", "ctx_size": 8192}'
```

**注意**: 修改 ctx-size 需要重新加载模型才能生效，因为这个值是在启动推理后端子进程时传递的。
