/// \file npu_utils.hpp
/// \brief npu_utils class
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This file contains the classes for managing the npu device
#pragma once

/*
    The NPU is managed in this way:
    For each program, there should only be one npu_xclbin_manager, example:
    npu_xclbin_manager npu_mgr(device_npu2, 0);

    Then, multiple xclbins could be used by this npu and each xclbin may have multiple runtime sequences (applications)
    Therefore, each registered xclbin will have a npu_app_manager, example:

    npu_app_manager* mvm_i8_xclbin = npu_mgr.register_xclbin("mvm_i8.xclbin");

    Notice that the return value is a pointer to the npu_app_manager. This is to avoid duplicated npu hardware context,
    all xclbins are managed by the npu_xclbin_manager centrally.

    Finally, an application can be create from the npu_app_manager, example:

    npu_app app = mvm_i8_xclbin->create_app();

    In this version, the app do not have a name.
*/

#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdio.h>
#ifndef __WINDOWS__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <drm/drm.h>
#include "amdxdna_accel.h"
#endif
#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
#include "buffer.hpp"
#include "utils/debug_utils.hpp"
#include <cmath>

#include "xrt/experimental/xrt_kernel.h"
#include "xrt/experimental/xrt_ext.h"
#include "xrt/experimental/xrt_module.h"
#include "xrt/experimental/xrt_elf.h"

#include "aiebu/aiebu.h"

#include "npu_instr_utils.hpp"


class npu_app_manager;
class npu_xclbin_manager;


///@brief npu_app, a application that both xclbin and instruction are specified
///@param device the pointer to the device
///@param context the pointer to the context
///@param kernel_name the name of the kernel
///@see xrt::kernel, xrt::device
class npu_app {
private:
    // from external
    xrt::hw_context* context;
    xrt::device* device;
    std::string kernel_name;
    npu_device device_gen;
    bool enable_preemption;

    // self-managed
    bool module_valid;
    uint8_t module_version;
    std::unique_ptr<xrt::module> module;
    std::unique_ptr<xrt::elf> elf;
    std::unique_ptr<xrt::ext::kernel> kernel;
    std::unique_ptr<npu_sequence> ctrl_seq;

    uint32_t _gen_elf(char** elf_buf, std::pair<uint32_t*, size_t>& instruction_data){
        uint32_t elf_buf_size = aiebu_assembler_get_elf(
            aiebu_assembler_buffer_type_blob_instr_transaction,
            (char*) instruction_data.first, instruction_data.second * sizeof(uint32_t),
            NULL, 0, (void**)elf_buf, NULL, 0, "", "", NULL, 0);
        assert(elf_buf_size > 0);
        if (elf_buf_size == 0){
            header_print_r("ERROR", "Failed to get elf from ctrl_seq");
            exit(1);
        }
        return elf_buf_size;
    }

    ///@brief Setup the kernel
    ///@note The function will create an elf file from the ctrl_seq
    ///@note The function will also update the module, elf, and kernel
    void _setup_kernel(){
        char* elf_buf;
        this->kernel.reset();
        this->module.reset();
        this->elf.reset();
        std::pair<uint32_t*, size_t> data = this->ctrl_seq->dump();
        assert(data.first != nullptr);
        assert(data.second > 0);
        uint32_t elf_buf_size = this->_gen_elf(&elf_buf, data);
        if (this->module_valid){
            this->module.reset();
            this->elf.reset();
            this->kernel.reset();
        }
        this->elf = std::make_unique<xrt::elf>(elf_buf, elf_buf_size);
        this->module = std::make_unique<xrt::module>(*this->elf);
        this->kernel = std::make_unique<xrt::ext::kernel>(*this->context, *this->module, this->kernel_name);
        this->module_valid = true;
        this->module_version = this->ctrl_seq->sequence_version();

        free((void*)elf_buf);
    }

public:
    // enum ert_cmd_state {
    //     ERT_CMD_STATE_NEW = 1,
    //     ERT_CMD_STATE_QUEUED = 2,
    //     ERT_CMD_STATE_RUNNING = 3,
    //     ERT_CMD_STATE_COMPLETED = 4,
    //     ERT_CMD_STATE_ERROR = 5,
    //     ERT_CMD_STATE_ABORT = 6,
    //     ERT_CMD_STATE_SUBMITTED = 7,
    //     ERT_CMD_STATE_TIMEOUT = 8,
    //     ERT_CMD_STATE_NORESPONSE = 9,
    //     ERT_CMD_STATE_SKERROR = 10, //Check for error return code from Soft Kernel
    //     ERT_CMD_STATE_SKCRASHED = 11, //Soft kernel has crashed
    //     ERT_CMD_STATE_MAX, // Always the last one
    // };
    std::map<ert_cmd_state, std::string> cmd_state_map = {
        {ERT_CMD_STATE_NEW, "new"},
        {ERT_CMD_STATE_QUEUED, "queued"},
        {ERT_CMD_STATE_RUNNING, "running"},
        {ERT_CMD_STATE_COMPLETED, "completed"},
        {ERT_CMD_STATE_ERROR, "error"},
        {ERT_CMD_STATE_ABORT, "abort"},
        {ERT_CMD_STATE_SUBMITTED, "submitted"},
        {ERT_CMD_STATE_TIMEOUT, "timeout"},
        {ERT_CMD_STATE_NORESPONSE, "noresponse"},
        {ERT_CMD_STATE_SKERROR, "skerror"},
        {ERT_CMD_STATE_SKCRASHED, "skcrashed"},
        {ERT_CMD_STATE_MAX, "max"},
    };

