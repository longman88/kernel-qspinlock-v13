#ifndef _ASM_X86_QSPINLOCK_H
#define _ASM_X86_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>

#ifndef CONFIG_X86_PPRO_FENCE

#define	queue_spin_unlock queue_spin_unlock
/**
 * queue_spin_unlock - release a queue spinlock
 * @lock : Pointer to queue spinlock structure
 *
 * An effective smp_store_release() on the least-significant byte.
 */
static inline void queue_spin_unlock(struct qspinlock *lock)
{
	barrier();
	ACCESS_ONCE(*(u8 *)lock) = 0;
}

#endif /* !CONFIG_X86_PPRO_FENCE */

#include <asm-generic/qspinlock.h>

#endif /* _ASM_X86_QSPINLOCK_H */
