/*
 *  MIPS32 emulation for qemu: main translation routines.
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *  Copyright (c) 2006 Marius Groeger (FPU operations)
 *  Copyright (c) 2006 Thiemo Seufer (MIPS32R2 support)
 *  Copyright (c) 2009 CodeSourcery (MIPS16 and microMIPS support)
 *  Copyright (c) 2012 Jia Liu & Dongxue Zhang (MIPS ASE DSP support)
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

#include "cpu.h"
#include "disas/disas.h"
#include "tcg-op.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

#define MIPS_DEBUG_DISAS 0
//#define MIPS_DEBUG_SIGN_EXTENSIONS

/* RISC-V major opcodes */
#define MASK_OP_MAJOR(op)  (op & 0x7F)
enum {
    /* rv32i, rv64i, rv32m */
    OPC_RISC_LUI    = (0x37),
    OPC_RISC_AUIPC  = (0x17),
    OPC_RISC_JAL    = (0x6F),
    OPC_RISC_JALR   = (0x67),
    OPC_RISC_BRANCH = (0x63),
    OPC_RISC_LOAD   = (0x03),
    OPC_RISC_STORE  = (0x23),
    OPC_RISC_ARITH_IMM  = (0x13),
    OPC_RISC_ARITH      = (0x33),
    OPC_RISC_FENCE      = (0x0F),
    OPC_RISC_SYSTEM     = (0x73),

    /* rv64i, rv64m */
    OPC_RISC_ARITH_IMM_W = (0x1B),
    OPC_RISC_ARITH_W = (0x3B),

    /* currently covers up to, but not including atomics */
};

#define MASK_OP_ARITH(op)   (MASK_OP_MAJOR(op) | (op & ((0x7 << 12) | (0x7F << 25))))
enum {
    OPC_RISC_ADD   = OPC_RISC_ARITH | (0x0 << 12) | (0x00 << 25),
    OPC_RISC_SUB   = OPC_RISC_ARITH | (0x0 << 12) | (0x20 << 25),
    OPC_RISC_SLL   = OPC_RISC_ARITH | (0x1 << 12) | (0x00 << 25),
    OPC_RISC_SLT   = OPC_RISC_ARITH | (0x2 << 12) | (0x00 << 25),
    OPC_RISC_SLTU  = OPC_RISC_ARITH | (0x3 << 12) | (0x00 << 25),
    OPC_RISC_XOR   = OPC_RISC_ARITH | (0x4 << 12) | (0x00 << 25),
    OPC_RISC_SRL   = OPC_RISC_ARITH | (0x5 << 12) | (0x00 << 25), 
    OPC_RISC_SRA   = OPC_RISC_ARITH | (0x5 << 12) | (0x20 << 25),
    OPC_RISC_OR    = OPC_RISC_ARITH | (0x6 << 12) | (0x00 << 25),
    OPC_RISC_AND   = OPC_RISC_ARITH | (0x7 << 12) | (0x00 << 25)
};


#define MASK_OP_ARITH_IMM(op)   (MASK_OP_MAJOR(op) | (op & (0x7 << 12)))
enum {
    OPC_RISC_ADDI   = OPC_RISC_ARITH_IMM | (0x0 << 12),
    OPC_RISC_SLTI   = OPC_RISC_ARITH_IMM | (0x2 << 12),
    OPC_RISC_SLTIU  = OPC_RISC_ARITH_IMM | (0x3 << 12),
    OPC_RISC_XORI   = OPC_RISC_ARITH_IMM | (0x4 << 12),
    OPC_RISC_ORI    = OPC_RISC_ARITH_IMM | (0x6 << 12),
    OPC_RISC_ANDI   = OPC_RISC_ARITH_IMM | (0x7 << 12),
    OPC_RISC_SLLI   = OPC_RISC_ARITH_IMM | (0x1 << 12), // additional part of IMM
    OPC_RISC_SHIFT_RIGHT_I = OPC_RISC_ARITH_IMM | (0x5 << 12) // SRAI, SRLI
};







/* global register indices */
static TCGv_ptr cpu_env;
static TCGv cpu_gpr[32], cpu_PC;
static TCGv cpu_HI[MIPS_DSP_ACC], cpu_LO[MIPS_DSP_ACC], cpu_ACX[MIPS_DSP_ACC];
static TCGv cpu_dspctrl, btarget, bcond;
static TCGv_i32 hflags;
static TCGv_i32 fpu_fcr0, fpu_fcr31;
static TCGv_i64 fpu_f64[32];

static uint32_t gen_opc_hflags[OPC_BUF_SIZE];
static target_ulong gen_opc_btarget[OPC_BUF_SIZE];

#include "exec/gen-icount.h"

#define gen_helper_0e0i(name, arg) do {                           \
    TCGv_i32 helper_tmp = tcg_const_i32(arg);                     \
    gen_helper_##name(cpu_env, helper_tmp);                       \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_0e1i(name, arg1, arg2) do {                    \
    TCGv_i32 helper_tmp = tcg_const_i32(arg2);                    \
    gen_helper_##name(cpu_env, arg1, helper_tmp);                 \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_1e0i(name, ret, arg1) do {                     \
    TCGv_i32 helper_tmp = tcg_const_i32(arg1);                    \
    gen_helper_##name(ret, cpu_env, helper_tmp);                  \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_1e1i(name, ret, arg1, arg2) do {               \
    TCGv_i32 helper_tmp = tcg_const_i32(arg2);                    \
    gen_helper_##name(ret, cpu_env, arg1, helper_tmp);            \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_0e2i(name, arg1, arg2, arg3) do {              \
    TCGv_i32 helper_tmp = tcg_const_i32(arg3);                    \
    gen_helper_##name(cpu_env, arg1, arg2, helper_tmp);           \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_1e2i(name, ret, arg1, arg2, arg3) do {         \
    TCGv_i32 helper_tmp = tcg_const_i32(arg3);                    \
    gen_helper_##name(ret, cpu_env, arg1, arg2, helper_tmp);      \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