    ///@brief Default Constructor
    ///@note Initialize the npu_app to nullptr
    npu_app() {
        this->device_gen = device_npu2;
        this->kernel = nullptr;
        this->device = nullptr;
        this->module_valid = false;
        this->module = nullptr;
        this->elf = nullptr;
        this->kernel = nullptr;
        this->module_version = 0xFF;
    }

    ///@brief Constructor, this shall not invoke by user, it shall only be invoked by npu_app_manager
    ///@param device_gen the npu device
    ///@param device the pointer to the device
    ///@param context the pointer to the context
    ///@param kernel_name the name of the kernel
    ///@see xrt::device, xrt::hw_context, xrt::kernel
    npu_app(npu_device device_gen, xrt::device* device, xrt::hw_context* context, std::string kernel_name, bool enable_preemption = false):
        device_gen(device_gen), device(device), context(context), kernel_name(kernel_name), enable_preemption(enable_preemption){
        this->module_valid = false;
        this->module = nullptr;
        this->elf = nullptr;
        this->kernel = nullptr;
        this->ctrl_seq = std::make_unique<npu_sequence>(device_gen, enable_preemption);
        this->module_version = 0xFF;
    }


    void update_ctrl_seq(){
        assert(this->ctrl_seq != nullptr);
        this->_setup_kernel();
    }

    void load_elf(std::string elf_name){
        this->ctrl_seq->clear_cmds();
        this->ctrl_seq->cmds2seq(); // just for making sure the sequence is valid
        this->elf = std::make_unique<xrt::elf>(elf_name);
        this->module = std::make_unique<xrt::module>(*this->elf);
        this->kernel = std::make_unique<xrt::ext::kernel>(*this->context, *this->module, this->kernel_name);
        this->module_valid = true;
        this->module_version = this->ctrl_seq->sequence_version(); // force sync the sequence version
    }

    void store_elf(std::string elf_name){
        char* elf_buf;
        if (this->module_valid == false) {
            this->_setup_kernel();
        }
        std::pair<uint32_t*, size_t> data = this->ctrl_seq->dump();
        uint32_t elf_buf_size = this->_gen_elf(&elf_buf, data);
        std::ofstream fout(elf_name, std::ios::binary);
        if (fout.is_open() == false) {
            header_print_r("ERROR", "Failed to open file: " << elf_name);
            exit(1);
        }
        fout.write((char*)elf_buf, elf_buf_size);
        fout.close();
        free((void*)elf_buf);
    }

    ///@brief Operator() for running the kernel
    ///@param args arguments, shall be the buffers with real bo
    ///@see xrt::run
    template<typename... BoArgs>
    ert_cmd_state operator()(BoArgs&&... args){
        if (this->module_valid == false || this->ctrl_seq->sequence_valid() == false || this->module_version != this->ctrl_seq->sequence_version()) {
            this->_setup_kernel();
        }
        auto run = this->kernel->operator()(3, 0, 0, args.bo()...);
        ert_cmd_state state = run.wait();
        LOG_VERBOSE(2, "ending state: " << this->cmd_state_map[state]);
        return state;
    }


