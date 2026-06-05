/// \file npu_cmd_issue_token.hpp
/// \brief npu issue token command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu issue token command
#pragma once

#include "npu_cmd.hpp"

struct npu_issue_token_cmd : public npu_cmd{
    constexpr static uint32_t bd_col_shift = 25;
    constexpr static uint32_t bd_row_shift = 20;
    constexpr static uint32_t bd_col_mask = 0x7F;
    constexpr static uint32_t bd_row_mask = 0x1F;

    constexpr static uint32_t queue_pkt_id_shift = 8;
    constexpr static uint32_t queue_pkt_id_mask = 0xFFFFFF;

    constexpr static uint32_t queue_channel_shift = 3;
    constexpr static uint32_t queue_channel_mask = 0x1;
    dma_direction channel_direction; // 0 is S2MM, 1 is MM2S
    uint32_t channel_id;
    uint32_t controller_packet_id;
    uint32_t row, col;
    constexpr static uint32_t op_size = 7;
    const uint32_t mask = 0x00001f00;

    void dump_cmd(uint32_t *bd){
        this->row = (bd[2] >> bd_row_shift) & bd_row_mask;
        this->col = (bd[2] >> bd_col_shift) & bd_col_mask;
        if ((bd[2] & 0x10) == 0){
            this->channel_direction = S2MM;
        }
        else{
            this->channel_direction = MM2S;
        }
        this->channel_id = (bd[2] >> queue_channel_shift) & queue_channel_mask;
        this->controller_packet_id = bd[4] >> queue_pkt_id_shift;
        uint32_t op_size_read = bd[6] >> 2;
        if (op_size_read != op_size){
            header_print("WARNING", "op_size mismatch: " + std::to_string(op_size_read) + " != " + std::to_string(op_size));
        }
    }

    int print_cmd(uint32_t *bd, int line_number, int op_count){
        MSG_BONDLINE(INSTR_PRINT_WIDTH);
        instr_print(line_number++, bd[0], "Issue token, OP count: " + std::to_string(op_count));
        instr_print(line_number++, bd[1], "Useless");
        if (this->channel_direction == false){
            instr_print(line_number++, bd[2], "--S2MM");
        }
        else{
            instr_print(line_number++, bd[2], "--MM2S");
        }
        instr_print(-1, bd[2], "--Location: (row: " + std::to_string(this->row) + ", col: " + std::to_string(this->col) + ")");
        instr_print(-1, bd[2], "--Channel: " + std::to_string(this->channel_id));
        instr_print(line_number++, bd[3], "Always 0");
        instr_print(line_number++, bd[4], "Controller packet ID: " + std::to_string(this->controller_packet_id));
        instr_print(line_number++, bd[5], "Mask");
        instr_print(line_number++, bd[6], "OP size: " + std::to_string(this->op_size));
        return line_number;
    }

    void to_npu(std::vector<uint32_t>& npu_seq){
        npu_seq.push_back(XAIE_IO_MASKWRITE);
        npu_seq.push_back(0x0);
        npu_seq.push_back(0x1D200 + this->channel_id * 0x08 + 0x10 * this->channel_direction + (this->row << bd_row_shift) + (this->col << bd_col_shift));
        npu_seq.push_back(0x0);
        npu_seq.push_back(this->controller_packet_id << queue_pkt_id_shift);
        npu_seq.push_back(this->mask);
        npu_seq.push_back(this->op_size << 2);
    }

    int get_op_lines(){
        return 7;
    }
};