#define gen_helper_0e3i(name, arg1, arg2, arg3, arg4) do {        \
    TCGv_i32 helper_tmp = tcg_const_i32(arg4);                    \
    gen_helper_##name(cpu_env, arg1, arg2, arg3, helper_tmp);     \
    tcg_temp_free_i32(helper_tmp);                                \
    } while(0)

typedef struct DisasContext {
    struct TranslationBlock *tb;
    target_ulong pc, saved_pc;
    uint32_t opcode;
    int singlestep_enabled;
    int insn_flags;
    /* Routine used to access memory */
    int mem_idx;
    uint32_t hflags, saved_hflags;
    int bstate;
    target_ulong btarget;
} DisasContext;

enum {
    BS_NONE     = 0, /* We go out of the TB without reaching a branch or an
                      * exception condition */
    BS_STOP     = 1, /* We want to stop translation for any reason */
    BS_BRANCH   = 2, /* We reached a branch condition     */
    BS_EXCP     = 3, /* We reached an exception condition */
};

static const char * const regnames[] = {
    "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};

static const char * const regnames_HI[] = {
    "HI0", "HI1", "HI2", "HI3",
};

static const char * const regnames_LO[] = {
    "LO0", "LO1", "LO2", "LO3",
};

static const char * const regnames_ACX[] = {
    "ACX0", "ACX1", "ACX2", "ACX3",
};

static const char * const fregnames[] = {
    "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
    "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
    "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
};

#define MIPS_DEBUG(fmt, ...)                                                  \
    do {                                                                      \
        if (MIPS_DEBUG_DISAS) {                                               \
            qemu_log_mask(CPU_LOG_TB_IN_ASM,                                  \
                          TARGET_FMT_lx ": %08x " fmt "\n",                   \
                          ctx->pc, ctx->opcode , ## __VA_ARGS__);             \
        }                                                                     \
    } while (0)

#define LOG_DISAS(...)                                                        \
    do {                                                                      \
        if (MIPS_DEBUG_DISAS) {                                               \
            qemu_log_mask(CPU_LOG_TB_IN_ASM, ## __VA_ARGS__);                 \
        }                                                                     \
    } while (0)

#define MIPS_INVAL(op)                                                        \
    MIPS_DEBUG("Invalid %s %03x %03x %03x", op, ctx->opcode >> 26,            \
               ctx->opcode & 0x3F, ((ctx->opcode >> 16) & 0x1F))

/* General purpose registers moves. */
static inline void gen_load_gpr (TCGv t, int reg)
{
    if (reg == 0)
        tcg_gen_movi_tl(t, 0);
    else
        tcg_gen_mov_tl(t, cpu_gpr[reg]);
}

static inline void gen_store_gpr (TCGv t, int reg)
{
    if (reg != 0)
        tcg_gen_mov_tl(cpu_gpr[reg], t);
}

/* Moves to/from ACX register.  */
static inline void gen_load_ACX (TCGv t, int reg)
{
    tcg_gen_mov_tl(t, cpu_ACX[reg]);
}

static inline void gen_store_ACX (TCGv t, int reg)
{
    tcg_gen_mov_tl(cpu_ACX[reg], t);
}

static inline int get_fp_bit (int cc)
{
    if (cc)
        return 24 + cc;
    else
        return 23;
}

/* Tests */
static inline void gen_save_pc(target_ulong pc)
{
    tcg_gen_movi_tl(cpu_PC, pc);
}

static inline void save_cpu_state (DisasContext *ctx, int do_save_pc)
{
    LOG_DISAS("hflags %08x saved %08x\n", ctx->hflags, ctx->saved_hflags);
    if (do_save_pc && ctx->pc != ctx->saved_pc) {
        gen_save_pc(ctx->pc);
        ctx->saved_pc = ctx->pc;
    }
    if (ctx->hflags != ctx->saved_hflags) {
        tcg_gen_movi_i32(hflags, ctx->hflags);
        ctx->saved_hflags = ctx->hflags;
        switch (ctx->hflags & MIPS_HFLAG_BMASK_BASE) {
        case MIPS_HFLAG_BR:
            break;
        case MIPS_HFLAG_BC:
        case MIPS_HFLAG_BL:
        case MIPS_HFLAG_B:
            tcg_gen_movi_tl(btarget, ctx->btarget);
            break;
        }
    }
}

static inline void restore_cpu_state (CPUMIPSState *env, DisasContext *ctx)
{
    ctx->saved_hflags = ctx->hflags;
    switch (ctx->hflags & MIPS_HFLAG_BMASK_BASE) {
    case MIPS_HFLAG_BR:
        break;
    case MIPS_HFLAG_BC:
    case MIPS_HFLAG_BL:
    case MIPS_HFLAG_B:
        ctx->btarget = env->btarget;
        break;
    }
}

static inline void
generate_exception_err (DisasContext *ctx, int excp, int err)
{
    TCGv_i32 texcp = tcg_const_i32(excp);
    TCGv_i32 terr = tcg_const_i32(err);
    save_cpu_state(ctx, 1);
    gen_helper_raise_exception_err(cpu_env, texcp, terr);
    tcg_temp_free_i32(terr);
    tcg_temp_free_i32(texcp);
}

static inline void
generate_exception (DisasContext *ctx, int excp)
{
    save_cpu_state(ctx, 1);
    gen_helper_0e0i(raise_exception, excp);
}

/* Addresses computation */
static inline void gen_op_addr_add (DisasContext *ctx, TCGv ret, TCGv arg0, TCGv arg1)
{
    tcg_gen_add_tl(ret, arg0, arg1);

#if defined(TARGET_MIPS64)
    /* For compatibility with 32-bit code, data reference in user mode
       with Status_UX = 0 should be casted to 32-bit and sign extended.
       See the MIPS64 PRA manual, section 4.10. */
    if (((ctx->hflags & MIPS_HFLAG_KSU) == MIPS_HFLAG_UM) &&
        !(ctx->hflags & MIPS_HFLAG_UX)) {
        tcg_gen_ext32s_i64(ret, ret);
    }
#endif
}

static inline void check_cp0_enabled(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_CP0)))
        generate_exception_err(ctx, EXCP_CpU, 0);
}