    ///@brief Operator() for running the kernel
    ///@param args arguments, shall be the buffers with real bo
    ///@see xrt::run
    template<typename... BoArgs>
    ert_cmd_state safe_run(BoArgs&&... args){
        if (this->module_valid == false || this->ctrl_seq->sequence_valid() == false || this->module_version != this->ctrl_seq->sequence_version()) {
            this->_setup_kernel();
        }
        std::array<bytes*, sizeof...(BoArgs)> bo_args = { &args... };
        for (size_t i = 0; i < sizeof...(args); i++){
            bo_args[i]->sync_to_device();
        }
        auto run = this->kernel->operator()(3, 0, 0, args.bo()...);
        ert_cmd_state state = run.wait();
        for (size_t i = 0; i < sizeof...(args); i++){
            bo_args[i]->sync_from_device();
        }
        LOG_VERBOSE(2, "ending state: " << this->cmd_state_map[state]);
        return state;
    }

    ///@brief Create a run object
    ///@param args arguments, shall be the buffers with real bo
    ///@return a run object for waiting
    ///@see xrt::run
    template<typename... BoArgs>
    xrt::run create_run(BoArgs&&... args){
        if (this->module_valid == false || this->ctrl_seq->sequence_valid() == false || this->module_version != this->ctrl_seq->sequence_version()) {
            this->_setup_kernel();
        }
        xrt::run run = xrt::run(*this->kernel);
        run.set_arg(0, 3);
        run.set_arg(1, 0);
        run.set_arg(2, 0);
        std::array<bytes*, sizeof...(BoArgs)> bo_args = { &args... };
        for (size_t i = 0; i < sizeof...(args); i++){
            run.set_arg(3 + i, bo_args[i]->bo());
        }
        return run;
    }

    ///@brief Create a buffer with real bo
    ///@param size size of the buffer
    ///@see buffer
    template<typename T>
    buffer<T> create_bo_buffer(size_t size){
        assert(size > 0);
        LOG_VERBOSE(2, "Creating buffer buffer with size: " << size);
        return buffer<T>(*this->device, size);
    }

    npu_sequence* seq() {
        return this->ctrl_seq.get();
    }
};

class npu_app_manager {
private:
    npu_device device_gen;
    bool enable_preemption;
    xrt::device* device;
    std::unique_ptr<xrt::hw_context> context;
    std::string kernel_name;
    std::string xclbin_name;
    bool xclbin_valid;
public:
    ///@brief Default Constructor
    ///@param device_gen the npu device
    ///@param device device object
    ///@param xclbin_name name of the xclbin file
    ///@see xrt::device, xrt::xclbin
    ///@note The function will initialize the npu_app_manager to nullptr
    npu_app_manager(){
        this->device_gen = device_npu2;
        this->device = nullptr;
        this->context = nullptr;
        this->kernel_name = "";
        this->xclbin_name = "";
        this->xclbin_valid = false;
        this->enable_preemption = false;
    }

    ///@brief Copy constructor, this shall not invoke by user, it shall only be invoked by npu_xclbin_manager
    ///@param other the other npu_app_manager to copy from
    npu_app_manager(const npu_app_manager& other) {
        this->device_gen = other.device_gen;
        this->device = other.device;
        this->kernel_name = other.kernel_name;
        this->xclbin_name = other.xclbin_name;
        this->xclbin_valid = other.xclbin_valid;
        this->enable_preemption = other.enable_preemption;
        if (other.context) {
            this->context = std::make_unique<xrt::hw_context>(*other.context);
        } else {
            this->context = nullptr;
        }
    }

    ///@brief Copy assignment operator
    ///@param other the other npu_app_manager to copy from
    ///@return reference to this object
    npu_app_manager& operator=(const npu_app_manager& other) {
        if (this != &other) {
            this->device_gen = other.device_gen;
            this->device = other.device;
            this->kernel_name = other.kernel_name;
            this->xclbin_name = other.xclbin_name;
            this->xclbin_valid = other.xclbin_valid;
            this->enable_preemption = other.enable_preemption;
            if (other.context) {
                this->context = std::make_unique<xrt::hw_context>(*other.context);
            } else {
                this->context = nullptr;
            }
        }
        return *this;
    }

