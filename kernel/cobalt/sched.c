/**
 * Copyright (C) 2001-2013 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * @ingroup sched
 */
#include <cobalt/kernel/sched.h>
#include <cobalt/kernel/thread.h>
#include <cobalt/kernel/timer.h>
#include <cobalt/kernel/intr.h>
#include <cobalt/kernel/heap.h>
#include <cobalt/kernel/shadow.h>
#include <cobalt/kernel/arith.h>

DEFINE_PER_CPU(struct xnsched, nksched);
EXPORT_PER_CPU_SYMBOL_GPL(nksched);

cpumask_t nkaffinity = CPU_MASK_ALL;
EXPORT_SYMBOL_GPL(nkaffinity);

LIST_HEAD(nkthreadq);

int nknrthreads;

#ifdef CONFIG_XENO_OPT_VFILE
struct xnvfile_rev_tag nkthreadlist_tag;
#endif

static struct xnsched_class *xnsched_class_highest;

#define for_each_xnsched_class(p) \
   for (p = xnsched_class_highest; p; p = p->next)

static void xnsched_register_class(struct xnsched_class *sched_class)
{
	sched_class->next = xnsched_class_highest;
	xnsched_class_highest = sched_class;

	/*
	 * Classes shall be registered by increasing priority order,
	 * idle first and up.
	 */
	XENO_BUGON(NUCLEUS, sched_class->next &&
		   sched_class->next->weight > sched_class->weight);

	printk(XENO_INFO "scheduling class %s registered.\n", sched_class->name);
}

void xnsched_register_classes(void)
{
	xnsched_register_class(&xnsched_class_idle);
#ifdef CONFIG_XENO_OPT_SCHED_WEAK
	xnsched_register_class(&xnsched_class_weak);
#endif
#ifdef CONFIG_XENO_OPT_SCHED_TP
	xnsched_register_class(&xnsched_class_tp);
#endif
#ifdef CONFIG_XENO_OPT_SCHED_SPORADIC
	xnsched_register_class(&xnsched_class_sporadic);
#endif
	xnsched_register_class(&xnsched_class_rt);
}

#ifdef CONFIG_XENO_OPT_WATCHDOG

static unsigned long wd_timeout_arg = CONFIG_XENO_OPT_WATCHDOG_TIMEOUT;
module_param_named(watchdog_timeout, wd_timeout_arg, ulong, 0644);
MODULE_PARM_DESC(watchdog_timeout, "Watchdog timeout (s)");

/**
 * @internal
 * @fn void watchdog_handler(struct xntimer *timer)
 * @brief Process watchdog ticks.
 *
 * This internal routine handles incoming watchdog ticks to detect
 * software lockups. It kills any offending thread which is found to
 * monopolize the CPU so as to starve the Linux kernel for too long.
 */
static void watchdog_handler(struct xntimer *timer)
{
	struct xnsched *sched = xnsched_current();
	struct xnthread *curr = sched->curr;

	if (likely(xnthread_test_state(curr, XNROOT))) {
		xnsched_reset_watchdog(sched);
		return;
	}

	if (likely(++sched->wdcount < wd_timeout_arg))
		return;

	trace_mark(xn_nucleus, watchdog_signal,
		   "thread %p thread_name %s",
		   curr, xnthread_name(curr));

	if (xnthread_test_state(curr, XNUSER)) {
		printk(XENO_WARN "watchdog triggered on CPU #%d -- runaway thread "
		       "'%s' signaled\n", xnsched_cpu(sched), xnthread_name(curr));
		xnshadow_call_mayday(curr, SIGDEBUG_WATCHDOG);
	} else {
		printk(XENO_WARN "watchdog triggered on CPU #%d -- runaway thread "
		       "'%s' canceled\n", xnsched_cpu(sched), xnthread_name(curr));
		/*
		 * On behalf on an IRQ handler, xnthread_cancel()
		 * would go half way cancelling the preempted
		 * thread. Therefore we manually raise XNKICKED to
		 * cause the next call to xnthread_suspend() to return
		 * early in XNBREAK condition, and XNCANCELD so that
		 * @thread exits next time it invokes
		 * xnthread_test_cancel().
		 */
		xnthread_set_info(curr, XNKICKED|XNCANCELD);
	}

	xnsched_reset_watchdog(sched);
}

#endif /* CONFIG_XENO_OPT_WATCHDOG */

static void roundrobin_handler(struct xntimer *timer)
{
	struct xnsched *sched = container_of(timer, struct xnsched, rrbtimer);
	xnsched_tick(sched);
}