static inline void check_cp1_enabled(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_FPU)))
        generate_exception_err(ctx, EXCP_CpU, 1);
}

/* Verify that the processor is running with COP1X instructions enabled.
   This is associated with the nabla symbol in the MIPS32 and MIPS64
   opcode tables.  */

static inline void check_cop1x(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_COP1X)))
        generate_exception(ctx, EXCP_RI);
}

/* Verify that the processor is running with 64-bit floating-point
   operations enabled.  */

static inline void check_cp1_64bitmode(DisasContext *ctx)
{
    if (unlikely(~ctx->hflags & (MIPS_HFLAG_F64 | MIPS_HFLAG_COP1X)))
        generate_exception(ctx, EXCP_RI);
}

/*
 * Verify if floating point register is valid; an operation is not defined
 * if bit 0 of any register specification is set and the FR bit in the
 * Status register equals zero, since the register numbers specify an
 * even-odd pair of adjacent coprocessor general registers. When the FR bit
 * in the Status register equals one, both even and odd register numbers
 * are valid. This limitation exists only for 64 bit wide (d,l,ps) registers.
 *
 * Multiple 64 bit wide registers can be checked by calling
 * gen_op_cp1_registers(freg1 | freg2 | ... | fregN);
 */
static inline void check_cp1_registers(DisasContext *ctx, int regs)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_F64) && (regs & 1)))
        generate_exception(ctx, EXCP_RI);
}

/* Verify that the processor is running with DSP instructions enabled.
   This is enabled by CP0 Status register MX(24) bit.
 */

static inline void check_dsp(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_DSP))) {
        if (ctx->insn_flags & ASE_DSP) {
            generate_exception(ctx, EXCP_DSPDIS);
        } else {
            generate_exception(ctx, EXCP_RI);
        }
    }
}

static inline void check_dspr2(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_DSPR2))) {
        if (ctx->insn_flags & ASE_DSP) {
            generate_exception(ctx, EXCP_DSPDIS);
        } else {
            generate_exception(ctx, EXCP_RI);
        }
    }
}

/* This code generates a "reserved instruction" exception if the
   CPU does not support the instruction set corresponding to flags. */
static inline void check_insn(DisasContext *ctx, int flags)
{
    if (unlikely(!(ctx->insn_flags & flags))) {
        generate_exception(ctx, EXCP_RI);
    }
}

/* This code generates a "reserved instruction" exception if 64-bit
   instructions are not enabled. */
static inline void check_mips_64(DisasContext *ctx)
{
    if (unlikely(!(ctx->hflags & MIPS_HFLAG_64)))
        generate_exception(ctx, EXCP_RI);
}

/* Define small wrappers for gen_load_fpr* so that we have a uniform
   calling interface for 32 and 64-bit FPRs.  No sense in changing
   all callers for gen_load_fpr32 when we need the CTX parameter for
   this one use.  */
#define gen_ldcmp_fpr32(ctx, x, y) gen_load_fpr32(x, y)
#define gen_ldcmp_fpr64(ctx, x, y) gen_load_fpr64(ctx, x, y)
#define FOP_CONDS(type, abs, fmt, ifmt, bits)                                 \
static inline void gen_cmp ## type ## _ ## fmt(DisasContext *ctx, int n,      \
                                               int ft, int fs, int cc)        \
{                                                                             \
    TCGv_i##bits fp0 = tcg_temp_new_i##bits ();                               \
    TCGv_i##bits fp1 = tcg_temp_new_i##bits ();                               \
    switch (ifmt) {                                                           \
    case FMT_PS:                                                              \
        check_cp1_64bitmode(ctx);                                             \
        break;                                                                \
    case FMT_D:                                                               \
        if (abs) {                                                            \
            check_cop1x(ctx);                                                 \
        }                                                                     \
        check_cp1_registers(ctx, fs | ft);                                    \
        break;                                                                \
    case FMT_S:                                                               \
        if (abs) {                                                            \
            check_cop1x(ctx);                                                 \
        }                                                                     \
        break;                                                                \
    }                                                                         \
    gen_ldcmp_fpr##bits (ctx, fp0, fs);                                       \
    gen_ldcmp_fpr##bits (ctx, fp1, ft);                                       \
    switch (n) {                                                              \
    case  0: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _f, fp0, fp1, cc);    break;\
    case  1: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _un, fp0, fp1, cc);   break;\
    case  2: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _eq, fp0, fp1, cc);   break;\
    case  3: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ueq, fp0, fp1, cc);  break;\
    case  4: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _olt, fp0, fp1, cc);  break;\
    case  5: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ult, fp0, fp1, cc);  break;\
    case  6: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ole, fp0, fp1, cc);  break;\
    case  7: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ule, fp0, fp1, cc);  break;\
    case  8: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _sf, fp0, fp1, cc);   break;\
    case  9: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ngle, fp0, fp1, cc); break;\
    case 10: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _seq, fp0, fp1, cc);  break;\
    case 11: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ngl, fp0, fp1, cc);  break;\
    case 12: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _lt, fp0, fp1, cc);   break;\
    case 13: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _nge, fp0, fp1, cc);  break;\
    case 14: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _le, fp0, fp1, cc);   break;\
    case 15: gen_helper_0e2i(cmp ## type ## _ ## fmt ## _ngt, fp0, fp1, cc);  break;\
    default: abort();                                                         \
    }                                                                         \
    tcg_temp_free_i##bits (fp0);                                              \
    tcg_temp_free_i##bits (fp1);                                              \
}

#undef FOP_CONDS
#undef gen_ldcmp_fpr32
#undef gen_ldcmp_fpr64

