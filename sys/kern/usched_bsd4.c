/*
 * Copyright (c) 1999 Peter Wemm <peter@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/sys/kern/usched_bsd4.c,v 1.12 2006/06/01 19:02:38 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/rtprio.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/resourcevar.h>
#include <sys/spinlock.h>
#include <machine/ipl.h>
#include <machine/cpu.h>
#include <machine/smp.h>

#include <sys/thread2.h>
#include <sys/spinlock2.h>

/*
 * Priorities.  Note that with 32 run queues per scheduler each queue
 * represents four priority levels.
 */

#define MAXPRI			128
#define PRIMASK			(MAXPRI - 1)
#define PRIBASE_REALTIME	0
#define PRIBASE_NORMAL		MAXPRI
#define PRIBASE_IDLE		(MAXPRI * 2)
#define PRIBASE_THREAD		(MAXPRI * 3)
#define PRIBASE_NULL		(MAXPRI * 4)

#define NQS	32			/* 32 run queues. */
#define PPQ	(MAXPRI / NQS)		/* priorities per queue */
#define PPQMASK	(PPQ - 1)

/*
 * NICEPPQ	- number of nice units per priority queue
 * ESTCPURAMP	- number of scheduler ticks for estcpu to switch queues
 *
 * ESTCPUPPQ	- number of estcpu units per priority queue
 * ESTCPUMAX	- number of estcpu units
 * ESTCPUINCR	- amount we have to increment p_estcpu per scheduling tick at
 *		  100% cpu.
 */
#define NICEPPQ		2
#define ESTCPURAMP	4
#define ESTCPUPPQ	512
#define ESTCPUMAX	(ESTCPUPPQ * NQS)
#define ESTCPUINCR	(ESTCPUPPQ / ESTCPURAMP)
#define PRIO_RANGE	(PRIO_MAX - PRIO_MIN + 1)

#define ESTCPULIM(v)	min((v), ESTCPUMAX)

TAILQ_HEAD(rq, lwp);

#define lwp_priority	lwp_usdata.bsd4.priority
#define lwp_rqindex	lwp_usdata.bsd4.rqindex
#define lwp_origcpu	lwp_usdata.bsd4.origcpu
#define lwp_estcpu	lwp_usdata.bsd4.estcpu
#define lwp_rqtype	lwp_usdata.bsd4.rqtype

static void bsd4_acquire_curproc(struct lwp *lp);
static void bsd4_release_curproc(struct lwp *lp);
static void bsd4_select_curproc(globaldata_t gd);
static void bsd4_setrunqueue(struct lwp *lp);
static void bsd4_schedulerclock(struct lwp *lp, sysclock_t period,
				sysclock_t cpstamp);
static void bsd4_recalculate_estcpu(struct lwp *lp);
static void bsd4_resetpriority(struct lwp *lp);
static void bsd4_forking(struct lwp *plp, struct lwp *lp);
static void bsd4_exiting(struct lwp *plp, struct lwp *lp);

#ifdef SMP
static void need_user_resched_remote(void *dummy);
#endif
static struct lwp *chooseproc_locked(struct lwp *chklp);
static void bsd4_remrunqueue_locked(struct lwp *lp);
static void bsd4_setrunqueue_locked(struct lwp *lp);

struct usched usched_bsd4 = {
	{ NULL },
	"bsd4", "Original DragonFly Scheduler",
	NULL,			/* default registration */
	NULL,			/* default deregistration */
	bsd4_acquire_curproc,
	bsd4_release_curproc,
	bsd4_select_curproc,
	bsd4_setrunqueue,
	bsd4_schedulerclock,
	bsd4_recalculate_estcpu,
	bsd4_resetpriority,
	bsd4_forking,
	bsd4_exiting,
	NULL			/* setcpumask not supported */
};

struct usched_bsd4_pcpu {
	struct thread helper_thread;
	short	rrcount;
	short	upri;
	struct lwp *uschedcp;
};

typedef struct usched_bsd4_pcpu	*bsd4_pcpu_t;

/*
 * We have NQS (32) run queues per scheduling class.  For the normal
 * class, there are 128 priorities scaled onto these 32 queues.  New
 * processes are added to the last entry in each queue, and processes
 * are selected for running by taking them from the head and maintaining
 * a simple FIFO arrangement.  Realtime and Idle priority processes have
 * and explicit 0-31 priority which maps directly onto their class queue
 * index.  When a queue has something in it, the corresponding bit is
 * set in the queuebits variable, allowing a single read to determine
 * the state of all 32 queues and then a ffs() to find the first busy
 * queue.
 */
static struct rq bsd4_queues[NQS];
static struct rq bsd4_rtqueues[NQS];
static struct rq bsd4_idqueues[NQS];
static u_int32_t bsd4_queuebits;
static u_int32_t bsd4_rtqueuebits;
static u_int32_t bsd4_idqueuebits;
static cpumask_t bsd4_curprocmask = -1;	/* currently running a user process */
static cpumask_t bsd4_rdyprocmask;	/* ready to accept a user process */
static int	 bsd4_runqcount;
#ifdef SMP
static volatile int bsd4_scancpu;
#endif
static struct spinlock bsd4_spin;
static struct usched_bsd4_pcpu bsd4_pcpu[MAXCPU];

