/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) ARM Limited, 2014
 *
 * Authors:
 *  Jean-Philippe Brucker <jean-philippe.brucker@arm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Condition check code is copied from Linux's
 * - arch/arm/kernel/opcodes.c
 * - arch/arm/kvm/emulate.c
 */

#include <asm/control.h>
#include <asm/gic_common.h>
#include <asm/platform.h>
#include <asm/psci.h>
#include <asm/traps.h>
#include <asm/sysregs.h>
#include <jailhouse/printk.h>
#include <jailhouse/control.h>

/*
 * condition code lookup table
 * index into the table is test code: EQ, NE, ... LT, GT, AL, NV
 *
 * bit position in short is condition code: NZCV
 */
static const unsigned short cc_map[16] = {
	0xF0F0,			/* EQ == Z set            */
	0x0F0F,			/* NE                     */
	0xCCCC,			/* CS == C set            */
	0x3333,			/* CC                     */
	0xFF00,			/* MI == N set            */
	0x00FF,			/* PL                     */
	0xAAAA,			/* VS == V set            */
	0x5555,			/* VC                     */
	0x0C0C,			/* HI == C set && Z clear */
	0xF3F3,			/* LS == C clear || Z set */
	0xAA55,			/* GE == (N==V)           */
	0x55AA,			/* LT == (N!=V)           */
	0x0A05,			/* GT == (!Z && (N==V))   */
	0xF5FA,			/* LE == (Z || (N!=V))    */
	0xFFFF,			/* AL always              */
	0			/* NV                     */
};

/* Check condition field either from HSR or from SPSR in thumb mode */
static bool arch_failed_condition(struct trap_context *ctx)
{
	u32 class = HSR_EC(ctx->hsr);
	u32 icc = HSR_ICC(ctx->hsr);
	u32 cpsr = ctx->cpsr;
	u32 flags = cpsr >> 28;
	u32 cond;
	/*
	 * Trapped instruction is unconditional, already passed the condition
	 * check, or is invalid
	 */
	if (class & 0x30 || class == 0)
		return false;

	/* Is condition field valid? */
	if (icc & HSR_ICC_CV_BIT) {
		cond = HSR_ICC_COND(icc);
	} else {
		/* This can happen in Thumb mode: examine IT state. */
		unsigned long it = PSR_IT(cpsr);

		/* it == 0 => unconditional. */
		if (it == 0)
			return false;

		/* The cond for this insn works out as the top 4 bits. */
		cond = (it >> 4);
	}

	/* Compare the apsr flags with the condition code */
	if ((cc_map[cond] >> flags) & 1)
		return false;

	return true;
}

/*
 * When exceptions occur while instructions are executed in Thumb IF-THEN
 * blocks, the ITSTATE field of the CPSR is not advanced (updated), so we have
 * to do this little bit of work manually. The fields map like this:
 *
 * IT[7:0] -> CPSR[26:25],CPSR[15:10]
 */
static void arch_advance_itstate(struct trap_context *ctx)
{
	unsigned long itbits, cond;
	unsigned long cpsr = ctx->cpsr;

	if (!(cpsr & PSR_IT_MASK(0xff)))
		return;

	itbits = PSR_IT(cpsr);
	cond = itbits >> 5;

	if ((itbits & 0x7) == 0)
		/* One instruction left in the block, next itstate is 0 */
		itbits = cond = 0;
	else
		itbits = (itbits << 1) & 0x1f;

	itbits |= (cond << 5);
	cpsr &= ~PSR_IT_MASK(0xff);
	cpsr |= PSR_IT_MASK(itbits);

	ctx->cpsr = cpsr;
}

void arch_skip_instruction(struct trap_context *ctx)
{
	u32 instruction_length = HSR_IL(ctx->hsr);

	ctx->pc += (instruction_length ? 4 : 2);
	arch_advance_itstate(ctx);
}

