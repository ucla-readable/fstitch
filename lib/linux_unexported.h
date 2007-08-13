#ifndef __KUDOS_LIB_LINUX_UNEXPORTED_H
#define __KUDOS_LIB_LINUX_UNEXPORTED_H

// Copies of unexported Linux kernel functions and reimplementations
// of Linux kernel functions that call unexported functions

#include <linux/highmem.h>

#ifdef __i386__

#include <asm/uaccess.h>

unsigned long __must_check kudos__copy_from_user_ll_nocache(void *to,
				const void __user *from, unsigned long n);
unsigned long __must_check kudos__copy_from_user_ll_nocache_nozero(void *to,
				const void __user *from, unsigned long n);

// Copy from 2.6.20 include/asm-i386.h since it calls a nonexported function
static __always_inline unsigned long kudos__copy_from_user_nocache(void *to,
				const void __user *from, unsigned long n)
{
	might_sleep();
	if (__builtin_constant_p(n)) {
		unsigned long ret;

		switch (n) {
		case 1:
			__get_user_size(*(u8 *)to, from, 1, ret, 1);
			return ret;
		case 2:
			__get_user_size(*(u16 *)to, from, 2, ret, 2);
			return ret;
		case 4:
			__get_user_size(*(u32 *)to, from, 4, ret, 4);
			return ret;
		}
	}
	return kudos__copy_from_user_ll_nocache(to, from, n);
}

// Copy from 2.6.20 include/asm-i386.h since it calls a nonexported function
static __always_inline unsigned long
kudos__copy_from_user_inatomic_nocache(void *to, const void __user *from, unsigned long n)
{
	return kudos__copy_from_user_ll_nocache_nozero(to, from, n);
}

#else
# error Import Linux __copy_from_user[_inatomic] functions for this arch
#endif


// Copy from 2.6.20 mm/filemap.h since the file is not for public including
// and it transitively calls nonexported functions
static inline size_t
kudos_filemap_copy_from_user(struct page *page, unsigned long offset,
                       const char __user *buf, unsigned bytes)
{           
	char *kaddr;
	int left;

	kaddr = kmap_atomic(page, KM_USER0);
	left = kudos__copy_from_user_inatomic_nocache(kaddr + offset, buf, bytes);
	kunmap_atomic(kaddr, KM_USER0);

	if (left != 0) {
		/* Do it the slow way */
		kaddr = kmap(page);
		left = kudos__copy_from_user_nocache(kaddr + offset, buf, bytes);
		kunmap(page);
	}   
	return bytes - left;
}

#endif
