#ifndef _ASM_X86_QSPINLOCK_H
#define _ASM_X86_QSPINLOCK_H

#include <asm/cpufeature.h>
#include <asm-generic/qspinlock_types.h>

#ifndef CONFIG_X86_PPRO_FENCE
static __always_inline void native_spin_unlock(struct qspinlock *lock)
{
	barrier();
	ACCESS_ONCE(*(u8 *)lock) = 0;
}
#else
static __always_inline void native_spin_unlock(struct qspinlock *lock)
{
	atomic_dec(&lock->val);
}
#endif /* !CONFIG_X86_PPRO_FENCE */

#define	queue_spin_unlock queue_spin_unlock
#ifdef CONFIG_PARAVIRT_SPINLOCKS
/*
 * The lock byte can have a value of _Q_LOCKED_SLOWPATH to indicate
 * that it needs to go through the slowpath to do the unlocking.
 */
#define _Q_LOCKED_SLOWPATH	(_Q_LOCKED_VAL | 2)

extern void queue_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void pv_queue_spin_lock_slowpath(struct qspinlock *lock, u32 val);

/*
 * Paravirtualized versions of queue_spin_lock and queue_spin_unlock
 */

#define queue_spin_lock	queue_spin_lock
/**
 * queue_spin_lock - acquire a queue spinlock
 * @lock: Pointer to queue spinlock structure
 *
 * N.B. INLINE_SPIN_LOCK should not be enabled when PARAVIRT_SPINLOCK is on.
 */
static __always_inline void queue_spin_lock(struct qspinlock *lock)
{
	u32 val;

	val = atomic_cmpxchg(&lock->val, 0, _Q_LOCKED_VAL);
	if (likely(val == 0))
		return;
	if (static_key_false(&paravirt_spinlocks_enabled))
		pv_queue_spin_lock_slowpath(lock, val);
	else
		queue_spin_lock_slowpath(lock, val);
}

extern void queue_spin_unlock_slowpath(struct qspinlock *lock);

/**
 * queue_spin_unlock - release a queue spinlock
 * @lock : Pointer to queue spinlock structure
 *
 * An effective smp_store_release() on the least-significant byte.
 *
 * Inlining of the unlock function is disabled when CONFIG_PARAVIRT_SPINLOCKS
 * is defined. So _raw_spin_unlock() will be the only call site that will
 * have to be patched.
 */
static inline void queue_spin_unlock(struct qspinlock *lock)
{
	barrier();
	if (!static_key_false(&paravirt_spinlocks_enabled)) {
		native_spin_unlock(lock);
		return;
	}

	/*
	 * Need to atomically clear the lock byte to avoid racing with
	 * queue head waiter trying to set _QLOCK_LOCKED_SLOWPATH.
	 */
	if (unlikely(cmpxchg((u8 *)lock, _Q_LOCKED_VAL, 0) != _Q_LOCKED_VAL))
		queue_spin_unlock_slowpath(lock);
}
#else
static inline void queue_spin_unlock(struct qspinlock *lock)
{
	native_spin_unlock(lock);
}
#endif /* CONFIG_PARAVIRT_SPINLOCKS */

#define virt_queue_spin_lock virt_queue_spin_lock

static inline bool virt_queue_spin_lock(struct qspinlock *lock)
{
	if (!static_cpu_has(X86_FEATURE_HYPERVISOR))
		return false;

	while (atomic_cmpxchg(&lock->val, 0, _Q_LOCKED_VAL) != 0)
		cpu_relax();

	return true;
}

#include <asm-generic/qspinlock.h>

#endif /* _ASM_X86_QSPINLOCK_H */
