/// \file npu_instr_utils.hpp
/// \brief npu_instr_utils class
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This file contains the classes for managing the npu instructions
#ifndef __NPU_INSTR_UTILS_HPP__
#define __NPU_INSTR_UTILS_HPP__

#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdio.h>
#include "buffer.hpp"
#include "utils/debug_utils.hpp"
#include "xrt/xrt_bo.h"
#include "instr_utils/npu_cmd.hpp"
#include "instr_utils/npu_cmd_write.hpp"
#include "instr_utils/npu_cmd_ddr.hpp"
#include "instr_utils/npu_cmd_write_dma.hpp"
#include "instr_utils/npu_cmd_issue_token.hpp"
#include "instr_utils/npu_cmd_wait.hpp"
#include "instr_utils/npu_cmd_maskwrite.hpp"
#include "instr_utils/npu_cmd_preemption.hpp"

// This function is used to interperate instructions
// Useful files:
// mlir-aie/lib/Dialect/AIEX/IR/AIEXDialect.cpp
// mlir-aie/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp
// mlir-aie/lib/Targets/AIETargetNPU.cpp

// found in AIETargetNPU.cpp

///@brief npu device
///@note This is an enum to specify the npu device
///@warning npu1 is not supported yet
typedef enum{
    device_npu1,
    device_npu2
} npu_device;

///@brief npu tiles
///@note This is an enum to specify the npu tiles
///@note The low 4 bit is col, the high 4 bit is row
///@warning Though npu1 only has 4 cols, the enum defines 8 cols for convenience
typedef enum: uint32_t{
    IT0 = 0x00,  IT1 = 0x01,  IT2 = 0x02,  IT3 = 0x03,
    IT4 = 0x04,  IT5 = 0x05,  IT6 = 0x06,  IT7 = 0x07,
    MT0 = 0x10,  MT1 = 0x11,  MT2 = 0x12,  MT3 = 0x13,
    MT4 = 0x14,  MT5 = 0x15,  MT6 = 0x16,  MT7 = 0x17,
    CT00 = 0x20, CT01 = 0x21, CT02 = 0x22, CT03 = 0x23,
    CT04 = 0x24, CT05 = 0x25, CT06 = 0x26, CT07 = 0x27,
    CT10 = 0x30, CT11 = 0x31, CT12 = 0x32, CT13 = 0x33,
    CT14 = 0x34, CT15 = 0x35, CT16 = 0x36, CT17 = 0x37,
    CT20 = 0x40, CT21 = 0x41, CT22 = 0x42, CT23 = 0x43,
    CT24 = 0x44, CT25 = 0x45, CT26 = 0x46, CT27 = 0x47,
    CT30 = 0x50, CT31 = 0x51, CT32 = 0x52, CT33 = 0x53,
    CT34 = 0x54, CT35 = 0x55, CT36 = 0x56, CT37 = 0x57,
} npu_tiles;

///@brief get the tile
///@param row row of the tile
///@param col column of the tile
///@return the tile
///@note This is a helper function to get the tile from the row and column
inline npu_tiles get_tile(uint32_t row, uint32_t col){
    assert(col < 8 && row < 6);
    return static_cast<npu_tiles>((row << 4) | col);
}

///@brief npu it channel
///@note This is an enum to specify the npu it channel
typedef enum: uint32_t{
    it_channel_0,
    it_channel_1
} npu_it_channel;

///@brief npu ct channel
///@note This is an enum to specify the npu ct channel
typedef enum: uint32_t{
    ct_channel_0,
    ct_channel_1
} npu_ct_channel;

///@brief npu mt channel
///@note This is an enum to specify the npu mt channel
typedef enum: uint32_t{
    mt_channel_0,
    mt_channel_1,
    mt_channel_2,
    mt_channel_3,
    mt_channel_4,
    mt_channel_5
} npu_mt_channel;