SYSCTL_INT(_debug, OID_AUTO, bsd4_runqcount, CTLFLAG_RD, &bsd4_runqcount, 0, "");
#ifdef INVARIANTS
static int usched_nonoptimal;
SYSCTL_INT(_debug, OID_AUTO, usched_nonoptimal, CTLFLAG_RW,
        &usched_nonoptimal, 0, "acquire_curproc() was not optimal");
static int usched_optimal;
SYSCTL_INT(_debug, OID_AUTO, usched_optimal, CTLFLAG_RW,
        &usched_optimal, 0, "acquire_curproc() was optimal");
#endif
static int usched_debug = -1;
SYSCTL_INT(_debug, OID_AUTO, scdebug, CTLFLAG_RW, &usched_debug, 0, "");
#ifdef SMP
static int remote_resched_nonaffinity;
static int remote_resched_affinity;
static int choose_affinity;
SYSCTL_INT(_debug, OID_AUTO, remote_resched_nonaffinity, CTLFLAG_RD,
        &remote_resched_nonaffinity, 0, "Number of remote rescheds");
SYSCTL_INT(_debug, OID_AUTO, remote_resched_affinity, CTLFLAG_RD,
        &remote_resched_affinity, 0, "Number of remote rescheds");
SYSCTL_INT(_debug, OID_AUTO, choose_affinity, CTLFLAG_RD,
        &choose_affinity, 0, "chooseproc() was smart");
#endif

static int usched_bsd4_rrinterval = (ESTCPUFREQ + 9) / 10;
SYSCTL_INT(_kern, OID_AUTO, usched_bsd4_rrinterval, CTLFLAG_RW,
        &usched_bsd4_rrinterval, 0, "");
static int usched_bsd4_decay = ESTCPUINCR / 2;
SYSCTL_INT(_kern, OID_AUTO, usched_bsd4_decay, CTLFLAG_RW,
        &usched_bsd4_decay, 0, "");

/*
 * Initialize the run queues at boot time.
 */
static void
rqinit(void *dummy)
{
	int i;

	spin_init(&bsd4_spin);
	for (i = 0; i < NQS; i++) {
		TAILQ_INIT(&bsd4_queues[i]);
		TAILQ_INIT(&bsd4_rtqueues[i]);
		TAILQ_INIT(&bsd4_idqueues[i]);
	}
	atomic_clear_int(&bsd4_curprocmask, 1);
}
SYSINIT(runqueue, SI_SUB_RUN_QUEUE, SI_ORDER_FIRST, rqinit, NULL)

/*
 * BSD4_ACQUIRE_CURPROC
 *
 * This function is called when the kernel intends to return to userland.
 * It is responsible for making the thread the current designated userland
 * thread for this cpu, blocking if necessary.
 *
 * We are expected to handle userland reschedule requests here too.
 *
 * WARNING! THIS FUNCTION IS ALLOWED TO CAUSE THE CURRENT THREAD TO MIGRATE
 * TO ANOTHER CPU!  Because most of the kernel assumes that no migration will
 * occur, this function is called only under very controlled circumstances.
 *
 * Basically we recalculate our estcpu to hopefully give us a more
 * favorable disposition, setrunqueue, then wait for the curlwp
 * designation to be handed to us (if the setrunqueue didn't do it).
 *
 * MPSAFE
 */
static void
bsd4_acquire_curproc(struct lwp *lp)
{
	globaldata_t gd = mycpu;
	bsd4_pcpu_t dd = &bsd4_pcpu[gd->gd_cpuid];

	/*
	 * Possibly select another thread, or keep the current thread.
	 */
	if (user_resched_wanted())
		bsd4_select_curproc(gd);

	/*
	 * If uschedcp is still pointing to us, we're done
	 */
	if (dd->uschedcp == lp)
		return;

	/*
	 * If this cpu has no current thread, and the run queue is
	 * empty, we can safely select ourself.
	 */
	if (dd->uschedcp == NULL && bsd4_runqcount == 0) {
		atomic_set_int(&bsd4_curprocmask, gd->gd_cpumask);
		dd->uschedcp = lp;
		dd->upri = lp->lwp_priority;
		return;
	}

	/*
	 * Adjust estcpu and recalculate our priority, then put us back on
	 * the user process scheduler's runq.  Only increment the involuntary
	 * context switch count if the setrunqueue call did not immediately
	 * schedule us.
	 *
	 * Loop until we become the currently scheduled process.  Note that
	 * calling setrunqueue can cause us to be migrated to another cpu
	 * after we switch away.
	 */
	do {
		crit_enter();
		bsd4_recalculate_estcpu(lp);
		lwkt_deschedule_self(gd->gd_curthread);
		bsd4_setrunqueue(lp);
		if ((gd->gd_curthread->td_flags & TDF_RUNQ) == 0)
			++lp->lwp_stats->p_ru.ru_nivcsw;
		lwkt_switch();
		crit_exit();
		gd = mycpu;
		dd = &bsd4_pcpu[gd->gd_cpuid];
	} while (dd->uschedcp != lp);
	KKASSERT((lp->lwp_proc->p_flag & P_ONRUNQ) == 0);
}

/*
 * BSD4_RELEASE_CURPROC
 *
 * This routine detaches the current thread from the userland scheduler,
 * usually because the thread needs to run in the kernel (at kernel priority)
 * for a while.
 *
 * This routine is also responsible for selecting a new thread to
 * make the current thread.
 *
 * NOTE: This implementation differs from the dummy example in that
 * bsd4_select_curproc() is able to select the current process, whereas
 * dummy_select_curproc() is not able to select the current process.
 * This means we have to NULL out uschedcp.
 *
 * Additionally, note that we may already be on a run queue if releasing
 * via the lwkt_switch() in bsd4_setrunqueue().
 *
 * WARNING!  The MP lock may be in an unsynchronized state due to the
 * way get_mplock() works and the fact that this function may be called
 * from a passive release during a lwkt_switch().   try_mplock() will deal 
 * with this for us but you should be aware that td_mpcount may not be
 * useable.
 *
 * MPSAFE
 */
