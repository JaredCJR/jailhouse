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
 */

#include <jailhouse/control.h>
#include <jailhouse/mmio.h>
#include <asm/gic.h>
#include <asm/irqchip.h>
#include <asm/setup.h>

static unsigned int gic_num_lr;

void *gicc_base;
void *gich_base;

static int gic_init(void)
{
	gicc_base = paging_map_device(
			system_config->platform_info.arm.gicc_base, GICC_SIZE);
	if (!gicc_base)
		return -ENOMEM;

	gich_base = paging_map_device(
			system_config->platform_info.arm.gich_base, GICH_SIZE);
	if (!gich_base)
		return -ENOMEM;

	return 0;
}

static void gic_clear_pending_irqs(void)
{
	unsigned int n;

	/* Clear list registers. */
	for (n = 0; n < gic_num_lr; n++)
		gic_write_lr(n, 0);

	/* Clear active priority bits. */
	mmio_write32(gich_base + GICH_APR, 0);
}

static void gic_cpu_reset(struct per_cpu *cpu_data, bool is_shutdown)
{
	unsigned int mnt_irq = system_config->platform_info.arm.maintenance_irq;
	unsigned int i;
	bool root_shutdown = is_shutdown && (cpu_data->cell == &root_cell);
	u32 active;
	u32 gich_vmcr = 0;
	u32 gicc_ctlr, gicc_pmr;

	gic_clear_pending_irqs();

	/* Deactivate all PPIs */
	active = mmio_read32(gicd_base + GICD_ISACTIVER);
	for (i = 16; i < 32; i++) {
		if (test_bit(i, (unsigned long *)&active))
			mmio_write32(gicc_base + GICC_DIR, i);
	}

	/* Ensure all IPIs and the maintenance PPI are enabled */
	mmio_write32(gicd_base + GICD_ISENABLER, 0x0000ffff | (1 << mnt_irq));

	/*
	 * Disable PPIs, except for the maintenance interrupt.
	 * On shutdown, the root cell expects to find all its PPIs still
	 * enabled - except for the maintenance interrupt we used.
	 */
	mmio_write32(gicd_base + GICD_ICENABLER,
		     root_shutdown ? 1 << mnt_irq :
				     0xffff0000 & ~(1 << mnt_irq));

	if (is_shutdown)
		mmio_write32(gich_base + GICH_HCR, 0);

	if (root_shutdown) {
		gich_vmcr = mmio_read32(gich_base + GICH_VMCR);
		gicc_ctlr = 0;
		gicc_pmr = (gich_vmcr >> GICH_VMCR_PMR_SHIFT) << GICV_PMR_SHIFT;

		if (gich_vmcr & GICH_VMCR_EN0)
			gicc_ctlr |= GICC_CTLR_GRPEN1;
		if (gich_vmcr & GICH_VMCR_EOImode)
			gicc_ctlr |= GICC_CTLR_EOImode;

		mmio_write32(gicc_base + GICC_CTLR, gicc_ctlr);
		mmio_write32(gicc_base + GICC_PMR, gicc_pmr);

		gich_vmcr = 0;
	}
	mmio_write32(gich_base + GICH_VMCR, gich_vmcr);
}

static int gic_cpu_init(struct per_cpu *cpu_data)
{
	unsigned int mnt_irq = system_config->platform_info.arm.maintenance_irq;
	u32 vtr, vmcr;
	u32 cell_gicc_ctlr, cell_gicc_pmr;

	/* Ensure all IPIs and the maintenance PPI are enabled. */
	mmio_write32(gicd_base + GICD_ISENABLER, 0x0000ffff | (1 << mnt_irq));

	cell_gicc_ctlr = mmio_read32(gicc_base + GICC_CTLR);
	cell_gicc_pmr = mmio_read32(gicc_base + GICC_PMR);

	mmio_write32(gicc_base + GICC_CTLR,
		     GICC_CTLR_GRPEN1 | GICC_CTLR_EOImode);
	mmio_write32(gicc_base + GICC_PMR, GICC_PMR_DEFAULT);

	vtr = mmio_read32(gich_base + GICH_VTR);
	gic_num_lr = (vtr & 0x3f) + 1;

	/* VMCR only contains 5 bits of priority */
	vmcr = (cell_gicc_pmr >> GICV_PMR_SHIFT) << GICH_VMCR_PMR_SHIFT;
	/*
	 * All virtual interrupts are group 0 in this driver since the GICV
	 * layout seen by the guest corresponds to GICC without security
	 * extensions:
	 * - A read from GICV_IAR doesn't acknowledge group 1 interrupts
	 *   (GICV_AIAR does it, but the guest never attempts to accesses it)
	 * - A write to GICV_CTLR.GRP0EN corresponds to the GICC_CTLR.GRP1EN bit
	 *   Since the guest's driver thinks that it is accessing a GIC with
	 *   security extensions, a write to GPR1EN will enable group 0
	 *   interrups.
	 * - Group 0 interrupts are presented as virtual IRQs (FIQEn = 0)
	 */
	if (cell_gicc_ctlr & GICC_CTLR_GRPEN1)
		vmcr |= GICH_VMCR_EN0;
	if (cell_gicc_ctlr & GICC_CTLR_EOImode)
		vmcr |= GICH_VMCR_EOImode;

	mmio_write32(gich_base + GICH_VMCR, vmcr);
	mmio_write32(gich_base + GICH_HCR, GICH_HCR_EN);

	/*
	 * Clear pending virtual IRQs in case anything is left from previous
	 * use. Physically pending IRQs will be forwarded to Linux once we
	 * enable interrupts for the hypervisor.
	 */
	gic_clear_pending_irqs();

	/* Register ourselves into the CPU itf map */
	gic_probe_cpu_id(cpu_data->cpu_id);

	return 0;
}

