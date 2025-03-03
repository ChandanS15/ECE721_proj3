// See LICENSE for license details.

#include "alu_op_template.h"


reg_t alu_rv64_addiw(payload_t &pay_buf, const state_t &state) {
   int xlen = 64;
   reg_t pc = pay_buf.pc;
   reg_t npc = sext_xlen(pc + insn_length( MATCH_ADDIW));
   insn_t insn = pay_buf.inst;
   pay_buf.fflags = 0;

#include "insns/addiw.h"

   pay_buf.c_next_pc = npc;

   return npc;
}