void access_cell_reg(struct trap_context *ctx, u8 reg, unsigned long *val,
		     bool is_read)
{
	unsigned long mode = ctx->cpsr & PSR_MODE_MASK;

	switch (reg) {
	case 0 ... 7:
		access_usr_reg(ctx, reg, val, is_read);
		break;
	case 8 ... 12:
		if (mode == PSR_FIQ_MODE)
			access_fiq_reg(reg, val, is_read);
		else
			access_usr_reg(ctx, reg, val, is_read);
		break;
	case 13 ... 14:
		switch (mode) {
		case PSR_USR_MODE:
		case PSR_SYS_MODE:
			/*
			 * lr is saved on the stack, as it is not banked in HYP
			 * mode. sp is banked, so lr is at offset 13 in the USR
			 * regs.
			 */
			if (reg == 13)
				access_banked_reg(usr, reg, val, is_read);
			else
				access_usr_reg(ctx, 13, val, is_read);
			break;
		case PSR_SVC_MODE:
			access_banked_reg(svc, reg, val, is_read);
			break;
		case PSR_UND_MODE:
			access_banked_reg(und, reg, val, is_read);
			break;
		case PSR_ABT_MODE:
			access_banked_reg(abt, reg, val, is_read);
			break;
		case PSR_IRQ_MODE:
			access_banked_reg(irq, reg, val, is_read);
			break;
		case PSR_FIQ_MODE:
			access_banked_reg(fiq, reg, val, is_read);
			break;
		}
		break;
	case 15:
		/*
		 * A trapped instruction that accesses the PC? Probably a bug,
		 * but nothing seems to prevent it.
		 */
		printk("WARNING: trapped instruction attempted to explicitly "
		       "access the PC.\n");
		if (is_read)
			*val = ctx->pc;
		else
			ctx->pc = *val;
		break;
	default:
		/* Programming error */
		printk("ERROR: attempt to write register %d\n", reg);
		break;
	}
}

static void dump_guest_regs(struct trap_context *ctx)
{
	u8 reg;
	unsigned long reg_val;

	panic_printk("pc=0x%08x cpsr=0x%08x hsr=0x%08x\n", ctx->pc, ctx->cpsr,
		     ctx->hsr);
	for (reg = 0; reg < 15; reg++) {
		access_cell_reg(ctx, reg, &reg_val, true);
		panic_printk("r%d=0x%08x ", reg, reg_val);
		if ((reg + 1) % 4 == 0)
			panic_printk("\n");
	}
	panic_printk("\n");
}

static int arch_handle_smc(struct trap_context *ctx)
{
	unsigned long *regs = ctx->regs;

	if (IS_PSCI_32(regs[0]) || IS_PSCI_UBOOT(regs[0]))
		regs[0] = psci_dispatch(ctx);
	else
		regs[0] = smc(regs[0], regs[1], regs[2], regs[3]);

	arch_skip_instruction(ctx);

	return TRAP_HANDLED;
}

static int arch_handle_hvc(struct trap_context *ctx)
{
	unsigned long *regs = ctx->regs;

	if (IS_PSCI_32(regs[0]) || IS_PSCI_UBOOT(regs[0]))
		regs[0] = psci_dispatch(ctx);
	else
		regs[0] = hypercall(regs[0], regs[1], regs[2]);

	return TRAP_HANDLED;
}

static int arch_handle_cp15_32(struct trap_context *ctx)
{
	u32 hsr = ctx->hsr;
	u32 rt = (hsr >> 5) & 0xf;
	u32 read = hsr & 1;
	unsigned long val;

#define CP15_32_PERFORM_WRITE(crn, opc1, crm, opc2) ({			\
	bool match = false;						\
	if (HSR_MATCH_MCR_MRC(hsr, crn, opc1, crm, opc2)) {		\
		arm_write_sysreg_32(opc1, c##crn, c##crm, opc2, val);	\
		match = true;						\
	}								\
	match;								\
})

	if (!read)
		access_cell_reg(ctx, rt, &val, true);

	/* trapped by HCR.TAC */
	if (HSR_MATCH_MCR_MRC(ctx->hsr, 1, 0, 0, 1)) { /* ACTLR */
		/* Do not let the guest disable coherency by writing ACTLR... */
		if (read)
			arm_read_sysreg(ACTLR_EL1, val);
	}
	/* all other regs are write-only / only trapped on writes */
	else if (read) {
		return TRAP_UNHANDLED;
	}
	/* trapped if HCR.TVM is set */
	else if (HSR_MATCH_MCR_MRC(hsr, 1, 0, 0, 0)) { /* SCTLR */
		// TODO: check if caches are turned on or off
		arm_write_sysreg(SCTLR_EL1, val);
	} else if (!(CP15_32_PERFORM_WRITE(2, 0, 0, 0) ||   /* TTBR0 */
		     CP15_32_PERFORM_WRITE(2, 0, 0, 1) ||   /* TTBR1 */
		     CP15_32_PERFORM_WRITE(2, 0, 0, 2) ||   /* TTBCR */
		     CP15_32_PERFORM_WRITE(3, 0, 0, 0) ||   /* DACR */
		     CP15_32_PERFORM_WRITE(5, 0, 0, 0) ||   /* DFSR */
		     CP15_32_PERFORM_WRITE(5, 0, 0, 1) ||   /* IFSR */
		     CP15_32_PERFORM_WRITE(6, 0, 0, 0) ||   /* DFAR */
		     CP15_32_PERFORM_WRITE(6, 0, 0, 2) ||   /* IFAR */
		     CP15_32_PERFORM_WRITE(5, 0, 1, 0) ||   /* ADFSR */
		     CP15_32_PERFORM_WRITE(5, 0, 1, 1) ||   /* AIDSR */
		     CP15_32_PERFORM_WRITE(10, 0, 2, 0) ||  /* PRRR / MAIR0 */
		     CP15_32_PERFORM_WRITE(10, 0, 2, 1) ||  /* NMRR / MAIR1 */
		     CP15_32_PERFORM_WRITE(13, 0, 0, 1))) { /* CONTEXTIDR */
		return TRAP_UNHANDLED;
	}

	if (read)
		access_cell_reg(ctx, rt, &val, false);

	arch_skip_instruction(ctx);

	return TRAP_HANDLED;
}

