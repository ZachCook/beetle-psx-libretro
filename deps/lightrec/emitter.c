/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "blockcache.h"
#include "debug.h"
#include "disassembler.h"
#include "emitter.h"
#include "regcache.h"

#include <lightning.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*lightrec_rec_func_t)(const struct block *,
				    const struct opcode *, u32);

/* Forward declarations */
static void rec_SPECIAL(const struct block *block,
		       const struct opcode *op, u32 pc);
static void rec_REGIMM(const struct block *block,
		      const struct opcode *op, u32 pc);
static void rec_CP0(const struct block *block, const struct opcode *op, u32 pc);
static void rec_CP2(const struct block *block, const struct opcode *op, u32 pc);


static void unknown_opcode(const struct block *block,
			   const struct opcode *op, u32 pc)
{
	pr_warn("Unknown opcode: 0x%08x at PC 0x%08x\n", op->opcode, pc);
}

static void lightrec_emit_end_of_block(const struct block *block, u32 pc,
				       s8 reg_new_pc, u32 imm, u8 ra_reg,
				       u32 link,
				       const struct opcode *delay_slot)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	bool is_last_eob = !delay_slot || !delay_slot->next;
	u32 cycles = state->cycles;
	jit_state_t *_jit = block->_jit;
	unsigned int i;

	jit_note(__FILE__, __LINE__);

	if (link) {
		/* Update the $ra register */
		u8 link_reg = lightrec_alloc_reg_out(reg_cache, _jit, ra_reg);
		jit_movi(link_reg, link);
		lightrec_free_reg(reg_cache, link_reg);
	}

	if (reg_new_pc < 0) {
		reg_new_pc = lightrec_alloc_reg(reg_cache, _jit, JIT_V0);
		jit_movi(reg_new_pc, imm);
	}

	if (delay_slot) {
		cycles += lightrec_cycles_of_opcode(delay_slot->c);

		/* Recompile the delay slot */
		if (delay_slot->opcode)
			lightrec_rec_opcode(block, delay_slot, pc + 4);
	}

	/* Store back remaining registers */
	lightrec_storeback_regs(reg_cache, _jit);

	jit_movr(JIT_V0, reg_new_pc);
	jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, cycles);

	if (is_last_eob) {
		for (i = 0; i < state->nb_branches; i++)
			jit_patch(state->branches[i]);

		jit_ldxi(JIT_R0, LIGHTREC_REG_STATE,
			 offsetof(struct lightrec_state, eob_wrapper_func));

		jit_jmpr(JIT_R0);
	} else {
		state->branches[state->nb_branches++] = jit_jmpi();
	}
}

static void rec_special_JR(const struct block *block,
			   const struct opcode *op, u32 pc)
{
	u8 rs = lightrec_request_reg_in(block->state->reg_cache,
					block->_jit, op->r.rs, JIT_V0);
	const struct opcode *ds = op->flags & LIGHTREC_NO_DS ? NULL : op->next;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(block, pc, rs, 0, 31, 0, ds);
}

static void rec_special_JALR(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	u8 rs = lightrec_request_reg_in(block->state->reg_cache,
					block->_jit, op->r.rs, JIT_V0);
	const struct opcode *ds = op->flags & LIGHTREC_NO_DS ? NULL : op->next;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(block, pc, rs, 0, op->r.rd, pc + 8, ds);
}

static void rec_J(const struct block *block, const struct opcode *op, u32 pc)
{
	const struct opcode *ds = op->flags & LIGHTREC_NO_DS ? NULL : op->next;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(block, pc, -1,
				   (pc & 0xf0000000) | (op->j.imm << 2),
				   31, 0, ds);
}

static void rec_JAL(const struct block *block, const struct opcode *op, u32 pc)
{
	const struct opcode *ds = op->flags & LIGHTREC_NO_DS ? NULL : op->next;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(block, pc, -1,
				   (pc & 0xf0000000) | (op->j.imm << 2),
				   31, pc + 8, ds);
}