///@brief npu bd id
///@note This is an enum to specify the npu bd id, there are 16 bd ids
typedef enum: uint32_t{
    bd_0,
    bd_1,
    bd_2,
    bd_3,
    bd_4,
    bd_5,
    bd_6,
    bd_7,
    bd_8,
    bd_9,
    bd_10,
    bd_11,
    bd_12,
    bd_13,
    bd_14,
    bd_15
} npu_bd_id;

///@brief npu sequence
///@note This is a class to store, create and process the npu sequence
class npu_sequence{
    public:
        npu_sequence() {
            this->npu_seq.resize(0);
        }; // default constructor, not allowed

        ///@brief constructor
        ///@param npu_seq the npu sequence
        ///@note The function will copy the npu sequence and parse it
        npu_sequence(npu_device device_gen, bool enable_preemption = false){
            this->device_gen = device_gen;
            this->setup_device(device_gen);
            this->is_valid = false;
            this->instr_version = 0xFF;
            this->enable_preemption = enable_preemption;
            this->clear_cmds();
        }

        ///@brief constructor
        ///@param filename the filename of the npu sequence or the name of the npu sequence if it is not a file
        ///@param from_file default is true, if true, the function will read the npu sequence from the file; if false, the function will use the filename as the name of the npu sequence;
        ///@note The function will read the npu sequence from the file and parse it
        ///@note If the file is not found, the function will throw an error
        ///@warning If the from_file is false, the function will not check if the filename is valid, and the npu sequence is empty
        void from_file(std::string filename, bool is_binary = true){
            std::ifstream instr_file(filename, std::ios::binary);
            if (!instr_file.is_open()){
                throw std::runtime_error("Failed to open file: " + filename);
            }

            if (is_binary){
                instr_file.seekg(0, std::ios::end);
                size_t instr_size = instr_file.tellg();
                instr_file.seekg(0, std::ios::beg);

                this->npu_seq.resize(instr_size / 4);

                if (instr_size % 4 != 0){
                    throw std::runtime_error("Instr file is invalied!");
                }

                if (!instr_file.read(reinterpret_cast<char *>(this->npu_seq.data()), instr_size)) {
                    throw std::runtime_error("Failed to read instruction file\n");
                }
            }
            else{
                std::string line;
                while (std::getline(instr_file, line)) {
                    std::istringstream iss(line);
                    uint32_t a;
                    if (!(iss >> std::hex >> a)) {
                        throw std::runtime_error("Unable to parse instruction file\n");
                    }
                    this->npu_seq.push_back(a);
                }
            }

            this->seq2cmds();
            this->is_valid = true;
            this->instr_version++;
        }

        /// @brief  write out the npu sequence to a file
        /// @param filename
        void write_out_sequence(std::string filename){
            assert(this->is_valid);
            std::ofstream file(filename, std::ios::binary);
            if (!file.is_open()){
                throw std::runtime_error("Failed to open file: " + filename);
            }

            file.write(reinterpret_cast<const char*>(this->npu_seq.data()), this->npu_seq.size() * sizeof(uint32_t));
            file.close();
        }

        ///@brief print the npu sequence
        ///@note The function will print the npu major, minor, dev gen, rows, cols, mem tile rows, instruction counts, instruction lines and the commands in the npu sequence
        void interpret(){
            int line_number = 0;
            MSG_BONDLINE(INSTR_PRINT_WIDTH);
            uint32_t* seq = this->npu_seq.data();
            instr_print(line_number, seq[line_number], "NPU information");
            instr_print(-1, seq[line_number], "--NPU version: " + std::to_string(this->npu_major) + "." + std::to_string(this->npu_minor));
            instr_print(-1, seq[line_number], "--NPU generation: " + std::to_string(this->npu_dev_gen));
            instr_print(-1, seq[line_number], "--NPU rows: " + std::to_string(this->npu_rows));
            line_number++;
            instr_print(line_number, seq[line_number], "--NPU cols: " + std::to_string(this->npu_cols));
            instr_print(-1, seq[line_number], "--NPU memory tile rows: " + std::to_string(this->npu_mem_tile_rows));
            line_number++;

            // Instruction commands
            instr_print(line_number, seq[line_number], "Instruction commands: " + std::to_string(seq[line_number]));
            line_number++;

            // Instruction lines
            instr_print(line_number, seq[line_number], "Instruction lines: " + std::to_string(seq[line_number] / 4));
            line_number++;

            for (int i = 0; i < this->cmds.size(); i++){
                line_number = this->cmds[i]->print_cmd(seq + line_number, line_number, i);
            }
            MSG_BONDLINE(INSTR_PRINT_WIDTH);
        }

