/// \file npu_cmd_wait.hpp
/// \brief npu wait command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu wait command
#pragma once

#include "npu_cmd.hpp"


struct npu_wait_cmd : public npu_cmd{
    constexpr static uint32_t wait_sync_row_mask = 0xFF;
    constexpr static uint32_t wait_sync_col_mask = 0xFF;
    constexpr static uint32_t wait_sync_channel_mask = 0xFF;
    constexpr static uint32_t wait_sync_direction_mask = 0x1;
    constexpr static uint32_t wait_sync_row_shift = 8;
    constexpr static uint32_t wait_sync_col_shift = 16;
    constexpr static uint32_t wait_sync_channel_shift = 24;
    constexpr static uint32_t wait_sync_direction_shift = 0;
    uint32_t wait_row;
    uint32_t wait_col;
    uint32_t wait_channel;
    constexpr static uint32_t op_size = 4;
    dma_direction channel_direction; // 0 is S2MM, 1 is MM2S
    void dump_cmd(uint32_t *bd){
        uint32_t op_size_read = bd[1] >> 2;
        if (op_size_read != op_size){
            header_print("WARNING", "op_size mismatch: " + std::to_string(op_size_read) + " != " + std::to_string(op_size));
        }
        if (((bd[2] >> wait_sync_direction_shift) & wait_sync_direction_mask) == 0){
            this->channel_direction = S2MM;
        }
        else{
            this->channel_direction = MM2S;
        }
        this->wait_row = (bd[2] >> wait_sync_row_shift) & wait_sync_row_mask;
        this->wait_col = (bd[2] >> wait_sync_col_shift) & wait_sync_col_mask;
        this->wait_channel = (bd[3] >> wait_sync_channel_shift) & wait_sync_channel_mask;
    }

    int print_cmd(uint32_t *bd, int line_number, int op_count){
        MSG_BONDLINE(INSTR_PRINT_WIDTH);
        instr_print(line_number++, bd[0], "Wait sync, OP count: " + std::to_string(op_count));
        instr_print(line_number++, bd[1], "--Operation size: " + std::to_string(this->op_size));
        instr_print(line_number++, bd[2], "--Location: (row: " + std::to_string(this->wait_row) + ", col: " + std::to_string(this->wait_col) + ")");
        if (this->channel_direction == S2MM){
            instr_print(-1, bd[2], "--S2MM");
        }
        else{
            instr_print(-1, bd[2], "--MM2S");
        }
        instr_print(line_number++, bd[3], "--Channel: " + std::to_string(this->wait_channel));
        return line_number;
    }

    void to_npu(std::vector<uint32_t>& npu_seq){
        npu_seq.push_back(XAIE_IO_CUSTOM_OP_TCT);
        npu_seq.push_back(this->op_size << 2);
        npu_seq.push_back((this->wait_row << wait_sync_row_shift) | (this->wait_col << wait_sync_col_shift) | (this->channel_direction << wait_sync_direction_shift));
        npu_seq.push_back((this->wait_channel << wait_sync_channel_shift) | 0x10100);
    }

    int get_op_lines(){
        return 4;
    }
};