/* load/store instructions. */
#ifdef CONFIG_USER_ONLY
#define OP_LD_ATOMIC(insn,fname)                                           \
static inline void op_ld_##insn(TCGv ret, TCGv arg1, DisasContext *ctx)    \
{                                                                          \
    TCGv t0 = tcg_temp_new();                                              \
    tcg_gen_mov_tl(t0, arg1);                                              \
    tcg_gen_qemu_##fname(ret, arg1, ctx->mem_idx);                         \
    tcg_gen_st_tl(t0, cpu_env, offsetof(CPUMIPSState, lladdr));                \
    tcg_gen_st_tl(ret, cpu_env, offsetof(CPUMIPSState, llval));                \
    tcg_temp_free(t0);                                                     \
}
#else
#define OP_LD_ATOMIC(insn,fname)                                           \
static inline void op_ld_##insn(TCGv ret, TCGv arg1, DisasContext *ctx)    \
{                                                                          \
    gen_helper_1e1i(insn, ret, arg1, ctx->mem_idx);                        \
}
#endif
OP_LD_ATOMIC(ll,ld32s);
#if defined(TARGET_MIPS64)
OP_LD_ATOMIC(lld,ld64);
#endif
#undef OP_LD_ATOMIC

#ifdef CONFIG_USER_ONLY
#define OP_ST_ATOMIC(insn,fname,ldname,almask)                               \
static inline void op_st_##insn(TCGv arg1, TCGv arg2, int rt, DisasContext *ctx) \
{                                                                            \
    TCGv t0 = tcg_temp_new();                                                \
    int l1 = gen_new_label();                                                \
    int l2 = gen_new_label();                                                \
                                                                             \
    tcg_gen_andi_tl(t0, arg2, almask);                                       \
    tcg_gen_brcondi_tl(TCG_COND_EQ, t0, 0, l1);                              \
    tcg_gen_st_tl(arg2, cpu_env, offsetof(CPUMIPSState, CP0_BadVAddr));          \
    generate_exception(ctx, EXCP_AdES);                                      \
    gen_set_label(l1);                                                       \
    tcg_gen_ld_tl(t0, cpu_env, offsetof(CPUMIPSState, lladdr));                  \
    tcg_gen_brcond_tl(TCG_COND_NE, arg2, t0, l2);                            \
    tcg_gen_movi_tl(t0, rt | ((almask << 3) & 0x20));                        \
    tcg_gen_st_tl(t0, cpu_env, offsetof(CPUMIPSState, llreg));                   \
    tcg_gen_st_tl(arg1, cpu_env, offsetof(CPUMIPSState, llnewval));              \
    gen_helper_0e0i(raise_exception, EXCP_SC);                               \
    gen_set_label(l2);                                                       \
    tcg_gen_movi_tl(t0, 0);                                                  \
    gen_store_gpr(t0, rt);                                                   \
    tcg_temp_free(t0);                                                       \
}
#else
#define OP_ST_ATOMIC(insn,fname,ldname,almask)                               \
static inline void op_st_##insn(TCGv arg1, TCGv arg2, int rt, DisasContext *ctx) \
{                                                                            \
    TCGv t0 = tcg_temp_new();                                                \
    gen_helper_1e2i(insn, t0, arg1, arg2, ctx->mem_idx);                     \
    gen_store_gpr(t0, rt);                                                   \
    tcg_temp_free(t0);                                                       \
}
#endif
OP_ST_ATOMIC(sc,st32,ld32s,0x3);
#if defined(TARGET_MIPS64)
OP_ST_ATOMIC(scd,st64,ld64,0x7);
#endif
#undef OP_ST_ATOMIC

static inline void gen_goto_tb(DisasContext *ctx, int n, target_ulong dest)
{
    TranslationBlock *tb;
    tb = ctx->tb;
    if ((tb->pc & TARGET_PAGE_MASK) == (dest & TARGET_PAGE_MASK) &&
        likely(!ctx->singlestep_enabled)) {
        tcg_gen_goto_tb(n);
        gen_save_pc(dest);
        tcg_gen_exit_tb((uintptr_t)tb + n);
    } else {
        gen_save_pc(dest);
        if (ctx->singlestep_enabled) {
            save_cpu_state(ctx, 0);
            gen_helper_0e0i(raise_exception, EXCP_DEBUG);
        }
        tcg_gen_exit_tb(0);
    }
}

