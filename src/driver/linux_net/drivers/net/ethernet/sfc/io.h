/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2005-2006 Fen Systems Ltd.
 * Copyright 2006-2017 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#ifndef EFX_IO_H
#define EFX_IO_H

#include <linux/io.h>
#include <linux/spinlock.h>

/**************************************************************************
 *
 * NIC register I/O
 *
 **************************************************************************
 *
 * The EF10 architecture exposes very few registers to the host and
 * most of them are only 32 bits wide.  The only exceptions are the MC
 * doorbell register pair, which has its own latching, and
 * TX_DESC_UPD.
 *
 * The TX_DESC_UPD DMA descriptor pointer is 128-bits but is a special
 * case in the BIU to avoid the need for locking in the host:
 *
 * - It is write-only.
 * - The semantics of writing to this register is such that
 *   replacing the low 96 bits with zero does not affect functionality.
 * - If the host writes to the last dword address of the register
 *   (i.e. the high 32 bits) the underlying register will always be
 *   written.  If the collector and the current write together do not
 *   provide values for all 128 bits of the register, the low 96 bits
 *   will be written as zero.
 */

#if BITS_PER_LONG == 64
#define EFX_USE_QWORD_IO 1
#endif

/* Hardware issue requires that only 64-bit naturally aligned writes
 * are seen by hardware. Its not strictly necessary to restrict to
 * x86_64 arch, but done for safety since unusual write combining behaviour
 * can break PIO.
 */
#ifdef CONFIG_X86_64
/* PIO is a win only if write-combining is possible */
#ifdef ARCH_HAS_IOREMAP_WC
#define EFX_USE_PIO 1
#endif
#endif

static inline void __iomem *efx_mem(struct efx_nic *efx, unsigned int addr)
{
	return efx->membase + addr;
}

static inline u32 efx_reg(struct efx_nic *efx, unsigned int reg)
{
	return efx->reg_base + reg;
}

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_X86_64)
static inline void _efx_writeo(struct efx_nic *efx, __le64 value[2],
			       unsigned int reg)
{
	unsigned long cr0;
	__le64 xmm_save[2];
	void __iomem *addr = efx_mem(efx, reg);

	preempt_disable();

	/* Save the xmm0 register to stack */
	asm volatile(
		"movq %%cr0,%0          ;\n\t"
		"clts                   ;\n\t"
		"movups %%xmm0,(%1)     ;\n\t"
		: "=&r" (cr0)
		: "r" (xmm_save)
		: "memory");

	/* First read the data into register xmm0
	 * Then write this out to the address that was given
	 */
	asm volatile(
		"movdqu %1,%%xmm0       ;\n\t"
		"movdqa %%xmm0,%0       ;\n\t"
		: "=m" (*(unsigned long __force *)addr)
		: "m" (*value)
		: "memory");

	/* Restore the xmm0 register */
	asm volatile(
		"sfence                 ;\n\t"
		"movups (%1),%%xmm0     ;\n\t"
		"movq   %0,%%cr0        ;\n\t"
		:
		: "r" (cr0), "r" (xmm_save)
		: "memory");

	preempt_enable();
}
#endif /* EFX_NOT_UPSTREAM && CONFIG_X86_64 */

#ifdef EFX_USE_QWORD_IO
static inline void _efx_writeq(struct efx_nic *efx, __le64 value,
				  unsigned int reg)
{
	__raw_writeq((__force u64)value, efx_mem(efx, reg));
}
static inline __le64 _efx_readq(struct efx_nic *efx, unsigned int reg)
{
	return (__force __le64)__raw_readq(efx_mem(efx, reg));
}
#endif

static inline void _efx_writed(struct efx_nic *efx, __le32 value,
				  unsigned int reg)
{
	__raw_writel((__force u32)value, efx_mem(efx, reg));
}
static inline __le32 _efx_readd(struct efx_nic *efx, unsigned int reg)
{
	return (__force __le32)__raw_readl(efx_mem(efx, reg));
}