void xnsched_init(struct xnsched *sched, int cpu)
{
	char rrbtimer_name[XNOBJECT_NAME_LEN];
	char htimer_name[XNOBJECT_NAME_LEN];
	char root_name[XNOBJECT_NAME_LEN];
	union xnsched_policy_param param;
	struct xnthread_init_attr attr;
	struct xnsched_class *p;

#ifdef CONFIG_SMP
	sched->cpu = cpu;
	sprintf(htimer_name, "[host-timer/%u]", cpu);
	sprintf(rrbtimer_name, "[rrb-timer/%u]", cpu);
	sprintf(root_name, "ROOT/%u", cpu);
	cpus_clear(sched->resched);
#else
	strcpy(htimer_name, "[host-timer]");
	strcpy(rrbtimer_name, "[rrb-timer]");
	strcpy(root_name, "ROOT");
#endif
	for_each_xnsched_class(p) {
		if (p->sched_init)
			p->sched_init(sched);
	}

	sched->status = 0;
	sched->lflags = 0;
	sched->inesting = 0;
	sched->curr = &sched->rootcb;

	attr.flags = XNROOT | XNFPU;
	attr.name = root_name;
	attr.personality = &xenomai_personality;
	attr.affinity = cpumask_of_cpu(cpu);
	param.idle.prio = XNSCHED_IDLE_PRIO;

	__xnthread_init(&sched->rootcb, &attr,
			sched, &xnsched_class_idle, &param);

	/*
	 * No direct handler here since the host timer processing is
	 * postponed to xnintr_irq_handler(), as part of the interrupt
	 * exit code.
	 */
	xntimer_init(&sched->htimer, &nkclock, NULL, &sched->rootcb);
	xntimer_set_priority(&sched->htimer, XNTIMER_LOPRIO);
	xntimer_set_name(&sched->htimer, htimer_name);
	xntimer_init(&sched->rrbtimer, &nkclock,
		     roundrobin_handler, &sched->rootcb);
	xntimer_set_name(&sched->rrbtimer, rrbtimer_name);
	xntimer_set_priority(&sched->rrbtimer, XNTIMER_LOPRIO);

	xnstat_exectime_set_current(sched, &sched->rootcb.stat.account);
#ifdef CONFIG_XENO_HW_FPU
	sched->fpuholder = &sched->rootcb;
#endif /* CONFIG_XENO_HW_FPU */

	xnthread_init_root_tcb(&sched->rootcb);
	list_add_tail(&sched->rootcb.glink, &nkthreadq);
	nknrthreads++;

#ifdef CONFIG_XENO_OPT_WATCHDOG
	xntimer_init_noblock(&sched->wdtimer, &nkclock,
			     watchdog_handler, &sched->rootcb);
	xntimer_set_name(&sched->wdtimer, "[watchdog]");
	xntimer_set_priority(&sched->wdtimer, XNTIMER_LOPRIO);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
}

void xnsched_destroy(struct xnsched *sched)
{
	xntimer_destroy(&sched->htimer);
	xntimer_destroy(&sched->rrbtimer);
	xntimer_destroy(&sched->rootcb.ptimer);
	xntimer_destroy(&sched->rootcb.rtimer);
#ifdef CONFIG_XENO_OPT_WATCHDOG
	xntimer_destroy(&sched->wdtimer);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
}

static inline void set_thread_running(struct xnsched *sched,
				      struct xnthread *thread)
{
	xnthread_clear_state(thread, XNREADY);
	if (xnthread_test_state(thread, XNRRB))
		xntimer_start(&sched->rrbtimer,
			      thread->rrperiod, XN_INFINITE, XN_RELATIVE);
	else
		xntimer_stop(&sched->rrbtimer);
}

/* Must be called with nklock locked, interrupts off. */
struct xnthread *xnsched_pick_next(struct xnsched *sched)
{
	struct xnsched_class *p __maybe_unused;
	struct xnthread *curr = sched->curr;
	struct xnthread *thread;

	if (!xnthread_test_state(curr, XNTHREAD_BLOCK_BITS | XNZOMBIE)) {
		/*
		 * Do not preempt the current thread if it holds the
		 * scheduler lock.
		 */
		if (xnthread_test_state(curr, XNLOCK)) {
			xnsched_set_self_resched(sched);
			return curr;
		}
		/*
		 * Push the current thread back to the runnable queue
		 * of the scheduling class it belongs to, if not yet
		 * linked to it (XNREADY tells us if it is).
		 */
		if (!xnthread_test_state(curr, XNREADY)) {
			xnsched_requeue(curr);
			xnthread_set_state(curr, XNREADY);
		}
	}

	/*
	 * Find the runnable thread having the highest priority among
	 * all scheduling classes, scanned by decreasing priority.
	 */
#ifdef CONFIG_XENO_OPT_SCHED_CLASSES
	for_each_xnsched_class(p) {
		thread = p->sched_pick(sched);
		if (thread) {
			set_thread_running(sched, thread);
			return thread;
		}
	}

	return NULL; /* Never executed because of the idle class. */
#else /* !CONFIG_XENO_OPT_SCHED_CLASSES */
	thread = __xnsched_rt_pick(sched);
	if (unlikely(thread == NULL))
		thread = &sched->rootcb;

	set_thread_running(sched, thread);

	return thread;
#endif /* CONFIG_XENO_OPT_SCHED_CLASSES */
}

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH

struct xnsched *xnsched_finish_unlocked_switch(struct xnsched *sched)
{
	struct xnthread *last;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

#ifdef CONFIG_SMP
	/* If current thread migrated while suspended */
	sched = xnsched_current();
#endif /* CONFIG_SMP */

	last = sched->last;
	sched->status &= ~XNINSW;

	/* Detect a thread which called xnthread_migrate() */
	if (last->sched != sched) {
		xnsched_putback(last);
		xnthread_clear_state(last, XNMIGRATE);
	}

	return sched;
}

#endif /* CONFIG_XENO_HW_UNLOCKED_SWITCH */

void ___xnsched_lock(struct xnsched *sched)
{
	struct xnthread *curr = sched->curr;

	if (xnthread_lock_count(curr)++ == 0) {
		sched->lflags |= XNINLOCK;
		xnthread_set_state(curr, XNLOCK);
	}
}
EXPORT_SYMBOL_GPL(___xnsched_lock);