static void gen_arith(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int rs2)
{
    switch (opc) {

    case OPC_RISC_ADD:
        tcg_gen_add_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SUB:
        tcg_gen_sub_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SLL: // TODO rs2 out of range check?
        tcg_gen_shl_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SLT:
        tcg_gen_setcond_tl(TCG_COND_LT, cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SLTU:
        tcg_gen_setcond_tl(TCG_COND_LTU, cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_XOR:
        tcg_gen_xor_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SRL: // TODO rs2 out of range check?
        tcg_gen_shr_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_SRA: // TODO rs2 out of range check?
        tcg_gen_sar_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_OR:
        tcg_gen_or_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    case OPC_RISC_AND:
        tcg_gen_and_tl(cpu_gpr[rd], cpu_gpr[rs1], cpu_gpr[rs2]);
        break;
    default:
        // TODO EXCEPTION
        break;

    }
}

/* lower 12 bits of imm are valid */
static void gen_arith_imm(DisasContext *ctx, uint32_t opc, 
                      int rd, int rs1, int16_t imm)
{

    target_ulong uimm = (target_long)imm; /* sign ext 16->64 bits */

    switch (opc) {

    case OPC_RISC_ADDI:
        tcg_gen_addi_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_SLTI:
        tcg_gen_setcondi_tl(TCG_COND_LT, cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_SLTIU:
        tcg_gen_setcondi_tl(TCG_COND_LTU, cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_XORI:
        tcg_gen_xori_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_ORI:
        tcg_gen_ori_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_ANDI:
        tcg_gen_andi_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_SLLI: // TODO: add immediate upper bits check?
        tcg_gen_shli_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        break;
    case OPC_RISC_SHIFT_RIGHT_I: // SRLI, SRAI, TODO: upper bits check
        // differentiate on IMM
        if (uimm & 0x20) {
            // SRAI
            tcg_gen_sari_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm ^ 0x20);
        } else {
            tcg_gen_shri_tl(cpu_gpr[rd], cpu_gpr[rs1], uimm);
        }
        break;
    default:
        // TODO EXCEPTION
        break;

    }

}


// SAGARSAYS: this is the fn called to decode each inst

#define SIGN_EXT_IMM_12_TO_16(imm)   ((int16_t)((imm << 4) >> 4))

static void decode_opc (CPUMIPSState *env, DisasContext *ctx)
{
    //int32_t offset;
    int rs1;
    int rs2;
    int rd;
    uint32_t op;// op1, op2;
    int16_t imm;

    /* make sure instructions are on a word boundary */
    if (ctx->pc & 0x3) {
        env->CP0_BadVAddr = ctx->pc;
        generate_exception(ctx, EXCP_AdEL);
        return;
    }

    /* Handle blikely not taken case */
    if ((ctx->hflags & MIPS_HFLAG_BMASK_BASE) == MIPS_HFLAG_BL) {
        int l1 = gen_new_label();

        MIPS_DEBUG("blikely condition (" TARGET_FMT_lx ")", ctx->pc + 4);
        tcg_gen_brcondi_tl(TCG_COND_NE, bcond, 0, l1);
        tcg_gen_movi_i32(hflags, ctx->hflags & ~MIPS_HFLAG_BMASK);
        gen_goto_tb(ctx, 1, ctx->pc + 4);
        gen_set_label(l1);
    }

    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT))) {
        tcg_gen_debug_insn_start(ctx->pc);
    }

    op = MASK_OP_MAJOR(ctx->opcode);
    rs1 = (ctx->opcode >> 15) & 0x1f;
    rs2 = (ctx->opcode >> 20) & 0x1f;
    rd = (ctx->opcode >> 7) & 0x1f;
    //sa = (ctx->opcode >> 6) & 0x1f;
    imm = (int16_t)((ctx->opcode >> 20) & 0xFFF);
    switch (op) {


    case OPC_RISC_LUI:
        // handle LUI
        tcg_gen_movi_tl(cpu_gpr[rd], (ctx->opcode & 0xFFFFF000));
        tcg_gen_ext32s_tl(cpu_gpr[rd], cpu_gpr[rd]);
        break;

    case OPC_RISC_AUIPC:
        // handle AUIPC
        tcg_gen_movi_tl(cpu_gpr[rd], (ctx->opcode & 0xFFFFF000));
        tcg_gen_ext32s_tl(cpu_gpr[rd], cpu_gpr[rd]);
        tcg_gen_add_tl(cpu_gpr[rd], cpu_gpr[rd], tcg_const_tl(ctx->pc)); // TODO: CHECK THIS
        break;

    case OPC_RISC_JAL:
        // TODO
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP
        break;


    case OPC_RISC_JALR:
        // TODO
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP

        break;



    case OPC_RISC_BRANCH:
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP


        break;

    case OPC_RISC_LOAD:
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP


        break;

    case OPC_RISC_STORE:
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP

        break;

    case OPC_RISC_ARITH_IMM:
        gen_arith_imm(ctx, MASK_OP_ARITH_IMM(ctx->opcode), rd, rs1, SIGN_EXT_IMM_12_TO_16(imm));
        break;

    case OPC_RISC_ARITH:
        gen_arith(ctx, MASK_OP_ARITH(ctx->opcode), rd, rs1, rs2);
        break;


    default:            /* Invalid */
        tcg_gen_movi_tl(cpu_gpr[0], (ctx->opcode & 0x0)); // NOP

        // TODO REMOVED FOR TESTING, REPLACE
/*        MIPS_INVAL("major opcode");
        generate_exception(ctx, EXCP_RI); */
        break;
    }
}




