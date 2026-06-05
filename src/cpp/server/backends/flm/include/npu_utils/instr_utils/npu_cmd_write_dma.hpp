/// \file npu_cmd_write_dma.hpp
/// \brief npu dma block write command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu dma block write command
#pragma once

#include "npu_cmd.hpp"

struct npu_dma_block_cmd : public npu_cmd{
    // bit shifts
    constexpr static uint32_t is_bd_shift = 12;
    constexpr static uint32_t bd_col_shift = 25;
    constexpr static uint32_t bd_row_shift = 20;
    constexpr static uint32_t bd_id_shift = 5;
    constexpr static uint32_t en_packet_shift = 30;
    constexpr static uint32_t out_of_order_shift = 24;
    constexpr static uint32_t packet_id_shift = 19;
    constexpr static uint32_t packet_type_shift = 16;
    constexpr static uint32_t dim_size_shift = 20;
    constexpr static uint32_t dim_stride_shift = 0;
    constexpr static uint32_t ax_cache_shift = 24;
    constexpr static uint32_t curr_iter_shift = 26;
    constexpr static uint32_t iter_size_shift = 20;
    constexpr static uint32_t iter_stride_shift = 0;
    constexpr static uint32_t next_bd_id_shift = 27;
    constexpr static uint32_t use_next_bd_shift = 26;
    constexpr static uint32_t valid_bd_shift = 25;
    constexpr static uint32_t get_lock_rel_val_shift = 18;
    constexpr static uint32_t get_lock_rel_id_shift = 13;
    constexpr static uint32_t get_lock_acq_enable_shift = 12;
    constexpr static uint32_t get_lock_acq_val_shift = 5;
    constexpr static uint32_t get_lock_acq_id_shift = 0;
    // bit masks
    constexpr static uint32_t is_bd_mask = 0xFF;
    constexpr static uint32_t bd_col_mask = 0x7F;
    constexpr static uint32_t bd_row_mask = 0x1F;
    constexpr static uint32_t bd_id_mask = 0xF;
    constexpr static uint32_t en_packet_mask = 0x1;
    constexpr static uint32_t out_of_order_mask = 0x3F;
    constexpr static uint32_t packet_id_mask = 0x1F;
    constexpr static uint32_t packet_type_mask = 0x7;
    constexpr static uint32_t dim_size_mask = 0x3FF;
    constexpr static uint32_t dim_stride_mask = 0xFFFFF;
    constexpr static uint32_t curr_iter_mask = 0x3FF;
    constexpr static uint32_t iter_size_mask = 0x3FF;
    constexpr static uint32_t iter_stride_mask = 0xFFFFF;
    constexpr static uint32_t ax_cache_mask = 0xF;
    constexpr static uint32_t next_bd_id_mask = 0xF;
    constexpr static uint32_t use_next_bd_mask = 0x1;
    constexpr static uint32_t valid_bd_mask = 0x1;
    constexpr static uint32_t get_lock_rel_val_mask = 0xEF;
    constexpr static uint32_t get_lock_rel_id_mask = 0xF;
    constexpr static uint32_t get_lock_acq_enable_mask = 0x1;
    constexpr static uint32_t get_lock_acq_val_mask = 0xEF;
    constexpr static uint32_t get_lock_acq_id_mask = 0xF;

    constexpr static uint32_t burst_size_shifted = 0xc0000000;
    // variables
    uint32_t col;
    uint32_t row;
    uint32_t bd_id;
    static const uint32_t op_size = 12;
    uint32_t buffer_length;
    uint32_t buffer_offset;
    uint32_t packet_enable;
    uint32_t out_of_order_id;
    uint32_t packet_id;
    uint32_t packet_type;
    uint32_t repeat_cnt;
    uint32_t issue_token;

    bool is_linear;
    uint32_t dim0_size;
    uint32_t dim0_stride;
    uint32_t dim1_size;
    uint32_t dim1_stride;
    uint32_t burst_size;
    uint32_t dim2_size;
    uint32_t dim2_stride;

    uint32_t iter_size;
    uint32_t iter_stride;
    uint32_t next_bd_id;
    uint32_t use_next_bd;
    uint32_t valid_bd;

    uint32_t get_lock_rel_val;
    uint32_t get_lock_rel_id;
    uint32_t get_lock_acq_enable;
    uint32_t get_lock_acq_val;
    uint32_t get_lock_acq_id;

    cache_flag_t cache_flag;

