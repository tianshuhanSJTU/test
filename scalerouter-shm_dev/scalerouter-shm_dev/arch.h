#ifndef ARCH_H
#define ARCH_H

/*
 * Architecture-specific defines.  Currently, an architecture is
 * required to implement the following operations:
 *
 * mb() - memory barrier.  No loads or stores may be reordered across
 *     this macro by either the compiler or the CPU.
 * rmb() - read memory barrier.  No loads may be reordered across this
 *     macro by either the compiler or the CPU.
 * wmb() - write memory barrier.  No stores may be reordered across
 *     this macro by either the compiler or the CPU.
 * wc_wmb() - flush write combine buffers.  No write-combined writes
 *     will be reordered across this macro by either the compiler or
 *     the CPU.
 */

#if defined(__i386__)

#define mb()	 asm volatile("lock; addl $0,0(%%esp) " ::: "memory")
#define rmb()	 mb()
#define wmb()	 asm volatile("" ::: "memory")
#define wc_wmb() mb()
#define nc_wmb() wmb()

#elif defined(__x86_64__)

#define mb()	 asm volatile("" ::: "memory")
#define rmb()	 mb()
#define wmb()	 asm volatile("" ::: "memory")
#define wc_wmb() asm volatile("sfence" ::: "memory")
#define nc_wmb() wmb()
#define WC_AUTO_EVICT_SIZE 64

#elif defined(__PPC64__)

#define mb()	 asm volatile("sync" ::: "memory")
#define rmb()	 asm volatile("lwsync" ::: "memory")
#define wmb()	 rmb()
#define wc_wmb() mb()
#define nc_wmb() mb()
#define WC_AUTO_EVICT_SIZE 64

#elif defined(__ia64__)

#define mb()	 asm volatile("mf" ::: "memory")
#define rmb()	 mb()
#define wmb()	 mb()
#define wc_wmb() asm volatile("fwb" ::: "memory")
#define nc_wmb() wmb()

#elif defined(__PPC__)

#define mb()	 asm volatile("sync" ::: "memory")
#define rmb()	 mb()
#define wmb()	 mb()
#define wc_wmb() wmb()
#define nc_wmb() wmb()

#elif defined(__sparc_v9__)

#define mb()	 asm volatile("membar #LoadLoad | #LoadStore | #StoreStore | #StoreLoad" ::: "memory")
#define rmb()	 asm volatile("membar #LoadLoad" ::: "memory")
#define wmb()	 asm volatile("membar #StoreStore" ::: "memory")
#define wc_wmb() wmb()
#define nc_wmb() wmb()

#elif defined(__sparc__)

#define mb()	 asm volatile("" ::: "memory")
#define rmb()	 mb()
#define wmb()	 mb()
#define wc_wmb() wmb()
#define nc_wmb() wmb()

#elif defined(__aarch64__)

/* Perhaps dmb would be sufficient? Let us be conservative for now. */
#define mb()	asm volatile("dsb sy" ::: "memory")
#define rmb()	asm volatile("dsb ld" ::: "memory")
#define wmb()	asm volatile("dsb st" ::: "memory")
#define wc_wmb() wmb()
#define nc_wmb() wmb()

#elif defined(__s390x__)

#define mb()     asm volatile("" ::: "memory")
#define rmb()    mb()
#define wmb()    mb()
#define wc_wmb() wmb()
#define nc_wmb() wmb()

#else

#error No architecture specific memory barrier defines found!

#endif

#endif /* TYPES_H */