        ///@brief setup the npu device
        ///@param device the npu device
        ///@note The function will setup the npu device
        ///@warning The function is only used for the npu sequence that is not pre-generated
        void setup_device(npu_device device){
            if (device == device_npu1){
                // Might be wrong
                this->npu_major = 0;
                this->npu_minor = 1;
                this->npu_dev_gen = 1;
                this->npu_rows = 6;
                this->npu_cols = 4;
                this->npu_mem_tile_rows = 1;
            }
            else if (device == device_npu2){
                this->npu_major = 0;
                this->npu_minor = 1;
                this->npu_dev_gen = 4;
                this->npu_rows = 6;
                this->npu_cols = 8;
                this->npu_mem_tile_rows = 1;
            }
        }


        ///@brief parse the npu sequence
        ///@note The function will parse the npu sequence and set the npu major, minor, dev gen, rows, cols, mem tile rows, instruction counts, instruction lines
        void seq2cmds(){
            assert(this->npu_seq.size() > 0);
            uint32_t* seq = this->npu_seq.data();
            this->clear_cmds();
            // Parse the npu sequence
            this->npu_major = (seq[0] >> dev_major_shift) & dev_major_mask;
            this->npu_minor = (seq[0] >> dev_minor_shift) & dev_minor_mask;
            this->npu_dev_gen = (seq[0] >> dev_gen_shift) & dev_gen_mask;
            this->npu_rows = (seq[0] >> dev_n_row_shift) & dev_n_row_mask;
            this->npu_cols = (seq[1] >> dev_num_cols_shift) & dev_num_cols_mask;
            this->npu_mem_tile_rows = (seq[1] >> dev_mem_tile_rows_shift) & dev_mem_tile_rows_mask;
            this->instruction_counts = seq[2];
            this->instruction_lines = seq[3] / 4;
            int i = 4;
            while (i < this->npu_seq.size()){
                if (seq[i] == op_headers::XAIE_IO_BLOCKWRITE){
                    LOG_VERBOSE(1, "DMA block write");
                    std::unique_ptr<npu_dma_block_cmd> cmd = std::make_unique<npu_dma_block_cmd>();
                    cmd->dump_cmd(seq + i);
                    i += cmd->get_op_lines();
                    this->cmds.push_back(std::move(cmd));
                    LOG_VERBOSE(1, "DMA block write" << i);
                }
                else if (seq[i] == op_headers::XAIE_IO_CUSTOM_OP_DDR_PATCH){
                    LOG_VERBOSE(1, "DMA DDR patch write");
                    std::unique_ptr<npu_ddr_cmd> cmd = std::make_unique<npu_ddr_cmd>();
                    cmd->dump_cmd(seq + i);
                    i += cmd->get_op_lines();
                    this->cmds.push_back(std::move(cmd));
                }
                else if (seq[i] == op_headers::XAIE_IO_MASKWRITE){
                    LOG_VERBOSE(1, "DMA issue token write");
                    std::unique_ptr<npu_issue_token_cmd> cmd = std::make_unique<npu_issue_token_cmd>();
                    cmd->dump_cmd(seq + i);
                    i += cmd->get_op_lines();
                    this->cmds.push_back(std::move(cmd));
                }
                else if (seq[i] == op_headers::XAIE_IO_WRITE){
                    LOG_VERBOSE(1, "Queue write");
                    std::unique_ptr<npu_write_cmd> cmd = std::make_unique<npu_write_cmd>();
                    cmd->dump_cmd(seq + i);
                    i += cmd->get_op_lines();
                    this->cmds.push_back(std::move(cmd));
                }
                else if (seq[i] == op_headers::XAIE_IO_CUSTOM_OP_TCT){ // Wait sync, AIETargetNPU.cpp line 62
                    LOG_VERBOSE(1, "DMA sync write");
                    std::unique_ptr<npu_wait_cmd> cmd = std::make_unique<npu_wait_cmd>();
                    cmd->dump_cmd(seq + i);
                    i += cmd->get_op_lines();
                    this->cmds.push_back(std::move(cmd));
                }
                else{
                    i++;
                }
            }
        }