static void gic_eoi_irq(u32 irq_id, bool deactivate)
{
	/*
	 * The GIC doesn't seem to care about the CPUID value written to EOIR,
	 * which is rather convenient...
	 */
	mmio_write32(gicc_base + GICC_EOIR, irq_id);
	if (deactivate)
		mmio_write32(gicc_base + GICC_DIR, irq_id);
}

static int gic_cell_init(struct cell *cell)
{
	int err;

	/*
	 * Let the guest access the virtual CPU interface instead of the
	 * physical one.
	 *
	 * WARN: some SoCs (EXYNOS4) use a modified GIC which doesn't have any
	 * banked CPU interface, so we should map per-CPU physical addresses
	 * here.
	 * As for now, none of them seem to have virtualization extensions.
	 */
	err = paging_create(&cell->arch.mm,
			    system_config->platform_info.arm.gicv_base,
			    GICC_SIZE,
			    system_config->platform_info.arm.gicc_base,
			    (PTE_FLAG_VALID | PTE_ACCESS_FLAG |
			     S2_PTE_ACCESS_RW | S2_PTE_FLAG_DEVICE),
			    PAGING_COHERENT);
	if (err)
		return err;

	mmio_region_register(cell, system_config->platform_info.arm.gicd_base,
			     GICD_SIZE, gic_handle_dist_access, NULL);
	return 0;
}

static void gic_cell_exit(struct cell *cell)
{
	paging_destroy(&cell->arch.mm,
		       system_config->platform_info.arm.gicc_base, GICC_SIZE,
		       PAGING_COHERENT);
}

static void gic_adjust_irq_target(struct cell *cell, u16 irq_id)
{
	void *itargetsr = gicd_base + GICD_ITARGETSR + (irq_id & ~0x3);
	u32 targets = mmio_read32(itargetsr);
	unsigned int shift = (irq_id % 4) * 8;

	if (gic_targets_in_cell(cell, (u8)(targets >> shift)))
		return;

	targets &= ~(0xff << shift);
	targets |= target_cpu_map[first_cpu(cell->cpu_set)] << shift;

	mmio_write32(itargetsr, targets);
}

static int gic_send_sgi(struct sgi *sgi)
{
	u32 val;

	if (!is_sgi(sgi->id))
		return -EINVAL;

	val = (sgi->routing_mode & 0x3) << 24
		| (sgi->targets & 0xff) << 16
		| (sgi->id & 0xf);

	mmio_write32(gicd_base + GICD_SGIR, val);

	return 0;
}

static int gic_inject_irq(struct per_cpu *cpu_data, u16 irq_id)
{
	int i;
	int first_free = -1;
	u32 lr;
	unsigned long elsr[2];

	elsr[0] = mmio_read32(gich_base + GICH_ELSR0);
	elsr[1] = mmio_read32(gich_base + GICH_ELSR1);
	for (i = 0; i < gic_num_lr; i++) {
		if (test_bit(i, elsr)) {
			/* Entry is available */
			if (first_free == -1)
				first_free = i;
			continue;
		}

		/* Check that there is no overlapping */
		lr = gic_read_lr(i);
		if ((lr & GICH_LR_VIRT_ID_MASK) == irq_id)
			return -EEXIST;
	}

	if (first_free == -1)
		return -EBUSY;

	/* Inject group 0 interrupt (seen as IRQ by the guest) */
	lr = irq_id;
	lr |= GICH_LR_PENDING_BIT;

	if (!is_sgi(irq_id)) {
		lr |= GICH_LR_HW_BIT;
		lr |= (u32)irq_id << GICH_LR_PHYS_ID_SHIFT;
	}

	gic_write_lr(first_free, lr);

	return 0;
}

static void gic_enable_maint_irq(bool enable)
{
	u32 hcr;

	hcr = mmio_read32(gich_base + GICH_HCR);
	if (enable)
		hcr |= GICH_HCR_UIE;
	else
		hcr &= ~GICH_HCR_UIE;
	mmio_write32(gich_base + GICH_HCR, hcr);
}

enum mmio_result gic_handle_irq_route(struct mmio_access *mmio,
				      unsigned int irq)
{
	/* doesn't exist in v2 - ignore access */
	return MMIO_HANDLED;
}

unsigned int irqchip_mmio_count_regions(struct cell *cell)
{
	return 1;
}

struct irqchip_ops irqchip = {
	.init = gic_init,
	.cpu_init = gic_cpu_init,
	.cpu_reset = gic_cpu_reset,
	.cell_init = gic_cell_init,
	.cell_exit = gic_cell_exit,
	.adjust_irq_target = gic_adjust_irq_target,

	.send_sgi = gic_send_sgi,
	.handle_irq = gic_handle_irq,
	.inject_irq = gic_inject_irq,
	.enable_maint_irq = gic_enable_maint_irq,
	.eoi_irq = gic_eoi_irq,
};