static void rec_b(const struct block *block, const struct opcode *op, u32 pc,
		  jit_code_t code, u32 link, bool unconditional, bool bz)
{
	struct regcache *reg_cache = block->state->reg_cache;
	const struct opcode *ds = op->flags & LIGHTREC_NO_DS ? NULL : op->next;
	struct native_register *regs_backup;
	jit_state_t *_jit = block->_jit;
	jit_node_t *addr;
	u8 link_reg;

	jit_note(__FILE__, __LINE__);

	if (!unconditional) {
		u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs),
		   rt = bz ? 0 : lightrec_alloc_reg_in(
				   reg_cache, _jit, op->i.rt);

#if __WORDSIZE == 64
		jit_extr_i(rs, rs);
		if (rt)
			jit_extr_i(rt, rt);
#endif

		/* Generate the branch opcode */
		addr = jit_new_node_pww(code, NULL, rs, rt);

		lightrec_free_regs(reg_cache);
		regs_backup = lightrec_regcache_enter_branch(reg_cache);
	}

	lightrec_emit_end_of_block(block, pc, -1,
			pc + 4 + ((s16)op->i.imm << 2), 31, link, ds);

	if (!unconditional) {
		jit_patch(addr);
		lightrec_regcache_leave_branch(reg_cache, regs_backup);

		if (bz && link) {
			/* Update the $ra register */
			link_reg = lightrec_alloc_reg_out(reg_cache, _jit, 31);
			jit_movi(link_reg, link);
			lightrec_free_reg(reg_cache, link_reg);
		}

		if (ds && ds->opcode)
			lightrec_rec_opcode(block, ds, pc + 4);
	}
}

static void rec_BNE(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_beqr, 0, false, false);
}

static void rec_BEQ(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_bner, 0,
			op->i.rs == op->i.rt, false);
}

static void rec_BLEZ(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_bgti, 0, op->i.rs == 0, true);
}

static void rec_BGTZ(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_blei, 0, false, true);
}

static void rec_regimm_BLTZ(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_bgei, 0, false, true);
}

static void rec_regimm_BLTZAL(const struct block *block,
			      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_bgei, pc + 8, false, true);
}

static void rec_regimm_BGEZ(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_blti, 0, !op->i.rs, true);
}

static void rec_regimm_BGEZAL(const struct block *block,
			      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_blti, pc + 8, !op->i.rs, true);
}

static void rec_alu_imm(const struct block *block, const struct opcode *op,
			jit_code_t code, bool sign_extend)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs),
	   rt = lightrec_alloc_reg_out(reg_cache, _jit, op->i.rt);

	jit_note(__FILE__, __LINE__);

	if (sign_extend) {
#if __WORDSIZE == 64
		jit_extr_i(rt, rt);
#endif
		jit_new_node_www(code, rt, rs, (s32)(s16) op->i.imm);
	} else {
		jit_new_node_www(code, rt, rs, (u32)(u16) op->i.imm);
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_alu_special(const struct block *block, const struct opcode *op,
			    jit_code_t code, bool is_reg_shift)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rs),
	   rt = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rt),
	   rd = lightrec_alloc_reg_out(reg_cache, _jit, op->r.rd);

	jit_note(__FILE__, __LINE__);
	if (!is_reg_shift) {
#if __WORDSIZE == 64
		jit_extr_i(rs, rs);
		jit_extr_i(rt, rt);
#endif
		jit_new_node_www(code, rd, rs, rt);
	} else {
		u8 temp = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_andi(temp, rs, 0x1f);

#if __WORDSIZE == 64
		if (code == jit_code_rshr)
			jit_extr_i(rt, rt);
		else if (code == jit_code_rshr_u)
			jit_extr_ui(rt, rt);
#endif
		jit_new_node_www(code, rd, rt, temp);

		lightrec_free_reg(reg_cache, temp);
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_ADDIU(const struct block *block,
		      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_addi, true);
}

static void rec_ADDI(const struct block *block, const struct opcode *op, u32 pc)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_addi, true);
}

static void rec_SLTIU(const struct block *block,
		      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_lti_u, true);
}

static void rec_SLTI(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_lti, true);
}

static void rec_ANDI(const struct block *block, const struct opcode *op, u32 pc)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs),
	   rt = lightrec_alloc_reg_out(reg_cache, _jit, op->i.rt);

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);

	/* PSX code uses ANDI 0xff / ANDI 0xffff a lot, which are basically
	 * casts to uint8_t / uint16_t. */
	if (op->i.imm == 0xff)
		jit_extr_uc(rt, rs);
	else if (op->i.imm == 0xffff)
		jit_extr_us(rt, rs);
	else
		jit_andi(rt, rs, (u32)(u16) op->i.imm);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_ORI(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_ori, false);
}

static void rec_XORI(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(block, op, jit_code_xori, false);
}

static void rec_LUI(const struct block *block, const struct opcode *op, u32 pc)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rt;

	jit_name(__func__);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, op->i.rt);

	jit_note(__FILE__, __LINE__);
	jit_movi(rt, op->i.imm << 16);

	lightrec_free_reg(reg_cache, rt);
}

