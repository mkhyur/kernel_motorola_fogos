/*
 * Copyright (C) 2023 Motorola Mobility LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <trace/hooks/mm.h>
#include <linux/pagemap.h>
#include <linux/version.h>

static int max_ra_pages = -1;
module_param(max_ra_pages, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(max_ra_pages, "Max read ahead pages");

/*
 * Detect availability of android_vh_tune_mmap_readaround hook.
 * On kernels where this hook is declared (5.15.105+, or 5.10.178+),
 * use the simpler tune_mmap_readaround path. Otherwise fall back to
 * the legacy filemap_fault hooks.
 */
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 104) || \
    (LINUX_VERSION_CODE > KERNEL_VERSION(5, 10, 177) && \
     LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0))
#ifndef TUNE_MMAP_READAROUND
#define TUNE_MMAP_READAROUND
#endif
#endif

/*
 * android_vendor_data1 is a u64 on vm_fault. Pack old_ra_pages and
 * max_ra_pages into it so we can save/restore across the two hooks.
 * Bit 0 of the upper 32 bits is used as an ownership flag to prevent
 * concurrent faults from clobbering the saved value.
 */
#define RA_OWNED_BIT		32
#define RA_PACK(old, mx)	((u64)(old) | ((u64)(mx) << 16) | \
				 (1ULL << RA_OWNED_BIT))
#define RA_UNPACK_OLD(packed)	((unsigned int)((packed) & 0xFFFF))
#define RA_UNPACK_MAX(packed)	((unsigned int)(((packed) >> 16) & 0xFFFF))
#define RA_IS_OWNED(packed)	((packed) & (1ULL << RA_OWNED_BIT))

#if defined(TUNE_MMAP_READAROUND)
static void __nocfi tune_mmap_readaround(void *p, unsigned int ra_pages,
		pgoff_t pgoff, pgoff_t *start, unsigned int *size,
		unsigned int *async_size)
{
	*start = max_t(long, 0, pgoff - max_ra_pages / 2);
	*size = max_ra_pages;
	*async_size = max_ra_pages / 4;
}
#else
static void __nocfi filemap_fault_get_page(void *p, struct vm_fault *vmf,
		struct page **page_out, bool *retry)
{
	struct file *file = vmf->vma->vm_file;
	pgoff_t offset = vmf->pgoff;
	struct page *page = NULL;
	struct file_ra_state *ra = NULL;
	struct address_space *mapping = NULL;
	unsigned int mmap_miss;

	if (!file)
		return;

	ra = &file->f_ra;
	if (!ra)
		return;

	mapping = file->f_mapping;
	page = find_get_page(mapping, offset);

	if (likely(page) && !(vmf->flags & FAULT_FLAG_TRIED)) {
		put_page(page);
	} else if (!page) {
		mmap_miss = READ_ONCE(ra->mmap_miss);
		if ((vmf->vma->vm_flags & VM_RAND_READ) ||
			(!ra->ra_pages) ||
			(vmf->vma->vm_flags & VM_SEQ_READ) ||
			mmap_miss > 100) {
			return;
		} else {
			if (ra->ra_pages > max_ra_pages) {
				unsigned long packed = READ_ONCE(
					vmf->android_vendor_data1);
				/*
				 * Atomically claim ownership: only the
				 * first fault saves the original value.
				 */
				if (!RA_IS_OWNED(packed)) {
					packed = RA_PACK(ra->ra_pages,
							 max_ra_pages);
					WRITE_ONCE(
						vmf->android_vendor_data1,
						packed);
				}
				WRITE_ONCE(ra->ra_pages, max_ra_pages);
			}
			return;
		}
	} else {
		put_page(page);
	}
}

static void __nocfi filemap_fault_cache_page(void *p, struct vm_fault *vmf,
		struct page *page)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = NULL;
	unsigned long packed;

	if (!file)
		return;

	ra = &file->f_ra;
	if (!ra)
		return;

	packed = READ_ONCE(vmf->android_vendor_data1);
	if ((ra->ra_pages == max_ra_pages) && RA_IS_OWNED(packed)) {
		/* Only the owning fault restores the original value */
		WRITE_ONCE(ra->ra_pages, RA_UNPACK_OLD(packed));
		WRITE_ONCE(vmf->android_vendor_data1, 0);
	}
}
#endif

static int __nocfi __init moto_mmap_fault_init(void)
{
	int ret = 0;
	int ramsize_GB = (totalram_pages() >> (30 - PAGE_SHIFT)) + 1;

	if (max_ra_pages == -1) {
		/* Set 8 pages for < 8G RAM and set 16 pages for >= 8G RAM */
		if (ramsize_GB < 8)
			max_ra_pages = 8;
		else
			max_ra_pages = 16;
	}

#if defined(TUNE_MMAP_READAROUND)
	pr_info("moto_mm: using tune_mmap_readaround, totalram=%dGB\n",
		ramsize_GB);
	ret = register_trace_android_vh_tune_mmap_readaround(
		tune_mmap_readaround, NULL);
#else
	pr_info("moto_mm: using legacy filemap hooks, totalram=%dGB\n",
		ramsize_GB);
	ret = register_trace_android_vh_filemap_fault_get_page(
		filemap_fault_get_page, NULL) ?:
	      register_trace_android_vh_filemap_fault_cache_page(
		filemap_fault_cache_page, NULL);
#endif
	if (ret != 0)
		return -ENXIO;
	return 0;
}

static void __nocfi __exit moto_mmap_fault_exit(void)
{
#if defined(TUNE_MMAP_READAROUND)
	unregister_trace_android_vh_tune_mmap_readaround(
		tune_mmap_readaround, NULL);
#else
	unregister_trace_android_vh_filemap_fault_get_page(
		filemap_fault_get_page, NULL);
	unregister_trace_android_vh_filemap_fault_cache_page(
		filemap_fault_cache_page, NULL);
#endif
}

module_init(moto_mmap_fault_init);
module_exit(moto_mmap_fault_exit);
MODULE_DESCRIPTION("Motorola vendor mmap fault driver");
MODULE_LICENSE("GPL v2");
