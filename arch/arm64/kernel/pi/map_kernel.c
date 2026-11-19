// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2023 Google LLC
// Author: Ard Biesheuvel <ardb@google.com>

#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/memory.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "pi.h"

extern const u8 __eh_frame_start[], __eh_frame_end[];

extern void idmap_cpu_replace_ttbr1(phys_addr_t pgdir);

static void __init map_segment(pgd_t *pg_dir, phys_addr_t *pgd, u64 va_offset,
			       void *start, void *end, pgprot_t prot,
			       bool may_use_cont, int root_level)
{
	map_range(pgd, ((u64)start + va_offset) & ~PAGE_OFFSET,
		  ((u64)end + va_offset) & ~PAGE_OFFSET, (u64)start,
		  prot, root_level, (pte_t *)pg_dir, may_use_cont, 0);
}

static void __init unmap_segment(pgd_t *pg_dir, u64 va_offset, void *start,
				 void *end, int root_level)
{
	map_segment(pg_dir, NULL, va_offset, start, end, __pgprot(0),
		    false, root_level);
}

static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level, u64 pa_base)
{
	phys_addr_t pgdp = (phys_addr_t)init_pg_dir + PAGE_SIZE;

	/*
	 * 1. 映射真实的 _text.._end，可读写可执行。
	 *
	 * 不能像之前那样固定映射 128MB：vmalloc 分配器只为
	 * declare_kernel_vmas() 里的 [_text, _end] 预留地址空间，
	 * _end 之后的地址会被当成空闲 vmalloc 区。
	 *
	 * 也不能把 _text.._end 一次性映射成一个区间：map_range() 会把
	 * 内部 2MB 对齐的部分放成 block 映射，而 free_initmem() 会用
	 * vunmap_range(__init_begin, __init_end) 卸载 init 段。init 段的
	 * 首尾都不是 2MB 对齐的，vunmap_pmd_range() 清掉一个覆盖边界的
	 * huge PMD 后，next - addr < PMD_SIZE，仍然触发
	 * mm/vmalloc.c:435 的 WARN_ON。
	 *
	 * 按上游的方式分段映射：inittext/initdata 段单独映射，且
	 * may_use_cont=false，让段边界用 4KB 页表；这样 free_initmem()
	 * 只会清到完整的 2MB block 或页表，不再触发 WARN。
	 */
	map_segment(init_pg_dir, &pgdp, va_offset, _text, _stext,
		    PAGE_KERNEL_EXEC, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, _stext, _etext,
		    PAGE_KERNEL_EXEC, true, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __start_rodata,
		    __inittext_begin, PAGE_KERNEL_EXEC, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __inittext_begin,
		    __inittext_end, PAGE_KERNEL_EXEC, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, __initdata_begin,
		    __initdata_end, PAGE_KERNEL_EXEC, false, root_level);
	map_segment(init_pg_dir, &pgdp, va_offset, _data, _end,
		    PAGE_KERNEL_EXEC, true, root_level);

	dsb(ishst);

	/*
	 * 2. 切换到 init_pg_dir。PI 代码在物理地址上运行，init_pg_dir
	 * 已经是物理地址，不能再减 va_offset（上游也是直接传它）。
	 */
	idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir);

	if (IS_ENABLED(CONFIG_RELOCATABLE))
		relocate_kernel(kaslr_offset);

	/*
	 * 3. 拷贝页表：目标必须用内核虚拟地址。swapper_pg_dir 位于 rodata，
	 * TTBR0 的初始 ID 映射把它映射成只读；通过刚装好的 TTBR1 内核映射
	 * 写入则是可写的（与上游 `swapper_pg_dir + va_offset` 一致）。
	 */
	memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
	dsb(ishst);

	/* 4. 切换到 swapper_pg_dir：PI 运行时它已经是物理地址 */
	idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);

	asm volatile(
		"dsb	nsh\n"
		"tlbi	vmalle1\n"
		"dsb	nsh\n"
		"isb\n"
		"ic	ialluis\n"
		"dsb	nsh\n"
		"isb\n"
		::: "memory"
	);
}

static void noinline __section(".idmap.text") set_ttbr0_for_lpa2(phys_addr_t ttbr)
{
	u64 sctlr = read_sysreg(sctlr_el1);
	u64 tcr = read_sysreg(tcr_el1) | TCR_EL1_DS;
	u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	u64 parange = cpuid_feature_extract_unsigned_field(mmfr0,
							   ID_AA64MMFR0_EL1_PARANGE_SHIFT);

	tcr &= ~TCR_EL1_IPS_MASK;
	tcr |= parange << TCR_EL1_IPS_SHIFT;

	asm("	msr	sctlr_el1, %0		;"
	    "	isb				;"
	    "   msr     ttbr0_el1, %1		;"
	    "   msr     tcr_el1, %2		;"
	    "	isb				;"
	    "	tlbi    vmalle1			;"
	    "	dsb     nsh			;"
	    "	isb				;"
	    "	msr     sctlr_el1, %3		;"
	    "	isb				;"
	    ::	"r"(sctlr & ~SCTLR_ELx_M), "r"(ttbr), "r"(tcr), "r"(sctlr));
}