static void rec_special_ADDU(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_addr, false);
}

static void rec_special_ADD(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_addr, false);
}

static void rec_special_SUBU(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_subr, false);
}

static void rec_special_SUB(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_subr, false);
}

static void rec_special_AND(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_andr, false);
}

static void rec_special_OR(const struct block *block,
			   const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_orr, false);
}

static void rec_special_XOR(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_xorr, false);
}

static void rec_special_NOR(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rd;

	jit_name(__func__);
	rec_alu_special(block, op, jit_code_orr, false);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, op->r.rd);

	jit_note(__FILE__, __LINE__);
	jit_comr(rd, rd);

	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_SLTU(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_ltr_u, false);
}

static void rec_special_SLT(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_ltr, false);
}

static void rec_special_SLLV(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_lshr, true);
}

static void rec_special_SRLV(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_rshr_u, true);
}

static void rec_special_SRAV(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(block, op, jit_code_rshr, true);
}

static void rec_alu_shift(const struct block *block,
			  const struct opcode *op, jit_code_t code)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rt = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rt),
	   rd = lightrec_alloc_reg_out(reg_cache, _jit, op->r.rd);

	jit_note(__FILE__, __LINE__);
#if __WORDSIZE == 64
	if (code == jit_code_rshi_u)
		jit_extr_ui(rt, rt);
	else if (code == jit_code_rshi)
		jit_extr_i(rt, rt);
#endif
	jit_new_node_www(code, rd, rt, op->r.imm);

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_SLL(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(block, op, jit_code_lshi);
}

static void rec_special_SRL(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(block, op, jit_code_rshi_u);
}

static void rec_special_SRA(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(block, op, jit_code_rshi);
}

static void rec_alu_mult(const struct block *block,
			 const struct opcode *op, bool is_signed)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rs),
	   rt = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rt),
	   lo = lightrec_alloc_reg_out(reg_cache, _jit, REG_LO),
	   hi = lightrec_alloc_reg_out(reg_cache, _jit, REG_HI);

	jit_note(__FILE__, __LINE__);
#if __WORDSIZE == 32
	/* On 32-bit systems, do a 32*32->64 bit operation. */
	if (is_signed)
		jit_qmulr(lo, hi, rs, rt);
	else
		jit_qmulr_u(lo, hi, rs, rt);
#else
	/* On 64-bit systems, do a 64*64->64 bit operation.
	 * The input registers must be 32 bits, so we first sign-extend (if
	 * mult) or clear (if multu) the input registers. */
	if (is_signed) {
		jit_extr_i(lo, rt);
		jit_extr_i(hi, rs);
	} else {
		jit_extr_ui(lo, rt);
		jit_extr_ui(hi, rs);
	}
	jit_mulr(lo, hi, lo);

	/* The 64-bit output value is in $lo, store the upper 32 bits in $hi */
	jit_rshi_u(hi, lo, 32);
#endif

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, lo);
	lightrec_free_reg(reg_cache, hi);
}

static void rec_alu_div(const struct block *block,
			const struct opcode *op, bool is_signed)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rs = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rs),
	   rt = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rt),
	   lo = lightrec_alloc_reg_out(reg_cache, _jit, REG_LO),
	   hi = lightrec_alloc_reg_out(reg_cache, _jit, REG_HI);
	jit_node_t *branch, *to_end;

	jit_note(__FILE__, __LINE__);

	/* Jump to special handler if dividing by zero  */
	branch = jit_beqi(rt, 0);

#if __WORDSIZE == 32
	if (is_signed)
		jit_qdivr(lo, hi, rs, rt);
	else
		jit_qdivr_u(lo, hi, rs, rt);
#else
	/* On 64-bit systems, the input registers must be 32 bits, so we first sign-extend
	 * (if div) or clear (if divu) the input registers. */
	if (is_signed) {
		jit_extr_i(lo, rt);
		jit_extr_i(hi, rs);
		jit_qdivr(lo, hi, hi, lo);
	} else {
		jit_extr_ui(lo, rt);
		jit_extr_ui(hi, rs);
		jit_qdivr_u(lo, hi, hi, lo);
	}