void ___xnsched_unlock(struct xnsched *sched)
{
	struct xnthread *curr = sched->curr;

	if (!XENO_ASSERT(NUCLEUS, xnthread_lock_count(curr) > 0))
		return;

	if (--xnthread_lock_count(curr) == 0) {
		xnthread_clear_state(curr, XNLOCK);
		sched->lflags &= ~XNINLOCK;
		xnsched_run();
	}
}
EXPORT_SYMBOL_GPL(___xnsched_unlock);

void ___xnsched_unlock_fully(struct xnsched *sched)
{
	struct xnthread *curr = sched->curr;

	xnthread_lock_count(curr) = 0;
	xnthread_clear_state(curr, XNLOCK);
	sched->lflags &= ~XNINLOCK;
	xnsched_run();
}
EXPORT_SYMBOL_GPL(___xnsched_unlock_fully);

/* Must be called with nklock locked, interrupts off. */
void xnsched_putback(struct xnthread *thread)
{
	if (xnthread_test_state(thread, XNREADY))
		xnsched_dequeue(thread);
	else
		xnthread_set_state(thread, XNREADY);

	xnsched_enqueue(thread);
	xnsched_set_resched(thread->sched);
}

/* Must be called with nklock locked, interrupts off. */
int xnsched_set_policy(struct xnthread *thread,
		       struct xnsched_class *sched_class,
		       const union xnsched_policy_param *p)
{
	int ret;

	/*
	 * Declaring a thread to a new scheduling class may fail, so
	 * we do that early, while the thread is still a member of the
	 * previous class. However, this also means that the
	 * declaration callback shall not do anything that might
	 * affect the previous class (such as touching thread->rlink
	 * for instance).
	 */
	if (sched_class != thread->base_class) {
		if (sched_class->sched_declare) {
			ret = sched_class->sched_declare(thread, p);
			if (ret)
				return ret;
		}
		sched_class->nthreads++;
	}

	/*
	 * As a special case, we may be called from __xnthread_init()
	 * with no previous scheduling class at all.
	 */
	if (likely(thread->base_class != NULL)) {
		if (xnthread_test_state(thread, XNREADY))
			xnsched_dequeue(thread);

		if (sched_class != thread->base_class)
			xnsched_forget(thread);
	}

	thread->sched_class = sched_class;
	thread->base_class = sched_class;
	xnsched_setparam(thread, p);
	thread->bprio = thread->cprio;
	thread->wprio = thread->cprio + sched_class->weight;

	if (xnthread_test_state(thread, XNREADY))
		xnsched_enqueue(thread);

	if (!xnthread_test_state(thread, XNDORMANT))
		xnsched_set_resched(thread->sched);

	return 0;
}
EXPORT_SYMBOL_GPL(xnsched_set_policy);

/* Must be called with nklock locked, interrupts off. */
void xnsched_track_policy(struct xnthread *thread,
			  struct xnthread *target)
{
	union xnsched_policy_param param;

	if (xnthread_test_state(thread, XNREADY))
		xnsched_dequeue(thread);
	/*
	 * Self-targeting means to reset the scheduling policy and
	 * parameters to the base ones. Otherwise, make thread inherit
	 * the scheduling data from target.
	 */
	if (target == thread) {
		thread->sched_class = thread->base_class;
		xnsched_trackprio(thread, NULL);
	} else {
		xnsched_getparam(target, &param);
		thread->sched_class = target->sched_class;
		xnsched_trackprio(thread, &param);
	}

	if (xnthread_test_state(thread, XNREADY))
		xnsched_enqueue(thread);

	xnsched_set_resched(thread->sched);
}

static void migrate_thread(struct xnthread *thread, struct xnsched *sched)
{
	struct xnsched_class *sched_class = thread->sched_class;

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	if (sched_class->sched_migrate)
		sched_class->sched_migrate(thread, sched);
	/*
	 * WARNING: the scheduling class may have just changed as a
	 * result of calling the per-class migration hook.
	 */
	xnsched_set_resched(thread->sched);
	thread->sched = sched;
}

/*
 * Must be called with nklock locked, interrupts off. thread must be
 * runnable.
 */
void xnsched_migrate(struct xnthread *thread, struct xnsched *sched)
{
	migrate_thread(thread, sched);

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	/*
	 * Mark the thread in flight, xnsched_finish_unlocked_switch()
	 * will put the thread on the remote runqueue.
	 */
	xnthread_set_state(thread, XNMIGRATE);
#else /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
	/* Move thread to the remote runnable queue. */
	xnsched_putback(thread);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
}

/*
 * Must be called with nklock locked, interrupts off. Thread may be
 * blocked.
 */
void xnsched_migrate_passive(struct xnthread *thread, struct xnsched *sched)
{
	migrate_thread(thread, sched);

	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
		xnsched_requeue(thread);
		xnthread_set_state(thread, XNREADY);
	}
}

#ifdef CONFIG_XENO_OPT_SCALABLE_SCHED

void xnsched_initq(struct xnsched_mlq *q, int loprio, int hiprio)
{
	int prio;

	XENO_BUGON(NUCLEUS, hiprio - loprio + 1 >= XNSCHED_MLQ_LEVELS);

	q->elems = 0;
	q->loprio = loprio;
	q->hiprio = hiprio;
	q->himap = 0;
	memset(&q->lomap, 0, sizeof(q->lomap));

	for (prio = 0; prio < XNSCHED_MLQ_LEVELS; prio++)
		INIT_LIST_HEAD(q->heads + prio);
}