static inline void
gen_intermediate_code_internal(MIPSCPU *cpu, TranslationBlock *tb,
                               bool search_pc)
{
    CPUState *cs = CPU(cpu);
    CPUMIPSState *env = &cpu->env;
    DisasContext ctx;
    target_ulong pc_start;
    uint16_t *gen_opc_end;
    CPUBreakpoint *bp;
    int j, lj = -1;
    int num_insns;
    int max_insns;
    int insn_bytes;

    if (search_pc)
        qemu_log("search pc %d\n", search_pc);

    pc_start = tb->pc;
    gen_opc_end = tcg_ctx.gen_opc_buf + OPC_MAX_SIZE;
    ctx.pc = pc_start;
    ctx.saved_pc = -1;
    ctx.singlestep_enabled = cs->singlestep_enabled;
    ctx.insn_flags = env->insn_flags;
    ctx.tb = tb;
    ctx.bstate = BS_NONE;
    /* Restore delay slot state from the tb context.  */
    ctx.hflags = (uint32_t)tb->flags; /* FIXME: maybe use 64 bits here? */
    restore_cpu_state(env, &ctx);
#ifdef CONFIG_USER_ONLY
        ctx.mem_idx = MIPS_HFLAG_UM;
#else
        ctx.mem_idx = ctx.hflags & MIPS_HFLAG_KSU;
#endif
    num_insns = 0;
    max_insns = tb->cflags & CF_COUNT_MASK;
    if (max_insns == 0)
        max_insns = CF_COUNT_MASK;
    LOG_DISAS("\ntb %p idx %d hflags %04x\n", tb, ctx.mem_idx, ctx.hflags);
    gen_tb_start();
    while (ctx.bstate == BS_NONE) {
        if (unlikely(!QTAILQ_EMPTY(&cs->breakpoints))) {
            QTAILQ_FOREACH(bp, &cs->breakpoints, entry) {
                if (bp->pc == ctx.pc) {
                    save_cpu_state(&ctx, 1);
                    ctx.bstate = BS_BRANCH;
                    gen_helper_0e0i(raise_exception, EXCP_DEBUG);
                    /* Include the breakpoint location or the tb won't
                     * be flushed when it must be.  */
                    ctx.pc += 4;
                    goto done_generating;
                }
            }
        }

        if (search_pc) {
            j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
            if (lj < j) {
                lj++;
                while (lj < j)
                    tcg_ctx.gen_opc_instr_start[lj++] = 0;
            }
            tcg_ctx.gen_opc_pc[lj] = ctx.pc;
            gen_opc_hflags[lj] = ctx.hflags & MIPS_HFLAG_BMASK;
            gen_opc_btarget[lj] = ctx.btarget;
            tcg_ctx.gen_opc_instr_start[lj] = 1;
            tcg_ctx.gen_opc_icount[lj] = num_insns;
        }
        if (num_insns + 1 == max_insns && (tb->cflags & CF_LAST_IO))
            gen_io_start();

        if (!(ctx.hflags & MIPS_HFLAG_M16)) {
            ctx.opcode = cpu_ldl_code(env, ctx.pc);
            insn_bytes = 4;
            decode_opc(env, &ctx);
        } else {
            generate_exception(&ctx, EXCP_RI);
            ctx.bstate = BS_STOP;
            break;
        }
        ctx.pc += insn_bytes;

        num_insns++;

        /* Execute a branch and its delay slot as a single instruction.
           This is what GDB expects and is consistent with what the
           hardware does (e.g. if a delay slot instruction faults, the
           reported PC is the PC of the branch).  */
        if (cs->singlestep_enabled && (ctx.hflags & MIPS_HFLAG_BMASK) == 0) {
            break;
        }

        if ((ctx.pc & (TARGET_PAGE_SIZE - 1)) == 0)
            break;

        if (tcg_ctx.gen_opc_ptr >= gen_opc_end) {
            break;
        }

        if (num_insns >= max_insns)
            break;

        if (singlestep)
            break;
    }
    if (tb->cflags & CF_LAST_IO) {
        gen_io_end();
    }
    if (cs->singlestep_enabled && ctx.bstate != BS_BRANCH) {
        save_cpu_state(&ctx, ctx.bstate == BS_NONE);
        gen_helper_0e0i(raise_exception, EXCP_DEBUG);
    } else {
        switch (ctx.bstate) {
        case BS_STOP:
            gen_goto_tb(&ctx, 0, ctx.pc);
            break;
        case BS_NONE:
            save_cpu_state(&ctx, 0);
            gen_goto_tb(&ctx, 0, ctx.pc);
            break;
        case BS_EXCP:
            tcg_gen_exit_tb(0);
            break;
        case BS_BRANCH:
        default:
            break;
        }
    }
done_generating:
    gen_tb_end(tb, num_insns);
    *tcg_ctx.gen_opc_ptr = INDEX_op_end;
    if (search_pc) {
        j = tcg_ctx.gen_opc_ptr - tcg_ctx.gen_opc_buf;
        lj++;
        while (lj <= j)
            tcg_ctx.gen_opc_instr_start[lj++] = 0;
    } else {
        tb->size = ctx.pc - pc_start;
        tb->icount = num_insns;
    }
#ifdef DEBUG_DISAS
    LOG_DISAS("\n");
    if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM)) {
        qemu_log("IN: %s\n", lookup_symbol(pc_start));
        log_target_disas(env, pc_start, ctx.pc - pc_start, 0);
        qemu_log("\n");
    }
#endif
}

void gen_intermediate_code (CPUMIPSState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(mips_env_get_cpu(env), tb, false);
}

void gen_intermediate_code_pc (CPUMIPSState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(mips_env_get_cpu(env), tb, true);
}

#if defined(TARGET_MIPS64) && defined(MIPS_DEBUG_SIGN_EXTENSIONS)
/* Debug help: The architecture requires 32bit code to maintain proper
   sign-extended values on 64bit machines.  */

#define SIGN_EXT_P(val) ((((val) & ~0x7fffffff) == 0) || (((val) & ~0x7fffffff) == ~0x7fffffff))

static void
cpu_mips_check_sign_extensions (CPUMIPSState *env, FILE *f,
                                fprintf_function cpu_fprintf,
                                int flags)
{
    int i;

    if (!SIGN_EXT_P(env->active_tc.PC))
        cpu_fprintf(f, "BROKEN: pc=0x" TARGET_FMT_lx "\n", env->active_tc.PC);
    if (!SIGN_EXT_P(env->active_tc.HI[0]))
        cpu_fprintf(f, "BROKEN: HI=0x" TARGET_FMT_lx "\n", env->active_tc.HI[0]);
    if (!SIGN_EXT_P(env->active_tc.LO[0]))
        cpu_fprintf(f, "BROKEN: LO=0x" TARGET_FMT_lx "\n", env->active_tc.LO[0]);
    if (!SIGN_EXT_P(env->btarget))
        cpu_fprintf(f, "BROKEN: btarget=0x" TARGET_FMT_lx "\n", env->btarget);

    for (i = 0; i < 32; i++) {
        if (!SIGN_EXT_P(env->active_tc.gpr[i]))
            cpu_fprintf(f, "BROKEN: %s=0x" TARGET_FMT_lx "\n", regnames[i], env->active_tc.gpr[i]);
    }

    if (!SIGN_EXT_P(env->CP0_EPC))
        cpu_fprintf(f, "BROKEN: EPC=0x" TARGET_FMT_lx "\n", env->CP0_EPC);
    if (!SIGN_EXT_P(env->lladdr))
        cpu_fprintf(f, "BROKEN: LLAddr=0x" TARGET_FMT_lx "\n", env->lladdr);
}
#endif