#endif

	/* Jump above the div-by-zero handler */
	to_end = jit_jmpi();

	jit_patch(branch);

	if (is_signed) {
		jit_lti(lo, rs, 0);
		jit_lshi(lo, lo, 1);
		jit_subi(lo, lo, 1);
	} else {
		jit_movi(lo, 0xffffffff);
	}

	jit_movr(hi, rs);

	jit_patch(to_end);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, lo);
	lightrec_free_reg(reg_cache, hi);
}

static void rec_special_MULT(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mult(block, op, true);
}

static void rec_special_MULTU(const struct block *block,
			      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mult(block, op, false);
}

static void rec_special_DIV(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_div(block, op, true);
}

static void rec_special_DIVU(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_div(block, op, false);
}

static void rec_alu_mv_lo_hi(const struct block *block, u8 dst, u8 src)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	src = lightrec_alloc_reg_in(reg_cache, _jit, src);
	dst = lightrec_alloc_reg_out(reg_cache, _jit, dst);

	jit_note(__FILE__, __LINE__);
	jit_movr(dst, src);

	lightrec_free_reg(reg_cache, src);
	lightrec_free_reg(reg_cache, dst);
}

static void rec_special_MFHI(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(block, op->r.rd, REG_HI);
}

static void rec_special_MTHI(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(block, REG_HI, op->r.rs);
}

static void rec_special_MFLO(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(block, op->r.rd, REG_LO);
}

static void rec_special_MTLO(const struct block *block,
			     const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(block, REG_LO, op->r.rs);
}

static void rec_io(const struct block *block, const struct opcode *op,
		   bool load_rt, bool read_rt)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rt, rs, tmp, tmp2;

	jit_note(__FILE__, __LINE__);

	rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs);
	jit_stxi_i(offsetof(struct lightrec_state, op_data.addr),
		   LIGHTREC_REG_STATE, rs);
	lightrec_free_reg(reg_cache, rs);

	if (load_rt) {
		rt = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rt);
		jit_stxi_i(offsetof(struct lightrec_state, op_data.data),
			   LIGHTREC_REG_STATE, rt);
		lightrec_free_reg(reg_cache, rt);
	}

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
	jit_ldxi(tmp2, LIGHTREC_REG_STATE,
		 offsetof(struct lightrec_state, rw_func));

	jit_movi(tmp, op->opcode);
	jit_stxi_i(offsetof(struct lightrec_state, op_data.op),
		   LIGHTREC_REG_STATE, tmp);

	jit_note(__FILE__, __LINE__);

	jit_callr(tmp2);
	lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, tmp2);

	lightrec_regcache_mark_live(reg_cache, _jit);

	if (read_rt && likely(op->i.rt)) {
		rt = lightrec_alloc_reg_out(reg_cache, _jit, op->i.rt);
		jit_ldxi_i(rt, LIGHTREC_REG_STATE,
			   offsetof(struct lightrec_state, op_data.data));
		lightrec_free_reg(reg_cache, rt);
	}
}

static void rec_store_direct_no_invalidate(const struct block *block,
					   const struct opcode *op,
					   jit_code_t code)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_end;
	u8 tmp, tmp2, rs, rt;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs);
	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

	/* Convert to KUNSEG and avoid RAM mirrors */
	if (op->i.imm) {
		jit_addi(tmp, rs, (s16)op->i.imm);
		jit_andi(tmp, tmp, 0x1f9fffff);
	} else {
		jit_andi(tmp, rs, 0x1f9fffff);
	}

	lightrec_free_reg(reg_cache, rs);

	if (state->offset_ram != state->offset_scratch) {
		to_not_ram = jit_bmsi(tmp, BIT(28));

		jit_movi(tmp2, state->offset_ram);

		to_end = jit_jmpi();
		jit_patch(to_not_ram);

		jit_movi(tmp2, state->offset_scratch);
		jit_patch(to_end);
	} else if (state->offset_ram) {
		jit_movi(tmp2, state->offset_ram);
	}

	if (state->offset_ram || state->offset_scratch)
		jit_addr(tmp, tmp, tmp2);

	lightrec_free_reg(reg_cache, tmp2);

	rt = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rt);
	jit_new_node_ww(code, tmp, rt);

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp);
}

static void rec_store_direct(const struct block *block, const struct opcode *op,
			     jit_code_t code)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_end;
	u8 tmp, tmp2, tmp3, rs, rt;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp3 = lightrec_alloc_reg_temp(reg_cache, _jit);

	jit_movi(tmp3, 0);

	/* Convert to KUNSEG and avoid RAM mirrors */
	if (op->i.imm) {
		jit_addi(tmp2, rs, (s16)op->i.imm);
		jit_andi(tmp2, tmp2, 0x1f9fffff);
	} else {
		jit_andi(tmp2, rs, 0x1f9fffff);
	}

	lightrec_free_reg(reg_cache, rs);
	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	to_not_ram = jit_bgei(tmp2, 0x1fffff);

	/* Compute the offset to the code LUT */