static void
bsd4_release_curproc(struct lwp *lp)
{
	globaldata_t gd = mycpu;
	bsd4_pcpu_t dd = &bsd4_pcpu[gd->gd_cpuid];

	if (dd->uschedcp == lp) {
		/*
		 * Note: we leave ou curprocmask bit set to prevent
		 * unnecessary scheduler helper wakeups.  
		 * bsd4_select_curproc() will clean it up.
		 */
		KKASSERT((lp->lwp_proc->p_flag & P_ONRUNQ) == 0);
		dd->uschedcp = NULL;	/* don't let lp be selected */
		bsd4_select_curproc(gd);
	}
}

/*
 * BSD4_SELECT_CURPROC
 *
 * Select a new current process for this cpu.  This satisfies a user
 * scheduler reschedule request so clear that too.
 *
 * This routine is also responsible for equal-priority round-robining,
 * typically triggered from bsd4_schedulerclock().  In our dummy example
 * all the 'user' threads are LWKT scheduled all at once and we just
 * call lwkt_switch().
 *
 * MPSAFE
 */
static
void
bsd4_select_curproc(globaldata_t gd)
{
	bsd4_pcpu_t dd = &bsd4_pcpu[gd->gd_cpuid];
	struct lwp *nlp;
	int cpuid = gd->gd_cpuid;

	crit_enter_gd(gd);
	clear_user_resched();	/* This satisfied the reschedule request */
	dd->rrcount = 0;	/* Reset the round-robin counter */

	spin_lock_wr(&bsd4_spin);
	if ((nlp = chooseproc_locked(dd->uschedcp)) != NULL) {
		atomic_set_int(&bsd4_curprocmask, 1 << cpuid);
		dd->upri = nlp->lwp_priority;
		dd->uschedcp = nlp;
		spin_unlock_wr(&bsd4_spin);
#ifdef SMP
		lwkt_acquire(nlp->lwp_thread);
#endif
		lwkt_schedule(nlp->lwp_thread);
	} else if (dd->uschedcp) {
		dd->upri = dd->uschedcp->lwp_priority;
		spin_unlock_wr(&bsd4_spin);
		KKASSERT(bsd4_curprocmask & (1 << cpuid));
	} else if (bsd4_runqcount && (bsd4_rdyprocmask & (1 << cpuid))) {
		atomic_clear_int(&bsd4_curprocmask, 1 << cpuid);
		atomic_clear_int(&bsd4_rdyprocmask, 1 << cpuid);
		dd->uschedcp = NULL;
		dd->upri = PRIBASE_NULL;
		spin_unlock_wr(&bsd4_spin);
		lwkt_schedule(&dd->helper_thread);
	} else {
		dd->uschedcp = NULL;
		dd->upri = PRIBASE_NULL;
		atomic_clear_int(&bsd4_curprocmask, 1 << cpuid);
		spin_unlock_wr(&bsd4_spin);
	}
	crit_exit_gd(gd);
}

/*
 * BSD4_SETRUNQUEUE
 *
 * This routine is called to schedule a new user process after a fork.
 *
 * The caller may set P_PASSIVE_ACQ in p_flag to indicate that we should
 * attempt to leave the thread on the current cpu.
 *
 * If P_PASSIVE_ACQ is set setrunqueue() will not wakeup potential target
 * cpus in an attempt to keep the process on the current cpu at least for
 * a little while to take advantage of locality of reference (e.g. fork/exec
 * or short fork/exit, and uio_yield()).
 *
 * CPU AFFINITY: cpu affinity is handled by attempting to either schedule
 * or (user level) preempt on the same cpu that a process was previously
 * scheduled to.  If we cannot do this but we are at enough of a higher
 * priority then the processes running on other cpus, we will allow the
 * process to be stolen by another cpu.
 *
 * WARNING!  This routine cannot block.  bsd4_acquire_curproc() does 
 * a deschedule/switch interlock and we can be moved to another cpu
 * the moment we are switched out.  Our LWKT run state is the only
 * thing preventing the transfer.
 *
 * The associated thread must NOT currently be scheduled (but can be the
 * current process after it has been LWKT descheduled).  It must NOT be on
 * a bsd4 scheduler queue either.  The purpose of this routine is to put
 * it on a scheduler queue or make it the current user process and LWKT
 * schedule it.  It is possible that the thread is in the middle of a LWKT
 * switchout on another cpu, lwkt_acquire() deals with that case.
 *
 * The process must be runnable.
 *
 * MPSAFE
 */