        ///@brief convert the npu sequence to the npu format
        ///@note The function will convert the npu sequence to the npu format
        ///@note If the npu sequence is pre-generated(created from a file or other vector), the function will compare the npu sequence with the pre-generated npu sequence and print the difference
        ///@note If the npu sequence is not pre-generated, the function will update the npu sequence inside the class
        ///@warning If a npu sequence is not pre-generated, this function must be called before the npu sequence is used by the npu_app
        ///@return the npu sequence in std::vector<uint32_t>
        void cmds2seq(){
            this->instruction_lines = 4;
            for (int i = 0; i < this->cmds.size(); i++){
                this->instruction_lines += this->cmds[i]->get_op_lines();
            }
            this->npu_seq.reserve(this->instruction_lines);

            this->npu_seq.push_back(
                (this->npu_major << dev_major_shift) |
                (this->npu_minor << dev_minor_shift) |
                (this->npu_dev_gen << dev_gen_shift) |
                (this->npu_rows << dev_n_row_shift)
            );

            this->npu_seq.push_back(
                (this->npu_cols << dev_num_cols_shift) |
                (this->npu_mem_tile_rows << dev_mem_tile_rows_shift)
            );

            this->instruction_counts = this->cmds.size();
            this->npu_seq.push_back(this->instruction_counts);
            this->npu_seq.push_back(this->instruction_lines << 2);
            for (int i = 0; i < this->cmds.size(); i++){
                this->cmds[i]->to_npu(this->npu_seq);
            }

            this->is_valid = true;
            this->instr_version++;
        }


        // all npu control sequence commands
        ///@{
        /**
         *  @brief generate a npu write command
         *  @param tile: the tile of the NPU
         *  @param addr: the address of the register
         *  @param value: the value to write to the register
         */
        void rtp_write(npu_tiles tile, uint32_t addr, uint32_t value){
            if (this->is_valid == true) {
                this->clear_cmds();
            }
            std::unique_ptr<npu_write_cmd> cmd = std::make_unique<npu_write_cmd>();
            uint32_t row = (tile >> 4) & 0xF;
            uint32_t col = tile & 0xF;
            cmd->row = row;
            cmd->col = col;
            cmd->reg_addr = addr;
            cmd->value = value;
            cmd->could_be_push_queue = false;
            this->cmds.push_back(std::move(cmd));
        }