#if __WORDSIZE == 64
	jit_lshi(tmp, tmp2, 1);
	jit_addr(tmp, LIGHTREC_REG_STATE, tmp);
#else
	jit_addr(tmp, LIGHTREC_REG_STATE, tmp2);
#endif

	/* Write NULL to the code LUT to invalidate any block that's there */
	jit_stxi(offsetof(struct lightrec_state, code_lut), tmp, tmp3);

	if (state->offset_ram != state->offset_scratch) {
		if (state->offset_ram)
			jit_movi(tmp3, state->offset_ram);

		to_end = jit_jmpi();
	}

	jit_patch(to_not_ram);

	if (state->offset_scratch)
		jit_movi(tmp3, state->offset_scratch);

	if (state->offset_ram != state->offset_scratch)
		jit_patch(to_end);

	if (state->offset_ram || state->offset_scratch)
		jit_addr(tmp2, tmp2, tmp3);

	lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, tmp3);

	rt = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rt);
	jit_new_node_ww(code, tmp2, rt);

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp2);
}

static void rec_store(const struct block *block, const struct opcode *op,
		     jit_code_t code)
{
	if (op->flags & LIGHTREC_NO_INVALIDATE)
		rec_store_direct_no_invalidate(block, op, code);
	else if (op->flags & LIGHTREC_DIRECT_IO)
		rec_store_direct(block, op, code);
	else
		rec_io(block, op, true, false);
}

static void rec_SB(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_store(block, op, jit_code_str_c);
}

static void rec_SH(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_store(block, op, jit_code_str_s);
}

static void rec_SW(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_store(block, op, jit_code_str_i);
}

static void rec_SWL(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, true, false);
}

static void rec_SWR(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, true, false);
}

static void rec_SWC2(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, false, false);
}

static void rec_load_direct(const struct block *block, const struct opcode *op,
			    jit_code_t code)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_not_bios, *to_end, *to_end2;
	u8 tmp, rs, rt, addr_reg;

	if (!op->i.rt)
		return;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, op->i.rt);

	if (op->i.imm) {
		jit_addi(rt, rs, (s16)op->i.imm);
		addr_reg = rt;

		if (op->i.rs != op->i.rt)
			lightrec_free_reg(reg_cache, rs);
	} else {
		addr_reg = rs;
	}

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	if (state->offset_ram == state->offset_bios &&
	    state->offset_ram == state->offset_scratch) {
		if (!state->mirrors_mapped) {
			jit_andi(tmp, addr_reg, BIT(28));
			jit_rshi_u(tmp, tmp, 28 - 22);
			jit_ori(tmp, tmp, 0x1f9fffff);
			jit_andr(rt, addr_reg, tmp);
		} else {
			jit_andi(rt, addr_reg, 0x1fffffff);
		}

		if (state->offset_ram)
			jit_movi(tmp, state->offset_ram);
	} else {
		to_not_ram = jit_bmsi(addr_reg, BIT(28));

		/* Convert to KUNSEG and avoid RAM mirrors */
		jit_andi(rt, addr_reg, 0x1fffff);

		if (state->offset_ram)
			jit_movi(tmp, state->offset_ram);

		to_end = jit_jmpi();

		jit_patch(to_not_ram);

		if (state->offset_bios != state->offset_scratch)
			to_not_bios = jit_bmci(addr_reg, BIT(22));

		/* Convert to KUNSEG */
		jit_andi(rt, addr_reg, 0x1fc7ffff);

		jit_movi(tmp, state->offset_bios);

		if (state->offset_bios != state->offset_scratch) {
			to_end2 = jit_jmpi();

			jit_patch(to_not_bios);

			/* Convert to KUNSEG */
			jit_andi(rt, addr_reg, 0x1f800fff);

			if (state->offset_scratch)
				jit_movi(tmp, state->offset_scratch);

			jit_patch(to_end2);
		}

		jit_patch(to_end);
	}

	if (state->offset_ram || state->offset_bios || state->offset_scratch)
		jit_addr(rt, rt, tmp);

	jit_new_node_ww(code, rt, rt);

	lightrec_free_reg(reg_cache, addr_reg);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp);
}

