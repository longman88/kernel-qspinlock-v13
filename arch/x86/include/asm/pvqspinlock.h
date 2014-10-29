#ifndef _ASM_X86_PVQSPINLOCK_H
#define _ASM_X86_PVQSPINLOCK_H

/*
 *	Queue Spinlock Para-Virtualization (PV) Support
 *
 * The PV support code for queue spinlock is roughly the same as that
 * of the ticket spinlock. Each CPU waiting for the lock will spin until it
 * reaches a threshold. When that happens, it will put itself to a halt state
 * so that the hypervisor can reuse the CPU cycles in some other guests as
 * well as returning other hold-up CPUs faster.
 *
 * Auxillary fields in the pv_qnode structure are used to hold information
 * relevant to the PV support so that it won't impact on the behavior and
 * performance of the bare metal code.
 *
 * There are 2 places where races can happen:
 *  1) Halting of the queue head CPU (in pv_wait_head) and the CPU
 *     kicking by the lock holder in the unlock path (in pv_kick_node).
 *  2) Halting of the queue node CPU (in pv_link_and_wait_node) and the
 *     the status check by the previous queue head (in pv_wait_check).
 *
 * See the comments on those functions to see how the races are being
 * addressed.
 */

/*
 * Spin thresholds for queue spinlock
 */
#define	QSPIN_THRESHOLD		SPIN_THRESHOLD
#define MAYHALT_THRESHOLD	0x10

/*
 * CPU state flags
 */
#define PV_CPU_ACTIVE	1	/* This CPU is active		 */
#define PV_CPU_KICKED   2	/* This CPU is being kicked	 */
#define PV_CPU_HALTED	-1	/* This CPU is halted		 */

/*
 * Special head node pointer value
 */
#define PV_INVALID_HEAD	NULL

/*
 * Additional fields to be added to the queue node structure
 *
 * The size of the mcs_spinlock structure is 16 bytes for x64 and 12 bytes
 * for i386. Four of those structures are defined per CPU. To add more fields
 * without increasing the size of the mcs_spinlock structure, we overlay those
 * additional data fields at an additional mcs_spinlock size bucket at exactly
 * 3 units away. As a result, we need to double the number of mcs_spinlock
 * buckets. The mcs_spinlock structure will be casted to the pv_qnode
 * internally.
 *
 * +------------+------------+------------+------------+
 * | MCS Node 0 | MCS Node 1 | MCS Node 2 | MCS Node 3 |
 * +------------+------------+------------+------------+
 * | PV  Node 0 | PV  Node 1 | PV  Node 2 | PV  Node 3 |
 * +------------+------------+------------+------------+
 */
struct pv_qnode {
	struct mcs_spinlock  mcs;	/* MCS node			*/
	struct mcs_spinlock  __res[3];	/* 3 reserved MCS nodes		*/
	s8		     cpustate;	/* CPU status flag		*/
	s8		     mayhalt;	/* May be halted soon		*/
	int		     mycpu;	/* CPU number of this node	*/
	struct mcs_spinlock  *head;	/* Queue head node pointer	*/
};

/**
 * pv_init_node - initialize fields in struct pv_qnode
 * @node: pointer to struct mcs_spinlock
 * @cpu : current CPU number
 */
static inline void pv_init_node(struct mcs_spinlock *node)
{
	struct pv_qnode *pn = (struct pv_qnode *)node;

	BUILD_BUG_ON(sizeof(struct pv_qnode) > 5*sizeof(struct mcs_spinlock));

	pn->cpustate = PV_CPU_ACTIVE;
	pn->mayhalt  = false;
	pn->mycpu    = smp_processor_id();
	pn->head     = PV_INVALID_HEAD;
}

/**
 * pv_decode_tail - initialize fields in struct pv_qnode
 * @tail: the tail code (lock value)
 * Return: a pointer to the tail pv_qnode structure
 */
static inline struct pv_qnode *pv_decode_tail(u32 tail)
{
	return (struct pv_qnode *)decode_tail(tail);
}

/**
 * pv_set_head_in_tail - set head node pointer in tail node
 * @lock: pointer to the qspinlock structure
 * @head: pointer to queue head mcs_spinlock structure
 */
static inline void
pv_set_head_in_tail(struct qspinlock *lock, struct mcs_spinlock *head)
{
	struct pv_qnode *tn, *new_tn;	/* Tail nodes */

	/*
	 * The writing is repeated in case the queue tail changes.
	 */
	new_tn = pv_decode_tail(atomic_read(&lock->val));
	do {
		tn = new_tn;
		while (tn->head == PV_INVALID_HEAD)
			cpu_relax();
		tn->head = head;
		new_tn   = pv_decode_tail(atomic_read(&lock->val));
	} while (tn != new_tn);
}

/**
 * pv_link_and_wait_node - perform para-virtualization checks for queue member
 * @old  : the old lock value
 * @node : pointer to the mcs_spinlock structure
 * Return: true if PV spinlock is enabled, false otherwise.
 */