static inline int get_qindex(struct xnsched_mlq *q, int prio)
{
	XENO_BUGON(NUCLEUS, prio < q->loprio || prio > q->hiprio);
	/*
	 * BIG FAT WARNING: We need to rescale the priority level to a
	 * 0-based range. We use ffnz() to scan the bitmap which MUST
	 * be based on a bit scan forward op. Therefore, the lower the
	 * index value, the higher the priority (since least
	 * significant bits will be found first when scanning the
	 * bitmaps).
	 */
	return q->hiprio - prio;
}

static struct list_head *add_q(struct xnsched_mlq *q, int prio)
{
	struct list_head *head;
	int hi, lo, idx;

	idx = get_qindex(q, prio);
	head = q->heads + idx;
	q->elems++;

	/* New item is not linked yet. */
	if (list_empty(head)) {
		hi = idx / BITS_PER_LONG;
		lo = idx % BITS_PER_LONG;
		q->himap |= (1UL << hi);
		q->lomap[hi] |= (1UL << lo);
	}

	return head;
}

void xnsched_addq(struct xnsched_mlq *q, struct xnthread *thread)
{
	struct list_head *head = add_q(q, thread->cprio);
	list_add(&thread->rlink, head);
}

void xnsched_addq_tail(struct xnsched_mlq *q, struct xnthread *thread)
{
	struct list_head *head = add_q(q, thread->cprio);
	list_add_tail(&thread->rlink, head);
}

static void del_q(struct xnsched_mlq *q,
		  struct list_head *entry, int idx)
{
	struct list_head *head;
	int hi, lo;

	head = q->heads + idx;
	list_del(entry);
	q->elems--;

	if (list_empty(head)) {
		hi = idx / BITS_PER_LONG;
		lo = idx % BITS_PER_LONG;
		q->lomap[hi] &= ~(1UL << lo);
		if (q->lomap[hi] == 0)
			q->himap &= ~(1UL << hi);
	}
}

void xnsched_delq(struct xnsched_mlq *q, struct xnthread *thread)
{
	del_q(q, &thread->rlink, get_qindex(q, thread->cprio));
}

static inline int ffs_q(struct xnsched_mlq *q)
{
	int hi = ffnz(q->himap);
	int lo = ffnz(q->lomap[hi]);
	return hi * BITS_PER_LONG + lo;	/* Result is undefined if none set. */
}

struct xnthread *xnsched_getq(struct xnsched_mlq *q)
{
	struct xnthread *thread;
	struct list_head *head;
	int idx;

	if (q->elems == 0)
		return NULL;

	idx = ffs_q(q);
	head = q->heads + idx;
	XENO_BUGON(NUCLEUS, list_empty(head));
	thread = list_first_entry(head, struct xnthread, rlink);
	del_q(q, &thread->rlink, idx);

	return thread;
}

struct xnthread *xnsched_findq(struct xnsched_mlq *q, int prio)
{
	struct list_head *head;
	int idx;

	idx = get_qindex(q, prio);
	head = q->heads + idx;
	if (list_empty(head))
		return NULL;

	return list_first_entry(head, struct xnthread, rlink);
}

#else /* !CONFIG_XENO_OPT_SCALABLE_SCHED */

struct xnthread *xnsched_findq(struct list_head *q, int prio)
{
	struct xnthread *thread;

	if (list_empty(q))
		return NULL;

	/* Find thread leading a priority group. */
	list_for_each_entry(thread, q, rlink) {
		if (prio == thread->cprio)
			return thread;
	}

	return NULL;
}

#endif /* !CONFIG_XENO_OPT_SCALABLE_SCHED */

static inline void switch_context(struct xnsched *sched,
				  xnthread_t *prev, xnthread_t *next)
{
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	sched->last = prev;
	sched->status |= XNINSW;
	xnlock_clear_irqon(&nklock);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */

	xnarch_switch_to(prev, next);
}

/**
 * @fn int xnsched_run(void)
 * @brief The rescheduling procedure.
 *
 * This is the central rescheduling routine which should be called to
 * validate and apply changes which have previously been made to the
 * nucleus scheduling state, such as suspending, resuming or changing
 * the priority of threads.  This call first determines if a thread
 * switch should take place, and performs it as needed. xnsched_run()
 * schedules out the current thread if:
 *
 * - the current thread is about to block.
 * - a runnable thread from a higher priority scheduling class is
 * waiting for the CPU.
 * - the current thread does not lead the runnable threads from its
 * own scheduling class (i.e. round-robin).
 *
 * The Cobalt core implements a lazy rescheduling scheme so that most
 * of the services affecting the threads state MUST be followed by a
 * call to the rescheduling procedure for the new scheduling state to
 * be applied.
 *
 * In other words, multiple changes on the scheduler state can be done
 * in a row, waking threads up, blocking others, without being
 * immediately translated into the corresponding context switches.
 * When all changes have been applied, xnsched_run() should be called
 * for considering those changes, and possibly switching context.
 *
 * As a notable exception to the previous principle however, every
 * action which ends up suspending the current thread begets an
 * implicit call to the rescheduling procedure on behalf of the
 * blocking service.
 *
 * Typically, self-suspension or sleeping on a synchronization object
 * automatically leads to a call to the rescheduling procedure,
 * therefore the caller does not need to explicitly issue
 * xnsched_run() after such operations.
 *
 * The rescheduling procedure always leads to a null-effect if it is
 * called on behalf of an interrupt service routine. Any outstanding
 * scheduler lock held by the outgoing thread will be restored when
 * the thread is scheduled back in.
 *
 * Calling this procedure with no applicable context switch pending is
 * harmless and simply leads to a null-effect.
 *
 * @return Non-zero is returned if a context switch actually happened,
 * otherwise zero if the current thread was left running.
 *
 * Environments:
 *
 * This service can be called from any context.
 */