static void rec_load(const struct block *block, const struct opcode *op,
		    jit_code_t code)
{
	if (op->flags & LIGHTREC_DIRECT_IO)
		rec_load_direct(block, op, code);
	else
		rec_io(block, op, false, true);
}

static void rec_LB(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_load(block, op, jit_code_ldr_c);
}

static void rec_LBU(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_load(block, op, jit_code_ldr_uc);
}

static void rec_LH(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_load(block, op, jit_code_ldr_s);
}

static void rec_LHU(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_load(block, op, jit_code_ldr_us);
}

static void rec_LWL(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, true, true);
}

static void rec_LWR(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, true, true);
}

static void rec_LW(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_load(block, op, jit_code_ldr_i);
}

static void rec_LWC2(const struct block *block, const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_io(block, op, false, false);
}

static void rec_break_syscall(const struct block *block, u32 pc, bool is_break)
{
	struct regcache *reg_cache = block->state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u32 offset;
	u8 tmp;

	if (is_break)
		offset = offsetof(struct lightrec_state, break_func);
	else
		offset = offsetof(struct lightrec_state, syscall_func);

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
	jit_ldxi(tmp, LIGHTREC_REG_STATE, offset);
	jit_callr(tmp);
	lightrec_free_reg(reg_cache, tmp);

	lightrec_regcache_mark_live(reg_cache, _jit);

	/* TODO: the return address should be "pc - 4" if we're a delay slot */
	lightrec_emit_end_of_block(block, pc, -1, pc, 31, 0, NULL);
}

static void rec_special_SYSCALL(const struct block *block,
				const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_break_syscall(block, pc, false);
}

static void rec_special_BREAK(const struct block *block,
			      const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_break_syscall(block, pc, true);
}

static void rec_mfc(const struct block *block, const struct opcode *op)
{
	u8 rt, tmp, tmp2;
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;

	jit_note(__FILE__, __LINE__);

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

	jit_ldxi(tmp2, LIGHTREC_REG_STATE,
		 offsetof(struct lightrec_state, mfc_func));

	jit_movi(tmp, op->opcode);
	jit_stxi_i(offsetof(struct lightrec_state, op_data.op),
		   LIGHTREC_REG_STATE, tmp);

	jit_callr(tmp2);
	lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, tmp2);

	lightrec_regcache_mark_live(reg_cache, _jit);

	rt = lightrec_alloc_reg_out(reg_cache, _jit, op->r.rt);
	jit_ldxi_i(rt, LIGHTREC_REG_STATE,
		   offsetof(struct lightrec_state, op_data.data));
	lightrec_free_reg(reg_cache, rt);
}

static void rec_mtc(const struct block *block, const struct opcode *op, u32 pc)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 rt, tmp, tmp2;

	jit_note(__FILE__, __LINE__);

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, op->r.rt);

	jit_ldxi(tmp2, LIGHTREC_REG_STATE,
		 offsetof(struct lightrec_state, mtc_func));

	jit_movi(tmp, op->opcode);
	jit_stxi_i(offsetof(struct lightrec_state, op_data.op),
		   LIGHTREC_REG_STATE, tmp);

	jit_stxi_i(offsetof(struct lightrec_state, op_data.data),
		   LIGHTREC_REG_STATE, rt);
	lightrec_free_reg(reg_cache, rt);

	jit_callr(tmp2);
	lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, tmp2);

	lightrec_regcache_mark_live(reg_cache, _jit);

	if (op->i.op == OP_CP0 && (op->r.rd == 12 || op->r.rd == 13))
		lightrec_emit_end_of_block(block, pc, -1, pc + 4, 0, 0, NULL);
}

static void rec_cp0_MFC0(const struct block *block,
			 const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mfc(block, op);
}

static void rec_cp0_CFC0(const struct block *block,
			 const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mfc(block, op);
}

static void rec_cp0_MTC0(const struct block *block,
			 const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mtc(block, op, pc);
}

static void rec_cp0_CTC0(const struct block *block,
			 const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mtc(block, op, pc);
}

static void rec_cp2_basic_MFC2(const struct block *block,
			       const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mfc(block, op);
}

static void rec_cp2_basic_CFC2(const struct block *block,
			       const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mfc(block, op);
}

static void rec_cp2_basic_MTC2(const struct block *block,
			       const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mtc(block, op, pc);
}

static void rec_cp2_basic_CTC2(const struct block *block,
			       const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_mtc(block, op, pc);
}