static inline bool pv_link_and_wait_node(u32 old, struct mcs_spinlock *node)
{
	struct pv_qnode *ppn, *pn = (struct pv_qnode *)node;
	unsigned int count;

	if (!(old & _Q_TAIL_MASK)) {
		node->locked = true;	/* At queue head now */
		goto ret;
	}

	ppn = pv_decode_tail(old);
	ACCESS_ONCE(ppn->mcs.next) = node;

	/*
	 * It is possible that this node will become the queue head while
	 * waiting for the head value of the previous node to be set.
	 */
	while (ppn->head == PV_INVALID_HEAD) {
		if (node->locked)
			goto ret;
		cpu_relax();
	}
	pn->head = ppn->head;

	for (;;) {
		count = QSPIN_THRESHOLD;

		while (count--) {
			if (smp_load_acquire(&node->locked))
				goto ret;
			if (count == MAYHALT_THRESHOLD) {
				pn->mayhalt = true;
				/*
				 * Make sure that the mayhalt flag is visible
				 * to others.
				 */
				smp_mb();
			}
			cpu_relax();
		}
		/*
		 * Halt oneself after QSPIN_THRESHOLD spins
		 */
		ACCESS_ONCE(pn->cpustate) = PV_CPU_HALTED;

		/*
		 * One way to avoid the racing between pv_wait_check()
		 * and pv_link_and_wait_node() is to use memory barrier or
		 * atomic instruction to synchronize between the two competing
		 * threads. However, that will slow down the queue spinlock
		 * slowpath. One way to eliminate this overhead for normal
		 * cases is to use another flag (mayhalt) to indicate that
		 * racing condition may happen. This flag is set when the
		 * loop count is getting close to the halting threshold.
		 *
		 * When that happens, a 2 variables (cpustate & node->locked
		 * handshake is used to make sure that pv_wait_check() won't
		 * miss setting the _Q_LOCKED_SLOWPATH when the CPU is about
		 * to be halted.
		 *
		 * pv_wait_check		pv_link_and_wait_node
		 * -------------		---------------------
		 * [1] node->locked = true	[3] cpustate = PV_CPU_HALTED
		 *     smp_mb()			    smp_mb()
		 * [2] if (cpustate		[4] if (node->locked)
		 *        == PV_CPU_HALTED)
		 *
		 * Sequence:
		 * *,1,*,4,* - halt is aborted as the node->locked flag is set,
		 *	       _Q_LOCKED_SLOWPATH may or may not be set
		 * 3,4,1,2 - the CPU is halt and _Q_LOCKED_SLOWPATH is set
		 */
		smp_mb();
		if (!ACCESS_ONCE(node->locked)) {
			/*
			 * Halt the CPU only if it is not the queue head
			 */
			pv_lockwait(NULL);
			pv_lockstat((pn->cpustate == PV_CPU_KICKED)
				   ? PV_WAKE_KICKED : PV_WAKE_SPURIOUS);
		}
		ACCESS_ONCE(pn->cpustate) = PV_CPU_ACTIVE;
		pn->mayhalt = false;

		if (smp_load_acquire(&node->locked))
			break;
	}
ret:
	pn->head = node;
	return true;
}

/**
 * pv_wait_head - para-virtualization waiting loop for the queue head
 * @lock : pointer to the qspinlock structure
 * @node : pointer to the mcs_spinlock structure
 * Return: the current lock value
 *
 * This function will halt itself if lock is still not available after
 * QSPIN_THRESHOLD iterations.
 */
static inline int
pv_wait_head(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_qnode *pn = (struct pv_qnode *)node;

	for (;;) {
		unsigned int count;
		s8 oldstate;
		int val;

reset:
		count = QSPIN_THRESHOLD;
		ACCESS_ONCE(pn->cpustate) = PV_CPU_ACTIVE;

		while (count--) {
			val = smp_load_acquire(&lock->val.counter);
			if (!(val & _Q_LOCKED_PENDING_MASK))
				return val;
			if (pn->cpustate == PV_CPU_KICKED)
				/*
				 * Reset count and flag
				 */
				goto reset;
			cpu_relax();
		}

		/*
		 * Write the head CPU number into the queue tail node before
		 * halting.
		 */
		pv_set_head_in_tail(lock, node);

		/*
		 * Set the lock byte to _Q_LOCKED_SLOWPATH before
		 * trying to halt itself. It is possible that the
		 * lock byte had been set to _Q_LOCKED_SLOWPATH
		 * already (spurious wakeup of queue head after a halt
		 * or opportunistic setting in pv_wait_check()).
		 * In this case, just proceeds to sleeping.
		 *
		 *     queue head		    lock holder
		 *     ----------		    -----------
		 *     cpustate = PV_CPU_HALTED
		 * [1] cmpxchg(_Q_LOCKED_VAL	[2] cmpxchg(_Q_LOCKED_VAL => 0)
		 * => _Q_LOCKED_SLOWPATH)	    if (cmpxchg fails &&
		 *     if (cmpxchg succeeds)	    cpustate == PV_CPU_HALTED)
		 *        halt()		       kick()
		 *
		 * Sequence:
		 * 1,2 - slowpath flag set, queue head halted & lock holder
		 *	 will call slowpath
		 * 2,1 - queue head cmpxchg fails, halt is aborted
		 *
		 * If the queue head CPU is woken up by a spurious interrupt
		 * at the same time as the lock holder check the cpustate,
		 * it is possible that the lock holder will try to kick
		 * the queue head CPU which isn't halted.
		 */
		oldstate = cmpxchg(&pn->cpustate, PV_CPU_ACTIVE, PV_CPU_HALTED);
		if (oldstate == PV_CPU_KICKED)
			continue;	/* Reset count & flag */

		val = cmpxchg((u8 *)lock,
				  _Q_LOCKED_VAL, _Q_LOCKED_SLOWPATH);
		if (val) {
			pv_lockwait((u8 *)lock);
			pv_lockstat((pn->cpustate == PV_CPU_KICKED)
				   ? PV_WAKE_KICKED : PV_WAKE_SPURIOUS);
		} else {
			/*
			 * The lock is free and no halting is needed
			 */
			ACCESS_ONCE(pn->cpustate) = PV_CPU_ACTIVE;
			return smp_load_acquire(&lock->val.counter);
		}
	}
	/* Unreachable */
	return 0;
}