static inline int test_resched(struct xnsched *sched)
{
	int resched = sched->status & XNRESCHED;
#ifdef CONFIG_SMP
	/* Send resched IPI to remote CPU(s). */
	if (unlikely(!cpus_empty(sched->resched))) {
		smp_mb();
		ipipe_send_ipi(IPIPE_RESCHEDULE_IPI, sched->resched);
		cpus_clear(sched->resched);
	}
#else
	resched = xnsched_resched_p(sched);
#endif
	sched->status &= ~XNRESCHED;

	return resched;
}

static inline void enter_root(struct xnthread *root)
{
	struct xnarchtcb *rootcb __maybe_unused = xnthread_archtcb(root);

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	if (rootcb->core.mm == NULL)
		set_ti_thread_flag(rootcb->core.tip, TIF_MMSWITCH_INT);
#endif
	ipipe_unmute_pic();
}

static inline void leave_root(struct xnthread *root)
{
	struct xnarchtcb *rootcb = xnthread_archtcb(root);
	struct task_struct *p = current;

	ipipe_notify_root_preemption();
	ipipe_mute_pic();
	/* Remember the preempted Linux task pointer. */
	rootcb->core.host_task = p;
	rootcb->core.tsp = &p->thread;
	rootcb->core.mm = rootcb->core.active_mm = ipipe_get_active_mm();
#ifdef CONFIG_XENO_HW_WANT_TIP
	rootcb->core.tip = task_thread_info(p);
#endif
	xnarch_leave_root(root);
}

void __xnsched_run_handler(void) /* hw interrupts off. */
{
	trace_mark(xn_nucleus, sched_remote, MARK_NOARGS);
	xnsched_run();
}

int __xnsched_run(struct xnsched *sched)
{
	struct xnthread *prev, *next, *curr;
	int switched, need_resched, shadow;
	spl_t s;

	if (xnarch_escalate())
		return 0;

	trace_mark(xn_nucleus, sched, MARK_NOARGS);

	xnlock_get_irqsave(&nklock, s);

	curr = sched->curr;
	xntrace_pid(xnthread_host_pid(curr), xnthread_current_priority(curr));
reschedule:
	switched = 0;
	need_resched = test_resched(sched);
#if !XENO_DEBUG(NUCLEUS)
	if (!need_resched)
		goto signal_unlock_and_exit;
#endif /* !XENO_DEBUG(NUCLEUS) */
	next = xnsched_pick_next(sched);
	if (next == curr) {
		if (unlikely(xnthread_test_state(next, XNROOT))) {
			if (sched->lflags & XNHTICK)
				xnintr_host_tick(sched);
			if (sched->lflags & XNHDEFER)
				xnclock_program_shot(&nkclock, sched);
		}
		goto signal_unlock_and_exit;
	}

	XENO_BUGON(NUCLEUS, need_resched == 0);

	prev = curr;

	trace_mark(xn_nucleus, sched_switch,
		   "prev %p prev_name %s "
		   "next %p next_name %s",
		   prev, xnthread_name(prev),
		   next, xnthread_name(next));

	if (xnthread_test_state(next, XNROOT))
		xnsched_reset_watchdog(sched);

	sched->curr = next;
	shadow = 1;

	if (xnthread_test_state(prev, XNROOT)) {
		leave_root(prev);
		shadow = 0;
	} else if (xnthread_test_state(next, XNROOT)) {
		if (sched->lflags & XNHTICK)
			xnintr_host_tick(sched);
		if (sched->lflags & XNHDEFER)
			xnclock_program_shot(&nkclock, sched);
		enter_root(next);
	}

	xnstat_exectime_switch(sched, &next->stat.account);
	xnstat_counter_inc(&next->stat.csw);

	switch_context(sched, prev, next);

	/*
	 * Test whether we transitioned from primary mode to secondary
	 * over a shadow thread. This may happen in two cases:
	 *
	 * 1) the shadow thread just relaxed.
	 * 2) the shadow TCB has just been deleted, in which case
	 * we have to reap the mated Linux side as well.
	 *
	 * In both cases, we are running over the epilogue of Linux's
	 * schedule, and should skip our epilogue code.
	 */
	if (shadow && ipipe_root_p)
		goto shadow_epilogue;

	switched = 1;
	sched = xnsched_finish_unlocked_switch(sched);
	/*
	 * Re-read the currently running thread, this is needed
	 * because of relaxed/hardened transitions.
	 */
	curr = sched->curr;
	xnthread_switch_fpu(sched);
	xntrace_pid(xnthread_host_pid(curr), xnthread_current_priority(curr));

signal_unlock_and_exit:

	if (switched &&
	    xnsched_maybe_resched_after_unlocked_switch(sched))
		goto reschedule;

	if (xnthread_lock_count(curr))
		sched->lflags |= XNINLOCK;

	xnlock_put_irqrestore(&nklock, s);

	return switched;

shadow_epilogue:

	__ipipe_complete_domain_migration();
	/*
	 * Shadow on entry and root without shadow extension on exit?
	 * Mmmm... This must be the user-space mate of a deleted
	 * real-time shadow we've just rescheduled in the Linux domain
	 * to have it exit properly.  Reap it now.
	 */
	if (xnshadow_current() == NULL) {
		splnone();
		__ipipe_reenter_root();
		do_exit(0);
	}

	/*
	 * Interrupts must be disabled here (has to be done on entry
	 * of the Linux [__]switch_to function), but it is what
	 * callers expect, specifically the reschedule of an IRQ
	 * handler that hit before we call xnsched_run in
	 * xnthread_suspend() when relaxing a thread.
	 */
	XENO_BUGON(NUCLEUS, !hard_irqs_disabled());

	return 1;
}
EXPORT_SYMBOL_GPL(__xnsched_run);

