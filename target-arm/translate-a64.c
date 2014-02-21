/*
 *  AArch64 translation
 *
 *  Copyright (c) 2013 Alexander Graf <agraf@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "cpu.h"
#include "tcg-op.h"
#include "qemu/log.h"
#include "translate.h"
#include "qemu/host-utils.h"

#include "exec/gen-icount.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

static TCGv_i64 cpu_X[32];
static TCGv_i64 cpu_pc;
static TCGv_i32 cpu_NF, cpu_ZF, cpu_CF, cpu_VF;

/* Load/store exclusive handling */
static TCGv_i64 cpu_exclusive_addr;
static TCGv_i64 cpu_exclusive_val;
static TCGv_i64 cpu_exclusive_high;
#ifdef CONFIG_USER_ONLY
static TCGv_i64 cpu_exclusive_test;
static TCGv_i32 cpu_exclusive_info;
#endif

static const char *regnames[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "lr", "sp"
};

enum a64_shift_type {
    A64_SHIFT_TYPE_LSL = 0,
    A64_SHIFT_TYPE_LSR = 1,
    A64_SHIFT_TYPE_ASR = 2,
    A64_SHIFT_TYPE_ROR = 3
};

/* Table based decoder typedefs - used when the relevant bits for decode
 * are too awkwardly scattered across the instruction (eg SIMD).
 */
typedef void AArch64DecodeFn(DisasContext *s, uint32_t insn);

typedef struct AArch64DecodeTable {
    uint32_t pattern;
    uint32_t mask;
    AArch64DecodeFn *disas_fn;
} AArch64DecodeTable;

/* initialize TCG globals.  */
void a64_translate_init(void)
{
    int i;

    cpu_pc = tcg_global_mem_new_i64(TCG_AREG0,
                                    offsetof(CPUARMState, pc),
                                    "pc");
    for (i = 0; i < 32; i++) {
        cpu_X[i] = tcg_global_mem_new_i64(TCG_AREG0,
                                          offsetof(CPUARMState, xregs[i]),
                                          regnames[i]);
    }

    cpu_NF = tcg_global_mem_new_i32(TCG_AREG0, offsetof(CPUARMState, NF), "NF");
    cpu_ZF = tcg_global_mem_new_i32(TCG_AREG0, offsetof(CPUARMState, ZF), "ZF");
    cpu_CF = tcg_global_mem_new_i32(TCG_AREG0, offsetof(CPUARMState, CF), "CF");
    cpu_VF = tcg_global_mem_new_i32(TCG_AREG0, offsetof(CPUARMState, VF), "VF");

    cpu_exclusive_addr = tcg_global_mem_new_i64(TCG_AREG0,
        offsetof(CPUARMState, exclusive_addr), "exclusive_addr");
    cpu_exclusive_val = tcg_global_mem_new_i64(TCG_AREG0,
        offsetof(CPUARMState, exclusive_val), "exclusive_val");
    cpu_exclusive_high = tcg_global_mem_new_i64(TCG_AREG0,
        offsetof(CPUARMState, exclusive_high), "exclusive_high");
#ifdef CONFIG_USER_ONLY
    cpu_exclusive_test = tcg_global_mem_new_i64(TCG_AREG0,
        offsetof(CPUARMState, exclusive_test), "exclusive_test");
    cpu_exclusive_info = tcg_global_mem_new_i32(TCG_AREG0,
        offsetof(CPUARMState, exclusive_info), "exclusive_info");
#endif
}

void aarch64_cpu_dump_state(CPUState *cs, FILE *f,
                            fprintf_function cpu_fprintf, int flags)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint32_t psr = pstate_read(env);
    int i;

    cpu_fprintf(f, "PC=%016"PRIx64"  SP=%016"PRIx64"\n",
            env->pc, env->xregs[31]);
    for (i = 0; i < 31; i++) {
        cpu_fprintf(f, "X%02d=%016"PRIx64, i, env->xregs[i]);
        if ((i % 4) == 3) {
            cpu_fprintf(f, "\n");
        } else {
            cpu_fprintf(f, " ");
        }
    }
    cpu_fprintf(f, "PSTATE=%08x (flags %c%c%c%c)\n",
                psr,
                psr & PSTATE_N ? 'N' : '-',
                psr & PSTATE_Z ? 'Z' : '-',
                psr & PSTATE_C ? 'C' : '-',
                psr & PSTATE_V ? 'V' : '-');
    cpu_fprintf(f, "\n");

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 32;
        for (i = 0; i < numvfpregs; i += 2) {
            uint64_t vlo = float64_val(env->vfp.regs[i * 2]);
            uint64_t vhi = float64_val(env->vfp.regs[(i * 2) + 1]);
            cpu_fprintf(f, "q%02d=%016" PRIx64 ":%016" PRIx64 " ",
                        i, vhi, vlo);
            vlo = float64_val(env->vfp.regs[(i + 1) * 2]);
            vhi = float64_val(env->vfp.regs[((i + 1) * 2) + 1]);
            cpu_fprintf(f, "q%02d=%016" PRIx64 ":%016" PRIx64 "\n",
                        i + 1, vhi, vlo);
        }
        cpu_fprintf(f, "FPCR: %08x  FPSR: %08x\n",
                    vfp_get_fpcr(env), vfp_get_fpsr(env));
    }
}

static int get_mem_index(DisasContext *s)
{
#ifdef CONFIG_USER_ONLY
    return 1;
#else
    return s->user;
#endif
}

void gen_a64_set_pc_im(uint64_t val)
{
    tcg_gen_movi_i64(cpu_pc, val);
}

static void gen_exception(int excp)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_movi_i32(tmp, excp);
    gen_helper_exception(cpu_env, tmp);
    tcg_temp_free_i32(tmp);
}

static void gen_exception_insn(DisasContext *s, int offset, int excp)
{
    gen_a64_set_pc_im(s->pc - offset);
    gen_exception(excp);
    s->is_jmp = DISAS_EXC;
}

static inline bool use_goto_tb(DisasContext *s, int n, uint64_t dest)
{
    /* No direct tb linking with singlestep or deterministic io */
    if (s->singlestep_enabled || (s->tb->cflags & CF_LAST_IO)) {
        return false;
    }

    /* Only link tbs from inside the same guest page */
    if ((s->tb->pc & TARGET_PAGE_MASK) != (dest & TARGET_PAGE_MASK)) {
        return false;
    }

    return true;
}

static inline void gen_goto_tb(DisasContext *s, int n, uint64_t dest)
{
    TranslationBlock *tb;

    tb = s->tb;
    if (use_goto_tb(s, n, dest)) {
        tcg_gen_goto_tb(n);
        gen_a64_set_pc_im(dest);
        tcg_gen_exit_tb((tcg_target_long)tb + n);
        s->is_jmp = DISAS_TB_JUMP;
    } else {
        gen_a64_set_pc_im(dest);
        if (s->singlestep_enabled) {
            gen_exception(EXCP_DEBUG);
        }
        tcg_gen_exit_tb(0);
        s->is_jmp = DISAS_JUMP;
    }
}

static void unallocated_encoding(DisasContext *s)
{
    gen_exception_insn(s, 4, EXCP_UDEF);
}

#define unsupported_encoding(s, insn)                                    \
    do {                                                                 \
        qemu_log_mask(LOG_UNIMP,                                         \
                      "%s:%d: unsupported instruction encoding 0x%08x "  \
                      "at pc=%016" PRIx64 "\n",                          \
                      __FILE__, __LINE__, insn, s->pc - 4);              \
        unallocated_encoding(s);                                         \
    } while (0);

static void init_tmp_a64_array(DisasContext *s)
{
#ifdef CONFIG_DEBUG_TCG
    int i;
    for (i = 0; i < ARRAY_SIZE(s->tmp_a64); i++) {
        TCGV_UNUSED_I64(s->tmp_a64[i]);
    }
#endif
    s->tmp_a64_count = 0;
}

static void free_tmp_a64(DisasContext *s)
{
    int i;
    for (i = 0; i < s->tmp_a64_count; i++) {
        tcg_temp_free_i64(s->tmp_a64[i]);
    }
    init_tmp_a64_array(s);
}

static TCGv_i64 new_tmp_a64(DisasContext *s)
{
    assert(s->tmp_a64_count < TMP_A64_MAX);
    return s->tmp_a64[s->tmp_a64_count++] = tcg_temp_new_i64();
}

static TCGv_i64 new_tmp_a64_zero(DisasContext *s)
{
    TCGv_i64 t = new_tmp_a64(s);
    tcg_gen_movi_i64(t, 0);
    return t;
}

/*
 * Register access functions
 *
 * These functions are used for directly accessing a register in where
 * changes to the final register value are likely to be made. If you
 * need to use a register for temporary calculation (e.g. index type
 * operations) use the read_* form.
 *
 * B1.2.1 Register mappings
 *
 * In instruction register encoding 31 can refer to ZR (zero register) or
 * the SP (stack pointer) depending on context. In QEMU's case we map SP
 * to cpu_X[31] and ZR accesses to a temporary which can be discarded.
 * This is the point of the _sp forms.
 */
static TCGv_i64 cpu_reg(DisasContext *s, int reg)
{
    if (reg == 31) {
        return new_tmp_a64_zero(s);
    } else {
        return cpu_X[reg];
    }
}

/* register access for when 31 == SP */
static TCGv_i64 cpu_reg_sp(DisasContext *s, int reg)
{
    return cpu_X[reg];
}

/* read a cpu register in 32bit/64bit mode. Returns a TCGv_i64
 * representing the register contents. This TCGv is an auto-freed
 * temporary so it need not be explicitly freed, and may be modified.
 */
static TCGv_i64 read_cpu_reg(DisasContext *s, int reg, int sf)
{
    TCGv_i64 v = new_tmp_a64(s);
    if (reg != 31) {
        if (sf) {
            tcg_gen_mov_i64(v, cpu_X[reg]);
        } else {
            tcg_gen_ext32u_i64(v, cpu_X[reg]);
        }
    } else {
        tcg_gen_movi_i64(v, 0);
    }
    return v;
}

static TCGv_i64 read_cpu_reg_sp(DisasContext *s, int reg, int sf)
{
    TCGv_i64 v = new_tmp_a64(s);
    if (sf) {
        tcg_gen_mov_i64(v, cpu_X[reg]);
    } else {
        tcg_gen_ext32u_i64(v, cpu_X[reg]);
    }
    return v;
}

/* Return the offset into CPUARMState of an element of specified
 * size, 'element' places in from the least significant end of
 * the FP/vector register Qn.
 */
static inline int vec_reg_offset(int regno, int element, TCGMemOp size)
{
    int offs = offsetof(CPUARMState, vfp.regs[regno * 2]);
#ifdef HOST_WORDS_BIGENDIAN
    /* This is complicated slightly because vfp.regs[2n] is
     * still the low half and  vfp.regs[2n+1] the high half
     * of the 128 bit vector, even on big endian systems.
     * Calculate the offset assuming a fully bigendian 128 bits,
     * then XOR to account for the order of the two 64 bit halves.
     */
    offs += (16 - ((element + 1) * (1 << size)));
    offs ^= 8;
#else
    offs += element * (1 << size);
#endif
    return offs;
}

/* Return the offset into CPUARMState of a slice (from
 * the least significant end) of FP register Qn (ie
 * Dn, Sn, Hn or Bn).
 * (Note that this is not the same mapping as for A32; see cpu.h)
 */
static inline int fp_reg_offset(int regno, TCGMemOp size)
{
    int offs = offsetof(CPUARMState, vfp.regs[regno * 2]);
#ifdef HOST_WORDS_BIGENDIAN
    offs += (8 - (1 << size));
#endif
    return offs;
}

/* Offset of the high half of the 128 bit vector Qn */
static inline int fp_reg_hi_offset(int regno)
{
    return offsetof(CPUARMState, vfp.regs[regno * 2 + 1]);
}

/* Convenience accessors for reading and writing single and double
 * FP registers. Writing clears the upper parts of the associated
 * 128 bit vector register, as required by the architecture.
 * Note that unlike the GP register accessors, the values returned
 * by the read functions must be manually freed.
 */
static TCGv_i64 read_fp_dreg(DisasContext *s, int reg)
{
    TCGv_i64 v = tcg_temp_new_i64();

    tcg_gen_ld_i64(v, cpu_env, fp_reg_offset(reg, MO_64));
    return v;
}

static TCGv_i32 read_fp_sreg(DisasContext *s, int reg)
{
    TCGv_i32 v = tcg_temp_new_i32();

    tcg_gen_ld_i32(v, cpu_env, fp_reg_offset(reg, MO_32));
    return v;
}

static void write_fp_dreg(DisasContext *s, int reg, TCGv_i64 v)
{
    TCGv_i64 tcg_zero = tcg_const_i64(0);

    tcg_gen_st_i64(v, cpu_env, fp_reg_offset(reg, MO_64));
    tcg_gen_st_i64(tcg_zero, cpu_env, fp_reg_hi_offset(reg));
    tcg_temp_free_i64(tcg_zero);
}

static void write_fp_sreg(DisasContext *s, int reg, TCGv_i32 v)
{
    TCGv_i64 tmp = tcg_temp_new_i64();

    tcg_gen_extu_i32_i64(tmp, v);
    write_fp_dreg(s, reg, tmp);
    tcg_temp_free_i64(tmp);
}

static TCGv_ptr get_fpstatus_ptr(void)
{
    TCGv_ptr statusptr = tcg_temp_new_ptr();
    int offset;

    /* In A64 all instructions (both FP and Neon) use the FPCR;
     * there is no equivalent of the A32 Neon "standard FPSCR value"
     * and all operations use vfp.fp_status.
     */
    offset = offsetof(CPUARMState, vfp.fp_status);
    tcg_gen_addi_ptr(statusptr, cpu_env, offset);
    return statusptr;
}

/* Set ZF and NF based on a 64 bit result. This is alas fiddlier
 * than the 32 bit equivalent.
 */
static inline void gen_set_NZ64(TCGv_i64 result)
{
    TCGv_i64 flag = tcg_temp_new_i64();

    tcg_gen_setcondi_i64(TCG_COND_NE, flag, result, 0);
    tcg_gen_trunc_i64_i32(cpu_ZF, flag);
    tcg_gen_shri_i64(flag, result, 32);
    tcg_gen_trunc_i64_i32(cpu_NF, flag);
    tcg_temp_free_i64(flag);
}

/* Set NZCV as for a logical operation: NZ as per result, CV cleared. */
static inline void gen_logic_CC(int sf, TCGv_i64 result)
{
    if (sf) {
        gen_set_NZ64(result);
    } else {
        tcg_gen_trunc_i64_i32(cpu_ZF, result);
        tcg_gen_trunc_i64_i32(cpu_NF, result);
    }
    tcg_gen_movi_i32(cpu_CF, 0);
    tcg_gen_movi_i32(cpu_VF, 0);
}

/* dest = T0 + T1; compute C, N, V and Z flags */
static void gen_add_CC(int sf, TCGv_i64 dest, TCGv_i64 t0, TCGv_i64 t1)
{
    if (sf) {
        TCGv_i64 result, flag, tmp;
        result = tcg_temp_new_i64();
        flag = tcg_temp_new_i64();
        tmp = tcg_temp_new_i64();

        tcg_gen_movi_i64(tmp, 0);
        tcg_gen_add2_i64(result, flag, t0, tmp, t1, tmp);

        tcg_gen_trunc_i64_i32(cpu_CF, flag);

        gen_set_NZ64(result);

        tcg_gen_xor_i64(flag, result, t0);
        tcg_gen_xor_i64(tmp, t0, t1);
        tcg_gen_andc_i64(flag, flag, tmp);
        tcg_temp_free_i64(tmp);
        tcg_gen_shri_i64(flag, flag, 32);
        tcg_gen_trunc_i64_i32(cpu_VF, flag);

        tcg_gen_mov_i64(dest, result);
        tcg_temp_free_i64(result);
        tcg_temp_free_i64(flag);
    } else {
        /* 32 bit arithmetic */
        TCGv_i32 t0_32 = tcg_temp_new_i32();
        TCGv_i32 t1_32 = tcg_temp_new_i32();
        TCGv_i32 tmp = tcg_temp_new_i32();

        tcg_gen_movi_i32(tmp, 0);
        tcg_gen_trunc_i64_i32(t0_32, t0);
        tcg_gen_trunc_i64_i32(t1_32, t1);
        tcg_gen_add2_i32(cpu_NF, cpu_CF, t0_32, tmp, t1_32, tmp);
        tcg_gen_mov_i32(cpu_ZF, cpu_NF);
        tcg_gen_xor_i32(cpu_VF, cpu_NF, t0_32);
        tcg_gen_xor_i32(tmp, t0_32, t1_32);
        tcg_gen_andc_i32(cpu_VF, cpu_VF, tmp);
        tcg_gen_extu_i32_i64(dest, cpu_NF);

        tcg_temp_free_i32(tmp);
        tcg_temp_free_i32(t0_32);
        tcg_temp_free_i32(t1_32);
    }
}

/* dest = T0 - T1; compute C, N, V and Z flags */
static void gen_sub_CC(int sf, TCGv_i64 dest, TCGv_i64 t0, TCGv_i64 t1)
{
    if (sf) {
        /* 64 bit arithmetic */
        TCGv_i64 result, flag, tmp;

        result = tcg_temp_new_i64();
        flag = tcg_temp_new_i64();
        tcg_gen_sub_i64(result, t0, t1);

        gen_set_NZ64(result);

        tcg_gen_setcond_i64(TCG_COND_GEU, flag, t0, t1);
        tcg_gen_trunc_i64_i32(cpu_CF, flag);

        tcg_gen_xor_i64(flag, result, t0);
        tmp = tcg_temp_new_i64();
        tcg_gen_xor_i64(tmp, t0, t1);
        tcg_gen_and_i64(flag, flag, tmp);
        tcg_temp_free_i64(tmp);
        tcg_gen_shri_i64(flag, flag, 32);
        tcg_gen_trunc_i64_i32(cpu_VF, flag);
        tcg_gen_mov_i64(dest, result);
        tcg_temp_free_i64(flag);
        tcg_temp_free_i64(result);
    } else {
        /* 32 bit arithmetic */
        TCGv_i32 t0_32 = tcg_temp_new_i32();
        TCGv_i32 t1_32 = tcg_temp_new_i32();
        TCGv_i32 tmp;

        tcg_gen_trunc_i64_i32(t0_32, t0);
        tcg_gen_trunc_i64_i32(t1_32, t1);
        tcg_gen_sub_i32(cpu_NF, t0_32, t1_32);
        tcg_gen_mov_i32(cpu_ZF, cpu_NF);
        tcg_gen_setcond_i32(TCG_COND_GEU, cpu_CF, t0_32, t1_32);
        tcg_gen_xor_i32(cpu_VF, cpu_NF, t0_32);
        tmp = tcg_temp_new_i32();
        tcg_gen_xor_i32(tmp, t0_32, t1_32);
        tcg_temp_free_i32(t0_32);
        tcg_temp_free_i32(t1_32);
        tcg_gen_and_i32(cpu_VF, cpu_VF, tmp);
        tcg_temp_free_i32(tmp);
        tcg_gen_extu_i32_i64(dest, cpu_NF);
    }
}

/* dest = T0 + T1 + CF; do not compute flags. */
static void gen_adc(int sf, TCGv_i64 dest, TCGv_i64 t0, TCGv_i64 t1)
{
    TCGv_i64 flag = tcg_temp_new_i64();
    tcg_gen_extu_i32_i64(flag, cpu_CF);
    tcg_gen_add_i64(dest, t0, t1);
    tcg_gen_add_i64(dest, dest, flag);
    tcg_temp_free_i64(flag);

    if (!sf) {
        tcg_gen_ext32u_i64(dest, dest);
    }
}

/* dest = T0 + T1 + CF; compute C, N, V and Z flags. */
static void gen_adc_CC(int sf, TCGv_i64 dest, TCGv_i64 t0, TCGv_i64 t1)
{
    if (sf) {
        TCGv_i64 result, cf_64, vf_64, tmp;
        result = tcg_temp_new_i64();
        cf_64 = tcg_temp_new_i64();
        vf_64 = tcg_temp_new_i64();
        tmp = tcg_const_i64(0);

        tcg_gen_extu_i32_i64(cf_64, cpu_CF);
        tcg_gen_add2_i64(result, cf_64, t0, tmp, cf_64, tmp);
        tcg_gen_add2_i64(result, cf_64, result, cf_64, t1, tmp);
        tcg_gen_trunc_i64_i32(cpu_CF, cf_64);
        gen_set_NZ64(result);

        tcg_gen_xor_i64(vf_64, result, t0);
        tcg_gen_xor_i64(tmp, t0, t1);
        tcg_gen_andc_i64(vf_64, vf_64, tmp);
        tcg_gen_shri_i64(vf_64, vf_64, 32);
        tcg_gen_trunc_i64_i32(cpu_VF, vf_64);

        tcg_gen_mov_i64(dest, result);

        tcg_temp_free_i64(tmp);
        tcg_temp_free_i64(vf_64);
        tcg_temp_free_i64(cf_64);
        tcg_temp_free_i64(result);
    } else {
        TCGv_i32 t0_32, t1_32, tmp;
        t0_32 = tcg_temp_new_i32();
        t1_32 = tcg_temp_new_i32();
        tmp = tcg_const_i32(0);

        tcg_gen_trunc_i64_i32(t0_32, t0);
        tcg_gen_trunc_i64_i32(t1_32, t1);
        tcg_gen_add2_i32(cpu_NF, cpu_CF, t0_32, tmp, cpu_CF, tmp);
        tcg_gen_add2_i32(cpu_NF, cpu_CF, cpu_NF, cpu_CF, t1_32, tmp);

        tcg_gen_mov_i32(cpu_ZF, cpu_NF);
        tcg_gen_xor_i32(cpu_VF, cpu_NF, t0_32);
        tcg_gen_xor_i32(tmp, t0_32, t1_32);
        tcg_gen_andc_i32(cpu_VF, cpu_VF, tmp);
        tcg_gen_extu_i32_i64(dest, cpu_NF);

        tcg_temp_free_i32(tmp);
        tcg_temp_free_i32(t1_32);
        tcg_temp_free_i32(t0_32);
    }
}

/*
 * Load/Store generators
 */

/*
 * Store from GPR register to memory
 */
static void do_gpr_st(DisasContext *s, TCGv_i64 source,
                      TCGv_i64 tcg_addr, int size)
{
    g_assert(size <= 3);
    tcg_gen_qemu_st_i64(source, tcg_addr, get_mem_index(s), MO_TE + size);
}

/*
 * Load from memory to GPR register
 */
static void do_gpr_ld(DisasContext *s, TCGv_i64 dest, TCGv_i64 tcg_addr,
                      int size, bool is_signed, bool extend)
{
    TCGMemOp memop = MO_TE + size;

    g_assert(size <= 3);

    if (is_signed) {
        memop += MO_SIGN;
    }

    tcg_gen_qemu_ld_i64(dest, tcg_addr, get_mem_index(s), memop);

    if (extend && is_signed) {
        g_assert(size < 3);
        tcg_gen_ext32u_i64(dest, dest);
    }
}

/*
 * Store from FP register to memory
 */
static void do_fp_st(DisasContext *s, int srcidx, TCGv_i64 tcg_addr, int size)
{
    /* This writes the bottom N bits of a 128 bit wide vector to memory */
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_ld_i64(tmp, cpu_env, fp_reg_offset(srcidx, MO_64));
    if (size < 4) {
        tcg_gen_qemu_st_i64(tmp, tcg_addr, get_mem_index(s), MO_TE + size);
    } else {
        TCGv_i64 tcg_hiaddr = tcg_temp_new_i64();
        tcg_gen_qemu_st_i64(tmp, tcg_addr, get_mem_index(s), MO_TEQ);
        tcg_gen_qemu_st64(tmp, tcg_addr, get_mem_index(s));
        tcg_gen_ld_i64(tmp, cpu_env, fp_reg_hi_offset(srcidx));
        tcg_gen_addi_i64(tcg_hiaddr, tcg_addr, 8);
        tcg_gen_qemu_st_i64(tmp, tcg_hiaddr, get_mem_index(s), MO_TEQ);
        tcg_temp_free_i64(tcg_hiaddr);
    }

    tcg_temp_free_i64(tmp);
}

/*
 * Load from memory to FP register
 */
static void do_fp_ld(DisasContext *s, int destidx, TCGv_i64 tcg_addr, int size)
{
    /* This always zero-extends and writes to a full 128 bit wide vector */
    TCGv_i64 tmplo = tcg_temp_new_i64();
    TCGv_i64 tmphi;

    if (size < 4) {
        TCGMemOp memop = MO_TE + size;
        tmphi = tcg_const_i64(0);
        tcg_gen_qemu_ld_i64(tmplo, tcg_addr, get_mem_index(s), memop);
    } else {
        TCGv_i64 tcg_hiaddr;
        tmphi = tcg_temp_new_i64();
        tcg_hiaddr = tcg_temp_new_i64();

        tcg_gen_qemu_ld_i64(tmplo, tcg_addr, get_mem_index(s), MO_TEQ);
        tcg_gen_addi_i64(tcg_hiaddr, tcg_addr, 8);
        tcg_gen_qemu_ld_i64(tmphi, tcg_hiaddr, get_mem_index(s), MO_TEQ);
        tcg_temp_free_i64(tcg_hiaddr);
    }

    tcg_gen_st_i64(tmplo, cpu_env, fp_reg_offset(destidx, MO_64));
    tcg_gen_st_i64(tmphi, cpu_env, fp_reg_hi_offset(destidx));

    tcg_temp_free_i64(tmplo);
    tcg_temp_free_i64(tmphi);
}

/*
 * Vector load/store helpers.
 *
 * The principal difference between this and a FP load is that we don't
 * zero extend as we are filling a partial chunk of the vector register.
 * These functions don't support 128 bit loads/stores, which would be
 * normal load/store operations.
 *
 * The _i32 versions are useful when operating on 32 bit quantities
 * (eg for floating point single or using Neon helper functions).
 */

/* Get value of an element within a vector register */
static void read_vec_element(DisasContext *s, TCGv_i64 tcg_dest, int srcidx,
                             int element, TCGMemOp memop)
{
    int vect_off = vec_reg_offset(srcidx, element, memop & MO_SIZE);
    switch (memop) {
    case MO_8:
        tcg_gen_ld8u_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_16:
        tcg_gen_ld16u_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_32:
        tcg_gen_ld32u_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_8|MO_SIGN:
        tcg_gen_ld8s_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_16|MO_SIGN:
        tcg_gen_ld16s_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_32|MO_SIGN:
        tcg_gen_ld32s_i64(tcg_dest, cpu_env, vect_off);
        break;
    case MO_64:
    case MO_64|MO_SIGN:
        tcg_gen_ld_i64(tcg_dest, cpu_env, vect_off);
        break;
    default:
        g_assert_not_reached();
    }
}