static void
bsd4_setrunqueue(struct lwp *lp)
{
	globaldata_t gd;
	bsd4_pcpu_t dd;
	int cpuid;
#ifdef SMP
	cpumask_t mask;
	cpumask_t tmpmask;
#endif

	/*
	 * First validate the process state relative to the current cpu.
	 * We don't need the spinlock for this, just a critical section.
	 * We are in control of the process.
	 */
	crit_enter();
	KASSERT(lp->lwp_proc->p_stat == SRUN, ("setrunqueue: proc not SRUN"));
	KASSERT((lp->lwp_proc->p_flag & P_ONRUNQ) == 0,
	    ("lwp %d/%d already on runq! flag %08x", lp->lwp_proc->p_pid,
	     lp->lwp_tid, lp->lwp_proc->p_flag));
	KKASSERT((lp->lwp_thread->td_flags & TDF_RUNQ) == 0);

	/*
	 * Note: gd and dd are relative to the target thread's last cpu,
	 * NOT our current cpu.
	 */
	gd = lp->lwp_thread->td_gd;
	dd = &bsd4_pcpu[gd->gd_cpuid];

	/*
	 * If setrunqueue is being called due to being woken up, verses
	 * being called when aquiring the current process, recalculate
	 * estcpu.
	 *
	 * Because recalculate is only called once or twice for long sleeps,
	 * not every second forever while the process is sleeping, we have 
	 * to manually call it to resynchronize p_cpbase on wakeup or it
	 * will wrap if the process was sleeping long enough (e.g. ~10 min
	 * with the ACPI timer) and really mess up the nticks calculation.
	 *
	 * NOTE: because P_ONRUNQ is not set, bsd4_recalculate_estcpu()'s
	 * calls to resetpriority will just play with the processes priority
	 * fields and not mess with any queues, so it is MPSAFE in this
	 * context.
	 */
	if (lp->lwp_slptime && (lp->lwp_thread->td_flags & TDF_RUNNING) == 0) {
	    bsd4_recalculate_estcpu(lp);
	    lp->lwp_slptime = 0;
	}

	/*
	 * This process is not supposed to be scheduled anywhere or assigned
	 * as the current process anywhere.  Assert the condition.
	 */
	KKASSERT(dd->uschedcp != lp);

	/*
	 * Check local cpu affinity.  The associated thread is stable at 
	 * the moment.  Note that we may be checking another cpu here so we
	 * have to be careful.  We can only assign uschedcp on OUR cpu.
	 *
	 * This allows us to avoid actually queueing the process.  
	 * acquire_curproc() will handle any threads we mistakenly schedule.
	 */
	cpuid = gd->gd_cpuid;
	if (gd == mycpu && (bsd4_curprocmask & (1 << cpuid)) == 0) {
		atomic_set_int(&bsd4_curprocmask, 1 << cpuid);
		dd->uschedcp = lp;
		dd->upri = lp->lwp_priority;
		lwkt_schedule(lp->lwp_thread);
		crit_exit();
		return;
	}

	/*
	 * gd and cpuid may still 'hint' at another cpu.  Even so we have
	 * to place this process on the userland scheduler's run queue for
	 * action by the target cpu.
	 */
#ifdef SMP
	/*
	 * XXX fixme.  Could be part of a remrunqueue/setrunqueue
	 * operation when the priority is recalculated, so TDF_MIGRATING
	 * may already be set.
	 */
	if ((lp->lwp_thread->td_flags & TDF_MIGRATING) == 0)
		lwkt_giveaway(lp->lwp_thread);
#endif

	/*
	 * We lose control of lp the moment we release the spinlock after
	 * having placed lp on the queue.  i.e. another cpu could pick it
	 * up and it could exit, or its priority could be further adjusted,
	 * or something like that.
	 */
	spin_lock_wr(&bsd4_spin);
	bsd4_setrunqueue_locked(lp);

	/*
	 * gd, dd, and cpuid are still our target cpu 'hint', not our current
	 * cpu info.
	 *
	 * We always try to schedule a LWP to its original cpu first.  It
	 * is possible for the scheduler helper or setrunqueue to assign
	 * the LWP to a different cpu before the one we asked for wakes
	 * up.
	 *
	 * If the LWP has higher priority (lower lwp_priority value) on
	 * its target cpu, reschedule on that cpu.
	 */
	if ((lp->lwp_thread->td_flags & TDF_NORESCHED) == 0) {
		if ((dd->upri & ~PRIMASK) > (lp->lwp_priority & ~PRIMASK)) {
			dd->upri = lp->lwp_priority;
			spin_unlock_wr(&bsd4_spin);
#ifdef SMP
			if (gd == mycpu) {
				need_user_resched();
			} else {
				lwkt_send_ipiq(gd, need_user_resched_remote,
					       NULL);
			}
#else
			need_user_resched();
#endif
			crit_exit();
			return;
		}
	}
	spin_unlock_wr(&bsd4_spin);

#ifdef SMP
	/*
	 * Otherwise the LWP has a lower priority or we were asked not
	 * to reschedule.  Look for an idle cpu whos scheduler helper
	 * is ready to accept more work.
	 *
	 * Look for an idle cpu starting at our rotator (bsd4_scancpu).
	 *
	 * If no cpus are ready to accept work, just return.
	 *
	 * XXX P_PASSIVE_ACQ
	 */
	mask = ~bsd4_curprocmask & bsd4_rdyprocmask & mycpu->gd_other_cpus;
	if (mask) {
		cpuid = bsd4_scancpu;
		if (++cpuid == ncpus)
			cpuid = 0;
		tmpmask = ~((1 << cpuid) - 1);
		if (mask & tmpmask)
			cpuid = bsfl(mask & tmpmask);
		else
			cpuid = bsfl(mask);
		atomic_clear_int(&bsd4_rdyprocmask, 1 << cpuid);
		bsd4_scancpu = cpuid;
		lwkt_schedule(&bsd4_pcpu[cpuid].helper_thread);
	}
#endif
	crit_exit();
}