#ifdef CONFIG_XENO_OPT_VFILE

static struct xnvfile_directory sched_vfroot;

struct vfile_schedlist_priv {
	struct xnthread *curr;
	xnticks_t start_time;
};

struct vfile_schedlist_data {
	int cpu;
	pid_t pid;
	char name[XNOBJECT_NAME_LEN];
	char sched_class[XNOBJECT_NAME_LEN];
	int cprio;
	xnticks_t timeout;
	unsigned long state;
};

static struct xnvfile_snapshot_ops vfile_schedlist_ops;

static struct xnvfile_snapshot schedlist_vfile = {
	.privsz = sizeof(struct vfile_schedlist_priv),
	.datasz = sizeof(struct vfile_schedlist_data),
	.tag = &nkthreadlist_tag,
	.ops = &vfile_schedlist_ops,
};

static int vfile_schedlist_rewind(struct xnvfile_snapshot_iterator *it)
{
	struct vfile_schedlist_priv *priv = xnvfile_iterator_priv(it);

	/* &nkthreadq cannot be empty (root thread(s)). */
	priv->curr = list_first_entry(&nkthreadq, struct xnthread, glink);
	priv->start_time = xnclock_read_monotonic(&nkclock);

	return nknrthreads;
}

static int vfile_schedlist_next(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedlist_priv *priv = xnvfile_iterator_priv(it);
	struct vfile_schedlist_data *p = data;
	xnticks_t timeout, period;
	struct xnthread *thread;

	if (priv->curr == NULL)
		return 0;	/* All done. */

	thread = priv->curr;
	if (list_is_last(&thread->glink, &nkthreadq))
		priv->curr = NULL;
	else
		priv->curr = list_next_entry(thread, glink);

	p->cpu = xnsched_cpu(thread->sched);
	p->pid = xnthread_host_pid(thread);
	memcpy(p->name, thread->name, sizeof(p->name));
	p->cprio = thread->cprio;
	p->state = xnthread_state_flags(thread);
	xnobject_copy_name(p->sched_class, thread->sched_class->name);
	period = xnthread_get_period(thread);
	timeout = xnthread_get_timeout(thread, priv->start_time);
	/*
	 * Here we cheat: thread is periodic and the sampling rate may
	 * be high, so it is indeed possible that the next tick date
	 * from the ptimer progresses fast enough while we are busy
	 * collecting output data in this loop, so that next_date -
	 * start_time > period. In such a case, we simply ceil the
	 * value to period to keep the result meaningful, even if not
	 * necessarily accurate. But what does accuracy mean when the
	 * sampling frequency is high, and the way to read it has to
	 * go through the vfile interface anyway?
	 */
	if (period > 0 && period < timeout &&
	    !xntimer_running_p(&thread->rtimer))
		timeout = period;

	p->timeout = timeout;

	return 1;
}

static int vfile_schedlist_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedlist_data *p = data;
	char sbuf[64], pbuf[16], tbuf[16];

	if (p == NULL)
		xnvfile_printf(it,
			       "%-3s  %-6s %-5s  %-8s %-8s  %-10s %s\n",
			       "CPU", "PID", "CLASS", "PRI", "TIMEOUT",
			       "STAT", "NAME");
	else {
		snprintf(pbuf, sizeof(pbuf), "%3d", p->cprio);
		xntimer_format_time(p->timeout, tbuf, sizeof(tbuf));
		xnthread_format_status(p->state, sbuf, sizeof(sbuf));

		xnvfile_printf(it,
			       "%3u  %-6d %-5s  %-8s %-8s  %-10s %s\n",
			       p->cpu,
			       p->pid,
			       p->sched_class,
			       pbuf,
			       tbuf,
			       sbuf,
			       p->name);
	}

	return 0;
}

static struct xnvfile_snapshot_ops vfile_schedlist_ops = {
	.rewind = vfile_schedlist_rewind,
	.next = vfile_schedlist_next,
	.show = vfile_schedlist_show,
};

#ifdef CONFIG_XENO_OPT_STATS

static spl_t vfile_schedstat_lock_s;

static int vfile_schedstat_get_lock(struct xnvfile *vfile)
{
	int ret;

	ret = xnintr_get_query_lock();
	if (ret < 0)
		return ret;
	xnlock_get_irqsave(&nklock, vfile_schedstat_lock_s);
	return 0;
}

static void vfile_schedstat_put_lock(struct xnvfile *vfile)
{
	xnlock_put_irqrestore(&nklock, vfile_schedstat_lock_s);
	xnintr_put_query_lock();
}