/* Write a normal 128-bit CSR, locking as appropriate. */
static inline void efx_writeo(struct efx_nic *efx, const efx_oword_t *value,
			      unsigned int reg)
{
	unsigned long flags __attribute__ ((unused));

	netif_vdbg(efx, hw, efx->net_dev,
		   "writing register %x with " EFX_OWORD_FMT "\n", reg,
		   EFX_OWORD_VAL(*value));

	spin_lock_irqsave(&efx->biu_lock, flags);
#ifdef EFX_USE_QWORD_IO
	_efx_writeq(efx, value->u64[0], reg + 0);
	_efx_writeq(efx, value->u64[1], reg + 8);
#else
	_efx_writed(efx, value->u32[0], reg + 0);
	_efx_writed(efx, value->u32[1], reg + 4);
	_efx_writed(efx, value->u32[2], reg + 8);
	_efx_writed(efx, value->u32[3], reg + 12);
#endif
#if defined(EFX_USE_KCOMPAT) && defined(EFX_HAVE_MMIOWB)
	/* Kernels from 5.2 onwards no longer have mmiowb(), because it is now
	 * implied by spin_unlock() on architectures that require it.  Hence we
	 * can simply ifdef it out for kernels where it doesn't exist.  We do
	 * this, rather than defining it to empty in kernel_compat.h, to make
	 * the unifdef'd driver match upstream.
	 */
	mmiowb();
#endif
	spin_unlock_irqrestore(&efx->biu_lock, flags);
}

static inline void efx_writeq(struct efx_nic *efx, const efx_qword_t *value,
                              unsigned int reg)
{
	unsigned long flags __attribute__ ((unused));

        netif_vdbg(efx, hw, efx->net_dev,
                   "writing register %x with "EFX_QWORD_FMT"\n",
                   reg, EFX_QWORD_VAL(*value));

	spin_lock_irqsave(&efx->biu_lock, flags);
#ifdef EFX_USE_QWORD_IO
        _efx_writeq(efx, value->u64[0], reg + 0);
#else
	_efx_writed(efx, value->u32[0], reg + 0);
	_efx_writed(efx, value->u32[1], reg + 4);
#endif
#if defined(EFX_USE_KCOMPAT) && defined(EFX_HAVE_MMIOWB)
	mmiowb();
#endif
	spin_unlock_irqrestore(&efx->biu_lock, flags);
}

/* Write a 32-bit CSR or the last dword of a special 128-bit CSR */
static inline void efx_writed(struct efx_nic *efx, const efx_dword_t *value,
			      unsigned int reg)
{
	netif_vdbg(efx, hw, efx->net_dev,
		   "writing register %x with "EFX_DWORD_FMT"\n",
		   reg, EFX_DWORD_VAL(*value));

	/* No lock required */
	_efx_writed(efx, value->u32[0], reg);
}

/* Read a 128-bit CSR, locking as appropriate. */
static inline void efx_reado(struct efx_nic *efx, efx_oword_t *value,
			     unsigned int reg)
{
	unsigned long flags __attribute__ ((unused));

	spin_lock_irqsave(&efx->biu_lock, flags);
	value->u32[0] = _efx_readd(efx, reg + 0);
	value->u32[1] = _efx_readd(efx, reg + 4);
	value->u32[2] = _efx_readd(efx, reg + 8);
	value->u32[3] = _efx_readd(efx, reg + 12);
	spin_unlock_irqrestore(&efx->biu_lock, flags);

	netif_vdbg(efx, hw, efx->net_dev,
		   "read from register %x, got " EFX_OWORD_FMT "\n", reg,
		   EFX_OWORD_VAL(*value));
}

/* Read a 32-bit CSR or SRAM */
static inline void efx_readd(struct efx_nic *efx, efx_dword_t *value,
				unsigned int reg)
{
	value->u32[0] = _efx_readd(efx, reg);
	netif_vdbg(efx, hw, efx->net_dev,
		   "read from register %x, got "EFX_DWORD_FMT"\n",
		   reg, EFX_DWORD_VAL(*value));
}

/* Write a 128-bit CSR forming part of a table */
static inline void
efx_writeo_table(struct efx_nic *efx, const efx_oword_t *value,
		 unsigned int reg, unsigned int index)
{
	efx_writeo(efx, value, reg + index * sizeof(efx_oword_t));
}