static void rec_cp0_RFE(const struct block *block,
			const struct opcode *op, u32 pc)
{
	struct lightrec_state *state = block->state;
	jit_state_t *_jit = block->_jit;
	u8 tmp;

	jit_name(__func__);
	jit_note(__FILE__, __LINE__);

	tmp = lightrec_alloc_reg_temp(state->reg_cache, _jit);
	jit_ldxi(tmp, LIGHTREC_REG_STATE,
		 offsetof(struct lightrec_state, rfe_func));
	jit_callr(tmp);
	lightrec_free_reg(state->reg_cache, tmp);

	lightrec_regcache_mark_live(state->reg_cache, _jit);
}

static void rec_CP(const struct block *block, const struct opcode *op, u32 pc)
{
	struct lightrec_state *state = block->state;
	jit_state_t *_jit = block->_jit;
	u8 tmp, tmp2;

	jit_name(__func__);
	jit_note(__FILE__, __LINE__);

	tmp = lightrec_alloc_reg_temp(state->reg_cache, _jit);
	tmp2 = lightrec_alloc_reg_temp(state->reg_cache, _jit);

	jit_ldxi(tmp2, LIGHTREC_REG_STATE,
		 offsetof(struct lightrec_state, cp_func));

	jit_movi(tmp, op->opcode);
	jit_stxi_i(offsetof(struct lightrec_state, op_data.op),
		   LIGHTREC_REG_STATE, tmp);

	jit_callr(tmp2);
	lightrec_free_reg(state->reg_cache, tmp);
	lightrec_free_reg(state->reg_cache, tmp2);

	lightrec_regcache_mark_live(state->reg_cache, _jit);
}

static void rec_meta_unload(const struct block *block,
			    const struct opcode *op, u32 pc)
{
	struct lightrec_state *state = block->state;
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 reg = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs);

	pr_debug("Unloading reg %s\n", lightrec_reg_name(op->i.rs));
	lightrec_unload_reg(reg_cache, _jit, reg);
}

static void rec_meta_BEQZ(const struct block *block,
			  const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_bnei, 0, false, true);
}

static void rec_meta_BNEZ(const struct block *block,
			  const struct opcode *op, u32 pc)
{
	_jit_name(block->_jit, __func__);
	rec_b(block, op, pc, jit_code_beqi, 0, false, true);
}

static const lightrec_rec_func_t rec_standard[64] = {
	[OP_SPECIAL]		= rec_SPECIAL,
	[OP_REGIMM]		= rec_REGIMM,
	[OP_J]			= rec_J,
	[OP_JAL]		= rec_JAL,
	[OP_BEQ]		= rec_BEQ,
	[OP_BNE]		= rec_BNE,
	[OP_BLEZ]		= rec_BLEZ,
	[OP_BGTZ]		= rec_BGTZ,
	[OP_ADDI]		= rec_ADDI,
	[OP_ADDIU]		= rec_ADDIU,
	[OP_SLTI]		= rec_SLTI,
	[OP_SLTIU]		= rec_SLTIU,
	[OP_ANDI]		= rec_ANDI,
	[OP_ORI]		= rec_ORI,
	[OP_XORI]		= rec_XORI,
	[OP_LUI]		= rec_LUI,
	[OP_CP0]		= rec_CP0,
	[OP_CP2]		= rec_CP2,
	[OP_LB]			= rec_LB,
	[OP_LH]			= rec_LH,
	[OP_LWL]		= rec_LWL,
	[OP_LW]			= rec_LW,
	[OP_LBU]		= rec_LBU,
	[OP_LHU]		= rec_LHU,
	[OP_LWR]		= rec_LWR,
	[OP_SB]			= rec_SB,
	[OP_SH]			= rec_SH,
	[OP_SWL]		= rec_SWL,
	[OP_SW]			= rec_SW,
	[OP_SWR]		= rec_SWR,
	[OP_LWC2]		= rec_LWC2,
	[OP_SWC2]		= rec_SWC2,

	[OP_META_REG_UNLOAD]	= rec_meta_unload,
	[OP_META_BEQZ]		= rec_meta_BEQZ,
	[OP_META_BNEZ]		= rec_meta_BNEZ,
};

