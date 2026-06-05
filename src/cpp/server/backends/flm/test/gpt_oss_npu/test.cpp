#include <iostream>
#include <cmath>
#define NOMINMAX
#ifdef __WINDOWS__
#include <windows.h>
#endif
#include "utils/utils.hpp"
#include "utils/vm_args.hpp"
#include "AutoModel/modeling_gpt_oss.hpp"
#include "model_list.hpp"
#include "utils/vm_args.hpp"

xrt::device npu_device_global;


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

    std::unique_ptr<AutoModel> chat = std::make_unique<GPT_OSS>(&npu_device_global);
    npu_device_global = xrt::device(0);

    try {
        chat->load_model(model_path, model_info, -1, preemption);
    }
    catch (const std::exception& e) {
        header_print("ERROR", "Failed to load model: " + std::string(e.what()));
        return 1;
    }
    chat_meta_info_t meta_info;
    lm_uniform_input_t uniformed_input;
    chat->set_max_length(16384);
    std::string reasoning_effort = "low";
    chat->configure_parameter("reasoning_effort", reasoning_effort);

    chat->set_topk(1);

    if (short_prompt) {
        uniformed_input.prompt = "Hello";
        std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        std::cout << "Response: ";
        chat->start_total_timer();
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
        // uniformed_input.prompt = "Write down all Maxwell's equations in differential form. Do not use LaTex, use utf-8 encoding characters for math symbols. Derive the light of speed according to the equations.";
        // meta_info = chat_meta_info_t();
        // uniformed_input.prompt = "How far is it from Beijing?";
        // std::cout << "Prompt: " << uniformed_input.prompt << std::endl;
        // std::cout << "Response: " << std::endl;
        // chat->start_total_timer();
        // response = chat->generate_with_prompt(meta_info, uniformed_input, 1024, std::cout);
        // chat->stop_total_timer();
        // std::cout << std::endl;
        // std::cout << std::endl;
        // std::cout << chat->show_profile() << std::endl;
    }
    else{
        chat->set_max_length(65536);
        std::ifstream file("../../../../tb_files/1k.txt", std::ios::binary);
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
        std::cout << "Prompt length: " << uniformed_input.prompt.size() << std::endl;
        std::cin.get();
        std::cout << "Response: ";
        chat->start_total_timer();
        std::string response = chat->generate_with_prompt(meta_info, uniformed_input, 32, std::cerr);
        chat->stop_total_timer();
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << chat->show_profile() << std::endl;
        std::cin.get();
    }

    // std::pair<std::string, std::vector<int>> history = chat->get_history();
    // std::cout << "History length: " << history.second.size() << std::endl;
    // std::cout << std::endl;
    // std::cout << "History: " << history.first << std::endl;
    // std::cout << "History tokens: "  << std::endl;
    // for (int token : history.second){
    //     std::cout << token << " ";
    // }

    std::cout << std::endl;
    // std::cout << "Press Enter to continue...";
    // std::cin.get();

    return 0;
}