/*
 * This routine is called from a systimer IPI.  It MUST be MP-safe and
 * the BGL IS NOT HELD ON ENTRY.  This routine is called at ESTCPUFREQ on
 * each cpu.
 *
 * Because this is effectively a 'fast' interrupt, we cannot safely
 * use spinlocks unless gd_spinlock_rd is NULL and gd_spinlocks_wr is 0,
 * even if the spinlocks are 'non conflicting'.  This is due to the way
 * spinlock conflicts against cached read locks are handled.
 *
 * MPSAFE
 */
static
void
bsd4_schedulerclock(struct lwp *lp, sysclock_t period, sysclock_t cpstamp)
{
	globaldata_t gd = mycpu;
	bsd4_pcpu_t dd = &bsd4_pcpu[gd->gd_cpuid];

	/*
	 * Do we need to round-robin?  We round-robin 10 times a second.
	 * This should only occur for cpu-bound batch processes.
	 */
	if (++dd->rrcount >= usched_bsd4_rrinterval) {
		dd->rrcount = 0;
		need_user_resched();
	}

	/*
	 * As the process accumulates cpu time p_estcpu is bumped and may
	 * push the process into another scheduling queue.  It typically
	 * takes 4 ticks to bump the queue.
	 */
	lp->lwp_estcpu = ESTCPULIM(lp->lwp_estcpu + ESTCPUINCR);

	/*
	 * Reducing p_origcpu over time causes more of our estcpu to be
	 * returned to the parent when we exit.  This is a small tweak
	 * for the batch detection heuristic.
	 */
	if (lp->lwp_origcpu)
		--lp->lwp_origcpu;

	/*
	 * We can only safely call bsd4_resetpriority(), which uses spinlocks,
	 * if we aren't interrupting a thread that is using spinlocks.
	 * Otherwise we can deadlock with another cpu waiting for our read
	 * spinlocks to clear.
	 */
	if (gd->gd_spinlock_rd == NULL && gd->gd_spinlocks_wr == 0)
		bsd4_resetpriority(lp);
	else
		need_user_resched();
}

/*
 * Called from acquire and from kern_synch's one-second timer (one of the
 * callout helper threads) with a critical section held. 
 *
 * Decay p_estcpu based on the number of ticks we haven't been running
 * and our p_nice.  As the load increases each process observes a larger
 * number of idle ticks (because other processes are running in them).
 * This observation leads to a larger correction which tends to make the
 * system more 'batchy'.
 *
 * Note that no recalculation occurs for a process which sleeps and wakes
 * up in the same tick.  That is, a system doing thousands of context
 * switches per second will still only do serious estcpu calculations
 * ESTCPUFREQ times per second.
 *
 * MPSAFE
 */
static
void 
bsd4_recalculate_estcpu(struct lwp *lp)
{
	globaldata_t gd = mycpu;
	sysclock_t cpbase;
	int loadfac;
	int ndecay;
	int nticks;
	int nleft;

	/*
	 * We have to subtract periodic to get the last schedclock
	 * timeout time, otherwise we would get the upcoming timeout.
	 * Keep in mind that a process can migrate between cpus and
	 * while the scheduler clock should be very close, boundary
	 * conditions could lead to a small negative delta.
	 */
	cpbase = gd->gd_schedclock.time - gd->gd_schedclock.periodic;

	if (lp->lwp_slptime > 1) {
		/*
		 * Too much time has passed, do a coarse correction.
		 */
		lp->lwp_estcpu = lp->lwp_estcpu >> 1;
		bsd4_resetpriority(lp);
		lp->lwp_cpbase = cpbase;
		lp->lwp_cpticks = 0;
	} else if (lp->lwp_cpbase != cpbase) {
		/*
		 * Adjust estcpu if we are in a different tick.  Don't waste
		 * time if we are in the same tick. 
		 * 
		 * First calculate the number of ticks in the measurement
		 * interval.  The nticks calculation can wind up 0 due to
		 * a bug in the handling of lwp_slptime  (as yet not found),
		 * so make sure we do not get a divide by 0 panic.
		 */
		nticks = (cpbase - lp->lwp_cpbase) / gd->gd_schedclock.periodic;
		if (nticks <= 0)
			nticks = 1;
		updatepcpu(lp, lp->lwp_cpticks, nticks);

		if ((nleft = nticks - lp->lwp_cpticks) < 0)
			nleft = 0;
		if (usched_debug == lp->lwp_proc->p_pid) {
			printf("pid %d tid %d estcpu %d cpticks %d nticks %d nleft %d",
				lp->lwp_proc->p_pid, lp->lwp_tid, lp->lwp_estcpu,
				lp->lwp_cpticks, nticks, nleft);
		}

		/*
		 * Calculate a decay value based on ticks remaining scaled
		 * down by the instantanious load and p_nice.
		 */
		if ((loadfac = bsd4_runqcount) < 2)
			loadfac = 2;
		ndecay = nleft * usched_bsd4_decay * 2 * 
			(PRIO_MAX * 2 - lp->lwp_proc->p_nice) / (loadfac * PRIO_MAX * 2);

		/*
		 * Adjust p_estcpu.  Handle a border case where batch jobs
		 * can get stalled long enough to decay to zero when they
		 * shouldn't.
		 */
		if (lp->lwp_estcpu > ndecay * 2)
			lp->lwp_estcpu -= ndecay;
		else
			lp->lwp_estcpu >>= 1;

		if (usched_debug == lp->lwp_proc->p_pid)
			printf(" ndecay %d estcpu %d\n", ndecay, lp->lwp_estcpu);
		bsd4_resetpriority(lp);
		lp->lwp_cpbase = cpbase;
		lp->lwp_cpticks = 0;
	}
}