        ///@{
        /**
         *  @brief generate a npu dma memory copy command
         *  @param elem_size: the size of the element in bytes
         *  @param arg_idx: the index of the argument in the kernel
         *  @param channel_direction: the direction of the channel
         *  @param tile: the tile of the NPU
         *  @param bd_id: the BD ID of the NPU
         *  @param it_channel: the IT channel of the NPU
         *  @param offset: the offset of the memory in the DDR
         *  @param size: the size of the memory in the DDR
         *  @param stride: the stride of the memory in the DDR
         *  @param packet_id: the ID of the packet, -1 is disable it
         *  @param packet_type: the type of the packet, mostly 0
         *  @param issue_token: whether to issue a token, MM2S is false, S2MM is true by default
         *  @param cache_flag: the cache flag of the NPU, no_cache, normal_cache, aggressive_cache
         *  @param shm_control_packet_id: the packet ID of the shared memory control, mostly 15
         *  @param submit_to_queue: whether to submit the command to the queue, default is true
         *  @return void
         */
        void npu_dma_memcpy_nd(
            int elem_size,
            int arg_idx,
            dma_direction channel_direction,
            npu_tiles tile,
            npu_bd_id bd_id,
            npu_it_channel it_channel,
            const std::vector<uint32_t>& _offset,
            const std::vector<uint32_t>& _size,
            const std::vector<uint32_t>& _stride,
            int packet_id = -1,
            int packet_type = 0,
            bool issue_token = false,
            cache_flag_t cache_flag = normal_cache,
            int shm_control_packet_id = 15,
            bool submit_to_queue = true
        ){
            assert(elem_size <= 4); // not supported
            if (this->is_valid == true) {
                this->clear_cmds();
            }
            std::vector<uint32_t> offset = _offset;
            std::vector<uint32_t> size = _size;
            std::vector<uint32_t> stride = _stride;
            uint32_t row = (tile >> 4) & 0xF;
            uint32_t col = tile & 0xF;
            if (channel_direction == S2MM){
                issue_token = true;
            }

            if (elem_size == 1){
                // 1 byte data, AIE convert it to uint32_t
                elem_size = 4;
                size[3] >>= 2;
                offset[3] >>= 2;
                for (int i = 2; i >= 0; i--){
                    stride[i] >>= 2;
                }
            }
            else if (elem_size == 2){
                // 2 byte data, AIE convert it to uint32_t
                elem_size = 4;
                size[3] >>= 1;
                offset[3] >>= 1;
                for (int i = 2; i >= 0; i--){
                    stride[i] >>= 1;
                }
            }
            std::unique_ptr<npu_dma_block_cmd> cmd = std::make_unique<npu_dma_block_cmd>();
            cmd->row = row;
            cmd->col = col;
            cmd->bd_id = bd_id;
            if (size[1] == 1 && size[2] == 1){
                cmd->is_linear = true;
            }
            else{
                cmd->is_linear = false;
            }
            // reverse the order of offset, size and stride
            cmd->buffer_offset = 0;
            cmd->buffer_length = size[3];
            for (int i = 2; i >= 1; i--){ // iteration size does not count
                cmd->buffer_length *= size[i];
            }

            cmd->packet_enable = packet_id != -1;
            cmd->out_of_order_id = 0;
            if (cmd->packet_enable){
                cmd->packet_id = packet_id;
                cmd->packet_type = packet_type;
            }
            else{
                cmd->packet_id = 0;
                cmd->packet_type = 0;
            }

            if (cmd->is_linear){
                cmd->dim0_size = 0;
                cmd->dim0_stride = 1;
                cmd->dim1_size = 0;
                cmd->dim1_stride = 1;
                cmd->dim2_size = 0;
                cmd->dim2_stride = 1;
            }
            else{
                //https://github.com/Xilinx/mlir-aie/blob/2c7887524141d929f919649e3f247cbd1e8b7ae2/lib/Dialect/AIEX/IR/AIEXDialect.cpp#L171
                constexpr int step_bits = 20;
                constexpr int wrap_bits = 10;

                if(size[3] > (1<< wrap_bits)){
                    header_print_r("ERROR", ": step_3 out ouf range");
                }
                if(stride[3] > (1<< step_bits)){
                    header_print_r("ERROR", ": stride_3 out of range");
                }
                if(size[2] > (1<< wrap_bits)){
                    header_print_r("ERROR", ": step_2 out ouf range");
                }
                if(stride[2] > (1<< step_bits)){
                    header_print_r("ERROR", ": stride_2 out of range");
                }
                if(size[1] > (1<< wrap_bits)){
                    header_print_r("ERROR", ": step_1 out ouf range");
                }
                if(stride[1] > (1<< step_bits)){
                    header_print_r("ERROR", ": stride_1 out of range");
                }
                // inverse of human's order
                cmd->dim0_size = size[3];
                cmd->dim0_stride = size[3] != 1 ? stride[3] : 1;
                cmd->dim1_size = size[2];
                cmd->dim1_stride = size[2] != 1 ? stride[2] : 1;
                cmd->dim2_size = size[1];
                cmd->dim2_stride = size[1] != 1 ? stride[1] : 1;
            }
            cmd->next_bd_id = 0;
            cmd->valid_bd = 1;
            cmd->cache_flag = cache_flag;
            if (cmd->is_linear){
                cmd->iter_size = 1;
                cmd->iter_stride = 1;
            }
            else{
                cmd->iter_size = size[0];
                if (cmd->iter_size > 1){
                    cmd->iter_size = (stride[0] == 0) ? 1 : size[0];
                    cmd->iter_stride = (stride[0] == 0) ? 1 : stride[0];
                }
                else{
                    cmd->iter_stride = 1;
                }
            }
            cmd->issue_token = issue_token;

            cmd->get_lock_rel_val = 128;
            cmd->get_lock_rel_id = 0;
            cmd->get_lock_acq_enable = 0;
            cmd->get_lock_acq_val = 0;
            cmd->get_lock_acq_id = 0;

            this->cmds.push_back(std::move(cmd));
            // add ddr patch

            std::unique_ptr<npu_ddr_cmd> ddr_cmd = std::make_unique<npu_ddr_cmd>();
            ddr_cmd->row = row;
            ddr_cmd->col = col;
            ddr_cmd->bd_id = bd_id;
            ddr_cmd->arg_offset = offset[3];
            for (int i = 2; i >= 0; i--){
                ddr_cmd->arg_offset += offset[i] * stride[i];
            }
            ddr_cmd->arg_offset *= elem_size;
            ddr_cmd->arg_idx = arg_idx;

            this->cmds.push_back(std::move(ddr_cmd));
            // add issue token
            if (issue_token){
                std::unique_ptr<npu_issue_token_cmd> issue_token_cmd = std::make_unique<npu_issue_token_cmd>();
                issue_token_cmd->row = row;
                issue_token_cmd->col = col;
                issue_token_cmd->channel_direction = channel_direction;
                issue_token_cmd->channel_id = it_channel;
                issue_token_cmd->controller_packet_id = shm_control_packet_id;
                this->cmds.push_back(std::move(issue_token_cmd));
            }

            // queue write
            if (submit_to_queue){
                std::unique_ptr<npu_write_cmd> queue_cmd = std::make_unique<npu_write_cmd>();
                queue_cmd->row = row;
                queue_cmd->col = col;
                queue_cmd->channel_direction = channel_direction;
                queue_cmd->could_be_push_queue = true;
                queue_cmd->channel_id = it_channel;
                queue_cmd->repeat_count = size[0] - 1;
                queue_cmd->issue_token = issue_token;
                queue_cmd->bd_id = bd_id;
                this->cmds.push_back(std::move(queue_cmd));
            }
        }