    ///@brief Move constructor
    ///@param other the other npu_app_manager to move from
    npu_app_manager(npu_app_manager&& other) noexcept {
        this->device_gen = other.device_gen;
        this->device = other.device;
        this->context = std::move(other.context);
        this->kernel_name = std::move(other.kernel_name);
        this->xclbin_name = std::move(other.xclbin_name);
        this->xclbin_valid = other.xclbin_valid;
        this->enable_preemption = other.enable_preemption;
        // Reset the moved-from object
        other.device = nullptr;
        other.xclbin_valid = false;
    }

    ///@brief Move assignment operator
    ///@param other the other npu_app_manager to move from
    ///@return reference to this object
    npu_app_manager& operator=(npu_app_manager&& other) noexcept {
        if (this != &other) {
            this->device_gen = other.device_gen;
            this->device = other.device;
            this->context = std::move(other.context);
            this->kernel_name = std::move(other.kernel_name);
            this->xclbin_name = std::move(other.xclbin_name);
            this->xclbin_valid = other.xclbin_valid;
            this->enable_preemption = other.enable_preemption;
            // Reset the moved-from object
            other.device = nullptr;
            other.xclbin_valid = false;
        }
        return *this;
    }

    ///@brief Constructor, this shall not invoke by user, it shall only be invoked by npu_xclbin_manager
    ///@param device_gen the npu device
    ///@param device device object
    ///@param xclbin_name name of the xclbin file
    ///@see xrt::device, xrt::xclbin
    npu_app_manager(npu_device device_gen, xrt::device* device, std::string xclbin_name, bool enable_preemption = false){
        assert(device != nullptr);
        assert(xclbin_name != "");
        this->device_gen = device_gen;
        this->device = device;
        this->xclbin_name = xclbin_name;
        this->enable_preemption = enable_preemption;
        #ifndef __WINDOWS__
        if (this->enable_preemption){
            header_print("warning", "Preemption is not supported on Linux host currently.");
            header_print("warning", "Preemption is set as disabled for NPU.");
            this->enable_preemption = false;
        }
        #endif
        LOG_VERBOSE(2, "Loading xclbin: " << xclbin_name);
        auto this_xclbin = xrt::xclbin(xclbin_name);
        // int verbosity = VERBOSE;
        std::string Node = "MLIR_AIE";
        auto xkernels = this_xclbin.get_kernels();
        auto xkernel = *std::find_if(
            xkernels.begin(),
            xkernels.end(),
            [Node](xrt::xclbin::kernel &k) {
                auto name = k.get_name();
                return name.rfind(Node, 0) == 0;
            }
        );
        this->device->register_xclbin(this_xclbin);
        auto kernelName = xkernel.get_name();
        this->context = std::make_unique<xrt::hw_context>(*this->device, this_xclbin.get_uuid());
        this->kernel_name = kernelName;
        this->xclbin_valid = true;
        LOG_VERBOSE(2, "Xclbin: " << xclbin_name << " loaded successfully!");
    }

    ~npu_app_manager() = default;

    ///@brief Create a npu_app
    ///@return a npu_app object
    ///@see npu_app
    npu_app create_app(){
        assert(this->xclbin_valid);
        return npu_app(this->device_gen, this->device, this->context.get(), this->kernel_name, this->enable_preemption);
    }

    ///@brief Create a buffer with real bo
    ///@param size size of the buffer
    ///@see buffer
    template<typename T>
    buffer<T> create_bo_buffer(size_t size){
        assert(size > 0);
        LOG_VERBOSE(2, "Creating buffer buffer with size: " << size);
        return buffer<T>(*this->device, size);
    }

    ///@brief Get the name of the xclbin
    ///@return the name of the xclbin
    std::string get_xclbin_name(){
        assert(this->xclbin_valid);
        return this->xclbin_name;
    }

    ///@brief Create a runlist
    ///@return a runlist object
    ///@see xrt::runlist
    xrt::runlist create_runlist(){
        assert(this->xclbin_valid);
        assert(this->enable_preemption == false); // preemption is not supported for runlist
        return xrt::runlist(*this->context);
    }
};

///@brief npu_xclbin_manager
///@note There should be only one npu_xclbin_manager inside main.
///@note It handles all xclbins
class npu_xclbin_manager{

private:
    std::vector<std::unique_ptr<npu_app_manager>> npu_xclbins;

    size_t xclbin_count;
    // the only device instance
    xrt::device* device;
    bool enable_preemption;
    npu_device npu_gen;
public:
    constexpr static int max_xclbins = 16; // This is hard constraint from the XRT driver