/*
 * Compute the priority of a process when running in user mode.
 * Arrange to reschedule if the resulting priority is better
 * than that of the current process.
 *
 * This routine may be called with any process.
 *
 * This routine is called by fork1() for initial setup with the process
 * of the run queue, and also may be called normally with the process on or
 * off the run queue.
 *
 * MPSAFE
 */
static void
bsd4_resetpriority(struct lwp *lp)
{
	bsd4_pcpu_t dd;
	int newpriority;
	u_short newrqtype;
	int reschedcpu;

	/*
	 * Calculate the new priority and queue type
	 */
	crit_enter();
	spin_lock_wr(&bsd4_spin);

	newrqtype = lp->lwp_rtprio.type;

	switch(newrqtype) {
	case RTP_PRIO_REALTIME:
		newpriority = PRIBASE_REALTIME +
			     (lp->lwp_rtprio.prio & PRIMASK);
		break;
	case RTP_PRIO_NORMAL:
		newpriority = (lp->lwp_proc->p_nice - PRIO_MIN) * PPQ / NICEPPQ;
		newpriority += lp->lwp_estcpu * PPQ / ESTCPUPPQ;
		newpriority = newpriority * MAXPRI / (PRIO_RANGE * PPQ /
			      NICEPPQ + ESTCPUMAX * PPQ / ESTCPUPPQ);
		newpriority = PRIBASE_NORMAL + (newpriority & PRIMASK);
		break;
	case RTP_PRIO_IDLE:
		newpriority = PRIBASE_IDLE + (lp->lwp_rtprio.prio & PRIMASK);
		break;
	case RTP_PRIO_THREAD:
		newpriority = PRIBASE_THREAD + (lp->lwp_rtprio.prio & PRIMASK);
		break;
	default:
		panic("Bad RTP_PRIO %d", newrqtype);
		/* NOT REACHED */
	}

	/*
	 * The newpriority incorporates the queue type so do a simple masked
	 * check to determine if the process has moved to another queue.  If
	 * it has, and it is currently on a run queue, then move it.
	 */
	if ((lp->lwp_priority ^ newpriority) & ~PPQMASK) {
		lp->lwp_priority = newpriority;
		if (lp->lwp_proc->p_flag & P_ONRUNQ) {
			bsd4_remrunqueue_locked(lp);
			lp->lwp_rqtype = newrqtype;
			lp->lwp_rqindex = (newpriority & PRIMASK) / PPQ;
			bsd4_setrunqueue_locked(lp);
			reschedcpu = lp->lwp_thread->td_gd->gd_cpuid;
		} else {
			lp->lwp_rqtype = newrqtype;
			lp->lwp_rqindex = (newpriority & PRIMASK) / PPQ;
			reschedcpu = -1;
		}
	} else {
		lp->lwp_priority = newpriority;
		reschedcpu = -1;
	}
	spin_unlock_wr(&bsd4_spin);

	/*
	 * Determine if we need to reschedule the target cpu.  This only
	 * occurs if the LWP is already on a scheduler queue, which means
	 * that idle cpu notification has already occured.  At most we
	 * need only issue a need_user_resched() on the appropriate cpu.
	 */
	if (reschedcpu >= 0) {
		dd = &bsd4_pcpu[reschedcpu];
		KKASSERT(dd->uschedcp != lp);
		if ((dd->upri & ~PRIMASK) > (lp->lwp_priority & ~PRIMASK)) {
			dd->upri = lp->lwp_priority;
#ifdef SMP
			if (reschedcpu == mycpu->gd_cpuid) {
				need_user_resched();
			} else {
				lwkt_send_ipiq(lp->lwp_thread->td_gd,
					       need_user_resched_remote, NULL);
			}
#else
			need_user_resched();
#endif
		}
	}
	crit_exit();
}

/*
 * Called from fork1() when a new child process is being created.
 *
 * Give the child process an initial estcpu that is more batch then
 * its parent and dock the parent for the fork (but do not
 * reschedule the parent).   This comprises the main part of our batch
 * detection heuristic for both parallel forking and sequential execs.
 *
 * Interactive processes will decay the boosted estcpu quickly while batch
 * processes will tend to compound it.
 * XXX lwp should be "spawning" instead of "forking"
 *
 * MPSAFE
 */
static void
bsd4_forking(struct lwp *plp, struct lwp *lp)
{
	lp->lwp_estcpu = ESTCPULIM(plp->lwp_estcpu + ESTCPUPPQ);
	lp->lwp_origcpu = lp->lwp_estcpu;
	plp->lwp_estcpu = ESTCPULIM(plp->lwp_estcpu + ESTCPUPPQ);
}

/*
 * Called when the parent reaps a child.   Propogate cpu use by the child
 * back to the parent.
 *
 * MPSAFE
 */
static void
bsd4_exiting(struct lwp *plp, struct lwp *lp)
{
	int delta;

	if (plp->lwp_proc->p_pid != 1) {
		delta = lp->lwp_estcpu - lp->lwp_origcpu;
		if (delta > 0)
			plp->lwp_estcpu = ESTCPULIM(plp->lwp_estcpu + delta);
	}
}


