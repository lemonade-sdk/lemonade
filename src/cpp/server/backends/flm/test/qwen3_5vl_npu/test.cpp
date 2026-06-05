#include <iostream>
#include <cmath>
#include "utils/utils.hpp"
#include "utils/vm_args.hpp"
#include "AutoModel/modeling_qwen3_5vl.hpp"
#include "model_list.hpp"

xrt::device npu_device_global;

// Model-specific factory function for Qwen family and DeepSeek_r1_0528_8b
inline std::pair<std::string, std::unique_ptr<AutoModel>> get_qwen3_5vl_model(const std::string& model_tag) {
    static std::unordered_set<std::string> qwen3_5_Tags = {
        "qwen3.5", "qwen3.5:4b", "qwen3.5:9b"
    };

    std::unique_ptr<AutoModel> auto_chat_engine = nullptr;
    std::string new_model_tag = model_tag;
    if (qwen3_5_Tags.count(model_tag))
        auto_chat_engine = std::make_unique<Qwen3_5VL>(&npu_device_global);
    else {
        new_model_tag = "qwen3.5:4b"; // Default to base Qwen3_5VL model
        auto_chat_engine = std::make_unique<Qwen3_5VL>(&npu_device_global);
    }

    return std::make_pair(new_model_tag, std::move(auto_chat_engine));
}

int main(int argc, char* argv[]) {
    #ifdef __WINDOWS__
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Set thread priority to low
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    #endif

    arg_utils::po::options_description desc("Allowed options");
    arg_utils::po::variables_map vm;
    desc.add_options()("model,m", arg_utils::po::value<std::string>()->required(), "Model file");
    desc.add_options()("Short,s", arg_utils::po::value<bool>()->default_value(true), "Short Prompt");
    desc.add_options()("Preemption,p", arg_utils::po::value<bool>()->default_value(false), "Preemption");
    arg_utils::po::store(arg_utils::po::parse_command_line(argc, argv, desc), vm);

    std::string tag = vm["model"].as<std::string>();
    bool short_prompt = vm["Short"].as<bool>();
    bool preemption = vm["Preemption"].as<bool>();
    std::cout << "Model: " << tag << std::endl;
    std::string exe_dir = utils::get_executable_directory();
    std::string model_dir = utils::get_models_directory();
    std::string model_list_path = exe_dir + "/model_list.json";
    model_list model_list(model_list_path, model_dir);



    header_print("info", "Initializing chat model...");
    std::string model_path = model_list.get_model_path(tag);
    std::pair<std::string, nlohmann::json> model_info_pair = model_list.get_model_info(tag);
    nlohmann::json model_info = model_info_pair.second;
    std::cout << "Model path: " << model_path << std::endl;

    std::unique_ptr<AutoModel> chat = std::make_unique<Qwen3_5VL>(&npu_device_global);
    npu_device_global = xrt::device(0);

    chat->load_model(model_path, model_info, -1, preemption);
    header_print("info", "Model loaded");
    chat_meta_info_t meta_info;
    lm_uniform_input_t uniformed_input;
    chat->set_topk(1);

    if (short_prompt) {
        uniformed_input.prompt = "What are these?";
        // uniformed_input.prompt = "Solve the equation x^3 - 1 = 0 for me.";
        uniformed_input.images.push_back("../../../tb_files/panda.png");
        uniformed_input.images.push_back("../../../tb_files/puppy.png");

        uniformed_input.images.push_back("../../../tb_files/mj_icon.jpg");
        uniformed_input.images.push_back("../../../tb_files/google_icon.png");
        uniformed_input.images.push_back("../../../tb_files/pcb.jpg");

        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: " << std::endl;
        chat->start_total_timer();
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
        // uniformed_input.images.clear();
        // uniformed_input.images.push_back("../../../tb_files/pcb.jpg");
        // uniformed_input.prompt = "How about this?";

        // std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        // std::cout << "Response: " << std::endl;
        // chat->start_total_timer();
        // response = chat->generate_with_prompt(meta_info, uniformed_input, 8192, std::cout);
        // chat->stop_total_timer();
        // std::cout << std::endl;
        // std::cout << std::endl;
        // std::cout << chat->show_profile() << std::endl;
    }
    else{
        std::ifstream file("../../../../prompt.txt", std::ios::binary);
        if (!file.is_open()) {
            std::cout << "Failed to open prompt file" << std::endl;
            return 1;
        }
        uniformed_input.prompt = "";
        file.seekg(0, std::ios::end);
        uniformed_input.prompt.resize(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read(uniformed_input.prompt.data(), uniformed_input.prompt.size());
        file.close();
        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: ";
        chat->start_total_timer();
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
    }

    std::pair<std::string, std::vector<int>> history = chat->get_history();
    std::cout << "History length: " << history.second.size() << std::endl;
    std::cout << std::endl;

    std::cout << std::endl;

    return 0;
}
