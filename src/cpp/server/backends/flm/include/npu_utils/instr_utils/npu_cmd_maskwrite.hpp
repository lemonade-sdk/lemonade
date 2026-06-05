/// \file npu_cmd_maskwrite.hpp
/// \brief npu maskwrite command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu maskwrite command
#ifndef __NPU_CMD_MASKWRITE_HPP__
#define __NPU_CMD_MASKWRITE_HPP__

#include "npu_cmd.hpp"

struct npu_maskwrite_cmd : public npu_cmd{
    constexpr static uint32_t bd_col_shift = 25;
    constexpr static uint32_t bd_row_shift = 20;
    constexpr static uint32_t bd_col_mask = 0x7F;
    constexpr static uint32_t bd_row_mask = 0x1F;

    constexpr static uint32_t queue_pkt_id_shift = 8;
    constexpr static uint32_t queue_pkt_id_mask = 0xFFFFFF;

    constexpr static uint32_t queue_channel_shift = 3;
    constexpr static uint32_t queue_channel_mask = 0x1;
    uint32_t addr;
    uint32_t value;
    uint32_t mask;
    uint32_t row, col;
    constexpr static uint32_t op_size = 7;


    void dump_cmd(uint32_t *bd){
        this->row = (bd[2] >> bd_row_shift) & bd_row_mask;
        this->col = (bd[2] >> bd_col_shift) & bd_col_mask;
        this->addr = bd[2] & 0xFFFFF; // 20 bits for address
        this->value = bd[4];
        this->mask = bd[5];
        uint32_t op_size_read = bd[6] >> 2;
        if (op_size_read != op_size){
            header_print("WARNING", "op_size mismatch: " + std::to_string(op_size_read) + " != " + std::to_string(op_size));
        }
    }

    int print_cmd(uint32_t *bd, int line_number, int op_count){
        MSG_BONDLINE(INSTR_PRINT_WIDTH);
        instr_print(line_number++, bd[0], "Maskwrite, OP count: " + std::to_string(op_count));
        instr_print(line_number++, bd[1], "Useless");
        instr_print(-1, bd[2], "--Location: (row: " + std::to_string(this->row) + ", col: " + std::to_string(this->col) + ")");
        instr_print(-1, bd[2], "--Address: " + std::to_string(this->addr));
        instr_print(line_number++, bd[3], "Always 0");
        instr_print(line_number++, bd[4], "Value: " + std::to_string(this->value));
        instr_print(line_number++, bd[5], "Mask: " + std::to_string(this->mask));
        instr_print(line_number++, bd[6], "OP size: " + std::to_string(this->op_size));
        return line_number;
    }

    void to_npu(std::vector<uint32_t>& npu_seq){
        npu_seq.push_back(XAIE_IO_MASKWRITE);
        npu_seq.push_back(0x0);
        npu_seq.push_back((this->addr&0xFFFFF)  | (this->row << bd_row_shift) + (this->col << bd_col_shift));
        npu_seq.push_back(0x0);
        npu_seq.push_back(this->value);
        npu_seq.push_back(this->mask);
        npu_seq.push_back(this->op_size << 2);
    }

    int get_op_lines(){
        return 7;
    }
};

#endif