void mips_cpu_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf,
                         int flags)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;
    int i;

    cpu_fprintf(f, "pc=0x" TARGET_FMT_lx " HI=0x" TARGET_FMT_lx
                " LO=0x" TARGET_FMT_lx " ds %04x "
                TARGET_FMT_lx " " TARGET_FMT_ld "\n",
                env->active_tc.PC, env->active_tc.HI[0], env->active_tc.LO[0],
                env->hflags, env->btarget, env->bcond);
    for (i = 0; i < 32; i++) {
        if ((i & 3) == 0)
            cpu_fprintf(f, "GPR%02d:", i);
        cpu_fprintf(f, " %s " TARGET_FMT_lx, regnames[i], env->active_tc.gpr[i]);
        if ((i & 3) == 3)
            cpu_fprintf(f, "\n");
    }

    cpu_fprintf(f, "CP0 Status  0x%08x Cause   0x%08x EPC    0x" TARGET_FMT_lx "\n",
                env->CP0_Status, env->CP0_Cause, env->CP0_EPC);
    cpu_fprintf(f, "    Config0 0x%08x Config1 0x%08x LLAddr 0x" TARGET_FMT_lx "\n",
                env->CP0_Config0, env->CP0_Config1, env->lladdr);
    if (env->hflags & MIPS_HFLAG_FPU) {
        ;
    }
#if defined(TARGET_MIPS64) && defined(MIPS_DEBUG_SIGN_EXTENSIONS)
    cpu_mips_check_sign_extensions(env, f, cpu_fprintf, flags);
#endif
}

void mips_tcg_init(void)
{
    int i;
    static int inited;

    /* Initialize various static tables. */
    if (inited)
        return;

    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");
    TCGV_UNUSED(cpu_gpr[0]);
    for (i = 1; i < 32; i++)
        cpu_gpr[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUMIPSState, active_tc.gpr[i]),
                                        regnames[i]);

    for (i = 0; i < 32; i++) {
        int off = offsetof(CPUMIPSState, active_fpu.fpr[i]);
        fpu_f64[i] = tcg_global_mem_new_i64(TCG_AREG0, off, fregnames[i]);
    }

    cpu_PC = tcg_global_mem_new(TCG_AREG0,
                                offsetof(CPUMIPSState, active_tc.PC), "PC");
    for (i = 0; i < MIPS_DSP_ACC; i++) {
        cpu_HI[i] = tcg_global_mem_new(TCG_AREG0,
                                       offsetof(CPUMIPSState, active_tc.HI[i]),
                                       regnames_HI[i]);
        cpu_LO[i] = tcg_global_mem_new(TCG_AREG0,
                                       offsetof(CPUMIPSState, active_tc.LO[i]),
                                       regnames_LO[i]);
        cpu_ACX[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUMIPSState, active_tc.ACX[i]),
                                        regnames_ACX[i]);
    }
    cpu_dspctrl = tcg_global_mem_new(TCG_AREG0,
                                     offsetof(CPUMIPSState, active_tc.DSPControl),
                                     "DSPControl");
    bcond = tcg_global_mem_new(TCG_AREG0,
                               offsetof(CPUMIPSState, bcond), "bcond");
    btarget = tcg_global_mem_new(TCG_AREG0,
                                 offsetof(CPUMIPSState, btarget), "btarget");
    hflags = tcg_global_mem_new_i32(TCG_AREG0,
                                    offsetof(CPUMIPSState, hflags), "hflags");

    fpu_fcr0 = tcg_global_mem_new_i32(TCG_AREG0,
                                      offsetof(CPUMIPSState, active_fpu.fcr0),
                                      "fcr0");
    fpu_fcr31 = tcg_global_mem_new_i32(TCG_AREG0,
                                       offsetof(CPUMIPSState, active_fpu.fcr31),
                                       "fcr31");

    inited = 1;
}

#include "translate_init.c"

MIPSCPU *cpu_mips_init(const char *cpu_model)
{
    MIPSCPU *cpu;
    CPUMIPSState *env;
    const mips_def_t *def;

    def = cpu_mips_find_by_name(cpu_model);
    if (!def)
        return NULL;
    cpu = MIPS_CPU(object_new(TYPE_MIPS_CPU));
    env = &cpu->env;
    env->cpu_model = def;

#ifndef CONFIG_USER_ONLY
    mmu_init(env, def);
#endif
    fpu_init(env, def);
    mvp_init(env, def);

    object_property_set_bool(OBJECT(cpu), true, "realized", NULL);

    return cpu;
}