static void read_vec_element_i32(DisasContext *s, TCGv_i32 tcg_dest, int srcidx,
                                 int element, TCGMemOp memop)
{
    int vect_off = vec_reg_offset(srcidx, element, memop & MO_SIZE);
    switch (memop) {
    case MO_8:
        tcg_gen_ld8u_i32(tcg_dest, cpu_env, vect_off);
        break;
    case MO_16:
        tcg_gen_ld16u_i32(tcg_dest, cpu_env, vect_off);
        break;
    case MO_8|MO_SIGN:
        tcg_gen_ld8s_i32(tcg_dest, cpu_env, vect_off);
        break;
    case MO_16|MO_SIGN:
        tcg_gen_ld16s_i32(tcg_dest, cpu_env, vect_off);
        break;
    case MO_32:
    case MO_32|MO_SIGN:
        tcg_gen_ld_i32(tcg_dest, cpu_env, vect_off);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Set value of an element within a vector register */
static void write_vec_element(DisasContext *s, TCGv_i64 tcg_src, int destidx,
                              int element, TCGMemOp memop)
{
    int vect_off = vec_reg_offset(destidx, element, memop & MO_SIZE);
    switch (memop) {
    case MO_8:
        tcg_gen_st8_i64(tcg_src, cpu_env, vect_off);
        break;
    case MO_16:
        tcg_gen_st16_i64(tcg_src, cpu_env, vect_off);
        break;
    case MO_32:
        tcg_gen_st32_i64(tcg_src, cpu_env, vect_off);
        break;
    case MO_64:
        tcg_gen_st_i64(tcg_src, cpu_env, vect_off);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Clear the high 64 bits of a 128 bit vector (in general non-quad
 * vector ops all need to do this).
 */
static void clear_vec_high(DisasContext *s, int rd)
{
    TCGv_i64 tcg_zero = tcg_const_i64(0);

    write_vec_element(s, tcg_zero, rd, 1, MO_64);
    tcg_temp_free_i64(tcg_zero);
}

/* Store from vector register to memory */
static void do_vec_st(DisasContext *s, int srcidx, int element,
                      TCGv_i64 tcg_addr, int size)
{
    TCGMemOp memop = MO_TE + size;
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();

    read_vec_element(s, tcg_tmp, srcidx, element, size);
    tcg_gen_qemu_st_i64(tcg_tmp, tcg_addr, get_mem_index(s), memop);

    tcg_temp_free_i64(tcg_tmp);
}

/* Load from memory to vector register */
static void do_vec_ld(DisasContext *s, int destidx, int element,
                      TCGv_i64 tcg_addr, int size)
{
    TCGMemOp memop = MO_TE + size;
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();

    tcg_gen_qemu_ld_i64(tcg_tmp, tcg_addr, get_mem_index(s), memop);
    write_vec_element(s, tcg_tmp, destidx, element, size);

    tcg_temp_free_i64(tcg_tmp);
}

/*
 * This utility function is for doing register extension with an
 * optional shift. You will likely want to pass a temporary for the
 * destination register. See DecodeRegExtend() in the ARM ARM.
 */
static void ext_and_shift_reg(TCGv_i64 tcg_out, TCGv_i64 tcg_in,
                              int option, unsigned int shift)
{
    int extsize = extract32(option, 0, 2);
    bool is_signed = extract32(option, 2, 1);

    if (is_signed) {
        switch (extsize) {
        case 0:
            tcg_gen_ext8s_i64(tcg_out, tcg_in);
            break;
        case 1:
            tcg_gen_ext16s_i64(tcg_out, tcg_in);
            break;
        case 2:
            tcg_gen_ext32s_i64(tcg_out, tcg_in);
            break;
        case 3:
            tcg_gen_mov_i64(tcg_out, tcg_in);
            break;
        }
    } else {
        switch (extsize) {
        case 0:
            tcg_gen_ext8u_i64(tcg_out, tcg_in);
            break;
        case 1:
            tcg_gen_ext16u_i64(tcg_out, tcg_in);
            break;
        case 2:
            tcg_gen_ext32u_i64(tcg_out, tcg_in);
            break;
        case 3:
            tcg_gen_mov_i64(tcg_out, tcg_in);
            break;
        }
    }

    if (shift) {
        tcg_gen_shli_i64(tcg_out, tcg_out, shift);
    }
}

static inline void gen_check_sp_alignment(DisasContext *s)
{
    /* The AArch64 architecture mandates that (if enabled via PSTATE
     * or SCTLR bits) there is a check that SP is 16-aligned on every
     * SP-relative load or store (with an exception generated if it is not).
     * In line with general QEMU practice regarding misaligned accesses,
     * we omit these checks for the sake of guest program performance.
     * This function is provided as a hook so we can more easily add these
     * checks in future (possibly as a "favour catching guest program bugs
     * over speed" user selectable option).
     */
}

/*
 * This provides a simple table based table lookup decoder. It is
 * intended to be used when the relevant bits for decode are too
 * awkwardly placed and switch/if based logic would be confusing and
 * deeply nested. Since it's a linear search through the table, tables
 * should be kept small.
 *
 * It returns the first handler where insn & mask == pattern, or
 * NULL if there is no match.
 * The table is terminated by an empty mask (i.e. 0)
 */
static inline AArch64DecodeFn *lookup_disas_fn(const AArch64DecodeTable *table,
                                               uint32_t insn)
{
    const AArch64DecodeTable *tptr = table;

    while (tptr->mask) {
        if ((insn & tptr->mask) == tptr->pattern) {
            return tptr->disas_fn;
        }
        tptr++;
    }
    return NULL;
}

/*
 * the instruction disassembly implemented here matches
 * the instruction encoding classifications in chapter 3 (C3)
 * of the ARM Architecture Reference Manual (DDI0487A_a)
 */

/* C3.2.7 Unconditional branch (immediate)
 *   31  30       26 25                                  0
 * +----+-----------+-------------------------------------+
 * | op | 0 0 1 0 1 |                 imm26               |
 * +----+-----------+-------------------------------------+
 */
static void disas_uncond_b_imm(DisasContext *s, uint32_t insn)
{
    uint64_t addr = s->pc + sextract32(insn, 0, 26) * 4 - 4;

    if (insn & (1 << 31)) {
        /* C5.6.26 BL Branch with link */
        tcg_gen_movi_i64(cpu_reg(s, 30), s->pc);
    }

    /* C5.6.20 B Branch / C5.6.26 BL Branch with link */
    gen_goto_tb(s, 0, addr);
}

/* C3.2.1 Compare & branch (immediate)
 *   31  30         25  24  23                  5 4      0
 * +----+-------------+----+---------------------+--------+
 * | sf | 0 1 1 0 1 0 | op |         imm19       |   Rt   |
 * +----+-------------+----+---------------------+--------+
 */
static void disas_comp_b_imm(DisasContext *s, uint32_t insn)
{
    unsigned int sf, op, rt;
    uint64_t addr;
    int label_match;
    TCGv_i64 tcg_cmp;

    sf = extract32(insn, 31, 1);
    op = extract32(insn, 24, 1); /* 0: CBZ; 1: CBNZ */
    rt = extract32(insn, 0, 5);
    addr = s->pc + sextract32(insn, 5, 19) * 4 - 4;

    tcg_cmp = read_cpu_reg(s, rt, sf);
    label_match = gen_new_label();

    tcg_gen_brcondi_i64(op ? TCG_COND_NE : TCG_COND_EQ,
                        tcg_cmp, 0, label_match);

    gen_goto_tb(s, 0, s->pc);
    gen_set_label(label_match);
    gen_goto_tb(s, 1, addr);
}

/* C3.2.5 Test & branch (immediate)
 *   31  30         25  24  23   19 18          5 4    0
 * +----+-------------+----+-------+-------------+------+
 * | b5 | 0 1 1 0 1 1 | op |  b40  |    imm14    |  Rt  |
 * +----+-------------+----+-------+-------------+------+
 */
static void disas_test_b_imm(DisasContext *s, uint32_t insn)
{
    unsigned int bit_pos, op, rt;
    uint64_t addr;
    int label_match;
    TCGv_i64 tcg_cmp;

    bit_pos = (extract32(insn, 31, 1) << 5) | extract32(insn, 19, 5);
    op = extract32(insn, 24, 1); /* 0: TBZ; 1: TBNZ */
    addr = s->pc + sextract32(insn, 5, 14) * 4 - 4;
    rt = extract32(insn, 0, 5);

    tcg_cmp = tcg_temp_new_i64();
    tcg_gen_andi_i64(tcg_cmp, cpu_reg(s, rt), (1ULL << bit_pos));
    label_match = gen_new_label();
    tcg_gen_brcondi_i64(op ? TCG_COND_NE : TCG_COND_EQ,
                        tcg_cmp, 0, label_match);
    tcg_temp_free_i64(tcg_cmp);
    gen_goto_tb(s, 0, s->pc);
    gen_set_label(label_match);
    gen_goto_tb(s, 1, addr);
}

/* C3.2.2 / C5.6.19 Conditional branch (immediate)
 *  31           25  24  23                  5   4  3    0
 * +---------------+----+---------------------+----+------+
 * | 0 1 0 1 0 1 0 | o1 |         imm19       | o0 | cond |
 * +---------------+----+---------------------+----+------+
 */
static void disas_cond_b_imm(DisasContext *s, uint32_t insn)
{
    unsigned int cond;
    uint64_t addr;

    if ((insn & (1 << 4)) || (insn & (1 << 24))) {
        unallocated_encoding(s);
        return;
    }
    addr = s->pc + sextract32(insn, 5, 19) * 4 - 4;
    cond = extract32(insn, 0, 4);

    if (cond < 0x0e) {
        /* genuinely conditional branches */
        int label_match = gen_new_label();
        arm_gen_test_cc(cond, label_match);
        gen_goto_tb(s, 0, s->pc);
        gen_set_label(label_match);
        gen_goto_tb(s, 1, addr);
    } else {
        /* 0xe and 0xf are both "always" conditions */
        gen_goto_tb(s, 0, addr);
    }
}

/* C5.6.68 HINT */
static void handle_hint(DisasContext *s, uint32_t insn,
                        unsigned int op1, unsigned int op2, unsigned int crm)
{
    unsigned int selector = crm << 3 | op2;

    if (op1 != 3) {
        unallocated_encoding(s);
        return;
    }

    switch (selector) {
    case 0: /* NOP */
        return;
    case 1: /* YIELD */
    case 2: /* WFE */
    case 3: /* WFI */
    case 4: /* SEV */
    case 5: /* SEVL */
        /* we treat all as NOP at least for now */
        return;
    default:
        /* default specified as NOP equivalent */
        return;
    }
}

static void gen_clrex(DisasContext *s, uint32_t insn)
{
    tcg_gen_movi_i64(cpu_exclusive_addr, -1);
}

/* CLREX, DSB, DMB, ISB */
static void handle_sync(DisasContext *s, uint32_t insn,
                        unsigned int op1, unsigned int op2, unsigned int crm)
{
    if (op1 != 3) {
        unallocated_encoding(s);
        return;
    }

    switch (op2) {
    case 2: /* CLREX */
        gen_clrex(s, insn);
        return;
    case 4: /* DSB */
    case 5: /* DMB */
    case 6: /* ISB */
        /* We don't emulate caches so barriers are no-ops */
        return;
    default:
        unallocated_encoding(s);
        return;
    }
}

/* C5.6.130 MSR (immediate) - move immediate to processor state field */
static void handle_msr_i(DisasContext *s, uint32_t insn,
                         unsigned int op1, unsigned int op2, unsigned int crm)
{
    unsupported_encoding(s, insn);
}

static void gen_get_nzcv(TCGv_i64 tcg_rt)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    TCGv_i32 nzcv = tcg_temp_new_i32();

    /* build bit 31, N */
    tcg_gen_andi_i32(nzcv, cpu_NF, (1 << 31));
    /* build bit 30, Z */
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, cpu_ZF, 0);
    tcg_gen_deposit_i32(nzcv, nzcv, tmp, 30, 1);
    /* build bit 29, C */
    tcg_gen_deposit_i32(nzcv, nzcv, cpu_CF, 29, 1);
    /* build bit 28, V */
    tcg_gen_shri_i32(tmp, cpu_VF, 31);
    tcg_gen_deposit_i32(nzcv, nzcv, tmp, 28, 1);
    /* generate result */
    tcg_gen_extu_i32_i64(tcg_rt, nzcv);

    tcg_temp_free_i32(nzcv);
    tcg_temp_free_i32(tmp);
}

static void gen_set_nzcv(TCGv_i64 tcg_rt)

{
    TCGv_i32 nzcv = tcg_temp_new_i32();

    /* take NZCV from R[t] */
    tcg_gen_trunc_i64_i32(nzcv, tcg_rt);

    /* bit 31, N */
    tcg_gen_andi_i32(cpu_NF, nzcv, (1 << 31));
    /* bit 30, Z */
    tcg_gen_andi_i32(cpu_ZF, nzcv, (1 << 30));
    tcg_gen_setcondi_i32(TCG_COND_EQ, cpu_ZF, cpu_ZF, 0);
    /* bit 29, C */
    tcg_gen_andi_i32(cpu_CF, nzcv, (1 << 29));
    tcg_gen_shri_i32(cpu_CF, cpu_CF, 29);
    /* bit 28, V */
    tcg_gen_andi_i32(cpu_VF, nzcv, (1 << 28));
    tcg_gen_shli_i32(cpu_VF, cpu_VF, 3);
    tcg_temp_free_i32(nzcv);
}

/* C5.6.129 MRS - move from system register
 * C5.6.131 MSR (register) - move to system register
 * C5.6.204 SYS
 * C5.6.205 SYSL
 * These are all essentially the same insn in 'read' and 'write'
 * versions, with varying op0 fields.
 */
static void handle_sys(DisasContext *s, uint32_t insn, bool isread,
                       unsigned int op0, unsigned int op1, unsigned int op2,
                       unsigned int crn, unsigned int crm, unsigned int rt)
{
    const ARMCPRegInfo *ri;
    TCGv_i64 tcg_rt;

    ri = get_arm_cp_reginfo(s->cp_regs,
                            ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP,
                                               crn, crm, op0, op1, op2));

    if (!ri) {
        /* Unknown register */
        unallocated_encoding(s);
        return;
    }

    /* Check access permissions */
    if (!cp_access_ok(s->current_pl, ri, isread)) {
        unallocated_encoding(s);
        return;
    }

    /* Handle special cases first */
    switch (ri->type & ~(ARM_CP_FLAG_MASK & ~ARM_CP_SPECIAL)) {
    case ARM_CP_NOP:
        return;
    case ARM_CP_NZCV:
        tcg_rt = cpu_reg(s, rt);
        if (isread) {
            gen_get_nzcv(tcg_rt);
        } else {
            gen_set_nzcv(tcg_rt);
        }
        return;
    default:
        break;
    }

    if (use_icount && (ri->type & ARM_CP_IO)) {
        gen_io_start();
    }

    tcg_rt = cpu_reg(s, rt);

    if (isread) {
        if (ri->type & ARM_CP_CONST) {
            tcg_gen_movi_i64(tcg_rt, ri->resetvalue);
        } else if (ri->readfn) {
            TCGv_ptr tmpptr;
            gen_a64_set_pc_im(s->pc - 4);
            tmpptr = tcg_const_ptr(ri);
            gen_helper_get_cp_reg64(tcg_rt, cpu_env, tmpptr);
            tcg_temp_free_ptr(tmpptr);
        } else {
            tcg_gen_ld_i64(tcg_rt, cpu_env, ri->fieldoffset);
        }
    } else {
        if (ri->type & ARM_CP_CONST) {
            /* If not forbidden by access permissions, treat as WI */
            return;
        } else if (ri->writefn) {
            TCGv_ptr tmpptr;
            gen_a64_set_pc_im(s->pc - 4);
            tmpptr = tcg_const_ptr(ri);
            gen_helper_set_cp_reg64(cpu_env, tmpptr, tcg_rt);
            tcg_temp_free_ptr(tmpptr);
        } else {
            tcg_gen_st_i64(tcg_rt, cpu_env, ri->fieldoffset);
        }
    }

    if (use_icount && (ri->type & ARM_CP_IO)) {
        /* I/O operations must end the TB here (whether read or write) */
        gen_io_end();
        s->is_jmp = DISAS_UPDATE;
    } else if (!isread && !(ri->type & ARM_CP_SUPPRESS_TB_END)) {
        /* We default to ending the TB on a coprocessor register write,
         * but allow this to be suppressed by the register definition
         * (usually only necessary to work around guest bugs).
         */
        s->is_jmp = DISAS_UPDATE;
    }
}

/* C3.2.4 System
 *  31                 22 21  20 19 18 16 15   12 11    8 7   5 4    0
 * +---------------------+---+-----+-----+-------+-------+-----+------+
 * | 1 1 0 1 0 1 0 1 0 0 | L | op0 | op1 |  CRn  |  CRm  | op2 |  Rt  |
 * +---------------------+---+-----+-----+-------+-------+-----+------+
 */
static void disas_system(DisasContext *s, uint32_t insn)
{
    unsigned int l, op0, op1, crn, crm, op2, rt;
    l = extract32(insn, 21, 1);
    op0 = extract32(insn, 19, 2);
    op1 = extract32(insn, 16, 3);
    crn = extract32(insn, 12, 4);
    crm = extract32(insn, 8, 4);
    op2 = extract32(insn, 5, 3);
    rt = extract32(insn, 0, 5);

    if (op0 == 0) {
        if (l || rt != 31) {
            unallocated_encoding(s);
            return;
        }
        switch (crn) {
        case 2: /* C5.6.68 HINT */
            handle_hint(s, insn, op1, op2, crm);
            break;
        case 3: /* CLREX, DSB, DMB, ISB */
            handle_sync(s, insn, op1, op2, crm);
            break;
        case 4: /* C5.6.130 MSR (immediate) */
            handle_msr_i(s, insn, op1, op2, crm);
            break;
        default:
            unallocated_encoding(s);
            break;
        }
        return;
    }
    handle_sys(s, insn, l, op0, op1, op2, crn, crm, rt);
}

/* C3.2.3 Exception generation
 *
 *  31             24 23 21 20                     5 4   2 1  0
 * +-----------------+-----+------------------------+-----+----+
 * | 1 1 0 1 0 1 0 0 | opc |          imm16         | op2 | LL |
 * +-----------------------+------------------------+----------+
 */
static void disas_exc(DisasContext *s, uint32_t insn)
{
    int opc = extract32(insn, 21, 3);
    int op2_ll = extract32(insn, 0, 5);

    switch (opc) {
    case 0:
        /* SVC, HVC, SMC; since we don't support the Virtualization
         * or TrustZone extensions these all UNDEF except SVC.
         */
        if (op2_ll != 1) {
            unallocated_encoding(s);
            break;
        }
        gen_exception_insn(s, 0, EXCP_SWI);
        break;
    case 1:
        if (op2_ll != 0) {
            unallocated_encoding(s);
            break;
        }
        /* BRK */
        gen_exception_insn(s, 0, EXCP_BKPT);
        break;
    case 2:
        if (op2_ll != 0) {
            unallocated_encoding(s);
            break;
        }
        /* HLT */
        unsupported_encoding(s, insn);
        break;
    case 5:
        if (op2_ll < 1 || op2_ll > 3) {
            unallocated_encoding(s);
            break;
        }
        /* DCPS1, DCPS2, DCPS3 */
        unsupported_encoding(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.2.7 Unconditional branch (register)
 *  31           25 24   21 20   16 15   10 9    5 4     0
 * +---------------+-------+-------+-------+------+-------+
 * | 1 1 0 1 0 1 1 |  opc  |  op2  |  op3  |  Rn  |  op4  |
 * +---------------+-------+-------+-------+------+-------+
 */
static void disas_uncond_b_reg(DisasContext *s, uint32_t insn)
{
    unsigned int opc, op2, op3, rn, op4;

    opc = extract32(insn, 21, 4);
    op2 = extract32(insn, 16, 5);
    op3 = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    op4 = extract32(insn, 0, 5);

    if (op4 != 0x0 || op3 != 0x0 || op2 != 0x1f) {
        unallocated_encoding(s);
        return;
    }

    switch (opc) {
    case 0: /* BR */
    case 2: /* RET */
        break;
    case 1: /* BLR */
        tcg_gen_movi_i64(cpu_reg(s, 30), s->pc);
        break;
    case 4: /* ERET */
    case 5: /* DRPS */
        if (rn != 0x1f) {
            unallocated_encoding(s);
        } else {
            unsupported_encoding(s, insn);
        }
        return;
    default:
        unallocated_encoding(s);
        return;
    }

    tcg_gen_mov_i64(cpu_pc, cpu_reg(s, rn));
    s->is_jmp = DISAS_JUMP;
}

/* C3.2 Branches, exception generating and system instructions */
static void disas_b_exc_sys(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 25, 7)) {
    case 0x0a: case 0x0b:
    case 0x4a: case 0x4b: /* Unconditional branch (immediate) */
        disas_uncond_b_imm(s, insn);
        break;
    case 0x1a: case 0x5a: /* Compare & branch (immediate) */
        disas_comp_b_imm(s, insn);
        break;
    case 0x1b: case 0x5b: /* Test & branch (immediate) */
        disas_test_b_imm(s, insn);
        break;
    case 0x2a: /* Conditional branch (immediate) */
        disas_cond_b_imm(s, insn);
        break;
    case 0x6a: /* Exception generation / System */
        if (insn & (1 << 24)) {
            disas_system(s, insn);
        } else {
            disas_exc(s, insn);
        }
        break;
    case 0x6b: /* Unconditional branch (register) */
        disas_uncond_b_reg(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/*
 * Load/Store exclusive instructions are implemented by remembering
 * the value/address loaded, and seeing if these are the same
 * when the store is performed. This is not actually the architecturally
 * mandated semantics, but it works for typical guest code sequences
 * and avoids having to monitor regular stores.
 *
 * In system emulation mode only one CPU will be running at once, so
 * this sequence is effectively atomic.  In user emulation mode we
 * throw an exception and handle the atomic operation elsewhere.
 */
static void gen_load_exclusive(DisasContext *s, int rt, int rt2,
                               TCGv_i64 addr, int size, bool is_pair)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGMemOp memop = MO_TE + size;

    g_assert(size <= 3);
    tcg_gen_qemu_ld_i64(tmp, addr, get_mem_index(s), memop);

    if (is_pair) {
        TCGv_i64 addr2 = tcg_temp_new_i64();
        TCGv_i64 hitmp = tcg_temp_new_i64();

        g_assert(size >= 2);
        tcg_gen_addi_i64(addr2, addr, 1 << size);
        tcg_gen_qemu_ld_i64(hitmp, addr2, get_mem_index(s), memop);
        tcg_temp_free_i64(addr2);
        tcg_gen_mov_i64(cpu_exclusive_high, hitmp);
        tcg_gen_mov_i64(cpu_reg(s, rt2), hitmp);
        tcg_temp_free_i64(hitmp);
    }

    tcg_gen_mov_i64(cpu_exclusive_val, tmp);
    tcg_gen_mov_i64(cpu_reg(s, rt), tmp);

    tcg_temp_free_i64(tmp);
    tcg_gen_mov_i64(cpu_exclusive_addr, addr);
}

#ifdef CONFIG_USER_ONLY
static void gen_store_exclusive(DisasContext *s, int rd, int rt, int rt2,
                                TCGv_i64 addr, int size, int is_pair)
{
    tcg_gen_mov_i64(cpu_exclusive_test, addr);
    tcg_gen_movi_i32(cpu_exclusive_info,
                     size | is_pair << 2 | (rd << 4) | (rt << 9) | (rt2 << 14));
    gen_exception_insn(s, 4, EXCP_STREX);
}
#else
static void gen_store_exclusive(DisasContext *s, int rd, int rt, int rt2,
                                TCGv_i64 addr, int size, int is_pair)
{
    qemu_log_mask(LOG_UNIMP,
                  "%s:%d: system mode store_exclusive unsupported "
                  "at pc=%016" PRIx64 "\n",
                  __FILE__, __LINE__, s->pc - 4);
}
#endif

/* C3.3.6 Load/store exclusive
 *
 *  31 30 29         24  23  22   21  20  16  15  14   10 9    5 4    0
 * +-----+-------------+----+---+----+------+----+-------+------+------+
 * | sz  | 0 0 1 0 0 0 | o2 | L | o1 |  Rs  | o0 |  Rt2  |  Rn  | Rt   |
 * +-----+-------------+----+---+----+------+----+-------+------+------+
 *
 *  sz: 00 -> 8 bit, 01 -> 16 bit, 10 -> 32 bit, 11 -> 64 bit
 *   L: 0 -> store, 1 -> load
 *  o2: 0 -> exclusive, 1 -> not
 *  o1: 0 -> single register, 1 -> register pair
 *  o0: 1 -> load-acquire/store-release, 0 -> not
 *
 *  o0 == 0 AND o2 == 1 is un-allocated
 *  o1 == 1 is un-allocated except for 32 and 64 bit sizes
 */
static void disas_ldst_excl(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rt2 = extract32(insn, 10, 5);
    int is_lasr = extract32(insn, 15, 1);
    int rs = extract32(insn, 16, 5);
    int is_pair = extract32(insn, 21, 1);
    int is_store = !extract32(insn, 22, 1);
    int is_excl = !extract32(insn, 23, 1);
    int size = extract32(insn, 30, 2);
    TCGv_i64 tcg_addr;

    if ((!is_excl && !is_lasr) ||
        (is_pair && size < 2)) {
        unallocated_encoding(s);
        return;
    }

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }
    tcg_addr = read_cpu_reg_sp(s, rn, 1);

    /* Note that since TCG is single threaded load-acquire/store-release
     * semantics require no extra if (is_lasr) { ... } handling.
     */

    if (is_excl) {
        if (!is_store) {
            gen_load_exclusive(s, rt, rt2, tcg_addr, size, is_pair);
        } else {
            gen_store_exclusive(s, rs, rt, rt2, tcg_addr, size, is_pair);
        }
    } else {
        TCGv_i64 tcg_rt = cpu_reg(s, rt);
        if (is_store) {
            do_gpr_st(s, tcg_rt, tcg_addr, size);
        } else {
            do_gpr_ld(s, tcg_rt, tcg_addr, size, false, false);
        }
        if (is_pair) {
            TCGv_i64 tcg_rt2 = cpu_reg(s, rt);
            tcg_gen_addi_i64(tcg_addr, tcg_addr, 1 << size);
            if (is_store) {
                do_gpr_st(s, tcg_rt2, tcg_addr, size);
            } else {
                do_gpr_ld(s, tcg_rt2, tcg_addr, size, false, false);
            }
        }
    }
}

/*
 * C3.3.5 Load register (literal)
 *
 *  31 30 29   27  26 25 24 23                5 4     0
 * +-----+-------+---+-----+-------------------+-------+
 * | opc | 0 1 1 | V | 0 0 |     imm19         |  Rt   |
 * +-----+-------+---+-----+-------------------+-------+
 *
 * V: 1 -> vector (simd/fp)
 * opc (non-vector): 00 -> 32 bit, 01 -> 64 bit,
 *                   10-> 32 bit signed, 11 -> prefetch
 * opc (vector): 00 -> 32 bit, 01 -> 64 bit, 10 -> 128 bit (11 unallocated)
 */
static void disas_ld_lit(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int64_t imm = sextract32(insn, 5, 19) << 2;
    bool is_vector = extract32(insn, 26, 1);
    int opc = extract32(insn, 30, 2);
    bool is_signed = false;
    int size = 2;
    TCGv_i64 tcg_rt, tcg_addr;

    if (is_vector) {
        if (opc == 3) {
            unallocated_encoding(s);
            return;
        }
        size = 2 + opc;
    } else {
        if (opc == 3) {
            /* PRFM (literal) : prefetch */
            return;
        }
        size = 2 + extract32(opc, 0, 1);
        is_signed = extract32(opc, 1, 1);
    }

    tcg_rt = cpu_reg(s, rt);

    tcg_addr = tcg_const_i64((s->pc - 4) + imm);
    if (is_vector) {
        do_fp_ld(s, rt, tcg_addr, size);
    } else {
        do_gpr_ld(s, tcg_rt, tcg_addr, size, is_signed, false);
    }
    tcg_temp_free_i64(tcg_addr);
}

/*
 * C5.6.80 LDNP (Load Pair - non-temporal hint)
 * C5.6.81 LDP (Load Pair - non vector)
 * C5.6.82 LDPSW (Load Pair Signed Word - non vector)
 * C5.6.176 STNP (Store Pair - non-temporal hint)
 * C5.6.177 STP (Store Pair - non vector)
 * C6.3.165 LDNP (Load Pair of SIMD&FP - non-temporal hint)
 * C6.3.165 LDP (Load Pair of SIMD&FP)
 * C6.3.284 STNP (Store Pair of SIMD&FP - non-temporal hint)
 * C6.3.284 STP (Store Pair of SIMD&FP)
 *
 *  31 30 29   27  26  25 24   23  22 21   15 14   10 9    5 4    0
 * +-----+-------+---+---+-------+---+-----------------------------+
 * | opc | 1 0 1 | V | 0 | index | L |  imm7 |  Rt2  |  Rn  | Rt   |
 * +-----+-------+---+---+-------+---+-------+-------+------+------+
 *
 * opc: LDP/STP/LDNP/STNP        00 -> 32 bit, 10 -> 64 bit
 *      LDPSW                    01
 *      LDP/STP/LDNP/STNP (SIMD) 00 -> 32 bit, 01 -> 64 bit, 10 -> 128 bit
 *   V: 0 -> GPR, 1 -> Vector
 * idx: 00 -> signed offset with non-temporal hint, 01 -> post-index,
 *      10 -> signed offset, 11 -> pre-index
 *   L: 0 -> Store 1 -> Load
 *
 * Rt, Rt2 = GPR or SIMD registers to be stored
 * Rn = general purpose register containing address
 * imm7 = signed offset (multiple of 4 or 8 depending on size)
 */
static void disas_ldst_pair(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rt2 = extract32(insn, 10, 5);
    int64_t offset = sextract32(insn, 15, 7);
    int index = extract32(insn, 23, 2);
    bool is_vector = extract32(insn, 26, 1);
    bool is_load = extract32(insn, 22, 1);
    int opc = extract32(insn, 30, 2);

    bool is_signed = false;
    bool postindex = false;
    bool wback = false;

    TCGv_i64 tcg_addr; /* calculated address */
    int size;

    if (opc == 3) {
        unallocated_encoding(s);
        return;
    }

    if (is_vector) {
        size = 2 + opc;
    } else {
        size = 2 + extract32(opc, 1, 1);
        is_signed = extract32(opc, 0, 1);
        if (!is_load && is_signed) {
            unallocated_encoding(s);
            return;
        }
    }

    switch (index) {
    case 1: /* post-index */
        postindex = true;
        wback = true;
        break;
    case 0:
        /* signed offset with "non-temporal" hint. Since we don't emulate
         * caches we don't care about hints to the cache system about
         * data access patterns, and handle this identically to plain
         * signed offset.
         */
        if (is_signed) {
            /* There is no non-temporal-hint version of LDPSW */
            unallocated_encoding(s);
            return;
        }
        postindex = false;
        break;
    case 2: /* signed offset, rn not updated */
        postindex = false;
        break;
    case 3: /* pre-index */
        postindex = false;
        wback = true;
        break;
    }

    offset <<= size;

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }

    tcg_addr = read_cpu_reg_sp(s, rn, 1);

    if (!postindex) {
        tcg_gen_addi_i64(tcg_addr, tcg_addr, offset);
    }

    if (is_vector) {
        if (is_load) {
            do_fp_ld(s, rt, tcg_addr, size);
        } else {
            do_fp_st(s, rt, tcg_addr, size);
        }
    } else {
        TCGv_i64 tcg_rt = cpu_reg(s, rt);
        if (is_load) {
            do_gpr_ld(s, tcg_rt, tcg_addr, size, is_signed, false);
        } else {
            do_gpr_st(s, tcg_rt, tcg_addr, size);
        }
    }
    tcg_gen_addi_i64(tcg_addr, tcg_addr, 1 << size);
    if (is_vector) {
        if (is_load) {
            do_fp_ld(s, rt2, tcg_addr, size);
        } else {
            do_fp_st(s, rt2, tcg_addr, size);
        }
    } else {
        TCGv_i64 tcg_rt2 = cpu_reg(s, rt2);
        if (is_load) {
            do_gpr_ld(s, tcg_rt2, tcg_addr, size, is_signed, false);
        } else {
            do_gpr_st(s, tcg_rt2, tcg_addr, size);
        }
    }

    if (wback) {
        if (postindex) {
            tcg_gen_addi_i64(tcg_addr, tcg_addr, offset - (1 << size));
        } else {
            tcg_gen_subi_i64(tcg_addr, tcg_addr, 1 << size);
        }
        tcg_gen_mov_i64(cpu_reg_sp(s, rn), tcg_addr);
    }
}

/*
 * C3.3.8 Load/store (immediate post-indexed)
 * C3.3.9 Load/store (immediate pre-indexed)
 * C3.3.12 Load/store (unscaled immediate)
 *
 * 31 30 29   27  26 25 24 23 22 21  20    12 11 10 9    5 4    0
 * +----+-------+---+-----+-----+---+--------+-----+------+------+
 * |size| 1 1 1 | V | 0 0 | opc | 0 |  imm9  | idx |  Rn  |  Rt  |
 * +----+-------+---+-----+-----+---+--------+-----+------+------+
 *
 * idx = 01 -> post-indexed, 11 pre-indexed, 00 unscaled imm. (no writeback)
 * V = 0 -> non-vector
 * size: 00 -> 8 bit, 01 -> 16 bit, 10 -> 32 bit, 11 -> 64bit
 * opc: 00 -> store, 01 -> loadu, 10 -> loads 64, 11 -> loads 32
 */
static void disas_ldst_reg_imm9(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm9 = sextract32(insn, 12, 9);
    int opc = extract32(insn, 22, 2);
    int size = extract32(insn, 30, 2);
    int idx = extract32(insn, 10, 2);
    bool is_signed = false;
    bool is_store = false;
    bool is_extended = false;
    bool is_vector = extract32(insn, 26, 1);
    bool post_index;
    bool writeback;

    TCGv_i64 tcg_addr;

    if (is_vector) {
        size |= (opc & 2) << 1;
        if (size > 4) {
            unallocated_encoding(s);
            return;
        }
        is_store = ((opc & 1) == 0);
    } else {
        if (size == 3 && opc == 2) {
            /* PRFM - prefetch */
            return;
        }
        if (opc == 3 && size > 1) {
            unallocated_encoding(s);
            return;
        }
        is_store = (opc == 0);
        is_signed = opc & (1<<1);
        is_extended = (size < 3) && (opc & 1);
    }

    switch (idx) {
    case 0:
        post_index = false;
        writeback = false;
        break;
    case 1:
        post_index = true;
        writeback = true;
        break;
    case 3:
        post_index = false;
        writeback = true;
        break;
    case 2:
        g_assert(false);
        break;
    }

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }
    tcg_addr = read_cpu_reg_sp(s, rn, 1);

    if (!post_index) {
        tcg_gen_addi_i64(tcg_addr, tcg_addr, imm9);
    }

    if (is_vector) {
        if (is_store) {
            do_fp_st(s, rt, tcg_addr, size);
        } else {
            do_fp_ld(s, rt, tcg_addr, size);
        }
    } else {
        TCGv_i64 tcg_rt = cpu_reg(s, rt);
        if (is_store) {
            do_gpr_st(s, tcg_rt, tcg_addr, size);
        } else {
            do_gpr_ld(s, tcg_rt, tcg_addr, size, is_signed, is_extended);
        }
    }

    if (writeback) {
        TCGv_i64 tcg_rn = cpu_reg_sp(s, rn);
        if (post_index) {
            tcg_gen_addi_i64(tcg_addr, tcg_addr, imm9);
        }
        tcg_gen_mov_i64(tcg_rn, tcg_addr);
    }
}

/*
 * C3.3.10 Load/store (register offset)
 *
 * 31 30 29   27  26 25 24 23 22 21  20  16 15 13 12 11 10 9  5 4  0
 * +----+-------+---+-----+-----+---+------+-----+--+-----+----+----+
 * |size| 1 1 1 | V | 0 0 | opc | 1 |  Rm  | opt | S| 1 0 | Rn | Rt |
 * +----+-------+---+-----+-----+---+------+-----+--+-----+----+----+
 *
 * For non-vector:
 *   size: 00-> byte, 01 -> 16 bit, 10 -> 32bit, 11 -> 64bit
 *   opc: 00 -> store, 01 -> loadu, 10 -> loads 64, 11 -> loads 32
 * For vector:
 *   size is opc<1>:size<1:0> so 100 -> 128 bit; 110 and 111 unallocated
 *   opc<0>: 0 -> store, 1 -> load
 * V: 1 -> vector/simd
 * opt: extend encoding (see DecodeRegExtend)
 * S: if S=1 then scale (essentially index by sizeof(size))
 * Rt: register to transfer into/out of
 * Rn: address register or SP for base
 * Rm: offset register or ZR for offset
 */
static void disas_ldst_reg_roffset(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int shift = extract32(insn, 12, 1);
    int rm = extract32(insn, 16, 5);
    int opc = extract32(insn, 22, 2);
    int opt = extract32(insn, 13, 3);
    int size = extract32(insn, 30, 2);
    bool is_signed = false;
    bool is_store = false;
    bool is_extended = false;
    bool is_vector = extract32(insn, 26, 1);

    TCGv_i64 tcg_rm;
    TCGv_i64 tcg_addr;

    if (extract32(opt, 1, 1) == 0) {
        unallocated_encoding(s);
        return;
    }

    if (is_vector) {
        size |= (opc & 2) << 1;
        if (size > 4) {
            unallocated_encoding(s);
            return;
        }
        is_store = !extract32(opc, 0, 1);
    } else {
        if (size == 3 && opc == 2) {
            /* PRFM - prefetch */
            return;
        }
        if (opc == 3 && size > 1) {
            unallocated_encoding(s);
            return;
        }
        is_store = (opc == 0);
        is_signed = extract32(opc, 1, 1);
        is_extended = (size < 3) && extract32(opc, 0, 1);
    }

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }
    tcg_addr = read_cpu_reg_sp(s, rn, 1);

    tcg_rm = read_cpu_reg(s, rm, 1);
    ext_and_shift_reg(tcg_rm, tcg_rm, opt, shift ? size : 0);

    tcg_gen_add_i64(tcg_addr, tcg_addr, tcg_rm);

    if (is_vector) {
        if (is_store) {
            do_fp_st(s, rt, tcg_addr, size);
        } else {
            do_fp_ld(s, rt, tcg_addr, size);
        }
    } else {
        TCGv_i64 tcg_rt = cpu_reg(s, rt);
        if (is_store) {
            do_gpr_st(s, tcg_rt, tcg_addr, size);
        } else {
            do_gpr_ld(s, tcg_rt, tcg_addr, size, is_signed, is_extended);
        }
    }
}