    void dump_cmd(uint32_t *bd){
        assert(*bd == XAIE_IO_BLOCKWRITE);
        LOG_VERBOSE(1, "bd_addr: " << bd);
        this->col = ((bd[2] >> bd_col_shift) & bd_col_mask);
        this->row = ((bd[2] >> bd_row_shift) & bd_row_mask);
        this->bd_id = ((bd[2] >> bd_id_shift) & bd_id_mask);
        // all words can be found in mlir-aie/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp close to line 577
        // word 0: unknown yet
        uint32_t op_size_read = bd[3] >> 2; // 4 bytes per instruction
        if (op_size_read != this->op_size){
            header_print("WARNING", "op_size_read: " + std::to_string(op_size_read) + " != " + std::to_string(this->op_size));
        }
        // word 1: Buffer length
        this->buffer_length = bd[4];
        // word 2: Buffer offset
        this->buffer_offset = bd[5];
        // word 3: Packet information
        this->packet_enable = (bd[6] >> en_packet_shift) & en_packet_mask;
        this->out_of_order_id = (bd[6] >> out_of_order_shift) & out_of_order_mask;
        this->packet_id = (bd[6] >> packet_id_shift) & packet_id_mask;
        this->packet_type = (bd[6] >> packet_type_shift) & packet_type_mask;
        // word 4: D0
        this->is_linear = (bd[7] == 0);
        this->dim0_size = (bd[7] >> dim_size_shift) & dim_size_mask;
        this->dim0_stride = (bd[7] >> dim_stride_shift) & dim_stride_mask;
        this->dim0_stride += 1; // The saved value is the stride - 1

        // word 5: D1
        this->burst_size = burst_size_shifted >> 30; // this is a constant value
        this->dim1_size = (bd[8] >> dim_size_shift) & dim_size_mask;
        this->dim1_stride = (bd[8] >> dim_stride_shift) & dim_stride_mask;
        this->dim1_stride += 1; // The saved value is the stride - 1
        // word 6: D2
        if (!this->is_linear){
            this->dim2_size = buffer_length / (this->dim0_size * this->dim1_size);
        }
        else{
            this->dim2_size = 0;
        }
        this->dim2_stride = (bd[9] >> dim_stride_shift) & dim_stride_mask;
        this->dim2_stride += 1; // The saved value is the stride - 1
        this->cache_flag = cache_flag_t((bd[9] >> ax_cache_shift) & ax_cache_mask);
        // word 7: D3, Iteration dimension
        this->iter_size = (bd[10] >> iter_size_shift) & iter_size_mask;
        this->iter_stride = (bd[10] >> iter_stride_shift) & iter_stride_mask;
        this->iter_stride += 1; // The saved value is the stride - 1
        this->iter_size += 1; // The saved value is the size - 1

        // word 8: Next BD, Lock information
        this->next_bd_id = (bd[11] >> next_bd_id_shift) & next_bd_id_mask;
        this->valid_bd = (bd[11] >> valid_bd_shift) & valid_bd_mask;

        // These informantion are provided but not used on NPU2
        this->get_lock_rel_val = (bd[11] >> get_lock_rel_val_shift) & get_lock_rel_val_mask;
        this->get_lock_rel_id = (bd[11] >> get_lock_rel_id_shift) & get_lock_rel_id_mask;
        this->get_lock_acq_enable = (bd[11] >> get_lock_acq_enable_shift) & get_lock_acq_enable_mask;
        this->get_lock_acq_val = (bd[11] >> get_lock_acq_val_shift) & get_lock_acq_val_mask;
        this->get_lock_acq_id = (bd[11] >> get_lock_acq_id_shift) & get_lock_acq_id_mask;
    }