static struct xnvfile_lock_ops vfile_schedstat_lockops = {
	.get = vfile_schedstat_get_lock,
	.put = vfile_schedstat_put_lock,
};

struct vfile_schedstat_priv {
	int irq;
	struct xnthread *curr;
	struct xnintr_iterator intr_it;
};

struct vfile_schedstat_data {
	int cpu;
	pid_t pid;
	unsigned long state;
	char name[XNOBJECT_NAME_LEN];
	unsigned long ssw;
	unsigned long csw;
	unsigned long xsc;
	unsigned long pf;
	xnticks_t exectime_period;
	xnticks_t account_period;
	xnticks_t exectime_total;
	struct xnsched_class *sched_class;
	xnticks_t period;
	int cprio;
};

static struct xnvfile_snapshot_ops vfile_schedstat_ops;

static struct xnvfile_snapshot schedstat_vfile = {
	.privsz = sizeof(struct vfile_schedstat_priv),
	.datasz = sizeof(struct vfile_schedstat_data),
	.tag = &nkthreadlist_tag,
	.ops = &vfile_schedstat_ops,
	.entry = { .lockops = &vfile_schedstat_lockops },
};

static int vfile_schedstat_rewind(struct xnvfile_snapshot_iterator *it)
{
	struct vfile_schedstat_priv *priv = xnvfile_iterator_priv(it);
	int irqnr;

	/*
	 * The activity numbers on each valid interrupt descriptor are
	 * grouped under a pseudo-thread.
	 */
	priv->curr = list_first_entry(&nkthreadq, struct xnthread, glink);
	priv->irq = 0;
	irqnr = xnintr_query_init(&priv->intr_it) * NR_CPUS;

	return irqnr + nknrthreads;
}

static int vfile_schedstat_next(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_priv *priv = xnvfile_iterator_priv(it);
	struct vfile_schedstat_data *p = data;
	struct xnthread *thread;
	struct xnsched *sched;
	xnticks_t period;
	int ret;

	if (priv->curr == NULL)
		/*
		 * We are done with actual threads, scan interrupt
		 * descriptors.
		 */
		goto scan_irqs;

	thread = priv->curr;
	if (list_is_last(&thread->glink, &nkthreadq))
		priv->curr = NULL;
	else
		priv->curr = list_next_entry(thread, glink);

	sched = thread->sched;
	p->cpu = xnsched_cpu(sched);
	p->pid = xnthread_host_pid(thread);
	memcpy(p->name, thread->name, sizeof(p->name));
	p->state = xnthread_state_flags(thread);
	p->ssw = xnstat_counter_get(&thread->stat.ssw);
	p->csw = xnstat_counter_get(&thread->stat.csw);
	p->xsc = xnstat_counter_get(&thread->stat.xsc);
	p->pf = xnstat_counter_get(&thread->stat.pf);
	p->sched_class = thread->sched_class;
	p->cprio = thread->cprio;
	p->period = xnthread_get_period(thread);

	period = sched->last_account_switch - thread->stat.lastperiod.start;
	if (period == 0 && thread == sched->curr) {
		p->exectime_period = 1;
		p->account_period = 1;
	} else {
		p->exectime_period = thread->stat.account.total -
			thread->stat.lastperiod.total;
		p->account_period = period;
	}
	p->exectime_total = thread->stat.account.total;
	thread->stat.lastperiod.total = thread->stat.account.total;
	thread->stat.lastperiod.start = sched->last_account_switch;

	return 1;

scan_irqs:
	if (priv->irq >= IPIPE_NR_IRQS)
		return 0;	/* All done. */

	ret = xnintr_query_next(priv->irq, &priv->intr_it, p->name);
	if (ret) {
		if (ret == -EAGAIN)
			xnvfile_touch(it->vfile); /* force rewind. */
		priv->irq++;
		return VFILE_SEQ_SKIP;
	}

	if (!xnsched_supported_cpu(priv->intr_it.cpu))
		return VFILE_SEQ_SKIP;

	p->cpu = priv->intr_it.cpu;
	p->csw = priv->intr_it.hits;
	p->exectime_period = priv->intr_it.exectime_period;
	p->account_period = priv->intr_it.account_period;
	p->exectime_total = priv->intr_it.exectime_total;
	p->pid = 0;
	p->state =  0;
	p->ssw = 0;
	p->xsc = 0;
	p->pf = 0;
	p->sched_class = &xnsched_class_idle;
	p->cprio = 0;
	p->period = 0;

	return 1;
}

static int vfile_schedstat_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_data *p = data;
	int usage = 0;

	if (p == NULL)
		xnvfile_printf(it,
			       "%-3s  %-6s %-10s %-10s %-10s %-4s  %-8s  %5s"
			       "  %s\n",
			       "CPU", "PID", "MSW", "CSW", "XSC", "PF", "STAT", "%CPU",
			       "NAME");
	else {
		if (p->account_period) {
			while (p->account_period > 0xffffffffUL) {
				p->exectime_period >>= 16;
				p->account_period >>= 16;
			}
			usage = xnarch_ulldiv(p->exectime_period * 1000LL +
					      (p->account_period >> 1),
					      p->account_period, NULL);
		}
		xnvfile_printf(it,
			       "%3u  %-6d %-10lu %-10lu %-10lu %-4lu  %.8lx  %3u.%u"
			       "  %s\n",
			       p->cpu, p->pid, p->ssw, p->csw, p->xsc, p->pf, p->state,
			       usage / 10, usage % 10, p->name);
	}

	return 0;
}