static int arch_handle_cp15_64(struct trap_context *ctx)
{
	u32 hsr  = ctx->hsr;
	u32 rt2  = (hsr >> 10) & 0xf;
	u32 rt   = (hsr >> 5) & 0xf;
	u32 read = hsr & 1;
	unsigned long lo, hi;

#define CP15_64_PERFORM_WRITE(opc1, crm) ({		\
	bool match = false;				\
	if (HSR_MATCH_MCRR_MRRC(hsr, opc1, crm)) {	\
		arm_write_sysreg_64(opc1, c##crm, ((u64)hi << 32) | lo); \
		match = true;				\
	}						\
	match;						\
})

	/* all regs are write-only / only trapped on writes */
	if (read)
		return TRAP_UNHANDLED;

	access_cell_reg(ctx, rt, &lo, true);
	access_cell_reg(ctx, rt2, &hi, true);

#ifdef CONFIG_ARM_GIC_V3
	/* trapped by HCR.IMO/FMO */
	if (HSR_MATCH_MCRR_MRRC(ctx->hsr, 0, 12)) /* ICC_SGI1R */
		gicv3_handle_sgir_write(((u64)hi << 32) | lo);
	else
#endif
	/* trapped if HCR.TVM is set */
	if (!(CP15_64_PERFORM_WRITE(0, 2) ||	/* TTBR0 */
	      CP15_64_PERFORM_WRITE(1, 2)))	/* TTBR1 */
		return TRAP_UNHANDLED;

	arch_skip_instruction(ctx);

	return TRAP_HANDLED;
}

static const trap_handler trap_handlers[38] =
{
	[HSR_EC_CP15_32]	= arch_handle_cp15_32,
	[HSR_EC_CP15_64]	= arch_handle_cp15_64,
	[HSR_EC_HVC]		= arch_handle_hvc,
	[HSR_EC_SMC]		= arch_handle_smc,
	[HSR_EC_DABT]		= arch_handle_dabt,
};

void arch_handle_trap(struct per_cpu *cpu_data, struct registers *guest_regs)
{
	struct trap_context ctx;
	u32 exception_class;
	int ret = TRAP_UNHANDLED;

	arm_read_banked_reg(ELR_hyp, ctx.pc);
	arm_read_banked_reg(SPSR_hyp, ctx.cpsr);
	arm_read_sysreg(HSR, ctx.hsr);
	exception_class = HSR_EC(ctx.hsr);
	ctx.regs = guest_regs->usr;

	/*
	 * On some implementations, instructions that fail their condition check
	 * can trap.
	 */
	if (arch_failed_condition(&ctx)) {
		arch_skip_instruction(&ctx);
		goto restore_context;
	}

	if (trap_handlers[exception_class])
		ret = trap_handlers[exception_class](&ctx);

	switch (ret) {
	case TRAP_UNHANDLED:
	case TRAP_FORBIDDEN:
		panic_printk("FATAL: %s (exception class 0x%02x)\n",
			     (ret == TRAP_UNHANDLED ? "unhandled trap" :
						      "forbidden access"),
			     exception_class);
		dump_guest_regs(&ctx);
		panic_park();
	}

restore_context:
	arm_write_banked_reg(SPSR_hyp, ctx.cpsr);
	arm_write_banked_reg(ELR_hyp, ctx.pc);
}