/* Read a 128-bit CSR forming part of a table */
static inline void efx_reado_table(struct efx_nic *efx, efx_oword_t *value,
				     unsigned int reg, unsigned int index)
{
	efx_reado(efx, value, reg + index * sizeof(efx_oword_t));
}

/* default VI stride (step between per-VI registers) is 8K on EF10 and
 * 64K on EF100
 */
#define EFX_DEFAULT_VI_STRIDE		0x2000
#define EF100_DEFAULT_VI_STRIDE		0x10000

/* Calculate offset to page-mapped register */
static inline unsigned int efx_paged_reg(struct efx_nic *efx, unsigned int page,
					 unsigned int reg)
{
	return page * efx->vi_stride + reg;
}

/* Write the whole of RX_DESC_UPD or TX_DESC_UPD */
static inline void _efx_writeo_page(struct efx_nic *efx, efx_oword_t *value,
				    unsigned int reg, unsigned int page)
{
	reg = efx_paged_reg(efx, page, reg);

	netif_vdbg(efx, hw, efx->net_dev,
		   "writing register %x with " EFX_OWORD_FMT "\n", reg,
		   EFX_OWORD_VAL(*value));

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_X86_64)
	_efx_writeo(efx, value->u64, reg + 0);
#else
#ifdef EFX_USE_QWORD_IO
	_efx_writeq(efx, value->u64[0], reg + 0);
	_efx_writeq(efx, value->u64[1], reg + 8);
#else
	_efx_writed(efx, value->u32[0], reg + 0);
	_efx_writed(efx, value->u32[1], reg + 4);
	_efx_writed(efx, value->u32[2], reg + 8);
	_efx_writed(efx, value->u32[3], reg + 12);
#endif
#endif
}
#define efx_writeo_page(efx, value, reg, page)				\
	_efx_writeo_page(efx, value,					\
			 reg +						\
			 BUILD_BUG_ON_ZERO((reg) != 0x830 && (reg) != 0xa10), \
			 page)

/* Write a page-mapped 32-bit CSR (EVQ_RPTR, EVQ_TMR (EF10 and EF100), the
 * RX/TX_RING_DOORBELL (EF100), or the high bits of RX_DESC_UPD or
 * TX_DESC_UPD (EF10).
 */
static inline void
_efx_writed_page(struct efx_nic *efx, const efx_dword_t *value,
		 unsigned int reg, unsigned int page)
{
	efx_writed(efx, value, efx_paged_reg(efx, page, reg));
}
#define efx_writed_page(efx, value, reg, page)				\
	_efx_writed_page(efx, value,					\
			 reg +						\
			 BUILD_BUG_ON_ZERO((reg) != 0x180 &&		\
					   (reg) != 0x200 &&		\
					   (reg) != 0x400 &&		\
					   (reg) != 0x420 &&		\
					   (reg) != 0x830 &&		\
					   (reg) != 0x83c &&		\
					   (reg) != 0xa18 &&		\
					   (reg) != 0xa1c),		\
			 page)

/* Write TIMER_COMMAND.  This is a page-mapped 32-bit CSR, but a bug
 * in the BIU means that writes to TIMER_COMMAND[0] invalidate the
 * collector register.
 */
static inline void _efx_writed_page_locked(struct efx_nic *efx,
					   const efx_dword_t *value,
					   unsigned int reg,
					   unsigned int page)
{
	unsigned long flags __attribute__ ((unused));

	if (page == 0) {
		spin_lock_irqsave(&efx->biu_lock, flags);
		efx_writed(efx, value, efx_paged_reg(efx, page, reg));
		spin_unlock_irqrestore(&efx->biu_lock, flags);
	} else {
		efx_writed(efx, value, efx_paged_reg(efx, page, reg));
	}
}
#define efx_writed_page_locked(efx, value, reg, page)			\
	_efx_writed_page_locked(efx, value,				\
				reg + BUILD_BUG_ON_ZERO((reg) != 0x420), \
				page)

#endif /* EFX_IO_H */