        ///@{
        /**
         *  @brief generate a npu wait command
         *  @param tile: the tile of the NPU
         *  @param channel_direction: the direction of the channel
         *  @param it_channel: the IT channel of the NPU
         *  @return void
         */
        void npu_dma_wait(npu_tiles tile, dma_direction channel_direction, npu_it_channel it_channel){
            if (this->is_valid == true) {
                this->clear_cmds();

            }
            std::unique_ptr<npu_wait_cmd> wait_cmd = std::make_unique<npu_wait_cmd>();
            uint32_t row = (tile >> 4) & 0xF;
            uint32_t col = tile & 0xF;
            wait_cmd->wait_row = row;
            wait_cmd->wait_col = col;
            wait_cmd->channel_direction = channel_direction;
            wait_cmd->wait_channel = it_channel;
            this->cmds.push_back(std::move(wait_cmd));
        }


        ///@{
        /**
         *  @brief generate a npu maskwrite command
         *  @param tile: the tile of the NPU
         *  @param addr: the address of the register
         *  @param value: the value to write to the register
         *  @param mask: the mask of the register
         *  @return void
         */
        void npu_maskwrite(npu_tiles tile, uint32_t addr, uint32_t value, uint32_t mask){
            if (this->is_valid == true) {
                this->clear_cmds();
            }
            std::unique_ptr<npu_maskwrite_cmd> maskwrite_cmd = std::make_unique<npu_maskwrite_cmd>();
            uint32_t row = (tile >> 4) & 0xF;
            uint32_t col = tile & 0xF;
            maskwrite_cmd->row = row;
            maskwrite_cmd->col = col;
            maskwrite_cmd->addr = addr;
            maskwrite_cmd->value = value;
            maskwrite_cmd->mask = mask;
            this->cmds.push_back(std::move(maskwrite_cmd));
        }

