// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Alban Read */
/*
 * rosgd-shm -- the backings region (RISCOSGrandDesign design 39, 3.2).
 *
 * On the Mac apps the host, our QEMU, composites the RISC OS desktop
 * itself, drawing each window from its backing where it lies in guest
 * RAM.  The host can use only memory that is physically contiguous, so
 * the box's backings are carved from one region taken from the
 * contiguous memory allocator, which this device gives out -- the box's
 * 128 MB and, past them, the compositor's pages for Linux programs'
 * windows (rosgd/include/rosgd/presentwire.h):
 *
 *   /dev/rosgd-shm     mmap at offset 0: the region, normal cached memory
 *                      read: "<physical address> <size>\n", in hex and
 *                      decimal, for the box to publish to the host
 *
 * The region is taken on the first mmap or read, zeroed, and kept for
 * the life of the system: the host may hold its mapping of it at any
 * time, so it is never given back.  CMA's default area (cma=, or
 * CONFIG_CMA_SIZE_MBYTES) must hold it.  Pages from a dma-buf heap would
 * be contiguous too, but their mappings are PFN maps, which
 * /proc/self/pagemap does not walk, so the box could not learn where
 * they are; this device says.
 */
#include <linux/dma-map-ops.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/string.h>

/* the box's backings window (ROS_BACKING_SIZE, 128 MB), then the
 * compositor's pages (presentwire.h's PW_PAGES_OFFSET on, 64 MB) */
#define ROSGD_SHM_SIZE  (SZ_128M + SZ_64M)

static struct page *region;
static DEFINE_MUTEX(region_lock);

static int region_get(void)
{
	int e = 0;

	mutex_lock(&region_lock);
	if (!region) {
		region = dma_alloc_from_contiguous(NULL, ROSGD_SHM_SIZE >> PAGE_SHIFT,
						   get_order(SZ_1M), false);
		if (region)
			memset(page_address(region), 0, ROSGD_SHM_SIZE);
		else
			e = -ENOMEM;
	}
	mutex_unlock(&region_lock);
	return e;
}

static int rosgd_shm_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	int e = region_get();

	if (e)
		return e;
	if (vma->vm_pgoff > (ROSGD_SHM_SIZE >> PAGE_SHIFT) ||
	    len > ROSGD_SHM_SIZE - (vma->vm_pgoff << PAGE_SHIFT))
		return -EINVAL;
	/* cached, as any memory: the box draws into it all the time */
	return remap_pfn_range(vma, vma->vm_start, page_to_pfn(region) + vma->vm_pgoff,
			       len, vma->vm_page_prot);
}

static ssize_t rosgd_shm_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	char line[48];
	int len, e = region_get();

	if (e)
		return e;
	len = scnprintf(line, sizeof(line), "%llx %lu\n",
			(unsigned long long)page_to_phys(region), (unsigned long)ROSGD_SHM_SIZE);
	return simple_read_from_buffer(buf, n, pos, line, len);
}

static const struct file_operations rosgd_shm_fops = {
	.owner = THIS_MODULE,
	.mmap = rosgd_shm_mmap,
	.read = rosgd_shm_read,
	.llseek = default_llseek,
};

static struct miscdevice rosgd_shm_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "rosgd-shm",
	.fops = &rosgd_shm_fops,
	.mode = 0600,
};

static int __init rosgd_shm_init(void)
{
	return misc_register(&rosgd_shm_dev);
}
device_initcall(rosgd_shm_init);