/**
 * pv_wait_check - check if the CPU has been halted & set _Q_LOCKED_SLOWPATH
 * @lock: pointer to the qspinlock structure
 * @node: pointer to the mcs_spinlock structure of lock holder
 * @next: pointer to the mcs_spinlock structure of new queue head
 *
 * The current CPU should have gotten the lock before calling this function.
 */
static inline void pv_wait_check(struct qspinlock *lock,
		   struct mcs_spinlock *node, struct mcs_spinlock *next)
{
	struct pv_qnode *pnxt = (struct pv_qnode *)next;
	struct pv_qnode *pcur = (struct pv_qnode *)node;

	/*
	 * Clear the locked and head values of lock holder
	 */
	pcur->mcs.locked = false;
	pcur->head	 = PV_INVALID_HEAD;

	/*
	 * Halt state checking will only be done if the mayhalt flag is set
	 * to avoid the overhead of the memory barrier in normal cases.
	 * It is highly unlikely that the actual writing to the node->locked
	 * flag will be more than 0x10 iterations later than the reading of
	 * the mayhalt flag so that it misses seeing the PV_CPU_HALTED state
	 * which causes lost wakeup.
	 */
	if (!ACCESS_ONCE(pnxt->mayhalt))
		return;

	/*
	 * A memory barrier is used here to make sure that the setting
	 * of node->locked flag prior to this function call is visible
	 * to others before checking the cpustate flag.
	 */
	smp_mb();
	if (pnxt->cpustate != PV_CPU_HALTED)
		return;

	ACCESS_ONCE(*(u8 *)lock) = _Q_LOCKED_SLOWPATH;
	pv_set_head_in_tail(lock, next);
}

/**
 * pv_kick_node - kick up the CPU of the given node
 * @node : pointer to struct mcs_spinlock of the node to be kicked
 */
static inline void pv_kick_node(struct mcs_spinlock *node)
{
	struct pv_qnode *pn = (struct pv_qnode *)node;
	s8 oldstate;

	if (!pn)
		return;

	oldstate = xchg(&pn->cpustate, PV_CPU_KICKED);
	/*
	 * Kick the CPU only if the state was set to PV_CPU_HALTED
	 */
	if (oldstate != PV_CPU_HALTED)
		pv_lockstat(PV_KICK_NOHALT);
	else
		pv_kick_cpu(pn->mycpu);
}

/*
 * pv_get_qhead - get node pointer of queue head
 * @lock : pointer to the qspinlock structure
 * Return: pointer to mcs_spinlock structure of queue head
 */
static inline struct mcs_spinlock *pv_get_qhead(struct qspinlock *lock)
{
	struct pv_qnode *pn = pv_decode_tail(atomic_read(&lock->val));

	while (pn->head == PV_INVALID_HEAD)
		cpu_relax();

	if (WARN_ON_ONCE(!pn->head->locked))
		return NULL;

	return pn->head;
}

/**
 * queue_spin_unlock_slowpath - kick up the CPU of the queue head
 * @lock : Pointer to queue spinlock structure
 *
 * The lock is released after finding the queue head to avoid racing
 * condition between the queue head and the lock holder.
 */
void queue_spin_unlock_slowpath(struct qspinlock *lock)
{
	struct mcs_spinlock *node = pv_get_qhead(lock);

	/*
	 * Found the queue head, now release the lock before waking it up
	 */
	native_spin_unlock(lock);
	pv_kick_node(node);
}
EXPORT_SYMBOL(queue_spin_unlock_slowpath);

#endif /* _ASM_X86_PVQSPINLOCK_H */
