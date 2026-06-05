/// \file npu_cmd.hpp
/// \brief npu command
/// \author FastFlowLM Team, Alfred
/// \date 2025-09-09
/// \note This is a class for the npu command, it is a virtual class for all npu commands
#ifndef __NPU_CMD_HPP__
#define __NPU_CMD_HPP__

#include <stdlib.h>
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

const int INSTR_PRINT_WIDTH = 80;

typedef enum : uint32_t {
	XAIE_IO_WRITE,
	XAIE_IO_BLOCKWRITE,
	XAIE_IO_BLOCKSET,
	XAIE_IO_MASKWRITE,
	XAIE_IO_MASKPOLL,
	XAIE_IO_NOOP,
	XAIE_IO_PREEMPT,
	XAIE_IO_MASKPOLL_BUSY,
	XAIE_IO_LOADPDI,
	XAIE_IO_LOAD_PM_START,
	XAIE_IO_CREATE_SCRATCHPAD,
	XAIE_IO_UPDATE_STATE_TABLE,
	XAIE_IO_UPDATE_REG,
	XAIE_IO_UPDATE_SCRATCH,
	XAIE_CONFIG_SHIMDMA_BD,
	XAIE_CONFIG_SHIMDMA_DMABUF_BD,
	XAIE_IO_CUSTOM_OP_BEGIN = 1U<<7U, // 0x80
	XAIE_IO_CUSTOM_OP_TCT = XAIE_IO_CUSTOM_OP_BEGIN, // still 0x80
	XAIE_IO_CUSTOM_OP_DDR_PATCH, // Previously this was XAIE_IO_CUSTOM_OP_BEGIN + 1, 0x81
	XAIE_IO_CUSTOM_OP_READ_REGS, // Previously this was XAIE_IO_CUSTOM_OP_BEGIN + 2
	XAIE_IO_CUSTOM_OP_RECORD_TIMER, // Previously this was XAIE_IO_CUSTOM_OP_BEGIN + 3
	XAIE_IO_CUSTOM_OP_MERGE_SYNC, // Previously this was XAIE_IO_CUSTOM_OP_BEGIN + 4
	XAIE_IO_CUSTOM_OP_NEXT,
	XAIE_IO_LOAD_PM_END_INTERNAL = 200,
	XAIE_IO_CUSTOM_OP_MAX = UCHAR_MAX,
} op_headers;


typedef enum{
    e_npu_cmd_ddr,
    e_npu_cmd_issue_token,
    e_npu_cmd_wait,
    e_npu_cmd_write_dma,
    e_npu_cmd_write
} npu_cmd_type;

typedef enum {
    S2MM,
    MM2S
} dma_direction;

typedef enum :uint32_t {
    no_cache = 0x00,
    normal_cache = 0x02,
    aggressive_cache = 0x0e
} cache_flag_t;

inline void instr_print(int line_number, uint32_t word, std::string msg){
    if (line_number == -1){ // -1 for the case when one line has multiple messages
        MSG_BOX_LINE(INSTR_PRINT_WIDTH, std::dec << std::setw(7) << " | " << std::setw(11) << " | " << msg);
    }
    else{
        MSG_BOX_LINE(INSTR_PRINT_WIDTH, std::dec << std::setw(4) << line_number << " | " << std::hex << std::setfill('0') << std::setw(8) << word << " | " << msg);
    }
}

///@brief npu command
///@note This is an interface (pure virtual class) for all npu commands
///@note The class is used to print the command, convert the command to the npu format and dump the command to the buffer
///@warning The class is not used directly, but is used as a base class for all npu commands
struct npu_cmd{
    ///@brief print the command
    ///@param bd the buffer to dump the command
    ///@param line_number the line number of the command
    ///@param op_count the operation count of the command
    ///@return the number of lines of the command
    virtual int print_cmd(uint32_t *bd, int line_number, int op_count) = 0;

    ///@brief convert the command to the npu format
    ///@param npu_seq the npu sequence to dump the command
    ///@note The function will convert the command to the npu format and dump the command to the buffer
    virtual void to_npu(std::vector<uint32_t>& npu_seq) = 0;

    // virtual npu_cmd_type get_type() = 0;
    ///@brief dump the command to the buffer
    ///@param bd the buffer to dump the command
    virtual void dump_cmd(uint32_t *bd) = 0;

    ///@brief get the number of lines of the command
    ///@return the number of lines of the command
    virtual int get_op_lines() = 0;
};



#endif
