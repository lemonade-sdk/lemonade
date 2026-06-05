/// \file npu_cmd_preemption.hpp
/// \brief npu preemption command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu preemption command
#ifndef __NPU_CMD_PREEMPTION_HPP__
#define __NPU_CMD_PREEMPTION_HPP__

#include "npu_cmd.hpp"

struct npu_preemption_cmd : public npu_cmd{

    constexpr static uint32_t preemption_level_shift = 8;
    constexpr static uint32_t preemption_level_mask = 0x3; // 0, 1, 2, 3

    uint32_t preemption_level;
    constexpr static uint32_t op_size = 1 ;


    void dump_cmd(uint32_t *bd){
        this->preemption_level = (bd[0] >> preemption_level_shift) & preemption_level_mask;
        // this commands does not have a op size check
    }

    int print_cmd(uint32_t *bd, int line_number, int op_count){
        MSG_BONDLINE(INSTR_PRINT_WIDTH);
        instr_print(line_number++, bd[0], "Preemption, OP count: " + std::to_string(op_count));
        instr_print(-1, bd[0], "Preemption level: " + std::to_string(this->preemption_level));
        return line_number;
    }

    void to_npu(std::vector<uint32_t>& npu_seq){
        npu_seq.push_back(XAIE_IO_PREEMPT | (this->preemption_level << preemption_level_shift));
    }

    int get_op_lines(){
        return 1;
    }
};

#endif