    ///@brief Constructor, this shall not invoke by user, it shall only be invoked by main
    ///@param device the npu device
    ///@param device_id the device id
    ///@see xrt::device
    npu_xclbin_manager(npu_device device = device_npu2, xrt::device* device_inst = nullptr, bool enable_preemption = false){
        this->device = device_inst;
        // this->npu_xclbins.resize(max_xclbins);
        this->npu_xclbins.reserve(max_xclbins);
        this->xclbin_count = 0;
        this->npu_gen = device;
        this->enable_preemption = enable_preemption;
    }

    ~npu_xclbin_manager() = default;

    ///@brief register an accel_user_desc to the npu_manager
    ///@param xclbin_name the name of the xclbin
    ///@return the npu_app_manager object
    ///@see npu_app_manager
    ///@note Different apps may have the same xclbin, but the sequence is unique.
    ///@note To avoid creating duplicated applications, the function checks if the xclbin is registered.
    ///@note If the xclbin is not registered, the function will register the xclbin and create a new application.
    npu_app_manager* register_xclbin(std::string xclbin_name){
        assert(xclbin_name != "");
        int xclbin_id = -1;
        for (size_t i = 0; i < this->xclbin_count; i++){

            if (this->npu_xclbins[i]->get_xclbin_name() == xclbin_name){
                xclbin_id = i;
                break;
            }
        }
        LOG_VERBOSE_IF_ELSE(2, xclbin_id > -1,
            "Found xclbin: " << xclbin_name << "registered as id " << xclbin_id << "!",
            "Xclbin: " << xclbin_name << " not registered yet!"
        );

        if (xclbin_id == -1){ // the xclbin is not registered yet
            if (this->xclbin_count >= max_xclbins){
                throw std::runtime_error("Max number of xclbins reached");
            }
            this->npu_xclbins.push_back(std::make_unique<npu_app_manager>(this->npu_gen, this->device, xclbin_name, this->enable_preemption));
            xclbin_id = this->xclbin_count;
            LOG_VERBOSE(2, "Xclbin: " << xclbin_name << " registered as id " << xclbin_id << "!");
            this->xclbin_count++;
            xclbin_id = this->xclbin_count - 1;
        }
        return this->npu_xclbins[xclbin_id].get();
    }

    void list_xclbins(){
        for (size_t i = 0; i < this->xclbin_count; i++){
            std::cout << "Xclbin " << i << " name: " << this->npu_xclbins[i]->get_xclbin_name() << std::endl;
        }
    }

    /// @brief Helper function to write out the trace to a file
    /// @param traceOutPtr pointer to the trace
    /// @param trace_size size of the trace
    /// @param path path to the file
    static void write_out_trace(char *traceOutPtr, size_t trace_size, std::string path) {
        std::ofstream fout(path);
        LOG_VERBOSE(1, "Writing out trace to: " << path);
        uint32_t *traceOut = (uint32_t *)traceOutPtr;
        for (size_t i = 0; i < trace_size / sizeof(traceOut[0]); i++) {
          fout << std::setfill('0') << std::setw(8) << std::hex << (int)traceOut[i];
          fout << std::endl;
        }
        fout.close();
        LOG_VERBOSE(1, "Trace written successfully!");
    }

    ///@brief Create a buffer with real bo
    ///@param size size of the buffer
    ///@see buffer
    template<typename T>
    buffer<T> create_bo_buffer(size_t size){
        assert(size > 0);
        LOG_VERBOSE(2, "Creating buffer buffer with size: " << size);
        return buffer<T>(*this->device, size);
    }

    inline bool is_preemption_enabled(){
        return this->enable_preemption;
    }

    #ifndef __WINDOWS__
    ///@brief print the npu information
    ///@note The function will print the npu version, clock frequency, column count, row count, core info, mem info, shim info.
    ///@note The information is read via the IOCTL interface.
    void print_npu_info(){
        int fd = open("/dev/accel/accel0", O_RDWR);
        if (fd < 0) {
            perror("Failed to open amdgpu device");
            return;
        }
        amdxdna_drm_query_clock_metadata query_clock_metadata;
        amdxdna_drm_get_info get_info = {
            .param = DRM_AMDXDNA_QUERY_CLOCK_METADATA,
            .buffer_size = sizeof(amdxdna_drm_query_clock_metadata),
            .buffer = (unsigned long)&query_clock_metadata,
        };
        int ret = ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info);
        if (ret < 0) {
            std::cout << "Error code: " << ret << std::endl;
            perror("Failed to get telemetry information");
            close(fd);
            return;
        }

