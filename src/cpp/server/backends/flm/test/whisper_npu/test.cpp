#include <iostream>
#include <cmath>
#define NOMINMAX
#ifdef __WINDOWS__
#include <windows.h>
#endif
#include "utils/utils.hpp"
#include "utils/vm_args.hpp"
#include "model_list.hpp"
#include "utils/vm_args.hpp"
#include "metrices.hpp"
#include "whisper/modeling_whisper.hpp"


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
    desc.add_options()("Preemption,p", arg_utils::po::value<bool>()->default_value(false), "Preemption");
    desc.add_options()("Audio,a", arg_utils::po::value<std::string>()->required(), "Audio file");
    desc.add_options()("Timestamp,t", arg_utils::po::value<bool>()->default_value(false), "Timestamp");
    desc.add_options()("Return_time_stamp,r", arg_utils::po::value<bool>()->default_value(false), "Return time stamp");
    arg_utils::po::store(arg_utils::po::parse_command_line(argc, argv, desc), vm);

    std::string tag = vm["model"].as<std::string>();
    bool preemption = vm["Preemption"].as<bool>();
    std::string audio_filename = vm["Audio"].as<std::string>();
    bool timestamp = vm["Timestamp"].as<bool>();
    bool return_time_stamp = vm["Return_time_stamp"].as<bool>();
    std::cout << "Model: " << tag << std::endl;
    std::string exe_dir = utils::get_executable_directory();
    std::string model_dir = utils::get_models_directory();
    std::string model_list_path = exe_dir + "/model_list.json";
    model_list model_list(model_list_path, model_dir);

    header_print("info", "Initializing chat model...");
    std::string model_path = model_list.get_model_path(tag);
    std::pair<std::string, nlohmann::json> model_info_pair = model_list.get_model_info(tag);
    nlohmann::json model_info = model_info_pair.second;

    xrt::device npu_device_global = xrt::device(0);

    Whisper whisper(&npu_device_global);

    std::string audio_path = "../../../tb_files/nvidia.mp3";

    whisper.load_model(model_path, model_info, preemption);
    header_print("info", "Loading audio...");
    bool ret = whisper.load_audio(audio_path);
    std::pair<std::string, std::string> result = whisper.generate(Whisper::whisper_task_type_t::e_transcribe, timestamp, return_time_stamp, std::cout);
    std::cout << std::endl;
    std::cout << "Language detected: " << result.second << std::endl;
    std::cout << "Result: " << result.first << std::endl;

    return 0;
}