    int print_cmd(uint32_t *bd, int line_number, int op_count){
        MSG_BONDLINE(INSTR_PRINT_WIDTH);
        // This is a dma bd
        instr_print(line_number++, bd[0], "DMA block write, OP count: " + std::to_string(op_count));
        instr_print(line_number++, bd[1], "Useless");
        // word 0:
        instr_print(line_number++, bd[2], "--Location: (row: " + std::to_string(row) + ", col: " + std::to_string(col) + ")");
        instr_print(-1, bd[2], "--BD ID: " + std::to_string(bd_id));

        // all words can be found in mlir-aie/lib/Dialect/AIEX/Transforms/AIEDmaToNpu.cpp close to line 577
        // word 0: unknown yet
        instr_print(line_number++, bd[3], "Operation size: " + std::to_string(this->op_size));
        // word 1: Buffer length
        instr_print(line_number++, bd[4], "--Buffer length: " + size_t_to_string(this->buffer_length));

        // word 2: Buffer offset
        instr_print(line_number++, bd[5], "--Buffer offset: " + size_t_to_string(this->buffer_offset));

        // word 3: Packet information
        if (this->packet_enable){
            // This is a packet
            instr_print(line_number++, bd[6], "--Packet enabled");
            instr_print(-1, bd[6], "--Out of order id: " + std::to_string(this->out_of_order_id));
            instr_print(-1, bd[6], "--Packet id: " + std::to_string(this->packet_id));
            instr_print(-1, bd[6], "--Packet type: " + std::to_string(this->packet_type));
        }
        else{
            instr_print(line_number++, bd[6], "Packet disabled");
        }

        // word 4: D0
        if (this->is_linear){
            instr_print(line_number++, bd[7], "A linear transfer, no D0");
        }
        else{
            instr_print(line_number++, bd[7], "--D0 size, stride: " + size_t_to_string(this->dim0_size) + ", " + size_t_to_string(this->dim0_stride));
        }

        // word 5: D1
        if (this->dim1_size == 0){
            instr_print(line_number++, bd[8], "--No D1");
        }
        else{
            instr_print(line_number++, bd[8], "--D1 size, stride: " + size_t_to_string(this->dim1_size) + ", " + size_t_to_string(this->dim1_stride));
        }

        // word 6: D2
        if (this->dim2_size == 0){
            instr_print(line_number++, bd[9], "--No D2");
        }
        else{
            instr_print(line_number++, bd[9], "--D2 stride: " + size_t_to_string(this->dim2_stride));
            instr_print(-1, bd[9], "--Inferred D2 size: " + size_t_to_string(this->dim2_size));
        }

        // word 7: D3, Iteration dimension
        if (this->iter_size == 0){
            instr_print(line_number++, bd[10], "--No Iteration dimension");
        }
        else{
            instr_print(line_number++, bd[10], "--Iteration size: " + size_t_to_string(this->iter_size));
            instr_print(-1, bd[10], "--Iteration stride: " + size_t_to_string(this->iter_stride));
        }

        // word 8: Next BD, Lock information
        instr_print(line_number++, bd[11], "--Next BD ID: " + std::to_string(this->next_bd_id));
        instr_print(-1, bd[11], "--Valid BD: " + std::to_string(this->valid_bd));
        instr_print(-1, bd[11], "--Lock relative value: " + size_t_to_string(this->get_lock_rel_val));
        instr_print(-1, bd[11], "--Lock relative id: " + std::to_string(this->get_lock_rel_id));
        instr_print(-1, bd[11], "--Lock acquire enable: " + std::to_string(this->get_lock_acq_enable));
        instr_print(-1, bd[11], "--Lock acquire value: " + size_t_to_string(this->get_lock_acq_val));
        instr_print(-1, bd[11], "--Lock acquire id: " + std::to_string(this->get_lock_acq_id));
        return line_number;
    }

    void to_npu(std::vector<uint32_t>& npu_seq){
        npu_seq.push_back(XAIE_IO_BLOCKWRITE);
        npu_seq.push_back(0x0);
        npu_seq.push_back((row << bd_row_shift) | (col << bd_col_shift) | (bd_id << bd_id_shift) | 0x1D000);
        npu_seq.push_back(this->op_size * 4);
        npu_seq.push_back(this->buffer_length);
        npu_seq.push_back(this->buffer_offset);
        npu_seq.push_back((this->packet_enable << en_packet_shift) | (this->out_of_order_id << out_of_order_shift) | (this->packet_id << packet_id_shift) | (this->packet_type << packet_type_shift));
        npu_seq.push_back((this->dim0_size << dim_size_shift) | ((this->dim0_stride - 1) << dim_stride_shift));
        npu_seq.push_back(0xc0000000 | (this->dim1_size << dim_size_shift) | ((this->dim1_stride - 1) << dim_stride_shift));
        // npu_seq.push_back((0 << dim_size_shift) | ((this->dim2_stride - 1) << dim_stride_shift)); // dim 2 size is not required to be set
        npu_seq.push_back((this->cache_flag << ax_cache_shift) | ((this->dim2_stride - 1) << dim_stride_shift)); // upper bits used for QoS fields ex. AxCache

        npu_seq.push_back(((this->iter_size - 1) << iter_size_shift) | ((this->iter_stride - 1) << iter_stride_shift));
        npu_seq.push_back(
            (this->next_bd_id << next_bd_id_shift) |
            (this->valid_bd << valid_bd_shift) |
            (this->get_lock_rel_val << get_lock_rel_val_shift) |
            (this->get_lock_rel_id << get_lock_rel_id_shift) |
            (this->get_lock_acq_enable << get_lock_acq_enable_shift) |
            (this->get_lock_acq_val << get_lock_acq_val_shift) |
            (this->get_lock_acq_id << get_lock_acq_id_shift)
        );
    }

    int get_op_lines(){
        return 12;
    }
};