static const lightrec_rec_func_t rec_special[64] = {
	[OP_SPECIAL_SLL]	= rec_special_SLL,
	[OP_SPECIAL_SRL]	= rec_special_SRL,
	[OP_SPECIAL_SRA]	= rec_special_SRA,
	[OP_SPECIAL_SLLV]	= rec_special_SLLV,
	[OP_SPECIAL_SRLV]	= rec_special_SRLV,
	[OP_SPECIAL_SRAV]	= rec_special_SRAV,
	[OP_SPECIAL_JR]		= rec_special_JR,
	[OP_SPECIAL_JALR]	= rec_special_JALR,
	[OP_SPECIAL_SYSCALL]	= rec_special_SYSCALL,
	[OP_SPECIAL_BREAK]	= rec_special_BREAK,
	[OP_SPECIAL_MFHI]	= rec_special_MFHI,
	[OP_SPECIAL_MTHI]	= rec_special_MTHI,
	[OP_SPECIAL_MFLO]	= rec_special_MFLO,
	[OP_SPECIAL_MTLO]	= rec_special_MTLO,
	[OP_SPECIAL_MULT]	= rec_special_MULT,
	[OP_SPECIAL_MULTU]	= rec_special_MULTU,
	[OP_SPECIAL_DIV]	= rec_special_DIV,
	[OP_SPECIAL_DIVU]	= rec_special_DIVU,
	[OP_SPECIAL_ADD]	= rec_special_ADD,
	[OP_SPECIAL_ADDU]	= rec_special_ADDU,
	[OP_SPECIAL_SUB]	= rec_special_SUB,
	[OP_SPECIAL_SUBU]	= rec_special_SUBU,
	[OP_SPECIAL_AND]	= rec_special_AND,
	[OP_SPECIAL_OR]		= rec_special_OR,
	[OP_SPECIAL_XOR]	= rec_special_XOR,
	[OP_SPECIAL_NOR]	= rec_special_NOR,
	[OP_SPECIAL_SLT]	= rec_special_SLT,
	[OP_SPECIAL_SLTU]	= rec_special_SLTU,
};

static const lightrec_rec_func_t rec_regimm[64] = {
	[OP_REGIMM_BLTZ]	= rec_regimm_BLTZ,
	[OP_REGIMM_BGEZ]	= rec_regimm_BGEZ,
	[OP_REGIMM_BLTZAL]	= rec_regimm_BLTZAL,
	[OP_REGIMM_BGEZAL]	= rec_regimm_BGEZAL,
};

static const lightrec_rec_func_t rec_cp0[64] = {
	[OP_CP0_MFC0]		= rec_cp0_MFC0,
	[OP_CP0_CFC0]		= rec_cp0_CFC0,
	[OP_CP0_MTC0]		= rec_cp0_MTC0,
	[OP_CP0_CTC0]		= rec_cp0_CTC0,
	[OP_CP0_RFE]		= rec_cp0_RFE,
};

static const lightrec_rec_func_t rec_cp2_basic[64] = {
	[OP_CP2_BASIC_MFC2]	= rec_cp2_basic_MFC2,
	[OP_CP2_BASIC_CFC2]	= rec_cp2_basic_CFC2,
	[OP_CP2_BASIC_MTC2]	= rec_cp2_basic_MTC2,
	[OP_CP2_BASIC_CTC2]	= rec_cp2_basic_CTC2,
};

static void rec_SPECIAL(const struct block *block,
			const struct opcode *op, u32 pc)
{
	lightrec_rec_func_t f = rec_special[op->r.op];
	if (likely(f))
		(*f)(block, op, pc);
	else
		unknown_opcode(block, op, pc);
}

static void rec_REGIMM(const struct block *block,
		       const struct opcode *op, u32 pc)
{
	lightrec_rec_func_t f = rec_regimm[op->r.rt];
	if (likely(f))
		(*f)(block, op, pc);
	else
		unknown_opcode(block, op, pc);
}

static void rec_CP0(const struct block *block, const struct opcode *op, u32 pc)
{
	lightrec_rec_func_t f = rec_cp0[op->r.rs];
	if (likely(f))
		(*f)(block, op, pc);
	else
		rec_CP(block, op, pc);
}

static void rec_CP2(const struct block *block, const struct opcode *op, u32 pc)
{
	if (op->r.op == OP_CP2_BASIC) {
		lightrec_rec_func_t f = rec_cp2_basic[op->r.rs];
		if (likely(f)) {
			(*f)(block, op, pc);
			return;
		}
	}

	rec_CP(block, op, pc);
}

void lightrec_rec_opcode(const struct block *block,
			 const struct opcode *op, u32 pc)
{
	lightrec_rec_func_t f = rec_standard[op->i.op];
	if (likely(f))
		(*f)(block, op, pc);
	else
		unknown_opcode(block, op, pc);
}