/*
 * chooseproc() is called when a cpu needs a user process to LWKT schedule,
 * it selects a user process and returns it.  If chklp is non-NULL and chklp
 * has a better or equal priority then the process that would otherwise be
 * chosen, NULL is returned.
 *
 * Until we fix the RUNQ code the chklp test has to be strict or we may
 * bounce between processes trying to acquire the current process designation.
 *
 * MPSAFE - must be called with bsd4_spin exclusive held.  The spinlock is
 *	    left intact through the entire routine.
 */
static
struct lwp *
chooseproc_locked(struct lwp *chklp)
{
	struct lwp *lp;
	struct rq *q;
	u_int32_t *which;
	u_int32_t pri;

	if (bsd4_rtqueuebits) {
		pri = bsfl(bsd4_rtqueuebits);
		q = &bsd4_rtqueues[pri];
		which = &bsd4_rtqueuebits;
	} else if (bsd4_queuebits) {
		pri = bsfl(bsd4_queuebits);
		q = &bsd4_queues[pri];
		which = &bsd4_queuebits;
	} else if (bsd4_idqueuebits) {
		pri = bsfl(bsd4_idqueuebits);
		q = &bsd4_idqueues[pri];
		which = &bsd4_idqueuebits;
	} else {
		return NULL;
	}
	lp = TAILQ_FIRST(q);
	KASSERT(lp, ("chooseproc: no lwp on busy queue"));

	/*
	 * If the passed lwp <chklp> is reasonably close to the selected
	 * lwp <lp>, return NULL (indicating that <chklp> should be kept).
	 * 
	 * Note that we must error on the side of <chklp> to avoid bouncing
	 * between threads in the acquire code.
	 */
	if (chklp) {
		if (chklp->lwp_priority < lp->lwp_priority + PPQ)
			return(NULL);
	}

#ifdef SMP
	/*
	 * If the chosen lwp does not reside on this cpu spend a few
	 * cycles looking for a better candidate at the same priority level.
	 * This is a fallback check, setrunqueue() tries to wakeup the
	 * correct cpu and is our front-line affinity.
	 */
	if (lp->lwp_thread->td_gd != mycpu &&
	    (chklp = TAILQ_NEXT(lp, lwp_procq)) != NULL
	) {
		if (chklp->lwp_thread->td_gd == mycpu) {
			++choose_affinity;
			lp = chklp;
		}
	}
#endif

	TAILQ_REMOVE(q, lp, lwp_procq);
	--bsd4_runqcount;
	if (TAILQ_EMPTY(q))
		*which &= ~(1 << pri);
	KASSERT((lp->lwp_proc->p_flag & P_ONRUNQ) != 0, ("not on runq6!"));
	lp->lwp_proc->p_flag &= ~P_ONRUNQ;
	return lp;
}

#ifdef SMP
/*
 * Called via an ipi message to reschedule on another cpu.
 *
 * MPSAFE
 */
static
void
need_user_resched_remote(void *dummy)
{
	need_user_resched();
}

#endif


/*
 * bsd4_remrunqueue_locked() removes a given process from the run queue
 * that it is on, clearing the queue busy bit if it becomes empty.
 *
 * Note that user process scheduler is different from the LWKT schedule.
 * The user process scheduler only manages user processes but it uses LWKT
 * underneath, and a user process operating in the kernel will often be
 * 'released' from our management.
 *
 * MPSAFE - bsd4_spin must be held exclusively on call
 */
static void
bsd4_remrunqueue_locked(struct lwp *lp)
{
	struct rq *q;
	u_int32_t *which;
	u_int8_t pri;

	KKASSERT(lp->lwp_proc->p_flag & P_ONRUNQ);
	lp->lwp_proc->p_flag &= ~P_ONRUNQ;
	--bsd4_runqcount;
	KKASSERT(bsd4_runqcount >= 0);

	pri = lp->lwp_rqindex;
	switch(lp->lwp_rqtype) {
	case RTP_PRIO_NORMAL:
		q = &bsd4_queues[pri];
		which = &bsd4_queuebits;
		break;
	case RTP_PRIO_REALTIME:
	case RTP_PRIO_FIFO:
		q = &bsd4_rtqueues[pri];
		which = &bsd4_rtqueuebits;
		break;
	case RTP_PRIO_IDLE:
		q = &bsd4_idqueues[pri];
		which = &bsd4_idqueuebits;
		break;
	default:
		panic("remrunqueue: invalid rtprio type");
		/* NOT REACHED */
	}
	TAILQ_REMOVE(q, lp, lwp_procq);
	if (TAILQ_EMPTY(q)) {
		KASSERT((*which & (1 << pri)) != 0,
			("remrunqueue: remove from empty queue"));
		*which &= ~(1 << pri);
	}
}

/*
 * bsd4_setrunqueue_locked()
 *
 * Add a process whos rqtype and rqindex had previously been calculated
 * onto the appropriate run queue.   Determine if the addition requires
 * a reschedule on a cpu and return the cpuid or -1.
 *
 * NOTE: Lower priorities are better priorities.
 *
 * MPSAFE - bsd4_spin must be held exclusively on call
 */