        amdxdna_drm_query_aie_metadata query_aie_metadata = {};
        get_info.param = DRM_AMDXDNA_QUERY_AIE_METADATA;
        get_info.buffer_size = sizeof(amdxdna_drm_query_aie_metadata);
        get_info.buffer = (unsigned long)&query_aie_metadata;
        ret = ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info);
        if (ret < 0) {
            std::cout << "Error code: " << ret << std::endl;
            perror("Failed to get telemetry information");
            close(fd);
            return;
        }

        close(fd);
        MSG_BONDLINE(40);
        MSG_BOX_LINE(40, "NPU version: " << query_aie_metadata.version.major << "." << query_aie_metadata.version.minor);
        MSG_BOX_LINE(40, "MP-NPU clock frequency: " << query_clock_metadata.mp_npu_clock.freq_mhz << " MHz");
        MSG_BOX_LINE(40, "H clock frequency: " << query_clock_metadata.h_clock.freq_mhz << " MHz");
        // What is the meaning of the column size?
        // std::cout << "NPU column size: " << query_aie_metadata.col_size << std::endl;
        MSG_BOX_LINE(40, "NPU column count: " << query_aie_metadata.cols);
        MSG_BOX_LINE(40, "NPU row count: " << query_aie_metadata.rows);
        MSG_BOX_LINE(40, "NPU core Info: ");
        MSG_BOX_LINE(40, "--Row count: " << query_aie_metadata.core.row_count);
        MSG_BOX_LINE(40, "--Row start: " << query_aie_metadata.core.row_start);
        MSG_BOX_LINE(40, "--DMA channel count: " << query_aie_metadata.core.dma_channel_count);
        MSG_BOX_LINE(40, "--Lock count: " << query_aie_metadata.core.lock_count);
        MSG_BOX_LINE(40, "--Event reg count: " << query_aie_metadata.core.event_reg_count);
        MSG_BOX_LINE(40, "NPU mem Info: ");
        MSG_BOX_LINE(40, "--Row count: " << query_aie_metadata.mem.row_count);
        MSG_BOX_LINE(40, "--Row start: " << query_aie_metadata.mem.row_start);
        MSG_BOX_LINE(40, "--DMA channel count: " << query_aie_metadata.mem.dma_channel_count);
        MSG_BOX_LINE(40, "--Lock count: " << query_aie_metadata.mem.lock_count);
        MSG_BOX_LINE(40, "--Event reg count: " << query_aie_metadata.mem.event_reg_count);
        MSG_BOX_LINE(40, "NPU shim Info: ");
        MSG_BOX_LINE(40, "--Row count: " << query_aie_metadata.shim.row_count);
        MSG_BOX_LINE(40, "--Row start: " << query_aie_metadata.shim.row_start);
        MSG_BOX_LINE(40, "--DMA channel count: " << query_aie_metadata.shim.dma_channel_count);
        MSG_BOX_LINE(40, "--Lock count: " << query_aie_metadata.shim.lock_count);
        MSG_BOX_LINE(40, "--Event reg count: " << query_aie_metadata.shim.event_reg_count);
        MSG_BONDLINE(40);
    }

    ///@brief get the npu power consumption
    ///@param print whether to print the power consumption
    ///@return the power consumption, unit is Watt
    float get_npu_power(bool print){
        // get the npu power consumption, unit is Watt
        int fd = open("/dev/accel/accel0", O_RDWR);
        if (fd < 0) {
            perror("Failed to open amdgpu device");
            return -1;
        }
        amdxdna_drm_query_sensor query_sensor;

        amdxdna_drm_get_info get_info = {
            .param = DRM_AMDXDNA_QUERY_SENSORS,
            .buffer_size = sizeof(amdxdna_drm_query_sensor),
            .buffer = (unsigned long)&query_sensor,
        };
        int ret = ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info);
        if (ret < 0) {
            std::cout << "Error code: " << ret << std::endl;
            perror("Failed to get telemetry information");
            close(fd);
            return -1;
        }
        if (print){
            MSG_BOX(40, "NPU power: " << query_sensor.input << " " << query_sensor.units);
        }
        close(fd);
        return (float)query_sensor.input * pow(10, query_sensor.unitm);
    }
    #endif
};