static int vfile_schedacct_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_data *p = data;

	if (p == NULL)
		return 0;

	xnvfile_printf(it, "%u %d %lu %lu %lu %lu %.8lx %Lu %Lu %Lu %s %s %d %Lu\n",
		       p->cpu, p->pid, p->ssw, p->csw, p->xsc, p->pf, p->state,
		       xnclock_ticks_to_ns(&nkclock, p->account_period),
		       xnclock_ticks_to_ns(&nkclock, p->exectime_period),
		       xnclock_ticks_to_ns(&nkclock, p->exectime_total),
		       p->name,
		       p->sched_class->name,
		       p->cprio,
		       p->period);

	return 0;
}

static struct xnvfile_snapshot_ops vfile_schedstat_ops = {
	.rewind = vfile_schedstat_rewind,
	.next = vfile_schedstat_next,
	.show = vfile_schedstat_show,
};

/*
 * An accounting vfile is a thread statistics vfile in disguise with a
 * different output format, which is parser-friendly.
 */
static struct xnvfile_snapshot_ops vfile_schedacct_ops;

static struct xnvfile_snapshot schedacct_vfile = {
	.privsz = sizeof(struct vfile_schedstat_priv),
	.datasz = sizeof(struct vfile_schedstat_data),
	.tag = &nkthreadlist_tag,
	.ops = &vfile_schedacct_ops,
};

static struct xnvfile_snapshot_ops vfile_schedacct_ops = {
	.rewind = vfile_schedstat_rewind,
	.next = vfile_schedstat_next,
	.show = vfile_schedacct_show,
};

#endif /* CONFIG_XENO_OPT_STATS */

#ifdef CONFIG_SMP

static int affinity_vfile_show(struct xnvfile_regular_iterator *it,
			       void *data)
{
	unsigned long val = 0;
	int cpu;

	for (cpu = 0; cpu < BITS_PER_LONG; cpu++)
		if (cpu_isset(cpu, nkaffinity))
			val |= (1UL << cpu);

	xnvfile_printf(it, "%08lx\n", val);

	return 0;
}

static ssize_t affinity_vfile_store(struct xnvfile_input *input)
{
	cpumask_t affinity, set;
	ssize_t ret;
	long val;
	int cpu;
	spl_t s;

	ret = xnvfile_get_integer(input, &val);
	if (ret < 0)
		return ret;

	if (val == 0)
		affinity = xnsched_realtime_cpus; /* Reset to default. */
	else {
		cpus_clear(affinity);
		for (cpu = 0; cpu < BITS_PER_LONG; cpu++, val >>= 1) {
			if (val & 1)
				cpu_set(cpu, affinity);
		}
	}

	cpus_and(set, affinity, *cpu_online_mask);
	if (cpus_empty(set))
		return -EINVAL;

	/*
	 * The new dynamic affinity must be a strict subset of the
	 * static set of supported CPUs.
	 */
	cpus_or(set, affinity, xnsched_realtime_cpus);
	if (!cpus_equal(set, xnsched_realtime_cpus))
		return -EINVAL;

	xnlock_get_irqsave(&nklock, s);
	nkaffinity = affinity;
	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

static struct xnvfile_regular_ops affinity_vfile_ops = {
	.show = affinity_vfile_show,
	.store = affinity_vfile_store,
};

static struct xnvfile_regular affinity_vfile = {
	.ops = &affinity_vfile_ops,
};

#endif /* CONFIG_SMP */

int xnsched_init_proc(void)
{
	struct xnsched_class *p;
	int ret;

	ret = xnvfile_init_dir("sched", &sched_vfroot, &nkvfroot);
	if (ret)
		return ret;

	ret = xnvfile_init_snapshot("threads", &schedlist_vfile, &sched_vfroot);
	if (ret)
		return ret;

	for_each_xnsched_class(p) {
		if (p->sched_init_vfile) {
			ret = p->sched_init_vfile(p, &sched_vfroot);
			if (ret)
				return ret;
		}
	}

#ifdef CONFIG_XENO_OPT_STATS
	ret = xnvfile_init_snapshot("stat", &schedstat_vfile, &sched_vfroot);
	if (ret)
		return ret;
	ret = xnvfile_init_snapshot("acct", &schedacct_vfile, &sched_vfroot);
	if (ret)
		return ret;
#endif /* CONFIG_XENO_OPT_STATS */

#ifdef CONFIG_SMP
	xnvfile_init_regular("affinity", &affinity_vfile, &nkvfroot);
#endif /* CONFIG_SMP */

	return 0;
}

void xnsched_cleanup_proc(void)
{
	struct xnsched_class *p;

	for_each_xnsched_class(p) {
		if (p->sched_cleanup_vfile)
			p->sched_cleanup_vfile(p);
	}

#ifdef CONFIG_SMP
	xnvfile_destroy_regular(&affinity_vfile);
#endif /* CONFIG_SMP */
#ifdef CONFIG_XENO_OPT_STATS
	xnvfile_destroy_snapshot(&schedacct_vfile);
	xnvfile_destroy_snapshot(&schedstat_vfile);
#endif /* CONFIG_XENO_OPT_STATS */
	xnvfile_destroy_snapshot(&schedlist_vfile);
	xnvfile_destroy_dir(&sched_vfroot);
}

#endif /* CONFIG_XENO_OPT_VFILE */