static void __init remap_idmap_for_lpa2(void)
{
	/* clear the bits that change meaning once LPA2 is turned on */
	ptval_t mask = PTE_SHARED;

	/*
	 * We have to clear bits [9:8] in all block or page descriptors in the
	 * initial ID map, as otherwise they will be (mis)interpreted as
	 * physical address bits once we flick the LPA2 switch (TCR.DS). Since
	 * we cannot manipulate live descriptors in that way without creating
	 * potential TLB conflicts, let's create another temporary ID map in a
	 * LPA2 compatible fashion, and update the initial ID map while running
	 * from that.
	 */
	create_init_idmap(init_pg_dir, mask);
	dsb(ishst);
	set_ttbr0_for_lpa2((phys_addr_t)init_pg_dir);

	/*
	 * Recreate the initial ID map with the same granularity as before.
	 * Don't bother with the FDT, we no longer need it after this.
	 */
	memset(init_idmap_pg_dir, 0,
	       (char *)init_idmap_pg_end - (char *)init_idmap_pg_dir);

	create_init_idmap(init_idmap_pg_dir, mask);
	dsb(ishst);

	/* switch back to the updated initial ID map */
	set_ttbr0_for_lpa2((phys_addr_t)init_idmap_pg_dir);

	/* wipe the temporary ID map from memory */
	memset(init_pg_dir, 0, (char *)init_pg_end - (char *)init_pg_dir);
}

static void *__init map_fdt(phys_addr_t fdt)
{
	static u8 ptes[INIT_IDMAP_FDT_SIZE] __initdata __aligned(PAGE_SIZE);
	phys_addr_t efdt = fdt + MAX_FDT_SIZE;
	phys_addr_t ptep = (phys_addr_t)ptes; /* We're idmapped when called */

	/*
	 * Map up to MAX_FDT_SIZE bytes, but avoid overlap with
	 * the kernel image.
	 */
	map_range(&ptep, fdt, (u64)_text > fdt ? min((u64)_text, efdt) : efdt,
		  fdt, PAGE_KERNEL, IDMAP_ROOT_LEVEL,
		  (pte_t *)init_idmap_pg_dir, false, 0);
	dsb(ishst);

	return (void *)fdt;
}

/*
 * PI version of the Cavium Eratum 27456 detection, which makes it
 * impossible to use non-global mappings.
 */
static bool __init ng_mappings_allowed(void)
{
	static const struct midr_range cavium_erratum_27456_cpus[] __initconst = {
		/* Cavium ThunderX, T88 pass 1.x - 2.1 */
		MIDR_RANGE(MIDR_THUNDERX, 0, 0, 1, 1),
		/* Cavium ThunderX, T81 pass 1.0 */
		MIDR_REV(MIDR_THUNDERX_81XX, 0, 0),
		{},
	};

	for (const struct midr_range *r = cavium_erratum_27456_cpus; r->model; r++) {
		if (midr_is_cpu_model_range(read_cpuid_id(), r->model,
					    r->rv_min, r->rv_max))
			return false;
	}

	return true;
}

asmlinkage phys_addr_t __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
{
	static char const chosen_str[] __initconst = "/chosen";
	u64 va_base, pa_base;
	u64 kaslr_offset;
	int root_level = 4 - CONFIG_PGTABLE_LEVELS;
	int va_bits = VA_BITS;
	int chosen;
	void *fdt_mapped = map_fdt(fdt);

	/* 用内联汇编获取物理地址 0x40000000，避免绝对重定位 */
	asm volatile(
		"movz %0, #0, lsl #0\n"
		"movk %0, #0x4000, lsl #16\n"
		: "=r"(pa_base)
	);

	kaslr_offset = pa_base % MIN_KIMG_ALIGN;

	/* Clear BSS and the initial page tables */
	memset(__bss_start, 0, (char *)init_pg_end - (char *)__bss_start);

	/* Parse the command line for CPU feature overrides */
	chosen = fdt_path_offset(fdt_mapped, chosen_str);
	init_feature_override(boot_status, fdt_mapped, chosen);

	if (IS_ENABLED(CONFIG_ARM64_64K_PAGES) && !cpu_has_lva()) {
		va_bits = VA_BITS_MIN;
	} else if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2()) {
		va_bits = VA_BITS_MIN;
		root_level++;
	}

	if (va_bits > VA_BITS_MIN)
		sysreg_clear_set(tcr_el1, TCR_EL1_T1SZ_MASK, TCR_T1SZ(va_bits));

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);

		if (kaslr_seed && kaslr_requires_kpti())
			arm64_use_ng_mappings = ng_mappings_allowed();

		kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
	}

	if (IS_ENABLED(CONFIG_ARM64_LPA2) && va_bits > VA_BITS_MIN)
		remap_idmap_for_lpa2();

	va_base = KIMAGE_VADDR + kaslr_offset;
	map_kernel(kaslr_offset, va_base - pa_base, root_level, pa_base);

	/* Return the physical base in x0 for __primary_switched's kimage_voffset */
	return pa_base;
}