/*
 * C3.3.13 Load/store (unsigned immediate)
 *
 * 31 30 29   27  26 25 24 23 22 21        10 9     5
 * +----+-------+---+-----+-----+------------+-------+------+
 * |size| 1 1 1 | V | 0 1 | opc |   imm12    |  Rn   |  Rt  |
 * +----+-------+---+-----+-----+------------+-------+------+
 *
 * For non-vector:
 *   size: 00-> byte, 01 -> 16 bit, 10 -> 32bit, 11 -> 64bit
 *   opc: 00 -> store, 01 -> loadu, 10 -> loads 64, 11 -> loads 32
 * For vector:
 *   size is opc<1>:size<1:0> so 100 -> 128 bit; 110 and 111 unallocated
 *   opc<0>: 0 -> store, 1 -> load
 * Rn: base address register (inc SP)
 * Rt: target register
 */
static void disas_ldst_reg_unsigned_imm(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    unsigned int imm12 = extract32(insn, 10, 12);
    bool is_vector = extract32(insn, 26, 1);
    int size = extract32(insn, 30, 2);
    int opc = extract32(insn, 22, 2);
    unsigned int offset;

    TCGv_i64 tcg_addr;

    bool is_store;
    bool is_signed = false;
    bool is_extended = false;

    if (is_vector) {
        size |= (opc & 2) << 1;
        if (size > 4) {
            unallocated_encoding(s);
            return;
        }
        is_store = !extract32(opc, 0, 1);
    } else {
        if (size == 3 && opc == 2) {
            /* PRFM - prefetch */
            return;
        }
        if (opc == 3 && size > 1) {
            unallocated_encoding(s);
            return;
        }
        is_store = (opc == 0);
        is_signed = extract32(opc, 1, 1);
        is_extended = (size < 3) && extract32(opc, 0, 1);
    }

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }
    tcg_addr = read_cpu_reg_sp(s, rn, 1);
    offset = imm12 << size;
    tcg_gen_addi_i64(tcg_addr, tcg_addr, offset);

    if (is_vector) {
        if (is_store) {
            do_fp_st(s, rt, tcg_addr, size);
        } else {
            do_fp_ld(s, rt, tcg_addr, size);
        }
    } else {
        TCGv_i64 tcg_rt = cpu_reg(s, rt);
        if (is_store) {
            do_gpr_st(s, tcg_rt, tcg_addr, size);
        } else {
            do_gpr_ld(s, tcg_rt, tcg_addr, size, is_signed, is_extended);
        }
    }
}

