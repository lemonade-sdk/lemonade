// SPDX-License-Identifier: MIT
// Copyright (C) 2024-2025, Advanced Micro Devices, Inc. All rights reserved.
#ifndef AIEBU_H_
#define AIEBU_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum aiebu_error_code {
  aiebu_invalid_asm = 1,
  aiebu_invalid_patch_schema,
  aiebu_invalid_batch_buffer_type,
  aiebu_invalid_buffer_type,
  aiebu_invalid_offset,
  aiebu_invalid_internal_error,
  aiebu_invalid_input,
  aiebu_invalid_elf,
  aiebu_invalid_opcode
};

enum aiebu_assembler_buffer_type {
  aiebu_assembler_buffer_type_blob_instr_dpu,
  aiebu_assembler_buffer_type_blob_instr_prepost,
  aiebu_assembler_buffer_type_blob_instr_transaction,
  aiebu_assembler_buffer_type_blob_control_packet,
  aiebu_assembler_buffer_type_asm_aie2ps,
  aiebu_assembler_buffer_type_asm_aie2,
  aiebu_assembler_buffer_type_asm_aie4,
  aiebu_assembler_buffer_type_aie2_config,
  aiebu_assembler_buffer_type_aie2ps_config,
  aiebu_assembler_buffer_type_aie4_config
};

struct pm_ctrlpkt {
  uint32_t pm_id;
  const char* pm_buffer;
  size_t pm_buffer_size;
};

/*
 * This API takes buffer type, 2 buffers, their sizes and external_buffer_id json
 * it also allocate elf_buf and It fill elf content in it.
 * return, on success return return elf size, else posix error(negative).
 * User may pass any combination like
 * 1. type as aiebu_assembler_buffer_type_blob_instr_transaction, buffer1 as instruction buffer
 *    and buffer2 as control_packet: in this case it will package buffers in text and data
 *    section of elf respectively.
 * 2. type as aiebu_assembler_buffer_type_blob_instr_transaction, buffer1 as instruction buffer
 *    and buffer2 as null: in this case it will package buffer in text section.
 * 3. type as aiebu_assembler_buffer_type_asm_aie2ps, buffer1 as asm buffer and buffer2
 *    as null: in this case it will assemble the asm code and package in elf.
 *
 * @type                buffer type
 * @instr_buf           first buffer
 * @instr_buf_size      first buffer size
 * @control_buf         second buffer
 * @control_buf_size    second buffer size
 * @elf_buf             elf buffer
 * @patch_json          external_buffer_id_json buffer.
 * @patch_json_size     patch_json array size
 * @libs                libs to be included, ";" separated.
 * @libpaths            paths to search for libs, ";" separated.
 * @ctrlpkt             array of pm_ctrlpkt holding pm buffer and id
 * @ctrlpkt_size        size of ctrlpkt array
 */
int
aiebu_assembler_get_elf(enum aiebu_assembler_buffer_type type,
                        const char* buffer1,
                        size_t buffer1_size,
                        const char* buffer2,
                        size_t buffer2_size,
                        void** elf_buf,
                        const char* patch_json,
                        size_t patch_json_size,
                        const char* libs,
                        const char* libpaths,
                        struct pm_ctrlpkt* pm_ctrlpkts,
                        size_t pm_ctrlpkt_size);

#ifdef __cplusplus
}
#endif

#endif
