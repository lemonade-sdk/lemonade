#include <iostream>
#include <cmath>
#define NOMINMAX
#include "utils/utils.hpp"
#include "utils/vm_args.hpp"
#include "AutoModel/modeling_lfm2.hpp"
#include "model_list.hpp"
#include "utils/vm_args.hpp"
#include "metrices.hpp"

xrt::device npu_device_global;

// Model-specific factory function for Llama family and DeepSeek_r1_8b
inline std::pair<std::string, std::unique_ptr<AutoModel>> get_lfm2_model(const std::string& model_tag) {
    static std::unordered_set<std::string> lfm2Tags = {
        "lfm2", "lfm2:1.2b", "lfm2:2.6b", "lfm2.5-it:1.2b", "lfm2.5-tk:1.2b", "lfm2-trans:2.6b"
    };

    std::unique_ptr<AutoModel> auto_chat_engine = nullptr;
    std::string new_model_tag = model_tag;

    if (lfm2Tags.count(model_tag))
        auto_chat_engine = std::make_unique<LFM2>(&npu_device_global);
    else {
        new_model_tag = "lfm2:1.2b"; // Default to LFM2 1.2B
        auto_chat_engine = std::make_unique<LFM2>(&npu_device_global);
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

    std::unique_ptr<AutoModel> chat = std::make_unique<LFM2>(&npu_device_global);

    npu_device_global = xrt::device(0);

    chat->load_model(model_path, model_info, -1, preemption);
    chat->set_topk(1);
    chat_meta_info_t meta_info;
    lm_uniform_input_t uniformed_input;


    if (short_prompt) {
        uniformed_input.prompt = "Who are you? ";
        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: ";
        chat->start_total_timer();
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
        uniformed_input.prompt = "What is C. elegans?";
        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: ";
        chat->start_total_timer();
        response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;

        uniformed_input.prompt = "How many cells does it have?";
        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: ";
        chat->start_total_timer();
        response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
    }
    else{
        std::ifstream file("../../../../test.txt", std::ios::binary);
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
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 128, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
    }

    // std::pair<std::string, std::vector<int>> history = chat->get_history();
    // std::cout << "History length: " << history.second.size() << std::endl;

    return 0;
}