static void
bsd4_setrunqueue_locked(struct lwp *lp)
{
	struct rq *q;
	u_int32_t *which;
	int pri;

	KKASSERT((lp->lwp_proc->p_flag & P_ONRUNQ) == 0);
	lp->lwp_proc->p_flag |= P_ONRUNQ;
	++bsd4_runqcount;

	pri = lp->lwp_rqindex;

	switch(lp->lwp_rqtype) {
	case RTP_PRIO_NORMAL:
		q = &bsd4_queues[pri];
		which = &bsd4_queuebits;
		break;
	case RTP_PRIO_REALTIME:
	case RTP_PRIO_FIFO:
		q = &bsd4_rtqueues[pri];
		which = &bsd4_rtqueuebits;
		break;
	case RTP_PRIO_IDLE:
		q = &bsd4_idqueues[pri];
		which = &bsd4_idqueuebits;
		break;
	default:
		panic("remrunqueue: invalid rtprio type");
		/* NOT REACHED */
	}

	/*
	 * Add to the correct queue and set the appropriate bit.  If no
	 * lower priority (i.e. better) processes are in the queue then
	 * we want a reschedule, calculate the best cpu for the job.
	 *
	 * Always run reschedules on the LWPs original cpu.
	 */
	TAILQ_INSERT_TAIL(q, lp, lwp_procq);
	*which |= 1 << pri;
}

#ifdef SMP

/*
 * For SMP systems a user scheduler helper thread is created for each
 * cpu and is used to allow one cpu to wakeup another for the purposes of
 * scheduling userland threads from setrunqueue().  UP systems do not
 * need the helper since there is only one cpu.  We can't use the idle
 * thread for this because we need to hold the MP lock.  Additionally,
 * doing things this way allows us to HLT idle cpus on MP systems.
 *
 * MPSAFE
 */
static void
sched_thread(void *dummy)
{
    globaldata_t gd;
    bsd4_pcpu_t  dd;
    struct lwp *nlp;
    cpumask_t cpumask;
    cpumask_t tmpmask;
    int cpuid;
    int tmpid;

    gd = mycpu;
    cpuid = gd->gd_cpuid;	/* doesn't change */
    cpumask = 1 << cpuid;	/* doesn't change */
    dd = &bsd4_pcpu[cpuid];

    /*
     * The scheduler thread does not need to hold the MP lock.  Since we
     * are woken up only when no user processes are scheduled on a cpu, we
     * can run at an ultra low priority.
     */
    rel_mplock();
    lwkt_setpri_self(TDPRI_USER_SCHEDULER);

    for (;;) {
	/*
	 * We use the LWKT deschedule-interlock trick to avoid racing
	 * bsd4_rdyprocmask.  This means we cannot block through to the
	 * manual lwkt_switch() call we make below.
	 */
	crit_enter_gd(gd);
	lwkt_deschedule_self(gd->gd_curthread);
	spin_lock_wr(&bsd4_spin);
	atomic_set_int(&bsd4_rdyprocmask, cpumask);
	if ((bsd4_curprocmask & cpumask) == 0) {
		if ((nlp = chooseproc_locked(NULL)) != NULL) {
			atomic_set_int(&bsd4_curprocmask, cpumask);
			dd->upri = nlp->lwp_priority;
			dd->uschedcp = nlp;
			spin_unlock_wr(&bsd4_spin);
			lwkt_acquire(nlp->lwp_thread);
			lwkt_schedule(nlp->lwp_thread);
		} else {
			spin_unlock_wr(&bsd4_spin);
		}
	} else {
		/*
		 * Someone scheduled us but raced.  In order to not lose
		 * track of the fact that there may be a LWP ready to go,
		 * forward the request to another cpu if available.
		 *
		 * Rotate through cpus starting with cpuid + 1.  Since cpuid
		 * is already masked out by gd_other_cpus, just use ~cpumask.
		 */
		tmpmask = ~bsd4_curprocmask & bsd4_rdyprocmask &
			  mycpu->gd_other_cpus;
		if (tmpmask) {
			if (tmpmask & ~(cpumask - 1))
				tmpid = bsfl(tmpmask & ~(cpumask - 1));
			else
				tmpid = bsfl(tmpmask);
			bsd4_scancpu = tmpid;
			atomic_clear_int(&bsd4_rdyprocmask, 1 << tmpid);
			spin_unlock_wr(&bsd4_spin);
			lwkt_schedule(&bsd4_pcpu[tmpid].helper_thread);
		} else {
			spin_unlock_wr(&bsd4_spin);
		}
	}
	crit_exit_gd(gd);
	lwkt_switch();
    }
}

/*
 * Setup our scheduler helpers.  Note that curprocmask bit 0 has already
 * been cleared by rqinit() and we should not mess with it further.
 */
static void
sched_thread_cpu_init(void)
{
    int i;

    if (bootverbose)
	printf("start scheduler helpers on cpus:");

    for (i = 0; i < ncpus; ++i) {
	bsd4_pcpu_t dd = &bsd4_pcpu[i];
	cpumask_t mask = 1 << i;

	if ((mask & smp_active_mask) == 0)
	    continue;

	if (bootverbose)
	    printf(" %d", i);

	lwkt_create(sched_thread, NULL, NULL, &dd->helper_thread, 
		    TDF_STOPREQ, i, "usched %d", i);

	/*
	 * Allow user scheduling on the target cpu.  cpu #0 has already
	 * been enabled in rqinit().
	 */
	if (i)
	    atomic_clear_int(&bsd4_curprocmask, mask);
	atomic_set_int(&bsd4_rdyprocmask, mask);
    }
    if (bootverbose)
	printf("\n");
}
SYSINIT(uschedtd, SI_SUB_FINISH_SMP, SI_ORDER_ANY, sched_thread_cpu_init, NULL)

#endif