void cpu_state_reset(CPUMIPSState *env)
{
    MIPSCPU *cpu = mips_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    /* Reset registers to their default values */
    env->CP0_PRid = env->cpu_model->CP0_PRid;
    env->CP0_Config0 = env->cpu_model->CP0_Config0;
#ifdef TARGET_WORDS_BIGENDIAN
    env->CP0_Config0 |= (1 << CP0C0_BE);
#endif
    env->CP0_Config1 = env->cpu_model->CP0_Config1;
    env->CP0_Config2 = env->cpu_model->CP0_Config2;
    env->CP0_Config3 = env->cpu_model->CP0_Config3;
    env->CP0_Config4 = env->cpu_model->CP0_Config4;
    env->CP0_Config4_rw_bitmask = env->cpu_model->CP0_Config4_rw_bitmask;
    env->CP0_Config5 = env->cpu_model->CP0_Config5;
    env->CP0_Config5_rw_bitmask = env->cpu_model->CP0_Config5_rw_bitmask;
    env->CP0_Config6 = env->cpu_model->CP0_Config6;
    env->CP0_Config7 = env->cpu_model->CP0_Config7;
    env->CP0_LLAddr_rw_bitmask = env->cpu_model->CP0_LLAddr_rw_bitmask
                                 << env->cpu_model->CP0_LLAddr_shift;
    env->CP0_LLAddr_shift = env->cpu_model->CP0_LLAddr_shift;
    env->SYNCI_Step = env->cpu_model->SYNCI_Step;
    env->CCRes = env->cpu_model->CCRes;
    env->CP0_Status_rw_bitmask = env->cpu_model->CP0_Status_rw_bitmask;
    env->CP0_TCStatus_rw_bitmask = env->cpu_model->CP0_TCStatus_rw_bitmask;
    env->CP0_SRSCtl = env->cpu_model->CP0_SRSCtl;
    env->current_tc = 0;
    env->SEGBITS = env->cpu_model->SEGBITS;
    env->SEGMask = (target_ulong)((1ULL << env->cpu_model->SEGBITS) - 1);
#if defined(TARGET_MIPS64)
    if (env->cpu_model->insn_flags & ISA_MIPS3) {
        env->SEGMask |= 3ULL << 62;
    }
#endif
    env->PABITS = env->cpu_model->PABITS;
    env->PAMask = (target_ulong)((1ULL << env->cpu_model->PABITS) - 1);
    env->CP0_SRSConf0_rw_bitmask = env->cpu_model->CP0_SRSConf0_rw_bitmask;
    env->CP0_SRSConf0 = env->cpu_model->CP0_SRSConf0;
    env->CP0_SRSConf1_rw_bitmask = env->cpu_model->CP0_SRSConf1_rw_bitmask;
    env->CP0_SRSConf1 = env->cpu_model->CP0_SRSConf1;
    env->CP0_SRSConf2_rw_bitmask = env->cpu_model->CP0_SRSConf2_rw_bitmask;
    env->CP0_SRSConf2 = env->cpu_model->CP0_SRSConf2;
    env->CP0_SRSConf3_rw_bitmask = env->cpu_model->CP0_SRSConf3_rw_bitmask;
    env->CP0_SRSConf3 = env->cpu_model->CP0_SRSConf3;
    env->CP0_SRSConf4_rw_bitmask = env->cpu_model->CP0_SRSConf4_rw_bitmask;
    env->CP0_SRSConf4 = env->cpu_model->CP0_SRSConf4;
    env->active_fpu.fcr0 = env->cpu_model->CP1_fcr0;
    env->insn_flags = env->cpu_model->insn_flags;

#if defined(CONFIG_USER_ONLY)
    env->CP0_Status = (MIPS_HFLAG_UM << CP0St_KSU);
# ifdef TARGET_MIPS64
    /* Enable 64-bit register mode.  */
    env->CP0_Status |= (1 << CP0St_PX);
# endif
# ifdef TARGET_ABI_MIPSN64
    /* Enable 64-bit address mode.  */
    env->CP0_Status |= (1 << CP0St_UX);
# endif
    /* Enable access to the CPUNum, SYNCI_Step, CC, and CCRes RDHWR
       hardware registers.  */
    env->CP0_HWREna |= 0x0000000F;
    if (env->CP0_Config1 & (1 << CP0C1_FP)) {
        env->CP0_Status |= (1 << CP0St_CU1);
    }
    if (env->CP0_Config3 & (1 << CP0C3_DSPP)) {
        env->CP0_Status |= (1 << CP0St_MX);
    }
# if defined(TARGET_MIPS64)
    /* For MIPS64, init FR bit to 1 if FPU unit is there and bit is writable. */
    if ((env->CP0_Config1 & (1 << CP0C1_FP)) &&
        (env->CP0_Status_rw_bitmask & (1 << CP0St_FR))) {
        env->CP0_Status |= (1 << CP0St_FR);
    }
# endif
#else
    if (env->hflags & MIPS_HFLAG_BMASK) {
        /* If the exception was raised from a delay slot,
           come back to the jump.  */
        env->CP0_ErrorEPC = env->active_tc.PC - 4;
    } else {
        env->CP0_ErrorEPC = env->active_tc.PC;
    }
    env->active_tc.PC = (int32_t)0xBFC00000;
    env->CP0_Random = env->tlb->nb_tlb - 1;
    env->tlb->tlb_in_use = env->tlb->nb_tlb;
    env->CP0_Wired = 0;
    env->CP0_EBase = 0x80000000 | (cs->cpu_index & 0x3FF);
    env->CP0_Status = (1 << CP0St_BEV) | (1 << CP0St_ERL);
    /* vectored interrupts not implemented, timer on int 7,
       no performance counters. */
    env->CP0_IntCtl = 0xe0000000;
    {
        int i;

        for (i = 0; i < 7; i++) {
            env->CP0_WatchLo[i] = 0;
            env->CP0_WatchHi[i] = 0x80000000;
        }
        env->CP0_WatchLo[7] = 0;
        env->CP0_WatchHi[7] = 0;
    }
    /* Count register increments in debug mode, EJTAG version 1 */
    env->CP0_Debug = (1 << CP0DB_CNT) | (0x1 << CP0DB_VER);

    if (env->CP0_Config3 & (1 << CP0C3_MT)) {
        int i;

        /* Only TC0 on VPE 0 starts as active.  */
        for (i = 0; i < ARRAY_SIZE(env->tcs); i++) {
            env->tcs[i].CP0_TCBind = cs->cpu_index << CP0TCBd_CurVPE;
            env->tcs[i].CP0_TCHalt = 1;
        }
        env->active_tc.CP0_TCHalt = 1;
        cs->halted = 1;

        if (cs->cpu_index == 0) {
            /* VPE0 starts up enabled.  */
            env->mvp->CP0_MVPControl |= (1 << CP0MVPCo_EVP);
            env->CP0_VPEConf0 |= (1 << CP0VPEC0_MVP) | (1 << CP0VPEC0_VPA);

            /* TC0 starts up unhalted.  */
            cs->halted = 0;
            env->active_tc.CP0_TCHalt = 0;
            env->tcs[0].CP0_TCHalt = 0;
            /* With thread 0 active.  */
            env->active_tc.CP0_TCStatus = (1 << CP0TCSt_A);
            env->tcs[0].CP0_TCStatus = (1 << CP0TCSt_A);
        }
    }
#endif
    compute_hflags(env);
    cs->exception_index = EXCP_NONE;
}

void restore_state_to_opc(CPUMIPSState *env, TranslationBlock *tb, int pc_pos)
{
    env->active_tc.PC = tcg_ctx.gen_opc_pc[pc_pos];
    env->hflags &= ~MIPS_HFLAG_BMASK;
    env->hflags |= gen_opc_hflags[pc_pos];
    switch (env->hflags & MIPS_HFLAG_BMASK_BASE) {
    case MIPS_HFLAG_BR:
        break;
    case MIPS_HFLAG_BC:
    case MIPS_HFLAG_BL:
    case MIPS_HFLAG_B:
        env->btarget = gen_opc_btarget[pc_pos];
        break;
    }
}