/* Load/store register (immediate forms) */
static void disas_ldst_reg_imm(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 10, 2)) {
    case 0: case 1: case 3:
        /* Load/store register (unscaled immediate) */
        /* Load/store immediate pre/post-indexed */
        disas_ldst_reg_imm9(s, insn);
        break;
    case 2:
        /* Load/store register unprivileged */
        unsupported_encoding(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* Load/store register (all forms) */
static void disas_ldst_reg(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 24, 2)) {
    case 0:
        if (extract32(insn, 21, 1) == 1 && extract32(insn, 10, 2) == 2) {
            disas_ldst_reg_roffset(s, insn);
        } else {
            disas_ldst_reg_imm(s, insn);
        }
        break;
    case 1:
        disas_ldst_reg_unsigned_imm(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.3.1 AdvSIMD load/store multiple structures
 *
 *  31  30  29           23 22  21         16 15    12 11  10 9    5 4    0
 * +---+---+---------------+---+-------------+--------+------+------+------+
 * | 0 | Q | 0 0 1 1 0 0 0 | L | 0 0 0 0 0 0 | opcode | size |  Rn  |  Rt  |
 * +---+---+---------------+---+-------------+--------+------+------+------+
 *
 * C3.3.2 AdvSIMD load/store multiple structures (post-indexed)
 *
 *  31  30  29           23 22  21  20     16 15    12 11  10 9    5 4    0
 * +---+---+---------------+---+---+---------+--------+------+------+------+
 * | 0 | Q | 0 0 1 1 0 0 1 | L | 0 |   Rm    | opcode | size |  Rn  |  Rt  |
 * +---+---+---------------+---+---+---------+--------+------+------+------+
 *
 * Rt: first (or only) SIMD&FP register to be transferred
 * Rn: base address or SP
 * Rm (post-index only): post-index register (when !31) or size dependent #imm
 */
static void disas_ldst_multiple_struct(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int size = extract32(insn, 10, 2);
    int opcode = extract32(insn, 12, 4);
    bool is_store = !extract32(insn, 22, 1);
    bool is_postidx = extract32(insn, 23, 1);
    bool is_q = extract32(insn, 30, 1);
    TCGv_i64 tcg_addr, tcg_rn;

    int ebytes = 1 << size;
    int elements = (is_q ? 128 : 64) / (8 << size);
    int rpt;    /* num iterations */
    int selem;  /* structure elements */
    int r;

    if (extract32(insn, 31, 1) || extract32(insn, 21, 1)) {
        unallocated_encoding(s);
        return;
    }

    /* From the shared decode logic */
    switch (opcode) {
    case 0x0:
        rpt = 1;
        selem = 4;
        break;
    case 0x2:
        rpt = 4;
        selem = 1;
        break;
    case 0x4:
        rpt = 1;
        selem = 3;
        break;
    case 0x6:
        rpt = 3;
        selem = 1;
        break;
    case 0x7:
        rpt = 1;
        selem = 1;
        break;
    case 0x8:
        rpt = 1;
        selem = 2;
        break;
    case 0xa:
        rpt = 2;
        selem = 1;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    if (size == 3 && !is_q && selem != 1) {
        /* reserved */
        unallocated_encoding(s);
        return;
    }

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }

    tcg_rn = cpu_reg_sp(s, rn);
    tcg_addr = tcg_temp_new_i64();
    tcg_gen_mov_i64(tcg_addr, tcg_rn);

    for (r = 0; r < rpt; r++) {
        int e;
        for (e = 0; e < elements; e++) {
            int tt = (rt + r) % 32;
            int xs;
            for (xs = 0; xs < selem; xs++) {
                if (is_store) {
                    do_vec_st(s, tt, e, tcg_addr, size);
                } else {
                    do_vec_ld(s, tt, e, tcg_addr, size);

                    /* For non-quad operations, setting a slice of the low
                     * 64 bits of the register clears the high 64 bits (in
                     * the ARM ARM pseudocode this is implicit in the fact
                     * that 'rval' is a 64 bit wide variable). We optimize
                     * by noticing that we only need to do this the first
                     * time we touch a register.
                     */
                    if (!is_q && e == 0 && (r == 0 || xs == selem - 1)) {
                        clear_vec_high(s, tt);
                    }
                }
                tcg_gen_addi_i64(tcg_addr, tcg_addr, ebytes);
                tt = (tt + 1) % 32;
            }
        }
    }

    if (is_postidx) {
        int rm = extract32(insn, 16, 5);
        if (rm == 31) {
            tcg_gen_mov_i64(tcg_rn, tcg_addr);
        } else {
            tcg_gen_add_i64(tcg_rn, tcg_rn, cpu_reg(s, rm));
        }
    }
    tcg_temp_free_i64(tcg_addr);
}

/* C3.3.3 AdvSIMD load/store single structure
 *
 *  31  30  29           23 22 21 20       16 15 13 12  11  10 9    5 4    0
 * +---+---+---------------+-----+-----------+-----+---+------+------+------+
 * | 0 | Q | 0 0 1 1 0 1 0 | L R | 0 0 0 0 0 | opc | S | size |  Rn  |  Rt  |
 * +---+---+---------------+-----+-----------+-----+---+------+------+------+
 *
 * C3.3.4 AdvSIMD load/store single structure (post-indexed)
 *
 *  31  30  29           23 22 21 20       16 15 13 12  11  10 9    5 4    0
 * +---+---+---------------+-----+-----------+-----+---+------+------+------+
 * | 0 | Q | 0 0 1 1 0 1 1 | L R |     Rm    | opc | S | size |  Rn  |  Rt  |
 * +---+---+---------------+-----+-----------+-----+---+------+------+------+
 *
 * Rt: first (or only) SIMD&FP register to be transferred
 * Rn: base address or SP
 * Rm (post-index only): post-index register (when !31) or size dependent #imm
 * index = encoded in Q:S:size dependent on size
 *
 * lane_size = encoded in R, opc
 * transfer width = encoded in opc, S, size
 */
static void disas_ldst_single_struct(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int size = extract32(insn, 10, 2);
    int S = extract32(insn, 12, 1);
    int opc = extract32(insn, 13, 3);
    int R = extract32(insn, 21, 1);
    int is_load = extract32(insn, 22, 1);
    int is_postidx = extract32(insn, 23, 1);
    int is_q = extract32(insn, 30, 1);

    int scale = extract32(opc, 1, 2);
    int selem = (extract32(opc, 0, 1) << 1 | R) + 1;
    bool replicate = false;
    int index = is_q << 3 | S << 2 | size;
    int ebytes, xs;
    TCGv_i64 tcg_addr, tcg_rn;

    switch (scale) {
    case 3:
        if (!is_load || S) {
            unallocated_encoding(s);
            return;
        }
        scale = size;
        replicate = true;
        break;
    case 0:
        break;
    case 1:
        if (extract32(size, 0, 1)) {
            unallocated_encoding(s);
            return;
        }
        index >>= 1;
        break;
    case 2:
        if (extract32(size, 1, 1)) {
            unallocated_encoding(s);
            return;
        }
        if (!extract32(size, 0, 1)) {
            index >>= 2;
        } else {
            if (S) {
                unallocated_encoding(s);
                return;
            }
            index >>= 3;
            scale = 3;
        }
        break;
    default:
        g_assert_not_reached();
    }

    ebytes = 1 << scale;

    if (rn == 31) {
        gen_check_sp_alignment(s);
    }

    tcg_rn = cpu_reg_sp(s, rn);
    tcg_addr = tcg_temp_new_i64();
    tcg_gen_mov_i64(tcg_addr, tcg_rn);

    for (xs = 0; xs < selem; xs++) {
        if (replicate) {
            /* Load and replicate to all elements */
            uint64_t mulconst;
            TCGv_i64 tcg_tmp = tcg_temp_new_i64();

            tcg_gen_qemu_ld_i64(tcg_tmp, tcg_addr,
                                get_mem_index(s), MO_TE + scale);
            switch (scale) {
            case 0:
                mulconst = 0x0101010101010101ULL;
                break;
            case 1:
                mulconst = 0x0001000100010001ULL;
                break;
            case 2:
                mulconst = 0x0000000100000001ULL;
                break;
            case 3:
                mulconst = 0;
                break;
            default:
                g_assert_not_reached();
            }
            if (mulconst) {
                tcg_gen_muli_i64(tcg_tmp, tcg_tmp, mulconst);
            }
            write_vec_element(s, tcg_tmp, rt, 0, MO_64);
            if (is_q) {
                write_vec_element(s, tcg_tmp, rt, 1, MO_64);
            } else {
                clear_vec_high(s, rt);
            }
            tcg_temp_free_i64(tcg_tmp);
        } else {
            /* Load/store one element per register */
            if (is_load) {
                do_vec_ld(s, rt, index, tcg_addr, MO_TE + scale);
            } else {
                do_vec_st(s, rt, index, tcg_addr, MO_TE + scale);
            }
        }
        tcg_gen_addi_i64(tcg_addr, tcg_addr, ebytes);
        rt = (rt + 1) % 32;
    }

    if (is_postidx) {
        int rm = extract32(insn, 16, 5);
        if (rm == 31) {
            tcg_gen_mov_i64(tcg_rn, tcg_addr);
        } else {
            tcg_gen_add_i64(tcg_rn, tcg_rn, cpu_reg(s, rm));
        }
    }
    tcg_temp_free_i64(tcg_addr);
}

/* C3.3 Loads and stores */
static void disas_ldst(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 24, 6)) {
    case 0x08: /* Load/store exclusive */
        disas_ldst_excl(s, insn);
        break;
    case 0x18: case 0x1c: /* Load register (literal) */
        disas_ld_lit(s, insn);
        break;
    case 0x28: case 0x29:
    case 0x2c: case 0x2d: /* Load/store pair (all forms) */
        disas_ldst_pair(s, insn);
        break;
    case 0x38: case 0x39:
    case 0x3c: case 0x3d: /* Load/store register (all forms) */
        disas_ldst_reg(s, insn);
        break;
    case 0x0c: /* AdvSIMD load/store multiple structures */
        disas_ldst_multiple_struct(s, insn);
        break;
    case 0x0d: /* AdvSIMD load/store single structure */
        disas_ldst_single_struct(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.4.6 PC-rel. addressing
 *   31  30   29 28       24 23                5 4    0
 * +----+-------+-----------+-------------------+------+
 * | op | immlo | 1 0 0 0 0 |       immhi       |  Rd  |
 * +----+-------+-----------+-------------------+------+
 */
static void disas_pc_rel_adr(DisasContext *s, uint32_t insn)
{
    unsigned int page, rd;
    uint64_t base;
    int64_t offset;

    page = extract32(insn, 31, 1);
    /* SignExtend(immhi:immlo) -> offset */
    offset = ((int64_t)sextract32(insn, 5, 19) << 2) | extract32(insn, 29, 2);
    rd = extract32(insn, 0, 5);
    base = s->pc - 4;

    if (page) {
        /* ADRP (page based) */
        base &= ~0xfff;
        offset <<= 12;
    }

    tcg_gen_movi_i64(cpu_reg(s, rd), base + offset);
}

/*
 * C3.4.1 Add/subtract (immediate)
 *
 *  31 30 29 28       24 23 22 21         10 9   5 4   0
 * +--+--+--+-----------+-----+-------------+-----+-----+
 * |sf|op| S| 1 0 0 0 1 |shift|    imm12    |  Rn | Rd  |
 * +--+--+--+-----------+-----+-------------+-----+-----+
 *
 *    sf: 0 -> 32bit, 1 -> 64bit
 *    op: 0 -> add  , 1 -> sub
 *     S: 1 -> set flags
 * shift: 00 -> LSL imm by 0, 01 -> LSL imm by 12
 */
static void disas_add_sub_imm(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    uint64_t imm = extract32(insn, 10, 12);
    int shift = extract32(insn, 22, 2);
    bool setflags = extract32(insn, 29, 1);
    bool sub_op = extract32(insn, 30, 1);
    bool is_64bit = extract32(insn, 31, 1);

    TCGv_i64 tcg_rn = cpu_reg_sp(s, rn);
    TCGv_i64 tcg_rd = setflags ? cpu_reg(s, rd) : cpu_reg_sp(s, rd);
    TCGv_i64 tcg_result;

    switch (shift) {
    case 0x0:
        break;
    case 0x1:
        imm <<= 12;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    tcg_result = tcg_temp_new_i64();
    if (!setflags) {
        if (sub_op) {
            tcg_gen_subi_i64(tcg_result, tcg_rn, imm);
        } else {
            tcg_gen_addi_i64(tcg_result, tcg_rn, imm);
        }
    } else {
        TCGv_i64 tcg_imm = tcg_const_i64(imm);
        if (sub_op) {
            gen_sub_CC(is_64bit, tcg_result, tcg_rn, tcg_imm);
        } else {
            gen_add_CC(is_64bit, tcg_result, tcg_rn, tcg_imm);
        }
        tcg_temp_free_i64(tcg_imm);
    }

    if (is_64bit) {
        tcg_gen_mov_i64(tcg_rd, tcg_result);
    } else {
        tcg_gen_ext32u_i64(tcg_rd, tcg_result);
    }

    tcg_temp_free_i64(tcg_result);
}

/* The input should be a value in the bottom e bits (with higher
 * bits zero); returns that value replicated into every element
 * of size e in a 64 bit integer.
 */
static uint64_t bitfield_replicate(uint64_t mask, unsigned int e)
{
    assert(e != 0);
    while (e < 64) {
        mask |= mask << e;
        e *= 2;
    }
    return mask;
}

/* Return a value with the bottom len bits set (where 0 < len <= 64) */
static inline uint64_t bitmask64(unsigned int length)
{
    assert(length > 0 && length <= 64);
    return ~0ULL >> (64 - length);
}

/* Simplified variant of pseudocode DecodeBitMasks() for the case where we
 * only require the wmask. Returns false if the imms/immr/immn are a reserved
 * value (ie should cause a guest UNDEF exception), and true if they are
 * valid, in which case the decoded bit pattern is written to result.
 */
static bool logic_imm_decode_wmask(uint64_t *result, unsigned int immn,
                                   unsigned int imms, unsigned int immr)
{
    uint64_t mask;
    unsigned e, levels, s, r;
    int len;

    assert(immn < 2 && imms < 64 && immr < 64);

    /* The bit patterns we create here are 64 bit patterns which
     * are vectors of identical elements of size e = 2, 4, 8, 16, 32 or
     * 64 bits each. Each element contains the same value: a run
     * of between 1 and e-1 non-zero bits, rotated within the
     * element by between 0 and e-1 bits.
     *
     * The element size and run length are encoded into immn (1 bit)
     * and imms (6 bits) as follows:
     * 64 bit elements: immn = 1, imms = <length of run - 1>
     * 32 bit elements: immn = 0, imms = 0 : <length of run - 1>
     * 16 bit elements: immn = 0, imms = 10 : <length of run - 1>
     *  8 bit elements: immn = 0, imms = 110 : <length of run - 1>
     *  4 bit elements: immn = 0, imms = 1110 : <length of run - 1>
     *  2 bit elements: immn = 0, imms = 11110 : <length of run - 1>
     * Notice that immn = 0, imms = 11111x is the only combination
     * not covered by one of the above options; this is reserved.
     * Further, <length of run - 1> all-ones is a reserved pattern.
     *
     * In all cases the rotation is by immr % e (and immr is 6 bits).
     */

    /* First determine the element size */
    len = 31 - clz32((immn << 6) | (~imms & 0x3f));
    if (len < 1) {
        /* This is the immn == 0, imms == 0x11111x case */
        return false;
    }
    e = 1 << len;

    levels = e - 1;
    s = imms & levels;
    r = immr & levels;

    if (s == levels) {
        /* <length of run - 1> mustn't be all-ones. */
        return false;
    }

    /* Create the value of one element: s+1 set bits rotated
     * by r within the element (which is e bits wide)...
     */
    mask = bitmask64(s + 1);
    mask = (mask >> r) | (mask << (e - r));
    /* ...then replicate the element over the whole 64 bit value */
    mask = bitfield_replicate(mask, e);
    *result = mask;
    return true;
}

/* C3.4.4 Logical (immediate)
 *   31  30 29 28         23 22  21  16 15  10 9    5 4    0
 * +----+-----+-------------+---+------+------+------+------+
 * | sf | opc | 1 0 0 1 0 0 | N | immr | imms |  Rn  |  Rd  |
 * +----+-----+-------------+---+------+------+------+------+
 */
static void disas_logic_imm(DisasContext *s, uint32_t insn)
{
    unsigned int sf, opc, is_n, immr, imms, rn, rd;
    TCGv_i64 tcg_rd, tcg_rn;
    uint64_t wmask;
    bool is_and = false;

    sf = extract32(insn, 31, 1);
    opc = extract32(insn, 29, 2);
    is_n = extract32(insn, 22, 1);
    immr = extract32(insn, 16, 6);
    imms = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    if (!sf && is_n) {
        unallocated_encoding(s);
        return;
    }

    if (opc == 0x3) { /* ANDS */
        tcg_rd = cpu_reg(s, rd);
    } else {
        tcg_rd = cpu_reg_sp(s, rd);
    }
    tcg_rn = cpu_reg(s, rn);

    if (!logic_imm_decode_wmask(&wmask, is_n, imms, immr)) {
        /* some immediate field values are reserved */
        unallocated_encoding(s);
        return;
    }

    if (!sf) {
        wmask &= 0xffffffff;
    }

    switch (opc) {
    case 0x3: /* ANDS */
    case 0x0: /* AND */
        tcg_gen_andi_i64(tcg_rd, tcg_rn, wmask);
        is_and = true;
        break;
    case 0x1: /* ORR */
        tcg_gen_ori_i64(tcg_rd, tcg_rn, wmask);
        break;
    case 0x2: /* EOR */
        tcg_gen_xori_i64(tcg_rd, tcg_rn, wmask);
        break;
    default:
        assert(FALSE); /* must handle all above */
        break;
    }

    if (!sf && !is_and) {
        /* zero extend final result; we know we can skip this for AND
         * since the immediate had the high 32 bits clear.
         */
        tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
    }

    if (opc == 3) { /* ANDS */
        gen_logic_CC(sf, tcg_rd);
    }
}

/*
 * C3.4.5 Move wide (immediate)
 *
 *  31 30 29 28         23 22 21 20             5 4    0
 * +--+-----+-------------+-----+----------------+------+
 * |sf| opc | 1 0 0 1 0 1 |  hw |  imm16         |  Rd  |
 * +--+-----+-------------+-----+----------------+------+
 *
 * sf: 0 -> 32 bit, 1 -> 64 bit
 * opc: 00 -> N, 10 -> Z, 11 -> K
 * hw: shift/16 (0,16, and sf only 32, 48)
 */
static void disas_movw_imm(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    uint64_t imm = extract32(insn, 5, 16);
    int sf = extract32(insn, 31, 1);
    int opc = extract32(insn, 29, 2);
    int pos = extract32(insn, 21, 2) << 4;
    TCGv_i64 tcg_rd = cpu_reg(s, rd);
    TCGv_i64 tcg_imm;

    if (!sf && (pos >= 32)) {
        unallocated_encoding(s);
        return;
    }

    switch (opc) {
    case 0: /* MOVN */
    case 2: /* MOVZ */
        imm <<= pos;
        if (opc == 0) {
            imm = ~imm;
        }
        if (!sf) {
            imm &= 0xffffffffu;
        }
        tcg_gen_movi_i64(tcg_rd, imm);
        break;
    case 3: /* MOVK */
        tcg_imm = tcg_const_i64(imm);
        tcg_gen_deposit_i64(tcg_rd, tcg_rd, tcg_imm, pos, 16);
        tcg_temp_free_i64(tcg_imm);
        if (!sf) {
            tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
        }
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.4.2 Bitfield
 *   31  30 29 28         23 22  21  16 15  10 9    5 4    0
 * +----+-----+-------------+---+------+------+------+------+
 * | sf | opc | 1 0 0 1 1 0 | N | immr | imms |  Rn  |  Rd  |
 * +----+-----+-------------+---+------+------+------+------+
 */
static void disas_bitfield(DisasContext *s, uint32_t insn)
{
    unsigned int sf, n, opc, ri, si, rn, rd, bitsize, pos, len;
    TCGv_i64 tcg_rd, tcg_tmp;

    sf = extract32(insn, 31, 1);
    opc = extract32(insn, 29, 2);
    n = extract32(insn, 22, 1);
    ri = extract32(insn, 16, 6);
    si = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);
    bitsize = sf ? 64 : 32;

    if (sf != n || ri >= bitsize || si >= bitsize || opc > 2) {
        unallocated_encoding(s);
        return;
    }

    tcg_rd = cpu_reg(s, rd);
    tcg_tmp = read_cpu_reg(s, rn, sf);

    /* OPTME: probably worth recognizing common cases of ext{8,16,32}{u,s} */

    if (opc != 1) { /* SBFM or UBFM */
        tcg_gen_movi_i64(tcg_rd, 0);
    }

    /* do the bit move operation */
    if (si >= ri) {
        /* Wd<s-r:0> = Wn<s:r> */
        tcg_gen_shri_i64(tcg_tmp, tcg_tmp, ri);
        pos = 0;
        len = (si - ri) + 1;
    } else {
        /* Wd<32+s-r,32-r> = Wn<s:0> */
        pos = bitsize - ri;
        len = si + 1;
    }

    tcg_gen_deposit_i64(tcg_rd, tcg_rd, tcg_tmp, pos, len);

    if (opc == 0) { /* SBFM - sign extend the destination field */
        tcg_gen_shli_i64(tcg_rd, tcg_rd, 64 - (pos + len));
        tcg_gen_sari_i64(tcg_rd, tcg_rd, 64 - (pos + len));
    }

    if (!sf) { /* zero extend final result */
        tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
    }
}

/* C3.4.3 Extract
 *   31  30  29 28         23 22   21  20  16 15    10 9    5 4    0
 * +----+------+-------------+---+----+------+--------+------+------+
 * | sf | op21 | 1 0 0 1 1 1 | N | o0 |  Rm  |  imms  |  Rn  |  Rd  |
 * +----+------+-------------+---+----+------+--------+------+------+
 */
static void disas_extract(DisasContext *s, uint32_t insn)
{
    unsigned int sf, n, rm, imm, rn, rd, bitsize, op21, op0;

    sf = extract32(insn, 31, 1);
    n = extract32(insn, 22, 1);
    rm = extract32(insn, 16, 5);
    imm = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);
    op21 = extract32(insn, 29, 2);
    op0 = extract32(insn, 21, 1);
    bitsize = sf ? 64 : 32;

    if (sf != n || op21 || op0 || imm >= bitsize) {
        unallocated_encoding(s);
    } else {
        TCGv_i64 tcg_rd, tcg_rm, tcg_rn;

        tcg_rd = cpu_reg(s, rd);

        if (imm) {
            /* OPTME: we can special case rm==rn as a rotate */
            tcg_rm = read_cpu_reg(s, rm, sf);
            tcg_rn = read_cpu_reg(s, rn, sf);
            tcg_gen_shri_i64(tcg_rm, tcg_rm, imm);
            tcg_gen_shli_i64(tcg_rn, tcg_rn, bitsize - imm);
            tcg_gen_or_i64(tcg_rd, tcg_rm, tcg_rn);
            if (!sf) {
                tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
            }
        } else {
            /* tcg shl_i32/shl_i64 is undefined for 32/64 bit shifts,
             * so an extract from bit 0 is a special case.
             */
            if (sf) {
                tcg_gen_mov_i64(tcg_rd, cpu_reg(s, rm));
            } else {
                tcg_gen_ext32u_i64(tcg_rd, cpu_reg(s, rm));
            }
        }

    }
}

/* C3.4 Data processing - immediate */
static void disas_data_proc_imm(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 23, 6)) {
    case 0x20: case 0x21: /* PC-rel. addressing */
        disas_pc_rel_adr(s, insn);
        break;
    case 0x22: case 0x23: /* Add/subtract (immediate) */
        disas_add_sub_imm(s, insn);
        break;
    case 0x24: /* Logical (immediate) */
        disas_logic_imm(s, insn);
        break;
    case 0x25: /* Move wide (immediate) */
        disas_movw_imm(s, insn);
        break;
    case 0x26: /* Bitfield */
        disas_bitfield(s, insn);
        break;
    case 0x27: /* Extract */
        disas_extract(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* Shift a TCGv src by TCGv shift_amount, put result in dst.
 * Note that it is the caller's responsibility to ensure that the
 * shift amount is in range (ie 0..31 or 0..63) and provide the ARM
 * mandated semantics for out of range shifts.
 */
static void shift_reg(TCGv_i64 dst, TCGv_i64 src, int sf,
                      enum a64_shift_type shift_type, TCGv_i64 shift_amount)
{
    switch (shift_type) {
    case A64_SHIFT_TYPE_LSL:
        tcg_gen_shl_i64(dst, src, shift_amount);
        break;
    case A64_SHIFT_TYPE_LSR:
        tcg_gen_shr_i64(dst, src, shift_amount);
        break;
    case A64_SHIFT_TYPE_ASR:
        if (!sf) {
            tcg_gen_ext32s_i64(dst, src);
        }
        tcg_gen_sar_i64(dst, sf ? src : dst, shift_amount);
        break;
    case A64_SHIFT_TYPE_ROR:
        if (sf) {
            tcg_gen_rotr_i64(dst, src, shift_amount);
        } else {
            TCGv_i32 t0, t1;
            t0 = tcg_temp_new_i32();
            t1 = tcg_temp_new_i32();
            tcg_gen_trunc_i64_i32(t0, src);
            tcg_gen_trunc_i64_i32(t1, shift_amount);
            tcg_gen_rotr_i32(t0, t0, t1);
            tcg_gen_extu_i32_i64(dst, t0);
            tcg_temp_free_i32(t0);
            tcg_temp_free_i32(t1);
        }
        break;
    default:
        assert(FALSE); /* all shift types should be handled */
        break;
    }

    if (!sf) { /* zero extend final result */
        tcg_gen_ext32u_i64(dst, dst);
    }
}

/* Shift a TCGv src by immediate, put result in dst.
 * The shift amount must be in range (this should always be true as the
 * relevant instructions will UNDEF on bad shift immediates).
 */
static void shift_reg_imm(TCGv_i64 dst, TCGv_i64 src, int sf,
                          enum a64_shift_type shift_type, unsigned int shift_i)
{
    assert(shift_i < (sf ? 64 : 32));

    if (shift_i == 0) {
        tcg_gen_mov_i64(dst, src);
    } else {
        TCGv_i64 shift_const;

        shift_const = tcg_const_i64(shift_i);
        shift_reg(dst, src, sf, shift_type, shift_const);
        tcg_temp_free_i64(shift_const);
    }
}

/* C3.5.10 Logical (shifted register)
 *   31  30 29 28       24 23   22 21  20  16 15    10 9    5 4    0
 * +----+-----+-----------+-------+---+------+--------+------+------+
 * | sf | opc | 0 1 0 1 0 | shift | N |  Rm  |  imm6  |  Rn  |  Rd  |
 * +----+-----+-----------+-------+---+------+--------+------+------+
 */
static void disas_logic_reg(DisasContext *s, uint32_t insn)
{
    TCGv_i64 tcg_rd, tcg_rn, tcg_rm;
    unsigned int sf, opc, shift_type, invert, rm, shift_amount, rn, rd;

    sf = extract32(insn, 31, 1);
    opc = extract32(insn, 29, 2);
    shift_type = extract32(insn, 22, 2);
    invert = extract32(insn, 21, 1);
    rm = extract32(insn, 16, 5);
    shift_amount = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    if (!sf && (shift_amount & (1 << 5))) {
        unallocated_encoding(s);
        return;
    }

    tcg_rd = cpu_reg(s, rd);

    if (opc == 1 && shift_amount == 0 && shift_type == 0 && rn == 31) {
        /* Unshifted ORR and ORN with WZR/XZR is the standard encoding for
         * register-register MOV and MVN, so it is worth special casing.
         */
        tcg_rm = cpu_reg(s, rm);
        if (invert) {
            tcg_gen_not_i64(tcg_rd, tcg_rm);
            if (!sf) {
                tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
            }
        } else {
            if (sf) {
                tcg_gen_mov_i64(tcg_rd, tcg_rm);
            } else {
                tcg_gen_ext32u_i64(tcg_rd, tcg_rm);
            }
        }
        return;
    }

    tcg_rm = read_cpu_reg(s, rm, sf);

    if (shift_amount) {
        shift_reg_imm(tcg_rm, tcg_rm, sf, shift_type, shift_amount);
    }

    tcg_rn = cpu_reg(s, rn);

    switch (opc | (invert << 2)) {
    case 0: /* AND */
    case 3: /* ANDS */
        tcg_gen_and_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    case 1: /* ORR */
        tcg_gen_or_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    case 2: /* EOR */
        tcg_gen_xor_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    case 4: /* BIC */
    case 7: /* BICS */
        tcg_gen_andc_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    case 5: /* ORN */
        tcg_gen_orc_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    case 6: /* EON */
        tcg_gen_eqv_i64(tcg_rd, tcg_rn, tcg_rm);
        break;
    default:
        assert(FALSE);
        break;
    }

    if (!sf) {
        tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
    }

    if (opc == 3) {
        gen_logic_CC(sf, tcg_rd);
    }
}

/*
 * C3.5.1 Add/subtract (extended register)
 *
 *  31|30|29|28       24|23 22|21|20   16|15  13|12  10|9  5|4  0|
 * +--+--+--+-----------+-----+--+-------+------+------+----+----+
 * |sf|op| S| 0 1 0 1 1 | opt | 1|  Rm   |option| imm3 | Rn | Rd |
 * +--+--+--+-----------+-----+--+-------+------+------+----+----+
 *
 *  sf: 0 -> 32bit, 1 -> 64bit
 *  op: 0 -> add  , 1 -> sub
 *   S: 1 -> set flags
 * opt: 00
 * option: extension type (see DecodeRegExtend)
 * imm3: optional shift to Rm
 *
 * Rd = Rn + LSL(extend(Rm), amount)
 */
static void disas_add_sub_ext_reg(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm3 = extract32(insn, 10, 3);
    int option = extract32(insn, 13, 3);
    int rm = extract32(insn, 16, 5);
    bool setflags = extract32(insn, 29, 1);
    bool sub_op = extract32(insn, 30, 1);
    bool sf = extract32(insn, 31, 1);

    TCGv_i64 tcg_rm, tcg_rn; /* temps */
    TCGv_i64 tcg_rd;
    TCGv_i64 tcg_result;

    if (imm3 > 4) {
        unallocated_encoding(s);
        return;
    }

    /* non-flag setting ops may use SP */
    if (!setflags) {
        tcg_rn = read_cpu_reg_sp(s, rn, sf);
        tcg_rd = cpu_reg_sp(s, rd);
    } else {
        tcg_rn = read_cpu_reg(s, rn, sf);
        tcg_rd = cpu_reg(s, rd);
    }

    tcg_rm = read_cpu_reg(s, rm, sf);
    ext_and_shift_reg(tcg_rm, tcg_rm, option, imm3);

    tcg_result = tcg_temp_new_i64();

    if (!setflags) {
        if (sub_op) {
            tcg_gen_sub_i64(tcg_result, tcg_rn, tcg_rm);
        } else {
            tcg_gen_add_i64(tcg_result, tcg_rn, tcg_rm);
        }
    } else {
        if (sub_op) {
            gen_sub_CC(sf, tcg_result, tcg_rn, tcg_rm);
        } else {
            gen_add_CC(sf, tcg_result, tcg_rn, tcg_rm);
        }
    }

    if (sf) {
        tcg_gen_mov_i64(tcg_rd, tcg_result);
    } else {
        tcg_gen_ext32u_i64(tcg_rd, tcg_result);
    }

    tcg_temp_free_i64(tcg_result);
}

/*
 * C3.5.2 Add/subtract (shifted register)
 *
 *  31 30 29 28       24 23 22 21 20   16 15     10 9    5 4    0
 * +--+--+--+-----------+-----+--+-------+---------+------+------+
 * |sf|op| S| 0 1 0 1 1 |shift| 0|  Rm   |  imm6   |  Rn  |  Rd  |
 * +--+--+--+-----------+-----+--+-------+---------+------+------+
 *
 *    sf: 0 -> 32bit, 1 -> 64bit
 *    op: 0 -> add  , 1 -> sub
 *     S: 1 -> set flags
 * shift: 00 -> LSL, 01 -> LSR, 10 -> ASR, 11 -> RESERVED
 *  imm6: Shift amount to apply to Rm before the add/sub
 */
static void disas_add_sub_reg(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm6 = extract32(insn, 10, 6);
    int rm = extract32(insn, 16, 5);
    int shift_type = extract32(insn, 22, 2);
    bool setflags = extract32(insn, 29, 1);
    bool sub_op = extract32(insn, 30, 1);
    bool sf = extract32(insn, 31, 1);

    TCGv_i64 tcg_rd = cpu_reg(s, rd);
    TCGv_i64 tcg_rn, tcg_rm;
    TCGv_i64 tcg_result;

    if ((shift_type == 3) || (!sf && (imm6 > 31))) {
        unallocated_encoding(s);
        return;
    }

    tcg_rn = read_cpu_reg(s, rn, sf);
    tcg_rm = read_cpu_reg(s, rm, sf);

    shift_reg_imm(tcg_rm, tcg_rm, sf, shift_type, imm6);

    tcg_result = tcg_temp_new_i64();

    if (!setflags) {
        if (sub_op) {
            tcg_gen_sub_i64(tcg_result, tcg_rn, tcg_rm);
        } else {
            tcg_gen_add_i64(tcg_result, tcg_rn, tcg_rm);
        }
    } else {
        if (sub_op) {
            gen_sub_CC(sf, tcg_result, tcg_rn, tcg_rm);
        } else {
            gen_add_CC(sf, tcg_result, tcg_rn, tcg_rm);
        }
    }

    if (sf) {
        tcg_gen_mov_i64(tcg_rd, tcg_result);
    } else {
        tcg_gen_ext32u_i64(tcg_rd, tcg_result);
    }

    tcg_temp_free_i64(tcg_result);
}

/* C3.5.9 Data-processing (3 source)

   31 30  29 28       24 23 21  20  16  15  14  10 9    5 4    0
  +--+------+-----------+------+------+----+------+------+------+
  |sf| op54 | 1 1 0 1 1 | op31 |  Rm  | o0 |  Ra  |  Rn  |  Rd  |
  +--+------+-----------+------+------+----+------+------+------+

 */
static void disas_data_proc_3src(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int ra = extract32(insn, 10, 5);
    int rm = extract32(insn, 16, 5);
    int op_id = (extract32(insn, 29, 3) << 4) |
        (extract32(insn, 21, 3) << 1) |
        extract32(insn, 15, 1);
    bool sf = extract32(insn, 31, 1);
    bool is_sub = extract32(op_id, 0, 1);
    bool is_high = extract32(op_id, 2, 1);
    bool is_signed = false;
    TCGv_i64 tcg_op1;
    TCGv_i64 tcg_op2;
    TCGv_i64 tcg_tmp;

    /* Note that op_id is sf:op54:op31:o0 so it includes the 32/64 size flag */
    switch (op_id) {
    case 0x42: /* SMADDL */
    case 0x43: /* SMSUBL */
    case 0x44: /* SMULH */
        is_signed = true;
        break;
    case 0x0: /* MADD (32bit) */
    case 0x1: /* MSUB (32bit) */
    case 0x40: /* MADD (64bit) */
    case 0x41: /* MSUB (64bit) */
    case 0x4a: /* UMADDL */
    case 0x4b: /* UMSUBL */
    case 0x4c: /* UMULH */
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    if (is_high) {
        TCGv_i64 low_bits = tcg_temp_new_i64(); /* low bits discarded */
        TCGv_i64 tcg_rd = cpu_reg(s, rd);
        TCGv_i64 tcg_rn = cpu_reg(s, rn);
        TCGv_i64 tcg_rm = cpu_reg(s, rm);

        if (is_signed) {
            tcg_gen_muls2_i64(low_bits, tcg_rd, tcg_rn, tcg_rm);
        } else {
            tcg_gen_mulu2_i64(low_bits, tcg_rd, tcg_rn, tcg_rm);
        }

        tcg_temp_free_i64(low_bits);
        return;
    }

    tcg_op1 = tcg_temp_new_i64();
    tcg_op2 = tcg_temp_new_i64();
    tcg_tmp = tcg_temp_new_i64();

    if (op_id < 0x42) {
        tcg_gen_mov_i64(tcg_op1, cpu_reg(s, rn));
        tcg_gen_mov_i64(tcg_op2, cpu_reg(s, rm));
    } else {
        if (is_signed) {
            tcg_gen_ext32s_i64(tcg_op1, cpu_reg(s, rn));
            tcg_gen_ext32s_i64(tcg_op2, cpu_reg(s, rm));
        } else {
            tcg_gen_ext32u_i64(tcg_op1, cpu_reg(s, rn));
            tcg_gen_ext32u_i64(tcg_op2, cpu_reg(s, rm));
        }
    }

    if (ra == 31 && !is_sub) {
        /* Special-case MADD with rA == XZR; it is the standard MUL alias */
        tcg_gen_mul_i64(cpu_reg(s, rd), tcg_op1, tcg_op2);
    } else {
        tcg_gen_mul_i64(tcg_tmp, tcg_op1, tcg_op2);
        if (is_sub) {
            tcg_gen_sub_i64(cpu_reg(s, rd), cpu_reg(s, ra), tcg_tmp);
        } else {
            tcg_gen_add_i64(cpu_reg(s, rd), cpu_reg(s, ra), tcg_tmp);
        }
    }

    if (!sf) {
        tcg_gen_ext32u_i64(cpu_reg(s, rd), cpu_reg(s, rd));
    }

    tcg_temp_free_i64(tcg_op1);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_tmp);
}

/* C3.5.3 - Add/subtract (with carry)
 *  31 30 29 28 27 26 25 24 23 22 21  20  16  15   10  9    5 4   0
 * +--+--+--+------------------------+------+---------+------+-----+
 * |sf|op| S| 1  1  0  1  0  0  0  0 |  rm  | opcode2 |  Rn  |  Rd |
 * +--+--+--+------------------------+------+---------+------+-----+
 *                                            [000000]
 */

static void disas_adc_sbc(DisasContext *s, uint32_t insn)
{
    unsigned int sf, op, setflags, rm, rn, rd;
    TCGv_i64 tcg_y, tcg_rn, tcg_rd;

    if (extract32(insn, 10, 6) != 0) {
        unallocated_encoding(s);
        return;
    }

    sf = extract32(insn, 31, 1);
    op = extract32(insn, 30, 1);
    setflags = extract32(insn, 29, 1);
    rm = extract32(insn, 16, 5);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    tcg_rd = cpu_reg(s, rd);
    tcg_rn = cpu_reg(s, rn);

    if (op) {
        tcg_y = new_tmp_a64(s);
        tcg_gen_not_i64(tcg_y, cpu_reg(s, rm));
    } else {
        tcg_y = cpu_reg(s, rm);
    }

    if (setflags) {
        gen_adc_CC(sf, tcg_rd, tcg_rn, tcg_y);
    } else {
        gen_adc(sf, tcg_rd, tcg_rn, tcg_y);
    }
}

/* C3.5.4 - C3.5.5 Conditional compare (immediate / register)
 *  31 30 29 28 27 26 25 24 23 22 21  20    16 15  12  11  10  9   5  4 3   0
 * +--+--+--+------------------------+--------+------+----+--+------+--+-----+
 * |sf|op| S| 1  1  0  1  0  0  1  0 |imm5/rm | cond |i/r |o2|  Rn  |o3|nzcv |
 * +--+--+--+------------------------+--------+------+----+--+------+--+-----+
 *        [1]                             y                [0]       [0]
 */
static void disas_cc(DisasContext *s, uint32_t insn)
{
    unsigned int sf, op, y, cond, rn, nzcv, is_imm;
    int label_continue = -1;
    TCGv_i64 tcg_tmp, tcg_y, tcg_rn;

    if (!extract32(insn, 29, 1)) {
        unallocated_encoding(s);
        return;
    }
    if (insn & (1 << 10 | 1 << 4)) {
        unallocated_encoding(s);
        return;
    }
    sf = extract32(insn, 31, 1);
    op = extract32(insn, 30, 1);
    is_imm = extract32(insn, 11, 1);
    y = extract32(insn, 16, 5); /* y = rm (reg) or imm5 (imm) */
    cond = extract32(insn, 12, 4);
    rn = extract32(insn, 5, 5);
    nzcv = extract32(insn, 0, 4);

    if (cond < 0x0e) { /* not always */
        int label_match = gen_new_label();
        label_continue = gen_new_label();
        arm_gen_test_cc(cond, label_match);
        /* nomatch: */
        tcg_tmp = tcg_temp_new_i64();
        tcg_gen_movi_i64(tcg_tmp, nzcv << 28);
        gen_set_nzcv(tcg_tmp);
        tcg_temp_free_i64(tcg_tmp);
        tcg_gen_br(label_continue);
        gen_set_label(label_match);
    }
    /* match, or condition is always */
    if (is_imm) {
        tcg_y = new_tmp_a64(s);
        tcg_gen_movi_i64(tcg_y, y);
    } else {
        tcg_y = cpu_reg(s, y);
    }
    tcg_rn = cpu_reg(s, rn);

    tcg_tmp = tcg_temp_new_i64();
    if (op) {
        gen_sub_CC(sf, tcg_tmp, tcg_rn, tcg_y);
    } else {
        gen_add_CC(sf, tcg_tmp, tcg_rn, tcg_y);
    }
    tcg_temp_free_i64(tcg_tmp);

    if (cond < 0x0e) { /* continue */
        gen_set_label(label_continue);
    }
}

/* C3.5.6 Conditional select
 *   31   30  29  28             21 20  16 15  12 11 10 9    5 4    0
 * +----+----+---+-----------------+------+------+-----+------+------+
 * | sf | op | S | 1 1 0 1 0 1 0 0 |  Rm  | cond | op2 |  Rn  |  Rd  |
 * +----+----+---+-----------------+------+------+-----+------+------+
 */
static void disas_cond_select(DisasContext *s, uint32_t insn)
{
    unsigned int sf, else_inv, rm, cond, else_inc, rn, rd;
    TCGv_i64 tcg_rd, tcg_src;

    if (extract32(insn, 29, 1) || extract32(insn, 11, 1)) {
        /* S == 1 or op2<1> == 1 */
        unallocated_encoding(s);
        return;
    }
    sf = extract32(insn, 31, 1);
    else_inv = extract32(insn, 30, 1);
    rm = extract32(insn, 16, 5);
    cond = extract32(insn, 12, 4);
    else_inc = extract32(insn, 10, 1);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    if (rd == 31) {
        /* silly no-op write; until we use movcond we must special-case
         * this to avoid a dead temporary across basic blocks.
         */
        return;
    }

    tcg_rd = cpu_reg(s, rd);

    if (cond >= 0x0e) { /* condition "always" */
        tcg_src = read_cpu_reg(s, rn, sf);
        tcg_gen_mov_i64(tcg_rd, tcg_src);
    } else {
        /* OPTME: we could use movcond here, at the cost of duplicating
         * a lot of the arm_gen_test_cc() logic.
         */
        int label_match = gen_new_label();
        int label_continue = gen_new_label();

        arm_gen_test_cc(cond, label_match);
        /* nomatch: */
        tcg_src = cpu_reg(s, rm);

        if (else_inv && else_inc) {
            tcg_gen_neg_i64(tcg_rd, tcg_src);
        } else if (else_inv) {
            tcg_gen_not_i64(tcg_rd, tcg_src);
        } else if (else_inc) {
            tcg_gen_addi_i64(tcg_rd, tcg_src, 1);
        } else {
            tcg_gen_mov_i64(tcg_rd, tcg_src);
        }
        if (!sf) {
            tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
        }
        tcg_gen_br(label_continue);
        /* match: */
        gen_set_label(label_match);
        tcg_src = read_cpu_reg(s, rn, sf);
        tcg_gen_mov_i64(tcg_rd, tcg_src);
        /* continue: */
        gen_set_label(label_continue);
    }
}

static void handle_clz(DisasContext *s, unsigned int sf,
                       unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_rd, tcg_rn;
    tcg_rd = cpu_reg(s, rd);
    tcg_rn = cpu_reg(s, rn);

    if (sf) {
        gen_helper_clz64(tcg_rd, tcg_rn);
    } else {
        TCGv_i32 tcg_tmp32 = tcg_temp_new_i32();
        tcg_gen_trunc_i64_i32(tcg_tmp32, tcg_rn);
        gen_helper_clz(tcg_tmp32, tcg_tmp32);
        tcg_gen_extu_i32_i64(tcg_rd, tcg_tmp32);
        tcg_temp_free_i32(tcg_tmp32);
    }
}

static void handle_cls(DisasContext *s, unsigned int sf,
                       unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_rd, tcg_rn;
    tcg_rd = cpu_reg(s, rd);
    tcg_rn = cpu_reg(s, rn);

    if (sf) {
        gen_helper_cls64(tcg_rd, tcg_rn);
    } else {
        TCGv_i32 tcg_tmp32 = tcg_temp_new_i32();
        tcg_gen_trunc_i64_i32(tcg_tmp32, tcg_rn);
        gen_helper_cls32(tcg_tmp32, tcg_tmp32);
        tcg_gen_extu_i32_i64(tcg_rd, tcg_tmp32);
        tcg_temp_free_i32(tcg_tmp32);
    }
}

static void handle_rbit(DisasContext *s, unsigned int sf,
                        unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_rd, tcg_rn;
    tcg_rd = cpu_reg(s, rd);
    tcg_rn = cpu_reg(s, rn);

    if (sf) {
        gen_helper_rbit64(tcg_rd, tcg_rn);
    } else {
        TCGv_i32 tcg_tmp32 = tcg_temp_new_i32();
        tcg_gen_trunc_i64_i32(tcg_tmp32, tcg_rn);
        gen_helper_rbit(tcg_tmp32, tcg_tmp32);
        tcg_gen_extu_i32_i64(tcg_rd, tcg_tmp32);
        tcg_temp_free_i32(tcg_tmp32);
    }
}

/* C5.6.149 REV with sf==1, opcode==3 ("REV64") */
static void handle_rev64(DisasContext *s, unsigned int sf,
                         unsigned int rn, unsigned int rd)
{
    if (!sf) {
        unallocated_encoding(s);
        return;
    }
    tcg_gen_bswap64_i64(cpu_reg(s, rd), cpu_reg(s, rn));
}

/* C5.6.149 REV with sf==0, opcode==2
 * C5.6.151 REV32 (sf==1, opcode==2)
 */
static void handle_rev32(DisasContext *s, unsigned int sf,
                         unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_rd = cpu_reg(s, rd);

    if (sf) {
        TCGv_i64 tcg_tmp = tcg_temp_new_i64();
        TCGv_i64 tcg_rn = read_cpu_reg(s, rn, sf);

        /* bswap32_i64 requires zero high word */
        tcg_gen_ext32u_i64(tcg_tmp, tcg_rn);
        tcg_gen_bswap32_i64(tcg_rd, tcg_tmp);
        tcg_gen_shri_i64(tcg_tmp, tcg_rn, 32);
        tcg_gen_bswap32_i64(tcg_tmp, tcg_tmp);
        tcg_gen_concat32_i64(tcg_rd, tcg_rd, tcg_tmp);

        tcg_temp_free_i64(tcg_tmp);
    } else {
        tcg_gen_ext32u_i64(tcg_rd, cpu_reg(s, rn));
        tcg_gen_bswap32_i64(tcg_rd, tcg_rd);
    }
}

/* C5.6.150 REV16 (opcode==1) */
static void handle_rev16(DisasContext *s, unsigned int sf,
                         unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_rd = cpu_reg(s, rd);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    TCGv_i64 tcg_rn = read_cpu_reg(s, rn, sf);

    tcg_gen_andi_i64(tcg_tmp, tcg_rn, 0xffff);
    tcg_gen_bswap16_i64(tcg_rd, tcg_tmp);

    tcg_gen_shri_i64(tcg_tmp, tcg_rn, 16);
    tcg_gen_andi_i64(tcg_tmp, tcg_tmp, 0xffff);
    tcg_gen_bswap16_i64(tcg_tmp, tcg_tmp);
    tcg_gen_deposit_i64(tcg_rd, tcg_rd, tcg_tmp, 16, 16);

    if (sf) {
        tcg_gen_shri_i64(tcg_tmp, tcg_rn, 32);
        tcg_gen_andi_i64(tcg_tmp, tcg_tmp, 0xffff);
        tcg_gen_bswap16_i64(tcg_tmp, tcg_tmp);
        tcg_gen_deposit_i64(tcg_rd, tcg_rd, tcg_tmp, 32, 16);

        tcg_gen_shri_i64(tcg_tmp, tcg_rn, 48);
        tcg_gen_bswap16_i64(tcg_tmp, tcg_tmp);
        tcg_gen_deposit_i64(tcg_rd, tcg_rd, tcg_tmp, 48, 16);
    }

    tcg_temp_free_i64(tcg_tmp);
}

/* C3.5.7 Data-processing (1 source)
 *   31  30  29  28             21 20     16 15    10 9    5 4    0
 * +----+---+---+-----------------+---------+--------+------+------+
 * | sf | 1 | S | 1 1 0 1 0 1 1 0 | opcode2 | opcode |  Rn  |  Rd  |
 * +----+---+---+-----------------+---------+--------+------+------+
 */
static void disas_data_proc_1src(DisasContext *s, uint32_t insn)
{
    unsigned int sf, opcode, rn, rd;

    if (extract32(insn, 29, 1) || extract32(insn, 16, 5)) {
        unallocated_encoding(s);
        return;
    }

    sf = extract32(insn, 31, 1);
    opcode = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    switch (opcode) {
    case 0: /* RBIT */
        handle_rbit(s, sf, rn, rd);
        break;
    case 1: /* REV16 */
        handle_rev16(s, sf, rn, rd);
        break;
    case 2: /* REV32 */
        handle_rev32(s, sf, rn, rd);
        break;
    case 3: /* REV64 */
        handle_rev64(s, sf, rn, rd);
        break;
    case 4: /* CLZ */
        handle_clz(s, sf, rn, rd);
        break;
    case 5: /* CLS */
        handle_cls(s, sf, rn, rd);
        break;
    }
}

static void handle_div(DisasContext *s, bool is_signed, unsigned int sf,
                       unsigned int rm, unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_n, tcg_m, tcg_rd;
    tcg_rd = cpu_reg(s, rd);

    if (!sf && is_signed) {
        tcg_n = new_tmp_a64(s);
        tcg_m = new_tmp_a64(s);
        tcg_gen_ext32s_i64(tcg_n, cpu_reg(s, rn));
        tcg_gen_ext32s_i64(tcg_m, cpu_reg(s, rm));
    } else {
        tcg_n = read_cpu_reg(s, rn, sf);
        tcg_m = read_cpu_reg(s, rm, sf);
    }

    if (is_signed) {
        gen_helper_sdiv64(tcg_rd, tcg_n, tcg_m);
    } else {
        gen_helper_udiv64(tcg_rd, tcg_n, tcg_m);
    }

    if (!sf) { /* zero extend final result */
        tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
    }
}

/* C5.6.115 LSLV, C5.6.118 LSRV, C5.6.17 ASRV, C5.6.154 RORV */
static void handle_shift_reg(DisasContext *s,
                             enum a64_shift_type shift_type, unsigned int sf,
                             unsigned int rm, unsigned int rn, unsigned int rd)
{
    TCGv_i64 tcg_shift = tcg_temp_new_i64();
    TCGv_i64 tcg_rd = cpu_reg(s, rd);
    TCGv_i64 tcg_rn = read_cpu_reg(s, rn, sf);

    tcg_gen_andi_i64(tcg_shift, cpu_reg(s, rm), sf ? 63 : 31);
    shift_reg(tcg_rd, tcg_rn, sf, shift_type, tcg_shift);
    tcg_temp_free_i64(tcg_shift);
}

/* C3.5.8 Data-processing (2 source)
 *   31   30  29 28             21 20  16 15    10 9    5 4    0
 * +----+---+---+-----------------+------+--------+------+------+
 * | sf | 0 | S | 1 1 0 1 0 1 1 0 |  Rm  | opcode |  Rn  |  Rd  |
 * +----+---+---+-----------------+------+--------+------+------+
 */
static void disas_data_proc_2src(DisasContext *s, uint32_t insn)
{
    unsigned int sf, rm, opcode, rn, rd;
    sf = extract32(insn, 31, 1);
    rm = extract32(insn, 16, 5);
    opcode = extract32(insn, 10, 6);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    if (extract32(insn, 29, 1)) {
        unallocated_encoding(s);
        return;
    }

    switch (opcode) {
    case 2: /* UDIV */
        handle_div(s, false, sf, rm, rn, rd);
        break;
    case 3: /* SDIV */
        handle_div(s, true, sf, rm, rn, rd);
        break;
    case 8: /* LSLV */
        handle_shift_reg(s, A64_SHIFT_TYPE_LSL, sf, rm, rn, rd);
        break;
    case 9: /* LSRV */
        handle_shift_reg(s, A64_SHIFT_TYPE_LSR, sf, rm, rn, rd);
        break;
    case 10: /* ASRV */
        handle_shift_reg(s, A64_SHIFT_TYPE_ASR, sf, rm, rn, rd);
        break;
    case 11: /* RORV */
        handle_shift_reg(s, A64_SHIFT_TYPE_ROR, sf, rm, rn, rd);
        break;
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23: /* CRC32 */
        unsupported_encoding(s, insn);
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.5 Data processing - register */
static void disas_data_proc_reg(DisasContext *s, uint32_t insn)
{
    switch (extract32(insn, 24, 5)) {
    case 0x0a: /* Logical (shifted register) */
        disas_logic_reg(s, insn);
        break;
    case 0x0b: /* Add/subtract */
        if (insn & (1 << 21)) { /* (extended register) */
            disas_add_sub_ext_reg(s, insn);
        } else {
            disas_add_sub_reg(s, insn);
        }
        break;
    case 0x1b: /* Data-processing (3 source) */
        disas_data_proc_3src(s, insn);
        break;
    case 0x1a:
        switch (extract32(insn, 21, 3)) {
        case 0x0: /* Add/subtract (with carry) */
            disas_adc_sbc(s, insn);
            break;
        case 0x2: /* Conditional compare */
            disas_cc(s, insn); /* both imm and reg forms */
            break;
        case 0x4: /* Conditional select */
            disas_cond_select(s, insn);
            break;
        case 0x6: /* Data-processing */
            if (insn & (1 << 30)) { /* (1 source) */
                disas_data_proc_1src(s, insn);
            } else {            /* (2 source) */
                disas_data_proc_2src(s, insn);
            }
            break;
        default:
            unallocated_encoding(s);
            break;
        }
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

static void handle_fp_compare(DisasContext *s, bool is_double,
                              unsigned int rn, unsigned int rm,
                              bool cmp_with_zero, bool signal_all_nans)
{
    TCGv_i64 tcg_flags = tcg_temp_new_i64();
    TCGv_ptr fpst = get_fpstatus_ptr();

    if (is_double) {
        TCGv_i64 tcg_vn, tcg_vm;

        tcg_vn = read_fp_dreg(s, rn);
        if (cmp_with_zero) {
            tcg_vm = tcg_const_i64(0);
        } else {
            tcg_vm = read_fp_dreg(s, rm);
        }
        if (signal_all_nans) {
            gen_helper_vfp_cmped_a64(tcg_flags, tcg_vn, tcg_vm, fpst);
        } else {
            gen_helper_vfp_cmpd_a64(tcg_flags, tcg_vn, tcg_vm, fpst);
        }
        tcg_temp_free_i64(tcg_vn);
        tcg_temp_free_i64(tcg_vm);
    } else {
        TCGv_i32 tcg_vn, tcg_vm;

        tcg_vn = read_fp_sreg(s, rn);
        if (cmp_with_zero) {
            tcg_vm = tcg_const_i32(0);
        } else {
            tcg_vm = read_fp_sreg(s, rm);
        }
        if (signal_all_nans) {
            gen_helper_vfp_cmpes_a64(tcg_flags, tcg_vn, tcg_vm, fpst);
        } else {
            gen_helper_vfp_cmps_a64(tcg_flags, tcg_vn, tcg_vm, fpst);
        }
        tcg_temp_free_i32(tcg_vn);
        tcg_temp_free_i32(tcg_vm);
    }

    tcg_temp_free_ptr(fpst);

    gen_set_nzcv(tcg_flags);

    tcg_temp_free_i64(tcg_flags);
}

/* C3.6.22 Floating point compare
 *   31  30  29 28       24 23  22  21 20  16 15 14 13  10    9    5 4     0
 * +---+---+---+-----------+------+---+------+-----+---------+------+-------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 |  Rm  | op  | 1 0 0 0 |  Rn  |  op2  |
 * +---+---+---+-----------+------+---+------+-----+---------+------+-------+
 */
static void disas_fp_compare(DisasContext *s, uint32_t insn)
{
    unsigned int mos, type, rm, op, rn, opc, op2r;

    mos = extract32(insn, 29, 3);
    type = extract32(insn, 22, 2); /* 0 = single, 1 = double */
    rm = extract32(insn, 16, 5);
    op = extract32(insn, 14, 2);
    rn = extract32(insn, 5, 5);
    opc = extract32(insn, 3, 2);
    op2r = extract32(insn, 0, 3);

    if (mos || op || op2r || type > 1) {
        unallocated_encoding(s);
        return;
    }

    handle_fp_compare(s, type, rn, rm, opc & 1, opc & 2);
}

/* C3.6.23 Floating point conditional compare
 *   31  30  29 28       24 23  22  21 20  16 15  12 11 10 9    5  4   3    0
 * +---+---+---+-----------+------+---+------+------+-----+------+----+------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 |  Rm  | cond | 0 1 |  Rn  | op | nzcv |
 * +---+---+---+-----------+------+---+------+------+-----+------+----+------+
 */
static void disas_fp_ccomp(DisasContext *s, uint32_t insn)
{
    unsigned int mos, type, rm, cond, rn, op, nzcv;
    TCGv_i64 tcg_flags;
    int label_continue = -1;

    mos = extract32(insn, 29, 3);
    type = extract32(insn, 22, 2); /* 0 = single, 1 = double */
    rm = extract32(insn, 16, 5);
    cond = extract32(insn, 12, 4);
    rn = extract32(insn, 5, 5);
    op = extract32(insn, 4, 1);
    nzcv = extract32(insn, 0, 4);

    if (mos || type > 1) {
        unallocated_encoding(s);
        return;
    }

    if (cond < 0x0e) { /* not always */
        int label_match = gen_new_label();
        label_continue = gen_new_label();
        arm_gen_test_cc(cond, label_match);
        /* nomatch: */
        tcg_flags = tcg_const_i64(nzcv << 28);
        gen_set_nzcv(tcg_flags);
        tcg_temp_free_i64(tcg_flags);
        tcg_gen_br(label_continue);
        gen_set_label(label_match);
    }

    handle_fp_compare(s, type, rn, rm, false, op);

    if (cond < 0x0e) {
        gen_set_label(label_continue);
    }
}

/* copy src FP register to dst FP register; type specifies single or double */
static void gen_mov_fp2fp(DisasContext *s, int type, int dst, int src)
{
    if (type) {
        TCGv_i64 v = read_fp_dreg(s, src);
        write_fp_dreg(s, dst, v);
        tcg_temp_free_i64(v);
    } else {
        TCGv_i32 v = read_fp_sreg(s, src);
        write_fp_sreg(s, dst, v);
        tcg_temp_free_i32(v);
    }
}

/* C3.6.24 Floating point conditional select
 *   31  30  29 28       24 23  22  21 20  16 15  12 11 10 9    5 4    0
 * +---+---+---+-----------+------+---+------+------+-----+------+------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 |  Rm  | cond | 1 1 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+------+------+-----+------+------+
 */
static void disas_fp_csel(DisasContext *s, uint32_t insn)
{
    unsigned int mos, type, rm, cond, rn, rd;
    int label_continue = -1;

    mos = extract32(insn, 29, 3);
    type = extract32(insn, 22, 2); /* 0 = single, 1 = double */
    rm = extract32(insn, 16, 5);
    cond = extract32(insn, 12, 4);
    rn = extract32(insn, 5, 5);
    rd = extract32(insn, 0, 5);

    if (mos || type > 1) {
        unallocated_encoding(s);
        return;
    }

    if (cond < 0x0e) { /* not always */
        int label_match = gen_new_label();
        label_continue = gen_new_label();
        arm_gen_test_cc(cond, label_match);
        /* nomatch: */
        gen_mov_fp2fp(s, type, rd, rm);
        tcg_gen_br(label_continue);
        gen_set_label(label_match);
    }

    gen_mov_fp2fp(s, type, rd, rn);

    if (cond < 0x0e) { /* continue */
        gen_set_label(label_continue);
    }
}

/* C3.6.25 Floating-point data-processing (1 source) - single precision */
static void handle_fp_1src_single(DisasContext *s, int opcode, int rd, int rn)
{
    TCGv_ptr fpst;
    TCGv_i32 tcg_op;
    TCGv_i32 tcg_res;

    fpst = get_fpstatus_ptr();
    tcg_op = read_fp_sreg(s, rn);
    tcg_res = tcg_temp_new_i32();

    switch (opcode) {
    case 0x0: /* FMOV */
        tcg_gen_mov_i32(tcg_res, tcg_op);
        break;
    case 0x1: /* FABS */
        gen_helper_vfp_abss(tcg_res, tcg_op);
        break;
    case 0x2: /* FNEG */
        gen_helper_vfp_negs(tcg_res, tcg_op);
        break;
    case 0x3: /* FSQRT */
        gen_helper_vfp_sqrts(tcg_res, tcg_op, cpu_env);
        break;
    case 0x8: /* FRINTN */
    case 0x9: /* FRINTP */
    case 0xa: /* FRINTM */
    case 0xb: /* FRINTZ */
    case 0xc: /* FRINTA */
    {
        TCGv_i32 tcg_rmode = tcg_const_i32(arm_rmode_to_sf(opcode & 7));

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);
        gen_helper_rints(tcg_res, tcg_op, fpst);

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);
        tcg_temp_free_i32(tcg_rmode);
        break;
    }
    case 0xe: /* FRINTX */
        gen_helper_rints_exact(tcg_res, tcg_op, fpst);
        break;
    case 0xf: /* FRINTI */
        gen_helper_rints(tcg_res, tcg_op, fpst);
        break;
    default:
        abort();
    }

    write_fp_sreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i32(tcg_op);
    tcg_temp_free_i32(tcg_res);
}

/* C3.6.25 Floating-point data-processing (1 source) - double precision */
static void handle_fp_1src_double(DisasContext *s, int opcode, int rd, int rn)
{
    TCGv_ptr fpst;
    TCGv_i64 tcg_op;
    TCGv_i64 tcg_res;

    fpst = get_fpstatus_ptr();
    tcg_op = read_fp_dreg(s, rn);
    tcg_res = tcg_temp_new_i64();

    switch (opcode) {
    case 0x0: /* FMOV */
        tcg_gen_mov_i64(tcg_res, tcg_op);
        break;
    case 0x1: /* FABS */
        gen_helper_vfp_absd(tcg_res, tcg_op);
        break;
    case 0x2: /* FNEG */
        gen_helper_vfp_negd(tcg_res, tcg_op);
        break;
    case 0x3: /* FSQRT */
        gen_helper_vfp_sqrtd(tcg_res, tcg_op, cpu_env);
        break;
    case 0x8: /* FRINTN */
    case 0x9: /* FRINTP */
    case 0xa: /* FRINTM */
    case 0xb: /* FRINTZ */
    case 0xc: /* FRINTA */
    {
        TCGv_i32 tcg_rmode = tcg_const_i32(arm_rmode_to_sf(opcode & 7));

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);
        gen_helper_rintd(tcg_res, tcg_op, fpst);

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);
        tcg_temp_free_i32(tcg_rmode);
        break;
    }
    case 0xe: /* FRINTX */
        gen_helper_rintd_exact(tcg_res, tcg_op, fpst);
        break;
    case 0xf: /* FRINTI */
        gen_helper_rintd(tcg_res, tcg_op, fpst);
        break;
    default:
        abort();
    }

    write_fp_dreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i64(tcg_op);
    tcg_temp_free_i64(tcg_res);
}

static void handle_fp_fcvt(DisasContext *s, int opcode,
                           int rd, int rn, int dtype, int ntype)
{
    switch (ntype) {
    case 0x0:
    {
        TCGv_i32 tcg_rn = read_fp_sreg(s, rn);
        if (dtype == 1) {
            /* Single to double */
            TCGv_i64 tcg_rd = tcg_temp_new_i64();
            gen_helper_vfp_fcvtds(tcg_rd, tcg_rn, cpu_env);
            write_fp_dreg(s, rd, tcg_rd);
            tcg_temp_free_i64(tcg_rd);
        } else {
            /* Single to half */
            TCGv_i32 tcg_rd = tcg_temp_new_i32();
            gen_helper_vfp_fcvt_f32_to_f16(tcg_rd, tcg_rn, cpu_env);
            /* write_fp_sreg is OK here because top half of tcg_rd is zero */
            write_fp_sreg(s, rd, tcg_rd);
            tcg_temp_free_i32(tcg_rd);
        }
        tcg_temp_free_i32(tcg_rn);
        break;
    }
    case 0x1:
    {
        TCGv_i64 tcg_rn = read_fp_dreg(s, rn);
        TCGv_i32 tcg_rd = tcg_temp_new_i32();
        if (dtype == 0) {
            /* Double to single */
            gen_helper_vfp_fcvtsd(tcg_rd, tcg_rn, cpu_env);
        } else {
            /* Double to half */
            gen_helper_vfp_fcvt_f64_to_f16(tcg_rd, tcg_rn, cpu_env);
            /* write_fp_sreg is OK here because top half of tcg_rd is zero */
        }
        write_fp_sreg(s, rd, tcg_rd);
        tcg_temp_free_i32(tcg_rd);
        tcg_temp_free_i64(tcg_rn);
        break;
    }
    case 0x3:
    {
        TCGv_i32 tcg_rn = read_fp_sreg(s, rn);
        tcg_gen_ext16u_i32(tcg_rn, tcg_rn);
        if (dtype == 0) {
            /* Half to single */
            TCGv_i32 tcg_rd = tcg_temp_new_i32();
            gen_helper_vfp_fcvt_f16_to_f32(tcg_rd, tcg_rn, cpu_env);
            write_fp_sreg(s, rd, tcg_rd);
            tcg_temp_free_i32(tcg_rd);
        } else {
            /* Half to double */
            TCGv_i64 tcg_rd = tcg_temp_new_i64();
            gen_helper_vfp_fcvt_f16_to_f64(tcg_rd, tcg_rn, cpu_env);
            write_fp_dreg(s, rd, tcg_rd);
            tcg_temp_free_i64(tcg_rd);
        }
        tcg_temp_free_i32(tcg_rn);
        break;
    }
    default:
        abort();
    }
}

/* C3.6.25 Floating point data-processing (1 source)
 *   31  30  29 28       24 23  22  21 20    15 14       10 9    5 4    0
 * +---+---+---+-----------+------+---+--------+-----------+------+------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 | opcode | 1 0 0 0 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+--------+-----------+------+------+
 */
static void disas_fp_1src(DisasContext *s, uint32_t insn)
{
    int type = extract32(insn, 22, 2);
    int opcode = extract32(insn, 15, 6);
    int rn = extract32(insn, 5, 5);
    int rd = extract32(insn, 0, 5);

    switch (opcode) {
    case 0x4: case 0x5: case 0x7:
    {
        /* FCVT between half, single and double precision */
        int dtype = extract32(opcode, 0, 2);
        if (type == 2 || dtype == type) {
            unallocated_encoding(s);
            return;
        }
        handle_fp_fcvt(s, opcode, rd, rn, dtype, type);
        break;
    }
    case 0x0 ... 0x3:
    case 0x8 ... 0xc:
    case 0xe ... 0xf:
        /* 32-to-32 and 64-to-64 ops */
        switch (type) {
        case 0:
            handle_fp_1src_single(s, opcode, rd, rn);
            break;
        case 1:
            handle_fp_1src_double(s, opcode, rd, rn);
            break;
        default:
            unallocated_encoding(s);
        }
        break;
    default:
        unallocated_encoding(s);
        break;
    }
}

/* C3.6.26 Floating-point data-processing (2 source) - single precision */
static void handle_fp_2src_single(DisasContext *s, int opcode,
                                  int rd, int rn, int rm)
{
    TCGv_i32 tcg_op1;
    TCGv_i32 tcg_op2;
    TCGv_i32 tcg_res;
    TCGv_ptr fpst;

    tcg_res = tcg_temp_new_i32();
    fpst = get_fpstatus_ptr();
    tcg_op1 = read_fp_sreg(s, rn);
    tcg_op2 = read_fp_sreg(s, rm);

    switch (opcode) {
    case 0x0: /* FMUL */
        gen_helper_vfp_muls(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x1: /* FDIV */
        gen_helper_vfp_divs(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x2: /* FADD */
        gen_helper_vfp_adds(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x3: /* FSUB */
        gen_helper_vfp_subs(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x4: /* FMAX */
        gen_helper_vfp_maxs(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x5: /* FMIN */
        gen_helper_vfp_mins(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x6: /* FMAXNM */
        gen_helper_vfp_maxnums(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x7: /* FMINNM */
        gen_helper_vfp_minnums(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x8: /* FNMUL */
        gen_helper_vfp_muls(tcg_res, tcg_op1, tcg_op2, fpst);
        gen_helper_vfp_negs(tcg_res, tcg_res);
        break;
    }

    write_fp_sreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i32(tcg_op1);
    tcg_temp_free_i32(tcg_op2);
    tcg_temp_free_i32(tcg_res);
}

/* C3.6.26 Floating-point data-processing (2 source) - double precision */
static void handle_fp_2src_double(DisasContext *s, int opcode,
                                  int rd, int rn, int rm)
{
    TCGv_i64 tcg_op1;
    TCGv_i64 tcg_op2;
    TCGv_i64 tcg_res;
    TCGv_ptr fpst;

    tcg_res = tcg_temp_new_i64();
    fpst = get_fpstatus_ptr();
    tcg_op1 = read_fp_dreg(s, rn);
    tcg_op2 = read_fp_dreg(s, rm);

    switch (opcode) {
    case 0x0: /* FMUL */
        gen_helper_vfp_muld(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x1: /* FDIV */
        gen_helper_vfp_divd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x2: /* FADD */
        gen_helper_vfp_addd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x3: /* FSUB */
        gen_helper_vfp_subd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x4: /* FMAX */
        gen_helper_vfp_maxd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x5: /* FMIN */
        gen_helper_vfp_mind(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x6: /* FMAXNM */
        gen_helper_vfp_maxnumd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x7: /* FMINNM */
        gen_helper_vfp_minnumd(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x8: /* FNMUL */
        gen_helper_vfp_muld(tcg_res, tcg_op1, tcg_op2, fpst);
        gen_helper_vfp_negd(tcg_res, tcg_res);
        break;
    }

    write_fp_dreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i64(tcg_op1);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_res);
}

/* C3.6.26 Floating point data-processing (2 source)
 *   31  30  29 28       24 23  22  21 20  16 15    12 11 10 9    5 4    0
 * +---+---+---+-----------+------+---+------+--------+-----+------+------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 |  Rm  | opcode | 1 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+------+--------+-----+------+------+
 */
static void disas_fp_2src(DisasContext *s, uint32_t insn)
{
    int type = extract32(insn, 22, 2);
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    int opcode = extract32(insn, 12, 4);

    if (opcode > 8) {
        unallocated_encoding(s);
        return;
    }

    switch (type) {
    case 0:
        handle_fp_2src_single(s, opcode, rd, rn, rm);
        break;
    case 1:
        handle_fp_2src_double(s, opcode, rd, rn, rm);
        break;
    default:
        unallocated_encoding(s);
    }
}

/* C3.6.27 Floating-point data-processing (3 source) - single precision */
static void handle_fp_3src_single(DisasContext *s, bool o0, bool o1,
                                  int rd, int rn, int rm, int ra)
{
    TCGv_i32 tcg_op1, tcg_op2, tcg_op3;
    TCGv_i32 tcg_res = tcg_temp_new_i32();
    TCGv_ptr fpst = get_fpstatus_ptr();

    tcg_op1 = read_fp_sreg(s, rn);
    tcg_op2 = read_fp_sreg(s, rm);
    tcg_op3 = read_fp_sreg(s, ra);

    /* These are fused multiply-add, and must be done as one
     * floating point operation with no rounding between the
     * multiplication and addition steps.
     * NB that doing the negations here as separate steps is
     * correct : an input NaN should come out with its sign bit
     * flipped if it is a negated-input.
     */
    if (o1 == true) {
        gen_helper_vfp_negs(tcg_op3, tcg_op3);
    }

    if (o0 != o1) {
        gen_helper_vfp_negs(tcg_op1, tcg_op1);
    }

    gen_helper_vfp_muladds(tcg_res, tcg_op1, tcg_op2, tcg_op3, fpst);

    write_fp_sreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i32(tcg_op1);
    tcg_temp_free_i32(tcg_op2);
    tcg_temp_free_i32(tcg_op3);
    tcg_temp_free_i32(tcg_res);
}

/* C3.6.27 Floating-point data-processing (3 source) - double precision */
static void handle_fp_3src_double(DisasContext *s, bool o0, bool o1,
                                  int rd, int rn, int rm, int ra)
{
    TCGv_i64 tcg_op1, tcg_op2, tcg_op3;
    TCGv_i64 tcg_res = tcg_temp_new_i64();
    TCGv_ptr fpst = get_fpstatus_ptr();

    tcg_op1 = read_fp_dreg(s, rn);
    tcg_op2 = read_fp_dreg(s, rm);
    tcg_op3 = read_fp_dreg(s, ra);

    /* These are fused multiply-add, and must be done as one
     * floating point operation with no rounding between the
     * multiplication and addition steps.
     * NB that doing the negations here as separate steps is
     * correct : an input NaN should come out with its sign bit
     * flipped if it is a negated-input.
     */
    if (o1 == true) {
        gen_helper_vfp_negd(tcg_op3, tcg_op3);
    }

    if (o0 != o1) {
        gen_helper_vfp_negd(tcg_op1, tcg_op1);
    }

    gen_helper_vfp_muladdd(tcg_res, tcg_op1, tcg_op2, tcg_op3, fpst);

    write_fp_dreg(s, rd, tcg_res);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i64(tcg_op1);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_op3);
    tcg_temp_free_i64(tcg_res);
}

/* C3.6.27 Floating point data-processing (3 source)
 *   31  30  29 28       24 23  22  21  20  16  15  14  10 9    5 4    0
 * +---+---+---+-----------+------+----+------+----+------+------+------+
 * | M | 0 | S | 1 1 1 1 1 | type | o1 |  Rm  | o0 |  Ra  |  Rn  |  Rd  |
 * +---+---+---+-----------+------+----+------+----+------+------+------+
 */
static void disas_fp_3src(DisasContext *s, uint32_t insn)
{
    int type = extract32(insn, 22, 2);
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int ra = extract32(insn, 10, 5);
    int rm = extract32(insn, 16, 5);
    bool o0 = extract32(insn, 15, 1);
    bool o1 = extract32(insn, 21, 1);

    switch (type) {
    case 0:
        handle_fp_3src_single(s, o0, o1, rd, rn, rm, ra);
        break;
    case 1:
        handle_fp_3src_double(s, o0, o1, rd, rn, rm, ra);
        break;
    default:
        unallocated_encoding(s);
    }
}

/* C3.6.28 Floating point immediate
 *   31  30  29 28       24 23  22  21 20        13 12   10 9    5 4    0
 * +---+---+---+-----------+------+---+------------+-------+------+------+
 * | M | 0 | S | 1 1 1 1 0 | type | 1 |    imm8    | 1 0 0 | imm5 |  Rd  |
 * +---+---+---+-----------+------+---+------------+-------+------+------+
 */
static void disas_fp_imm(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int imm8 = extract32(insn, 13, 8);
    int is_double = extract32(insn, 22, 2);
    uint64_t imm;
    TCGv_i64 tcg_res;

    if (is_double > 1) {
        unallocated_encoding(s);
        return;
    }

    /* The imm8 encodes the sign bit, enough bits to represent
     * an exponent in the range 01....1xx to 10....0xx,
     * and the most significant 4 bits of the mantissa; see
     * VFPExpandImm() in the v8 ARM ARM.
     */
    if (is_double) {
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
            (extract32(imm8, 6, 1) ? 0x3fc0 : 0x4000) |
            extract32(imm8, 0, 6);
        imm <<= 48;
    } else {
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
            (extract32(imm8, 6, 1) ? 0x3e00 : 0x4000) |
            (extract32(imm8, 0, 6) << 3);
        imm <<= 16;
    }

    tcg_res = tcg_const_i64(imm);
    write_fp_dreg(s, rd, tcg_res);
    tcg_temp_free_i64(tcg_res);
}

/* Handle floating point <=> fixed point conversions. Note that we can
 * also deal with fp <=> integer conversions as a special case (scale == 64)
 * OPTME: consider handling that special case specially or at least skipping
 * the call to scalbn in the helpers for zero shifts.
 */
static void handle_fpfpcvt(DisasContext *s, int rd, int rn, int opcode,
                           bool itof, int rmode, int scale, int sf, int type)
{
    bool is_signed = !(opcode & 1);
    bool is_double = type;
    TCGv_ptr tcg_fpstatus;
    TCGv_i32 tcg_shift;

    tcg_fpstatus = get_fpstatus_ptr();

    tcg_shift = tcg_const_i32(64 - scale);

    if (itof) {
        TCGv_i64 tcg_int = cpu_reg(s, rn);
        if (!sf) {
            TCGv_i64 tcg_extend = new_tmp_a64(s);

            if (is_signed) {
                tcg_gen_ext32s_i64(tcg_extend, tcg_int);
            } else {
                tcg_gen_ext32u_i64(tcg_extend, tcg_int);
            }

            tcg_int = tcg_extend;
        }

        if (is_double) {
            TCGv_i64 tcg_double = tcg_temp_new_i64();
            if (is_signed) {
                gen_helper_vfp_sqtod(tcg_double, tcg_int,
                                     tcg_shift, tcg_fpstatus);
            } else {
                gen_helper_vfp_uqtod(tcg_double, tcg_int,
                                     tcg_shift, tcg_fpstatus);
            }
            write_fp_dreg(s, rd, tcg_double);
            tcg_temp_free_i64(tcg_double);
        } else {
            TCGv_i32 tcg_single = tcg_temp_new_i32();
            if (is_signed) {
                gen_helper_vfp_sqtos(tcg_single, tcg_int,
                                     tcg_shift, tcg_fpstatus);
            } else {
                gen_helper_vfp_uqtos(tcg_single, tcg_int,
                                     tcg_shift, tcg_fpstatus);
            }
            write_fp_sreg(s, rd, tcg_single);
            tcg_temp_free_i32(tcg_single);
        }
    } else {
        TCGv_i64 tcg_int = cpu_reg(s, rd);
        TCGv_i32 tcg_rmode;

        if (extract32(opcode, 2, 1)) {
            /* There are too many rounding modes to all fit into rmode,
             * so FCVTA[US] is a special case.
             */
            rmode = FPROUNDING_TIEAWAY;
        }

        tcg_rmode = tcg_const_i32(arm_rmode_to_sf(rmode));

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);

        if (is_double) {
            TCGv_i64 tcg_double = read_fp_dreg(s, rn);
            if (is_signed) {
                if (!sf) {
                    gen_helper_vfp_tosld(tcg_int, tcg_double,
                                         tcg_shift, tcg_fpstatus);
                } else {
                    gen_helper_vfp_tosqd(tcg_int, tcg_double,
                                         tcg_shift, tcg_fpstatus);
                }
            } else {
                if (!sf) {
                    gen_helper_vfp_tould(tcg_int, tcg_double,
                                         tcg_shift, tcg_fpstatus);
                } else {
                    gen_helper_vfp_touqd(tcg_int, tcg_double,
                                         tcg_shift, tcg_fpstatus);
                }
            }
            tcg_temp_free_i64(tcg_double);
        } else {
            TCGv_i32 tcg_single = read_fp_sreg(s, rn);
            if (sf) {
                if (is_signed) {
                    gen_helper_vfp_tosqs(tcg_int, tcg_single,
                                         tcg_shift, tcg_fpstatus);
                } else {
                    gen_helper_vfp_touqs(tcg_int, tcg_single,
                                         tcg_shift, tcg_fpstatus);
                }
            } else {
                TCGv_i32 tcg_dest = tcg_temp_new_i32();
                if (is_signed) {
                    gen_helper_vfp_tosls(tcg_dest, tcg_single,
                                         tcg_shift, tcg_fpstatus);
                } else {
                    gen_helper_vfp_touls(tcg_dest, tcg_single,
                                         tcg_shift, tcg_fpstatus);
                }
                tcg_gen_extu_i32_i64(tcg_int, tcg_dest);
                tcg_temp_free_i32(tcg_dest);
            }
            tcg_temp_free_i32(tcg_single);
        }

        gen_helper_set_rmode(tcg_rmode, tcg_rmode, cpu_env);
        tcg_temp_free_i32(tcg_rmode);

        if (!sf) {
            tcg_gen_ext32u_i64(tcg_int, tcg_int);
        }
    }

    tcg_temp_free_ptr(tcg_fpstatus);
    tcg_temp_free_i32(tcg_shift);
}

/* C3.6.29 Floating point <-> fixed point conversions
 *   31   30  29 28       24 23  22  21 20   19 18    16 15   10 9    5 4    0
 * +----+---+---+-----------+------+---+-------+--------+-------+------+------+
 * | sf | 0 | S | 1 1 1 1 0 | type | 0 | rmode | opcode | scale |  Rn  |  Rd  |
 * +----+---+---+-----------+------+---+-------+--------+-------+------+------+
 */
static void disas_fp_fixed_conv(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int scale = extract32(insn, 10, 6);
    int opcode = extract32(insn, 16, 3);
    int rmode = extract32(insn, 19, 2);
    int type = extract32(insn, 22, 2);
    bool sbit = extract32(insn, 29, 1);
    bool sf = extract32(insn, 31, 1);
    bool itof;

    if (sbit || (type > 1)
        || (!sf && scale < 32)) {
        unallocated_encoding(s);
        return;
    }

    switch ((rmode << 3) | opcode) {
    case 0x2: /* SCVTF */
    case 0x3: /* UCVTF */
        itof = true;
        break;
    case 0x18: /* FCVTZS */
    case 0x19: /* FCVTZU */
        itof = false;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    handle_fpfpcvt(s, rd, rn, opcode, itof, FPROUNDING_ZERO, scale, sf, type);
}

static void handle_fmov(DisasContext *s, int rd, int rn, int type, bool itof)
{
    /* FMOV: gpr to or from float, double, or top half of quad fp reg,
     * without conversion.
     */

    if (itof) {
        TCGv_i64 tcg_rn = cpu_reg(s, rn);

        switch (type) {
        case 0:
        {
            /* 32 bit */
            TCGv_i64 tmp = tcg_temp_new_i64();
            tcg_gen_ext32u_i64(tmp, tcg_rn);
            tcg_gen_st_i64(tmp, cpu_env, fp_reg_offset(rd, MO_64));
            tcg_gen_movi_i64(tmp, 0);
            tcg_gen_st_i64(tmp, cpu_env, fp_reg_hi_offset(rd));
            tcg_temp_free_i64(tmp);
            break;
        }
        case 1:
        {
            /* 64 bit */
            TCGv_i64 tmp = tcg_const_i64(0);
            tcg_gen_st_i64(tcg_rn, cpu_env, fp_reg_offset(rd, MO_64));
            tcg_gen_st_i64(tmp, cpu_env, fp_reg_hi_offset(rd));
            tcg_temp_free_i64(tmp);
            break;
        }
        case 2:
            /* 64 bit to top half. */
            tcg_gen_st_i64(tcg_rn, cpu_env, fp_reg_hi_offset(rd));
            break;
        }
    } else {
        TCGv_i64 tcg_rd = cpu_reg(s, rd);

        switch (type) {
        case 0:
            /* 32 bit */
            tcg_gen_ld32u_i64(tcg_rd, cpu_env, fp_reg_offset(rn, MO_32));
            break;
        case 1:
            /* 64 bit */
            tcg_gen_ld_i64(tcg_rd, cpu_env, fp_reg_offset(rn, MO_64));
            break;
        case 2:
            /* 64 bits from top half */
            tcg_gen_ld_i64(tcg_rd, cpu_env, fp_reg_hi_offset(rn));
            break;
        }
    }
}

/* C3.6.30 Floating point <-> integer conversions
 *   31   30  29 28       24 23  22  21 20   19 18 16 15         10 9  5 4  0
 * +----+---+---+-----------+------+---+-------+-----+-------------+----+----+
 * | sf | 0 | S | 1 1 1 1 0 | type | 1 | rmode | opc | 0 0 0 0 0 0 | Rn | Rd |
 * +----+---+---+-----------+------+---+-------+-----+-------------+----+----+
 */
static void disas_fp_int_conv(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 16, 3);
    int rmode = extract32(insn, 19, 2);
    int type = extract32(insn, 22, 2);
    bool sbit = extract32(insn, 29, 1);
    bool sf = extract32(insn, 31, 1);

    if (sbit) {
        unallocated_encoding(s);
        return;
    }

    if (opcode > 5) {
        /* FMOV */
        bool itof = opcode & 1;

        if (rmode >= 2) {
            unallocated_encoding(s);
            return;
        }

        switch (sf << 3 | type << 1 | rmode) {
        case 0x0: /* 32 bit */
        case 0xa: /* 64 bit */
        case 0xd: /* 64 bit to top half of quad */
            break;
        default:
            /* all other sf/type/rmode combinations are invalid */
            unallocated_encoding(s);
            break;
        }

        handle_fmov(s, rd, rn, type, itof);
    } else {
        /* actual FP conversions */
        bool itof = extract32(opcode, 1, 1);

        if (type > 1 || (rmode != 0 && opcode > 1)) {
            unallocated_encoding(s);
            return;
        }

        handle_fpfpcvt(s, rd, rn, opcode, itof, rmode, 64, sf, type);
    }
}

/* FP-specific subcases of table C3-6 (SIMD and FP data processing)
 *   31  30  29 28     25 24                          0
 * +---+---+---+---------+-----------------------------+
 * |   | 0 |   | 1 1 1 1 |                             |
 * +---+---+---+---------+-----------------------------+
 */
static void disas_data_proc_fp(DisasContext *s, uint32_t insn)
{
    if (extract32(insn, 24, 1)) {
        /* Floating point data-processing (3 source) */
        disas_fp_3src(s, insn);
    } else if (extract32(insn, 21, 1) == 0) {
        /* Floating point to fixed point conversions */
        disas_fp_fixed_conv(s, insn);
    } else {
        switch (extract32(insn, 10, 2)) {
        case 1:
            /* Floating point conditional compare */
            disas_fp_ccomp(s, insn);
            break;
        case 2:
            /* Floating point data-processing (2 source) */
            disas_fp_2src(s, insn);
            break;
        case 3:
            /* Floating point conditional select */
            disas_fp_csel(s, insn);
            break;
        case 0:
            switch (ctz32(extract32(insn, 12, 4))) {
            case 0: /* [15:12] == xxx1 */
                /* Floating point immediate */
                disas_fp_imm(s, insn);
                break;
            case 1: /* [15:12] == xx10 */
                /* Floating point compare */
                disas_fp_compare(s, insn);
                break;
            case 2: /* [15:12] == x100 */
                /* Floating point data-processing (1 source) */
                disas_fp_1src(s, insn);
                break;
            case 3: /* [15:12] == 1000 */
                unallocated_encoding(s);
                break;
            default: /* [15:12] == 0000 */
                /* Floating point <-> integer conversions */
                disas_fp_int_conv(s, insn);
                break;
            }
            break;
        }
    }
}

static void do_ext64(DisasContext *s, TCGv_i64 tcg_left, TCGv_i64 tcg_right,
                     int pos)
{
    /* Extract 64 bits from the middle of two concatenated 64 bit
     * vector register slices left:right. The extracted bits start
     * at 'pos' bits into the right (least significant) side.
     * We return the result in tcg_right, and guarantee not to
     * trash tcg_left.
     */
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    assert(pos > 0 && pos < 64);

    tcg_gen_shri_i64(tcg_right, tcg_right, pos);
    tcg_gen_shli_i64(tcg_tmp, tcg_left, 64 - pos);
    tcg_gen_or_i64(tcg_right, tcg_right, tcg_tmp);

    tcg_temp_free_i64(tcg_tmp);
}

/* C3.6.1 EXT
 *   31  30 29         24 23 22  21 20  16 15  14  11 10  9    5 4    0
 * +---+---+-------------+-----+---+------+---+------+---+------+------+
 * | 0 | Q | 1 0 1 1 1 0 | op2 | 0 |  Rm  | 0 | imm4 | 0 |  Rn  |  Rd  |
 * +---+---+-------------+-----+---+------+---+------+---+------+------+
 */
static void disas_simd_ext(DisasContext *s, uint32_t insn)
{
    int is_q = extract32(insn, 30, 1);
    int op2 = extract32(insn, 22, 2);
    int imm4 = extract32(insn, 11, 4);
    int rm = extract32(insn, 16, 5);
    int rn = extract32(insn, 5, 5);
    int rd = extract32(insn, 0, 5);
    int pos = imm4 << 3;
    TCGv_i64 tcg_resl, tcg_resh;

    if (op2 != 0 || (!is_q && extract32(imm4, 3, 1))) {
        unallocated_encoding(s);
        return;
    }

    tcg_resh = tcg_temp_new_i64();
    tcg_resl = tcg_temp_new_i64();

    /* Vd gets bits starting at pos bits into Vm:Vn. This is
     * either extracting 128 bits from a 128:128 concatenation, or
     * extracting 64 bits from a 64:64 concatenation.
     */
    if (!is_q) {
        read_vec_element(s, tcg_resl, rn, 0, MO_64);
        if (pos != 0) {
            read_vec_element(s, tcg_resh, rm, 0, MO_64);
            do_ext64(s, tcg_resh, tcg_resl, pos);
        }
        tcg_gen_movi_i64(tcg_resh, 0);
    } else {
        TCGv_i64 tcg_hh;
        typedef struct {
            int reg;
            int elt;
        } EltPosns;
        EltPosns eltposns[] = { {rn, 0}, {rn, 1}, {rm, 0}, {rm, 1} };
        EltPosns *elt = eltposns;

        if (pos >= 64) {
            elt++;
            pos -= 64;
        }

        read_vec_element(s, tcg_resl, elt->reg, elt->elt, MO_64);
        elt++;
        read_vec_element(s, tcg_resh, elt->reg, elt->elt, MO_64);
        elt++;
        if (pos != 0) {
            do_ext64(s, tcg_resh, tcg_resl, pos);
            tcg_hh = tcg_temp_new_i64();
            read_vec_element(s, tcg_hh, elt->reg, elt->elt, MO_64);
            do_ext64(s, tcg_hh, tcg_resh, pos);
            tcg_temp_free_i64(tcg_hh);
        }
    }

    write_vec_element(s, tcg_resl, rd, 0, MO_64);
    tcg_temp_free_i64(tcg_resl);
    write_vec_element(s, tcg_resh, rd, 1, MO_64);
    tcg_temp_free_i64(tcg_resh);
}

/* C3.6.2 TBL/TBX
 *   31  30 29         24 23 22  21 20  16 15  14 13  12  11 10 9    5 4    0
 * +---+---+-------------+-----+---+------+---+-----+----+-----+------+------+
 * | 0 | Q | 0 0 1 1 1 0 | op2 | 0 |  Rm  | 0 | len | op | 0 0 |  Rn  |  Rd  |
 * +---+---+-------------+-----+---+------+---+-----+----+-----+------+------+
 */
static void disas_simd_tb(DisasContext *s, uint32_t insn)
{
    int op2 = extract32(insn, 22, 2);
    int is_q = extract32(insn, 30, 1);
    int rm = extract32(insn, 16, 5);
    int rn = extract32(insn, 5, 5);
    int rd = extract32(insn, 0, 5);
    int is_tblx = extract32(insn, 12, 1);
    int len = extract32(insn, 13, 2);
    TCGv_i64 tcg_resl, tcg_resh, tcg_idx;
    TCGv_i32 tcg_regno, tcg_numregs;

    if (op2 != 0) {
        unallocated_encoding(s);
        return;
    }

    /* This does a table lookup: for every byte element in the input
     * we index into a table formed from up to four vector registers,
     * and then the output is the result of the lookups. Our helper
     * function does the lookup operation for a single 64 bit part of
     * the input.
     */
    tcg_resl = tcg_temp_new_i64();
    tcg_resh = tcg_temp_new_i64();

    if (is_tblx) {
        read_vec_element(s, tcg_resl, rd, 0, MO_64);
    } else {
        tcg_gen_movi_i64(tcg_resl, 0);
    }
    if (is_tblx && is_q) {
        read_vec_element(s, tcg_resh, rd, 1, MO_64);
    } else {
        tcg_gen_movi_i64(tcg_resh, 0);
    }

    tcg_idx = tcg_temp_new_i64();
    tcg_regno = tcg_const_i32(rn);
    tcg_numregs = tcg_const_i32(len + 1);
    read_vec_element(s, tcg_idx, rm, 0, MO_64);
    gen_helper_simd_tbl(tcg_resl, cpu_env, tcg_resl, tcg_idx,
                        tcg_regno, tcg_numregs);
    if (is_q) {
        read_vec_element(s, tcg_idx, rm, 1, MO_64);
        gen_helper_simd_tbl(tcg_resh, cpu_env, tcg_resh, tcg_idx,
                            tcg_regno, tcg_numregs);
    }
    tcg_temp_free_i64(tcg_idx);
    tcg_temp_free_i32(tcg_regno);
    tcg_temp_free_i32(tcg_numregs);

    write_vec_element(s, tcg_resl, rd, 0, MO_64);
    tcg_temp_free_i64(tcg_resl);
    write_vec_element(s, tcg_resh, rd, 1, MO_64);
    tcg_temp_free_i64(tcg_resh);
}

/* C3.6.3 ZIP/UZP/TRN
 *   31  30 29         24 23  22  21 20   16 15 14 12 11 10 9    5 4    0
 * +---+---+-------------+------+---+------+---+------------------+------+
 * | 0 | Q | 0 0 1 1 1 0 | size | 0 |  Rm  | 0 | opc | 1 0 |  Rn  |  Rd  |
 * +---+---+-------------+------+---+------+---+------------------+------+
 */
static void disas_simd_zip_trn(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    int size = extract32(insn, 22, 2);
    /* opc field bits [1:0] indicate ZIP/UZP/TRN;
     * bit 2 indicates 1 vs 2 variant of the insn.
     */
    int opcode = extract32(insn, 12, 2);
    bool part = extract32(insn, 14, 1);
    bool is_q = extract32(insn, 30, 1);
    int esize = 8 << size;
    int i, ofs;
    int datasize = is_q ? 128 : 64;
    int elements = datasize / esize;
    TCGv_i64 tcg_res, tcg_resl, tcg_resh;

    if (opcode == 0 || (size == 3 && !is_q)) {
        unallocated_encoding(s);
        return;
    }

    tcg_resl = tcg_const_i64(0);
    tcg_resh = tcg_const_i64(0);
    tcg_res = tcg_temp_new_i64();

    for (i = 0; i < elements; i++) {
        switch (opcode) {
        case 1: /* UZP1/2 */
        {
            int midpoint = elements / 2;
            if (i < midpoint) {
                read_vec_element(s, tcg_res, rn, 2 * i + part, size);
            } else {
                read_vec_element(s, tcg_res, rm,
                                 2 * (i - midpoint) + part, size);
            }
            break;
        }
        case 2: /* TRN1/2 */
            if (i & 1) {
                read_vec_element(s, tcg_res, rm, (i & ~1) + part, size);
            } else {
                read_vec_element(s, tcg_res, rn, (i & ~1) + part, size);
            }
            break;
        case 3: /* ZIP1/2 */
        {
            int base = part * elements / 2;
            if (i & 1) {
                read_vec_element(s, tcg_res, rm, base + (i >> 1), size);
            } else {
                read_vec_element(s, tcg_res, rn, base + (i >> 1), size);
            }
            break;
        }
        default:
            g_assert_not_reached();
        }

        ofs = i * esize;
        if (ofs < 64) {
            tcg_gen_shli_i64(tcg_res, tcg_res, ofs);
            tcg_gen_or_i64(tcg_resl, tcg_resl, tcg_res);
        } else {
            tcg_gen_shli_i64(tcg_res, tcg_res, ofs - 64);
            tcg_gen_or_i64(tcg_resh, tcg_resh, tcg_res);
        }
    }

    tcg_temp_free_i64(tcg_res);

    write_vec_element(s, tcg_resl, rd, 0, MO_64);
    tcg_temp_free_i64(tcg_resl);
    write_vec_element(s, tcg_resh, rd, 1, MO_64);
    tcg_temp_free_i64(tcg_resh);
}

static void do_minmaxop(DisasContext *s, TCGv_i32 tcg_elt1, TCGv_i32 tcg_elt2,
                        int opc, bool is_min, TCGv_ptr fpst)
{
    /* Helper function for disas_simd_across_lanes: do a single precision
     * min/max operation on the specified two inputs,
     * and return the result in tcg_elt1.
     */
    if (opc == 0xc) {
        if (is_min) {
            gen_helper_vfp_minnums(tcg_elt1, tcg_elt1, tcg_elt2, fpst);
        } else {
            gen_helper_vfp_maxnums(tcg_elt1, tcg_elt1, tcg_elt2, fpst);
        }
    } else {
        assert(opc == 0xf);
        if (is_min) {
            gen_helper_vfp_mins(tcg_elt1, tcg_elt1, tcg_elt2, fpst);
        } else {
            gen_helper_vfp_maxs(tcg_elt1, tcg_elt1, tcg_elt2, fpst);
        }
    }
}

/* C3.6.4 AdvSIMD across lanes
 *   31  30  29 28       24 23  22 21       17 16    12 11 10 9    5 4    0
 * +---+---+---+-----------+------+-----------+--------+-----+------+------+
 * | 0 | Q | U | 0 1 1 1 0 | size | 1 1 0 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+-----------+--------+-----+------+------+
 */
static void disas_simd_across_lanes(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int size = extract32(insn, 22, 2);
    int opcode = extract32(insn, 12, 5);
    bool is_q = extract32(insn, 30, 1);
    bool is_u = extract32(insn, 29, 1);
    bool is_fp = false;
    bool is_min = false;
    int esize;
    int elements;
    int i;
    TCGv_i64 tcg_res, tcg_elt;

    switch (opcode) {
    case 0x1b: /* ADDV */
        if (is_u) {
            unallocated_encoding(s);
            return;
        }
        /* fall through */
    case 0x3: /* SADDLV, UADDLV */
    case 0xa: /* SMAXV, UMAXV */
    case 0x1a: /* SMINV, UMINV */
        if (size == 3 || (size == 2 && !is_q)) {
            unallocated_encoding(s);
            return;
        }
        break;
    case 0xc: /* FMAXNMV, FMINNMV */
    case 0xf: /* FMAXV, FMINV */
        if (!is_u || !is_q || extract32(size, 0, 1)) {
            unallocated_encoding(s);
            return;
        }
        /* Bit 1 of size field encodes min vs max, and actual size is always
         * 32 bits: adjust the size variable so following code can rely on it
         */
        is_min = extract32(size, 1, 1);
        is_fp = true;
        size = 2;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    esize = 8 << size;
    elements = (is_q ? 128 : 64) / esize;

    tcg_res = tcg_temp_new_i64();
    tcg_elt = tcg_temp_new_i64();

    /* These instructions operate across all lanes of a vector
     * to produce a single result. We can guarantee that a 64
     * bit intermediate is sufficient:
     *  + for [US]ADDLV the maximum element size is 32 bits, and
     *    the result type is 64 bits
     *  + for FMAX*V, FMIN*V, ADDV the intermediate type is the
     *    same as the element size, which is 32 bits at most
     * For the integer operations we can choose to work at 64
     * or 32 bits and truncate at the end; for simplicity
     * we use 64 bits always. The floating point
     * ops do require 32 bit intermediates, though.
     */
    if (!is_fp) {
        read_vec_element(s, tcg_res, rn, 0, size | (is_u ? 0 : MO_SIGN));

        for (i = 1; i < elements; i++) {
            read_vec_element(s, tcg_elt, rn, i, size | (is_u ? 0 : MO_SIGN));

            switch (opcode) {
            case 0x03: /* SADDLV / UADDLV */
            case 0x1b: /* ADDV */
                tcg_gen_add_i64(tcg_res, tcg_res, tcg_elt);
                break;
            case 0x0a: /* SMAXV / UMAXV */
                tcg_gen_movcond_i64(is_u ? TCG_COND_GEU : TCG_COND_GE,
                                    tcg_res,
                                    tcg_res, tcg_elt, tcg_res, tcg_elt);
                break;
            case 0x1a: /* SMINV / UMINV */
                tcg_gen_movcond_i64(is_u ? TCG_COND_LEU : TCG_COND_LE,
                                    tcg_res,
                                    tcg_res, tcg_elt, tcg_res, tcg_elt);
                break;
                break;
            default:
                g_assert_not_reached();
            }

        }
    } else {
        /* Floating point ops which work on 32 bit (single) intermediates.
         * Note that correct NaN propagation requires that we do these
         * operations in exactly the order specified by the pseudocode.
         */
        TCGv_i32 tcg_elt1 = tcg_temp_new_i32();
        TCGv_i32 tcg_elt2 = tcg_temp_new_i32();
        TCGv_i32 tcg_elt3 = tcg_temp_new_i32();
        TCGv_ptr fpst = get_fpstatus_ptr();

        assert(esize == 32);
        assert(elements == 4);

        read_vec_element(s, tcg_elt, rn, 0, MO_32);
        tcg_gen_trunc_i64_i32(tcg_elt1, tcg_elt);
        read_vec_element(s, tcg_elt, rn, 1, MO_32);
        tcg_gen_trunc_i64_i32(tcg_elt2, tcg_elt);

        do_minmaxop(s, tcg_elt1, tcg_elt2, opcode, is_min, fpst);

        read_vec_element(s, tcg_elt, rn, 2, MO_32);
        tcg_gen_trunc_i64_i32(tcg_elt2, tcg_elt);
        read_vec_element(s, tcg_elt, rn, 3, MO_32);
        tcg_gen_trunc_i64_i32(tcg_elt3, tcg_elt);

        do_minmaxop(s, tcg_elt2, tcg_elt3, opcode, is_min, fpst);

        do_minmaxop(s, tcg_elt1, tcg_elt2, opcode, is_min, fpst);

        tcg_gen_extu_i32_i64(tcg_res, tcg_elt1);
        tcg_temp_free_i32(tcg_elt1);
        tcg_temp_free_i32(tcg_elt2);
        tcg_temp_free_i32(tcg_elt3);
        tcg_temp_free_ptr(fpst);
    }

    tcg_temp_free_i64(tcg_elt);

    /* Now truncate the result to the width required for the final output */
    if (opcode == 0x03) {
        /* SADDLV, UADDLV: result is 2*esize */
        size++;
    }

    switch (size) {
    case 0:
        tcg_gen_ext8u_i64(tcg_res, tcg_res);
        break;
    case 1:
        tcg_gen_ext16u_i64(tcg_res, tcg_res);
        break;
    case 2:
        tcg_gen_ext32u_i64(tcg_res, tcg_res);
        break;
    case 3:
        break;
    default:
        g_assert_not_reached();
    }

    write_fp_dreg(s, rd, tcg_res);
    tcg_temp_free_i64(tcg_res);
}

/* C6.3.31 DUP (Element, Vector)
 *
 *  31  30   29              21 20    16 15        10  9    5 4    0
 * +---+---+-------------------+--------+-------------+------+------+
 * | 0 | Q | 0 0 1 1 1 0 0 0 0 |  imm5  | 0 0 0 0 0 1 |  Rn  |  Rd  |
 * +---+---+-------------------+--------+-------------+------+------+
 *
 * size: encoded in imm5 (see ARM ARM LowestSetBit())
 */
static void handle_simd_dupe(DisasContext *s, int is_q, int rd, int rn,
                             int imm5)
{
    int size = ctz32(imm5);
    int esize = 8 << size;
    int elements = (is_q ? 128 : 64) / esize;
    int index, i;
    TCGv_i64 tmp;

    if (size > 3 || (size == 3 && !is_q)) {
        unallocated_encoding(s);
        return;
    }

    index = imm5 >> (size + 1);

    tmp = tcg_temp_new_i64();
    read_vec_element(s, tmp, rn, index, size);

    for (i = 0; i < elements; i++) {
        write_vec_element(s, tmp, rd, i, size);
    }

    if (!is_q) {
        clear_vec_high(s, rd);
    }

    tcg_temp_free_i64(tmp);
}

/* C6.3.31 DUP (element, scalar)
 *  31                   21 20    16 15        10  9    5 4    0
 * +-----------------------+--------+-------------+------+------+
 * | 0 1 0 1 1 1 1 0 0 0 0 |  imm5  | 0 0 0 0 0 1 |  Rn  |  Rd  |
 * +-----------------------+--------+-------------+------+------+
 */
static void handle_simd_dupes(DisasContext *s, int rd, int rn,
                              int imm5)
{
    int size = ctz32(imm5);
    int index;
    TCGv_i64 tmp;

    if (size > 3) {
        unallocated_encoding(s);
        return;
    }

    index = imm5 >> (size + 1);

    /* This instruction just extracts the specified element and
     * zero-extends it into the bottom of the destination register.
     */
    tmp = tcg_temp_new_i64();
    read_vec_element(s, tmp, rn, index, size);
    write_fp_dreg(s, rd, tmp);
    tcg_temp_free_i64(tmp);
}

/* C6.3.32 DUP (General)
 *
 *  31  30   29              21 20    16 15        10  9    5 4    0
 * +---+---+-------------------+--------+-------------+------+------+
 * | 0 | Q | 0 0 1 1 1 0 0 0 0 |  imm5  | 0 0 0 0 1 1 |  Rn  |  Rd  |
 * +---+---+-------------------+--------+-------------+------+------+
 *
 * size: encoded in imm5 (see ARM ARM LowestSetBit())
 */
static void handle_simd_dupg(DisasContext *s, int is_q, int rd, int rn,
                             int imm5)
{
    int size = ctz32(imm5);
    int esize = 8 << size;
    int elements = (is_q ? 128 : 64)/esize;
    int i = 0;

    if (size > 3 || ((size == 3) && !is_q)) {
        unallocated_encoding(s);
        return;
    }
    for (i = 0; i < elements; i++) {
        write_vec_element(s, cpu_reg(s, rn), rd, i, size);
    }
    if (!is_q) {
        clear_vec_high(s, rd);
    }
}

/* C6.3.150 INS (Element)
 *
 *  31                   21 20    16 15  14    11  10 9    5 4    0
 * +-----------------------+--------+------------+---+------+------+
 * | 0 1 1 0 1 1 1 0 0 0 0 |  imm5  | 0 |  imm4  | 1 |  Rn  |  Rd  |
 * +-----------------------+--------+------------+---+------+------+
 *
 * size: encoded in imm5 (see ARM ARM LowestSetBit())
 * index: encoded in imm5<4:size+1>
 */
static void handle_simd_inse(DisasContext *s, int rd, int rn,
                             int imm4, int imm5)
{
    int size = ctz32(imm5);
    int src_index, dst_index;
    TCGv_i64 tmp;

    if (size > 3) {
        unallocated_encoding(s);
        return;
    }
    dst_index = extract32(imm5, 1+size, 5);
    src_index = extract32(imm4, size, 4);

    tmp = tcg_temp_new_i64();

    read_vec_element(s, tmp, rn, src_index, size);
    write_vec_element(s, tmp, rd, dst_index, size);

    tcg_temp_free_i64(tmp);
}


/* C6.3.151 INS (General)
 *
 *  31                   21 20    16 15        10  9    5 4    0
 * +-----------------------+--------+-------------+------+------+
 * | 0 1 0 0 1 1 1 0 0 0 0 |  imm5  | 0 0 0 1 1 1 |  Rn  |  Rd  |
 * +-----------------------+--------+-------------+------+------+
 *
 * size: encoded in imm5 (see ARM ARM LowestSetBit())
 * index: encoded in imm5<4:size+1>
 */
static void handle_simd_insg(DisasContext *s, int rd, int rn, int imm5)
{
    int size = ctz32(imm5);
    int idx;

    if (size > 3) {
        unallocated_encoding(s);
        return;
    }

    idx = extract32(imm5, 1 + size, 4 - size);
    write_vec_element(s, cpu_reg(s, rn), rd, idx, size);
}

/*
 * C6.3.321 UMOV (General)
 * C6.3.237 SMOV (General)
 *
 *  31  30   29              21 20    16 15    12   10 9    5 4    0
 * +---+---+-------------------+--------+-------------+------+------+
 * | 0 | Q | 0 0 1 1 1 0 0 0 0 |  imm5  | 0 0 1 U 1 1 |  Rn  |  Rd  |
 * +---+---+-------------------+--------+-------------+------+------+
 *
 * U: unsigned when set
 * size: encoded in imm5 (see ARM ARM LowestSetBit())
 */
static void handle_simd_umov_smov(DisasContext *s, int is_q, int is_signed,
                                  int rn, int rd, int imm5)
{
    int size = ctz32(imm5);
    int element;
    TCGv_i64 tcg_rd;

    /* Check for UnallocatedEncodings */
    if (is_signed) {
        if (size > 2 || (size == 2 && !is_q)) {
            unallocated_encoding(s);
            return;
        }
    } else {
        if (size > 3
            || (size < 3 && is_q)
            || (size == 3 && !is_q)) {
            unallocated_encoding(s);
            return;
        }
    }
    element = extract32(imm5, 1+size, 4);

    tcg_rd = cpu_reg(s, rd);
    read_vec_element(s, tcg_rd, rn, element, size | (is_signed ? MO_SIGN : 0));
    if (is_signed && !is_q) {
        tcg_gen_ext32u_i64(tcg_rd, tcg_rd);
    }
}

/* C3.6.5 AdvSIMD copy
 *   31  30  29  28             21 20  16 15  14  11 10  9    5 4    0
 * +---+---+----+-----------------+------+---+------+---+------+------+
 * | 0 | Q | op | 0 1 1 1 0 0 0 0 | imm5 | 0 | imm4 | 1 |  Rn  |  Rd  |
 * +---+---+----+-----------------+------+---+------+---+------+------+
 */
static void disas_simd_copy(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm4 = extract32(insn, 11, 4);
    int op = extract32(insn, 29, 1);
    int is_q = extract32(insn, 30, 1);
    int imm5 = extract32(insn, 16, 5);

    if (op) {
        if (is_q) {
            /* INS (element) */
            handle_simd_inse(s, rd, rn, imm4, imm5);
        } else {
            unallocated_encoding(s);
        }
    } else {
        switch (imm4) {
        case 0:
            /* DUP (element - vector) */
            handle_simd_dupe(s, is_q, rd, rn, imm5);
            break;
        case 1:
            /* DUP (general) */
            handle_simd_dupg(s, is_q, rd, rn, imm5);
            break;
        case 3:
            if (is_q) {
                /* INS (general) */
                handle_simd_insg(s, rd, rn, imm5);
            } else {
                unallocated_encoding(s);
            }
            break;
        case 5:
        case 7:
            /* UMOV/SMOV (is_q indicates 32/64; imm4 indicates signedness) */
            handle_simd_umov_smov(s, is_q, (imm4 == 5), rn, rd, imm5);
            break;
        default:
            unallocated_encoding(s);
            break;
        }
    }
}

/* C3.6.6 AdvSIMD modified immediate
 *  31  30   29  28                 19 18 16 15   12  11  10  9     5 4    0
 * +---+---+----+---------------------+-----+-------+----+---+-------+------+
 * | 0 | Q | op | 0 1 1 1 1 0 0 0 0 0 | abc | cmode | o2 | 1 | defgh |  Rd  |
 * +---+---+----+---------------------+-----+-------+----+---+-------+------+
 *
 * There are a number of operations that can be carried out here:
 *   MOVI - move (shifted) imm into register
 *   MVNI - move inverted (shifted) imm into register
 *   ORR  - bitwise OR of (shifted) imm with register
 *   BIC  - bitwise clear of (shifted) imm with register
 */
static void disas_simd_mod_imm(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int cmode = extract32(insn, 12, 4);
    int cmode_3_1 = extract32(cmode, 1, 3);
    int cmode_0 = extract32(cmode, 0, 1);
    int o2 = extract32(insn, 11, 1);
    uint64_t abcdefgh = extract32(insn, 5, 5) | (extract32(insn, 16, 3) << 5);
    bool is_neg = extract32(insn, 29, 1);
    bool is_q = extract32(insn, 30, 1);
    uint64_t imm = 0;
    TCGv_i64 tcg_rd, tcg_imm;
    int i;

    if (o2 != 0 || ((cmode == 0xf) && is_neg && !is_q)) {
        unallocated_encoding(s);
        return;
    }

    /* See AdvSIMDExpandImm() in ARM ARM */
    switch (cmode_3_1) {
    case 0: /* Replicate(Zeros(24):imm8, 2) */
    case 1: /* Replicate(Zeros(16):imm8:Zeros(8), 2) */
    case 2: /* Replicate(Zeros(8):imm8:Zeros(16), 2) */
    case 3: /* Replicate(imm8:Zeros(24), 2) */
    {
        int shift = cmode_3_1 * 8;
        imm = bitfield_replicate(abcdefgh << shift, 32);
        break;
    }
    case 4: /* Replicate(Zeros(8):imm8, 4) */
    case 5: /* Replicate(imm8:Zeros(8), 4) */
    {
        int shift = (cmode_3_1 & 0x1) * 8;
        imm = bitfield_replicate(abcdefgh << shift, 16);
        break;
    }
    case 6:
        if (cmode_0) {
            /* Replicate(Zeros(8):imm8:Ones(16), 2) */
            imm = (abcdefgh << 16) | 0xffff;
        } else {
            /* Replicate(Zeros(16):imm8:Ones(8), 2) */
            imm = (abcdefgh << 8) | 0xff;
        }
        imm = bitfield_replicate(imm, 32);
        break;
    case 7:
        if (!cmode_0 && !is_neg) {
            imm = bitfield_replicate(abcdefgh, 8);
        } else if (!cmode_0 && is_neg) {
            int i;
            imm = 0;
            for (i = 0; i < 8; i++) {
                if ((abcdefgh) & (1 << i)) {
                    imm |= 0xffULL << (i * 8);
                }
            }
        } else if (cmode_0) {
            if (is_neg) {
                imm = (abcdefgh & 0x3f) << 48;
                if (abcdefgh & 0x80) {
                    imm |= 0x8000000000000000ULL;
                }
                if (abcdefgh & 0x40) {
                    imm |= 0x3fc0000000000000ULL;
                } else {
                    imm |= 0x4000000000000000ULL;
                }
            } else {
                imm = (abcdefgh & 0x3f) << 19;
                if (abcdefgh & 0x80) {
                    imm |= 0x80000000;
                }
                if (abcdefgh & 0x40) {
                    imm |= 0x3e000000;
                } else {
                    imm |= 0x40000000;
                }
                imm |= (imm << 32);
            }
        }
        break;
    }

    if (cmode_3_1 != 7 && is_neg) {
        imm = ~imm;
    }

    tcg_imm = tcg_const_i64(imm);
    tcg_rd = new_tmp_a64(s);

    for (i = 0; i < 2; i++) {
        int foffs = i ? fp_reg_hi_offset(rd) : fp_reg_offset(rd, MO_64);

        if (i == 1 && !is_q) {
            /* non-quad ops clear high half of vector */
            tcg_gen_movi_i64(tcg_rd, 0);
        } else if ((cmode & 0x9) == 0x1 || (cmode & 0xd) == 0x9) {
            tcg_gen_ld_i64(tcg_rd, cpu_env, foffs);
            if (is_neg) {
                /* AND (BIC) */
                tcg_gen_and_i64(tcg_rd, tcg_rd, tcg_imm);
            } else {
                /* ORR */
                tcg_gen_or_i64(tcg_rd, tcg_rd, tcg_imm);
            }
        } else {
            /* MOVI */
            tcg_gen_mov_i64(tcg_rd, tcg_imm);
        }
        tcg_gen_st_i64(tcg_rd, cpu_env, foffs);
    }

    tcg_temp_free_i64(tcg_imm);
}

/* C3.6.7 AdvSIMD scalar copy
 *  31 30  29  28             21 20  16 15  14  11 10  9    5 4    0
 * +-----+----+-----------------+------+---+------+---+------+------+
 * | 0 1 | op | 1 1 1 1 0 0 0 0 | imm5 | 0 | imm4 | 1 |  Rn  |  Rd  |
 * +-----+----+-----------------+------+---+------+---+------+------+
 */
static void disas_simd_scalar_copy(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm4 = extract32(insn, 11, 4);
    int imm5 = extract32(insn, 16, 5);
    int op = extract32(insn, 29, 1);

    if (op != 0 || imm4 != 0) {
        unallocated_encoding(s);
        return;
    }

    /* DUP (element, scalar) */
    handle_simd_dupes(s, rd, rn, imm5);
}

/* C3.6.8 AdvSIMD scalar pairwise
 *  31 30  29 28       24 23  22 21       17 16    12 11 10 9    5 4    0
 * +-----+---+-----------+------+-----------+--------+-----+------+------+
 * | 0 1 | U | 1 1 1 1 0 | size | 1 1 0 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +-----+---+-----------+------+-----------+--------+-----+------+------+
 */
static void disas_simd_scalar_pairwise(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.9 AdvSIMD scalar shift by immediate
 *  31 30  29 28         23 22  19 18  16 15    11  10 9    5 4    0
 * +-----+---+-------------+------+------+--------+---+------+------+
 * | 0 1 | U | 1 1 1 1 1 0 | immh | immb | opcode | 1 |  Rn  |  Rd  |
 * +-----+---+-------------+------+------+--------+---+------+------+
 */
static void disas_simd_scalar_shift_imm(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.10 AdvSIMD scalar three different
 *  31 30  29 28       24 23  22  21 20  16 15    12 11 10 9    5 4    0
 * +-----+---+-----------+------+---+------+--------+-----+------+------+
 * | 0 1 | U | 1 1 1 1 0 | size | 1 |  Rm  | opcode | 0 0 |  Rn  |  Rd  |
 * +-----+---+-----------+------+---+------+--------+-----+------+------+
 */
static void disas_simd_scalar_three_reg_diff(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

static void handle_3same_64(DisasContext *s, int opcode, bool u,
                            TCGv_i64 tcg_rd, TCGv_i64 tcg_rn, TCGv_i64 tcg_rm)
{
    /* Handle 64x64->64 opcodes which are shared between the scalar
     * and vector 3-same groups. We cover every opcode where size == 3
     * is valid in either the three-reg-same (integer, not pairwise)
     * or scalar-three-reg-same groups. (Some opcodes are not yet
     * implemented.)
     */
    TCGCond cond;

    switch (opcode) {
    case 0x6: /* CMGT, CMHI */
        /* 64 bit integer comparison, result = test ? (2^64 - 1) : 0.
         * We implement this using setcond (test) and then negating.
         */
        cond = u ? TCG_COND_GTU : TCG_COND_GT;
    do_cmop:
        tcg_gen_setcond_i64(cond, tcg_rd, tcg_rn, tcg_rm);
        tcg_gen_neg_i64(tcg_rd, tcg_rd);
        break;
    case 0x7: /* CMGE, CMHS */
        cond = u ? TCG_COND_GEU : TCG_COND_GE;
        goto do_cmop;
    case 0x11: /* CMTST, CMEQ */
        if (u) {
            cond = TCG_COND_EQ;
            goto do_cmop;
        }
        /* CMTST : test is "if (X & Y != 0)". */
        tcg_gen_and_i64(tcg_rd, tcg_rn, tcg_rm);
        tcg_gen_setcondi_i64(TCG_COND_NE, tcg_rd, tcg_rd, 0);
        tcg_gen_neg_i64(tcg_rd, tcg_rd);
        break;
    case 0x10: /* ADD, SUB */
        if (u) {
            tcg_gen_sub_i64(tcg_rd, tcg_rn, tcg_rm);
        } else {
            tcg_gen_add_i64(tcg_rd, tcg_rn, tcg_rm);
        }
        break;
    case 0x1: /* SQADD */
    case 0x5: /* SQSUB */
    case 0x8: /* SSHL, USHL */
    case 0x9: /* SQSHL, UQSHL */
    case 0xa: /* SRSHL, URSHL */
    case 0xb: /* SQRSHL, UQRSHL */
    default:
        g_assert_not_reached();
    }
}

/* C3.6.11 AdvSIMD scalar three same
 *  31 30  29 28       24 23  22  21 20  16 15    11  10 9    5 4    0
 * +-----+---+-----------+------+---+------+--------+---+------+------+
 * | 0 1 | U | 1 1 1 1 0 | size | 1 |  Rm  | opcode | 1 |  Rn  |  Rd  |
 * +-----+---+-----------+------+---+------+--------+---+------+------+
 */
static void disas_simd_scalar_three_reg_same(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 11, 5);
    int rm = extract32(insn, 16, 5);
    int size = extract32(insn, 22, 2);
    bool u = extract32(insn, 29, 1);
    TCGv_i64 tcg_rn;
    TCGv_i64 tcg_rm;
    TCGv_i64 tcg_rd;

    if (opcode >= 0x18) {
        /* Floating point: U, size[1] and opcode indicate operation */
        int fpopcode = opcode | (extract32(size, 1, 1) << 5) | (u << 6);
        switch (fpopcode) {
        case 0x1b: /* FMULX */
        case 0x1c: /* FCMEQ */
        case 0x1f: /* FRECPS */
        case 0x3f: /* FRSQRTS */
        case 0x5c: /* FCMGE */
        case 0x5d: /* FACGE */
        case 0x7a: /* FABD  */
        case 0x7c: /* FCMGT */
        case 0x7d: /* FACGT */
            unsupported_encoding(s, insn);
            return;
        default:
            unallocated_encoding(s);
            return;
        }
    }

    switch (opcode) {
    case 0x1: /* SQADD, UQADD */
    case 0x5: /* SQSUB, UQSUB */
    case 0x8: /* SSHL, USHL */
    case 0xa: /* SRSHL, URSHL */
        unsupported_encoding(s, insn);
        return;
    case 0x6: /* CMGT, CMHI */
    case 0x7: /* CMGE, CMHS */
    case 0x11: /* CMTST, CMEQ */
    case 0x10: /* ADD, SUB (vector) */
        if (size != 3) {
            unallocated_encoding(s);
            return;
        }
        break;
    case 0x9: /* SQSHL, UQSHL */
    case 0xb: /* SQRSHL, UQRSHL */
        unsupported_encoding(s, insn);
        return;
    case 0x16: /* SQDMULH, SQRDMULH (vector) */
        if (size != 1 && size != 2) {
            unallocated_encoding(s);
            return;
        }
        unsupported_encoding(s, insn);
        return;
    default:
        unallocated_encoding(s);
        return;
    }

    tcg_rn = read_fp_dreg(s, rn);       /* op1 */
    tcg_rm = read_fp_dreg(s, rm);       /* op2 */
    tcg_rd = tcg_temp_new_i64();

    /* For the moment we only support the opcodes which are
     * 64-bit-width only. The size != 3 cases will
     * be handled later when the relevant ops are implemented.
     */
    handle_3same_64(s, opcode, u, tcg_rd, tcg_rn, tcg_rm);

    write_fp_dreg(s, rd, tcg_rd);

    tcg_temp_free_i64(tcg_rn);
    tcg_temp_free_i64(tcg_rm);
    tcg_temp_free_i64(tcg_rd);
}

/* C3.6.12 AdvSIMD scalar two reg misc
 *  31 30  29 28       24 23  22 21       17 16    12 11 10 9    5 4    0
 * +-----+---+-----------+------+-----------+--------+-----+------+------+
 * | 0 1 | U | 1 1 1 1 0 | size | 1 0 0 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +-----+---+-----------+------+-----------+--------+-----+------+------+
 */
static void disas_simd_scalar_two_reg_misc(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.13 AdvSIMD scalar x indexed element
 *  31 30  29 28       24 23  22 21  20  19  16 15 12  11  10 9    5 4    0
 * +-----+---+-----------+------+---+---+------+-----+---+---+------+------+
 * | 0 1 | U | 1 1 1 1 1 | size | L | M |  Rm  | opc | H | 0 |  Rn  |  Rd  |
 * +-----+---+-----------+------+---+---+------+-----+---+---+------+------+
 */
static void disas_simd_scalar_indexed(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.14 AdvSIMD shift by immediate
 *  31  30   29 28         23 22  19 18  16 15    11  10 9    5 4    0
 * +---+---+---+-------------+------+------+--------+---+------+------+
 * | 0 | Q | U | 0 1 1 1 1 0 | immh | immb | opcode | 1 |  Rn  |  Rd  |
 * +---+---+---+-------------+------+------+--------+---+------+------+
 */
static void disas_simd_shift_imm(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

static void handle_3rd_widening(DisasContext *s, int is_q, int is_u, int size,
                                int opcode, int rd, int rn, int rm)
{
    /* 3-reg-different widening insns: 64 x 64 -> 128 */
    TCGv_i64 tcg_res[2];
    int pass, accop;

    tcg_res[0] = tcg_temp_new_i64();
    tcg_res[1] = tcg_temp_new_i64();

    /* Does this op do an adding accumulate, a subtracting accumulate,
     * or no accumulate at all?
     */
    switch (opcode) {
    case 5:
    case 8:
    case 9:
        accop = 1;
        break;
    case 10:
    case 11:
        accop = -1;
        break;
    default:
        accop = 0;
        break;
    }

    if (accop != 0) {
        read_vec_element(s, tcg_res[0], rd, 0, MO_64);
        read_vec_element(s, tcg_res[1], rd, 1, MO_64);
    }

    /* size == 2 means two 32x32->64 operations; this is worth special
     * casing because we can generally handle it inline.
     */
    if (size == 2) {
        for (pass = 0; pass < 2; pass++) {
            TCGv_i64 tcg_op1 = tcg_temp_new_i64();
            TCGv_i64 tcg_op2 = tcg_temp_new_i64();
            TCGv_i64 tcg_passres;
            TCGMemOp memop = MO_32 | (is_u ? 0 : MO_SIGN);

            int elt = pass + is_q * 2;

            read_vec_element(s, tcg_op1, rn, elt, memop);
            read_vec_element(s, tcg_op2, rm, elt, memop);

            if (accop == 0) {
                tcg_passres = tcg_res[pass];
            } else {
                tcg_passres = tcg_temp_new_i64();
            }

            switch (opcode) {
            case 5: /* SABAL, SABAL2, UABAL, UABAL2 */
            case 7: /* SABDL, SABDL2, UABDL, UABDL2 */
            {
                TCGv_i64 tcg_tmp1 = tcg_temp_new_i64();
                TCGv_i64 tcg_tmp2 = tcg_temp_new_i64();

                tcg_gen_sub_i64(tcg_tmp1, tcg_op1, tcg_op2);
                tcg_gen_sub_i64(tcg_tmp2, tcg_op2, tcg_op1);
                tcg_gen_movcond_i64(is_u ? TCG_COND_GEU : TCG_COND_GE,
                                    tcg_passres,
                                    tcg_op1, tcg_op2, tcg_tmp1, tcg_tmp2);
                tcg_temp_free_i64(tcg_tmp1);
                tcg_temp_free_i64(tcg_tmp2);
                break;
            }
            case 8: /* SMLAL, SMLAL2, UMLAL, UMLAL2 */
            case 10: /* SMLSL, SMLSL2, UMLSL, UMLSL2 */
            case 12: /* UMULL, UMULL2, SMULL, SMULL2 */
                tcg_gen_mul_i64(tcg_passres, tcg_op1, tcg_op2);
                break;
            default:
                g_assert_not_reached();
            }

            if (accop > 0) {
                tcg_gen_add_i64(tcg_res[pass], tcg_res[pass], tcg_passres);
                tcg_temp_free_i64(tcg_passres);
            } else if (accop < 0) {
                tcg_gen_sub_i64(tcg_res[pass], tcg_res[pass], tcg_passres);
                tcg_temp_free_i64(tcg_passres);
            }

            tcg_temp_free_i64(tcg_op1);
            tcg_temp_free_i64(tcg_op2);
        }
    } else {
        /* size 0 or 1, generally helper functions */
        for (pass = 0; pass < 2; pass++) {
            TCGv_i32 tcg_op1 = tcg_temp_new_i32();
            TCGv_i32 tcg_op2 = tcg_temp_new_i32();
            TCGv_i64 tcg_passres;
            int elt = pass + is_q * 2;

            read_vec_element_i32(s, tcg_op1, rn, elt, MO_32);
            read_vec_element_i32(s, tcg_op2, rm, elt, MO_32);

            if (accop == 0) {
                tcg_passres = tcg_res[pass];
            } else {
                tcg_passres = tcg_temp_new_i64();
            }

            switch (opcode) {
            case 5: /* SABAL, SABAL2, UABAL, UABAL2 */
            case 7: /* SABDL, SABDL2, UABDL, UABDL2 */
                if (size == 0) {
                    if (is_u) {
                        gen_helper_neon_abdl_u16(tcg_passres, tcg_op1, tcg_op2);
                    } else {
                        gen_helper_neon_abdl_s16(tcg_passres, tcg_op1, tcg_op2);
                    }
                } else {
                    if (is_u) {
                        gen_helper_neon_abdl_u32(tcg_passres, tcg_op1, tcg_op2);
                    } else {
                        gen_helper_neon_abdl_s32(tcg_passres, tcg_op1, tcg_op2);
                    }
                }
                break;
            case 8: /* SMLAL, SMLAL2, UMLAL, UMLAL2 */
            case 10: /* SMLSL, SMLSL2, UMLSL, UMLSL2 */
            case 12: /* UMULL, UMULL2, SMULL, SMULL2 */
                if (size == 0) {
                    if (is_u) {
                        gen_helper_neon_mull_u8(tcg_passres, tcg_op1, tcg_op2);
                    } else {
                        gen_helper_neon_mull_s8(tcg_passres, tcg_op1, tcg_op2);
                    }
                } else {
                    if (is_u) {
                        gen_helper_neon_mull_u16(tcg_passres, tcg_op1, tcg_op2);
                    } else {
                        gen_helper_neon_mull_s16(tcg_passres, tcg_op1, tcg_op2);
                    }
                }
                break;
            default:
                g_assert_not_reached();
            }
            tcg_temp_free_i32(tcg_op1);
            tcg_temp_free_i32(tcg_op2);

            if (accop > 0) {
                if (size == 0) {
                    gen_helper_neon_addl_u16(tcg_res[pass], tcg_res[pass],
                                             tcg_passres);
                } else {
                    gen_helper_neon_addl_u32(tcg_res[pass], tcg_res[pass],
                                             tcg_passres);
                }
                tcg_temp_free_i64(tcg_passres);
            } else if (accop < 0) {
                if (size == 0) {
                    gen_helper_neon_subl_u16(tcg_res[pass], tcg_res[pass],
                                             tcg_passres);
                } else {
                    gen_helper_neon_subl_u32(tcg_res[pass], tcg_res[pass],
                                             tcg_passres);
                }
                tcg_temp_free_i64(tcg_passres);
            }
        }
    }

    write_vec_element(s, tcg_res[0], rd, 0, MO_64);
    write_vec_element(s, tcg_res[1], rd, 1, MO_64);
    tcg_temp_free_i64(tcg_res[0]);
    tcg_temp_free_i64(tcg_res[1]);
}

/* C3.6.15 AdvSIMD three different
 *   31  30  29 28       24 23  22  21 20  16 15    12 11 10 9    5 4    0
 * +---+---+---+-----------+------+---+------+--------+-----+------+------+
 * | 0 | Q | U | 0 1 1 1 0 | size | 1 |  Rm  | opcode | 0 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+------+--------+-----+------+------+
 */
static void disas_simd_three_reg_diff(DisasContext *s, uint32_t insn)
{
    /* Instructions in this group fall into three basic classes
     * (in each case with the operation working on each element in
     * the input vectors):
     * (1) widening 64 x 64 -> 128 (with possibly Vd as an extra
     *     128 bit input)
     * (2) wide 64 x 128 -> 128
     * (3) narrowing 128 x 128 -> 64
     * Here we do initial decode, catch unallocated cases and
     * dispatch to separate functions for each class.
     */
    int is_q = extract32(insn, 30, 1);
    int is_u = extract32(insn, 29, 1);
    int size = extract32(insn, 22, 2);
    int opcode = extract32(insn, 12, 4);
    int rm = extract32(insn, 16, 5);
    int rn = extract32(insn, 5, 5);
    int rd = extract32(insn, 0, 5);

    switch (opcode) {
    case 1: /* SADDW, SADDW2, UADDW, UADDW2 */
    case 3: /* SSUBW, SSUBW2, USUBW, USUBW2 */
        /* 64 x 128 -> 128 */
        unsupported_encoding(s, insn);
        break;
    case 4: /* ADDHN, ADDHN2, RADDHN, RADDHN2 */
    case 6: /* SUBHN, SUBHN2, RSUBHN, RSUBHN2 */
        /* 128 x 128 -> 64 */
        unsupported_encoding(s, insn);
        break;
    case 9:
    case 11:
    case 13:
    case 14:
        if (is_u) {
            unallocated_encoding(s);
            return;
        }
        /* fall through */
    case 0:
    case 2:
        unsupported_encoding(s, insn);
        break;
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        /* 64 x 64 -> 128 */
        if (size == 3) {
            unallocated_encoding(s);
            return;
        }
        handle_3rd_widening(s, is_q, is_u, size, opcode, rd, rn, rm);
        break;
    default:
        /* opcode 15 not allocated */
        unallocated_encoding(s);
        break;
    }
}

/* Logic op (opcode == 3) subgroup of C3.6.16. */
static void disas_simd_3same_logic(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* Pairwise op subgroup of C3.6.16. */
static void disas_simd_3same_pair(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* Floating point op subgroup of C3.6.16. */
static void disas_simd_3same_float(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* Integer op subgroup of C3.6.16. */
static void disas_simd_3same_int(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.16 AdvSIMD three same
 *  31  30  29  28       24 23  22  21 20  16 15    11  10 9    5 4    0
 * +---+---+---+-----------+------+---+------+--------+---+------+------+
 * | 0 | Q | U | 0 1 1 1 0 | size | 1 |  Rm  | opcode | 1 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+------+--------+---+------+------+
 */
static void disas_simd_three_reg_same(DisasContext *s, uint32_t insn)
{
    int opcode = extract32(insn, 11, 5);

    switch (opcode) {
    case 0x3: /* logic ops */
        disas_simd_3same_logic(s, insn);
        break;
    case 0x17: /* ADDP */
    case 0x14: /* SMAXP, UMAXP */
    case 0x15: /* SMINP, UMINP */
        /* Pairwise operations */
        disas_simd_3same_pair(s, insn);
        break;
    case 0x18 ... 0x31:
        /* floating point ops, sz[1] and U are part of opcode */
        disas_simd_3same_float(s, insn);
        break;
    default:
        disas_simd_3same_int(s, insn);
        break;
    }
}

/* C3.6.17 AdvSIMD two reg misc
 *   31  30  29 28       24 23  22 21       17 16    12 11 10 9    5 4    0
 * +---+---+---+-----------+------+-----------+--------+-----+------+------+
 * | 0 | Q | U | 0 1 1 1 0 | size | 1 0 0 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+-----------+--------+-----+------+------+
 */
static void disas_simd_two_reg_misc(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.18 AdvSIMD vector x indexed element
 *   31  30  29 28       24 23  22 21  20  19  16 15 12  11  10 9    5 4    0
 * +---+---+---+-----------+------+---+---+------+-----+---+---+------+------+
 * | 0 | Q | U | 0 1 1 1 1 | size | L | M |  Rm  | opc | H | 0 |  Rn  |  Rd  |
 * +---+---+---+-----------+------+---+---+------+-----+---+---+------+------+
 */
static void disas_simd_indexed_vector(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.19 Crypto AES
 *  31             24 23  22 21       17 16    12 11 10 9    5 4    0
 * +-----------------+------+-----------+--------+-----+------+------+
 * | 0 1 0 0 1 1 1 0 | size | 1 0 1 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +-----------------+------+-----------+--------+-----+------+------+
 */
static void disas_crypto_aes(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.20 Crypto three-reg SHA
 *  31             24 23  22  21 20  16  15 14    12 11 10 9    5 4    0
 * +-----------------+------+---+------+---+--------+-----+------+------+
 * | 0 1 0 1 1 1 1 0 | size | 0 |  Rm  | 0 | opcode | 0 0 |  Rn  |  Rd  |
 * +-----------------+------+---+------+---+--------+-----+------+------+
 */
static void disas_crypto_three_reg_sha(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6.21 Crypto two-reg SHA
 *  31             24 23  22 21       17 16    12 11 10 9    5 4    0
 * +-----------------+------+-----------+--------+-----+------+------+
 * | 0 1 0 1 1 1 1 0 | size | 1 0 1 0 0 | opcode | 1 0 |  Rn  |  Rd  |
 * +-----------------+------+-----------+--------+-----+------+------+
 */
static void disas_crypto_two_reg_sha(DisasContext *s, uint32_t insn)
{
    unsupported_encoding(s, insn);
}

/* C3.6 Data processing - SIMD, inc Crypto
 *
 * As the decode gets a little complex we are using a table based
 * approach for this part of the decode.
 */
static const AArch64DecodeTable data_proc_simd[] = {
    /* pattern  ,  mask     ,  fn                        */
    { 0x0e200400, 0x9f200400, disas_simd_three_reg_same },
    { 0x0e200000, 0x9f200c00, disas_simd_three_reg_diff },
    { 0x0e200800, 0x9f3e0c00, disas_simd_two_reg_misc },
    { 0x0e300800, 0x9f3e0c00, disas_simd_across_lanes },
    { 0x0e000400, 0x9fe08400, disas_simd_copy },
    { 0x0f000000, 0x9f000400, disas_simd_indexed_vector },
    /* simd_mod_imm decode is a subset of simd_shift_imm, so must precede it */
    { 0x0f000400, 0x9ff80400, disas_simd_mod_imm },
    { 0x0f000400, 0x9f800400, disas_simd_shift_imm },
    { 0x0e000000, 0xbf208c00, disas_simd_tb },
    { 0x0e000800, 0xbf208c00, disas_simd_zip_trn },
    { 0x2e000000, 0xbf208400, disas_simd_ext },
    { 0x5e200400, 0xdf200400, disas_simd_scalar_three_reg_same },
    { 0x5e200000, 0xdf200c00, disas_simd_scalar_three_reg_diff },
    { 0x5e200800, 0xdf3e0c00, disas_simd_scalar_two_reg_misc },
    { 0x5e300800, 0xdf3e0c00, disas_simd_scalar_pairwise },
    { 0x5e000400, 0xdfe08400, disas_simd_scalar_copy },
    { 0x5f000000, 0xdf000400, disas_simd_scalar_indexed },
    { 0x5f000400, 0xdf800400, disas_simd_scalar_shift_imm },
    { 0x4e280800, 0xff3e0c00, disas_crypto_aes },
    { 0x5e000000, 0xff208c00, disas_crypto_three_reg_sha },
    { 0x5e280800, 0xff3e0c00, disas_crypto_two_reg_sha },
    { 0x00000000, 0x00000000, NULL }
};

static void disas_data_proc_simd(DisasContext *s, uint32_t insn)
{
    /* Note that this is called with all non-FP cases from
     * table C3-6 so it must UNDEF for entries not specifically
     * allocated to instructions in that table.
     */
    AArch64DecodeFn *fn = lookup_disas_fn(&data_proc_simd[0], insn);
    if (fn) {
        fn(s, insn);
    } else {
        unallocated_encoding(s);
    }
}

/* C3.6 Data processing - SIMD and floating point */
static void disas_data_proc_simd_fp(DisasContext *s, uint32_t insn)
{
    if (extract32(insn, 28, 1) == 1 && extract32(insn, 30, 1) == 0) {
        disas_data_proc_fp(s, insn);
    } else {
        /* SIMD, including crypto */
        disas_data_proc_simd(s, insn);
    }
}

/* C3.1 A64 instruction index by encoding */
static void disas_a64_insn(CPUARMState *env, DisasContext *s)
{
    uint32_t insn;

    insn = arm_ldl_code(env, s->pc, s->bswap_code);
    s->insn = insn;
    s->pc += 4;

    switch (extract32(insn, 25, 4)) {
    case 0x0: case 0x1: case 0x2: case 0x3: /* UNALLOCATED */
        unallocated_encoding(s);
        break;
    case 0x8: case 0x9: /* Data processing - immediate */
        disas_data_proc_imm(s, insn);
        break;
    case 0xa: case 0xb: /* Branch, exception generation and system insns */
        disas_b_exc_sys(s, insn);
        break;
    case 0x4:
    case 0x6:
    case 0xc:
    case 0xe:      /* Loads and stores */
        disas_ldst(s, insn);
        break;
    case 0x5:
    case 0xd:      /* Data processing - register */
        disas_data_proc_reg(s, insn);
        break;
    case 0x7:
    case 0xf:      /* Data processing - SIMD and floating point */
        disas_data_proc_simd_fp(s, insn);
        break;
    default:
        assert(FALSE); /* all 15 cases should be handled above */
        break;
    }

    /* if we allocated any temporaries, free them here */
    free_tmp_a64(s);
}

void gen_intermediate_code_internal_a64(ARMCPU *cpu,
                                        TranslationBlock *tb,
                                        bool search_pc)
{
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    DisasContext dc1, *dc = &dc1;
    CPUBreakpoint *bp;
    uint16_t *gen_opc_end;
    int j, lj;
    target_ulong pc_start;
    target_ulong next_page_start;
    int num_insns;
    int max_insns;

    pc_start = tb->pc;

    dc->tb = tb;

    gen_opc_end = tcg_ctx.gen_opc_buf + OPC_MAX_SIZE;

    dc->is_jmp = DISAS_NEXT;
    dc->pc = pc_start;
    dc->singlestep_enabled = cs->singlestep_enabled;
    dc->condjmp = 0;

    dc->aarch64 = 1;
    dc->thumb = 0;
    dc->bswap_code = 0;
    dc->condexec_mask = 0;
    dc->condexec_cond = 0;
#if !defined(CONFIG_USER_ONLY)
    dc->user = 0;
#endif
    dc->vfp_enabled = 0;
    dc->vec_len = 0;
    dc->vec_stride = 0;
    dc->cp_regs = cpu->cp_regs;
    dc->current_pl = arm_current_pl(env);

    init_tmp_a64_array(dc);

    next_page_start = (pc_start & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
    lj = -1;
    num_insns = 0;
    max_insns = tb->cflags & CF_COUNT_MASK;
    if (max_insns == 0) {
        max_insns = CF_COUNT_MASK;
    }

    gen_tb_start();

    tcg_clear_temp_count();

    do {
        if (unlikely(!QTAILQ_EMPTY(&env->breakpoints))) {
            QTAILQ_FOREACH(bp, &env->breakpoints, entry) {
                if (bp->pc == dc->pc) {
                    gen_exception_insn(dc, 0, EXCP_DEBUG);
                    /* Advance PC so that clearing the breakpoint will
                       invalidate this TB.  */
                    dc->pc += 2;
                    goto done_generating;
                }
            }
        }

        if (search_pc) {
            j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
            if (lj < j) {
                lj++;
                while (lj < j) {
                    tcg_ctx.gen_opc_instr_start[lj++] = 0;
                }
            }
            tcg_ctx.gen_opc_pc[lj] = dc->pc;
            tcg_ctx.gen_opc_instr_start[lj] = 1;
            tcg_ctx.gen_opc_icount[lj] = num_insns;
        }

        if (num_insns + 1 == max_insns && (tb->cflags & CF_LAST_IO)) {
            gen_io_start();
        }

        if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT))) {
            tcg_gen_debug_insn_start(dc->pc);
        }

        disas_a64_insn(env, dc);

        if (tcg_check_temp_count()) {
            fprintf(stderr, "TCG temporary leak before "TARGET_FMT_lx"\n",
                    dc->pc);
        }

        /* Translation stops when a conditional branch is encountered.
         * Otherwise the subsequent code could get translated several times.
         * Also stop translation when a page boundary is reached.  This
         * ensures prefetch aborts occur at the right place.
         */
        num_insns++;
    } while (!dc->is_jmp && tcg_ctx.gen_opc_ptr < gen_opc_end &&
             !cs->singlestep_enabled &&
             !singlestep &&
             dc->pc < next_page_start &&
             num_insns < max_insns);

    if (tb->cflags & CF_LAST_IO) {
        gen_io_end();
    }

    if (unlikely(cs->singlestep_enabled) && dc->is_jmp != DISAS_EXC) {
        /* Note that this means single stepping WFI doesn't halt the CPU.
         * For conditional branch insns this is harmless unreachable code as
         * gen_goto_tb() has already handled emitting the debug exception
         * (and thus a tb-jump is not possible when singlestepping).
         */
        assert(dc->is_jmp != DISAS_TB_JUMP);
        if (dc->is_jmp != DISAS_JUMP) {
            gen_a64_set_pc_im(dc->pc);
        }
        gen_exception(EXCP_DEBUG);
    } else {
        switch (dc->is_jmp) {
        case DISAS_NEXT:
            gen_goto_tb(dc, 1, dc->pc);
            break;
        default:
        case DISAS_UPDATE:
            gen_a64_set_pc_im(dc->pc);
            /* fall through */
        case DISAS_JUMP:
            /* indicate that the hash table must be used to find the next TB */
            tcg_gen_exit_tb(0);
            break;
        case DISAS_TB_JUMP:
        case DISAS_EXC:
        case DISAS_SWI:
            break;
        case DISAS_WFI:
            /* This is a special case because we don't want to just halt the CPU
             * if trying to debug across a WFI.
             */
            gen_helper_wfi(cpu_env);
            break;
        }
    }

done_generating:
    gen_tb_end(tb, num_insns);
    *tcg_ctx.gen_opc_ptr = INDEX_op_end;

#ifdef DEBUG_DISAS
    if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM)) {
        qemu_log("----------------\n");
        qemu_log("IN: %s\n", lookup_symbol(pc_start));
        log_target_disas(env, pc_start, dc->pc - pc_start,
                         dc->thumb | (dc->bswap_code << 1));
        qemu_log("\n");
    }
#endif
    if (search_pc) {
        j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
        lj++;
        while (lj <= j) {
            tcg_ctx.gen_opc_instr_start[lj++] = 0;
        }
    } else {
        tb->size = dc->pc - pc_start;
        tb->icount = num_insns;
    }
}