        ///@{
        /**
         *  @brief generate a npu preemption command
         *  @param preemption_level: the level of the preemption
         *  @return void
         */
        void npu_preemption(uint32_t preemption_level){
            if (this->enable_preemption == false){
                return;
            }
            if (this->is_valid == true) {
                this->clear_cmds();
            }
            // LOG_VERBOSE(2, "Adding preemption command with level: " + std::to_string(preemption_level));
            std::unique_ptr<npu_preemption_cmd> preemption_cmd = std::make_unique<npu_preemption_cmd>();
            preemption_cmd->preemption_level = preemption_level;
            this->cmds.push_back(std::move(preemption_cmd));
        }



        ///@brief clear the npu commands
        void clear_cmds(){
            this->cmds.clear();
            this->npu_seq.clear();
            this->is_valid = false;
        }

        ///@brief get the size of the npu sequence in bytes
        ///@return size_t
        size_t size() {return this->npu_seq.size() * sizeof(uint32_t);}

        /// @brief dump the sequence to a uint32 array, return the pointer and size
        /// @return std::pair<uint32_t*, size_t>
        std::pair<uint32_t*, size_t> dump(){
            if (this->is_valid == false){
                this->cmds2seq();
            }
            return std::make_pair(this->npu_seq.data(), this->npu_seq.size());
        }

        inline bool sequence_valid(){return this->is_valid;}
        inline uint8_t sequence_version(){return this->instr_version;}
        npu_device device_gen;
    private:
        constexpr static uint32_t dev_n_row_shift = 24;
        constexpr static uint32_t dev_gen_shift = 16;
        constexpr static uint32_t dev_minor_shift = 8;
        constexpr static uint32_t dev_major_shift = 0;
        constexpr static uint32_t dev_mem_tile_rows_shift = 8;
        constexpr static uint32_t dev_num_cols_shift = 0;
        constexpr static uint32_t dev_n_row_mask = 0xFF;
        constexpr static uint32_t dev_gen_mask = 0xFF;
        constexpr static uint32_t dev_minor_mask = 0xFF;
        constexpr static uint32_t dev_major_mask = 0xFF;
        constexpr static uint32_t dev_mem_tile_rows_mask = 0xFF;
        constexpr static uint32_t dev_num_cols_mask = 0xFF;
        uint32_t npu_rows;
        uint32_t npu_cols;
        uint32_t npu_dev_gen;
        uint32_t npu_mem_tile_rows;
        uint32_t npu_minor;
        uint32_t npu_major;
        uint32_t instruction_counts;
        uint32_t instruction_lines;
        bool enable_preemption;

        std::vector<std::unique_ptr<npu_cmd>> cmds;
        std::vector<uint32_t> npu_seq;
        bool is_valid;
        uint8_t instr_version;
};

#endif
