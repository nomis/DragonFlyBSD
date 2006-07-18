/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)vfs_subr.c	8.31 (Berkeley) 5/26/95
 * $FreeBSD: src/sys/kern/vfs_subr.c,v 1.249.2.30 2003/04/04 20:35:57 tegge Exp $
 * $DragonFly: src/sys/kern/vfs_sync.c,v 1.11 2006/07/18 22:22:12 dillon Exp $
 */

/*
 * External virtual filesystem routines
 */
#include "opt_ddb.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/domain.h>
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/vmmeter.h>
#include <sys/vnode.h>

#include <machine/limits.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>

#include <sys/buf2.h>
#include <sys/thread2.h>

/*
 * The workitem queue.
 */
#define SYNCER_MAXDELAY		32
static int syncer_maxdelay = SYNCER_MAXDELAY;	/* maximum delay time */
time_t syncdelay = 30;		/* max time to delay syncing data */
SYSCTL_INT(_kern, OID_AUTO, syncdelay, CTLFLAG_RW,
		&syncdelay, 0, "VFS data synchronization delay");
time_t filedelay = 30;		/* time to delay syncing files */
SYSCTL_INT(_kern, OID_AUTO, filedelay, CTLFLAG_RW,
		&filedelay, 0, "File synchronization delay");
time_t dirdelay = 29;		/* time to delay syncing directories */
SYSCTL_INT(_kern, OID_AUTO, dirdelay, CTLFLAG_RW,
		&dirdelay, 0, "Directory synchronization delay");
time_t metadelay = 28;		/* time to delay syncing metadata */
SYSCTL_INT(_kern, OID_AUTO, metadelay, CTLFLAG_RW,
		&metadelay, 0, "VFS metadata synchronization delay");
static int rushjob;			/* number of slots to run ASAP */
static int stat_rush_requests;	/* number of times I/O speeded up */
SYSCTL_INT(_debug, OID_AUTO, rush_requests, CTLFLAG_RW,
		&stat_rush_requests, 0, "");

static int syncer_delayno = 0;
static long syncer_mask; 
LIST_HEAD(synclist, vnode);
static struct synclist *syncer_workitem_pending;

/*
 * Called from vfsinit()
 */
void
vfs_sync_init(void)
{
	syncer_workitem_pending = hashinit(syncer_maxdelay, M_DEVBUF,
					    &syncer_mask);
	syncer_maxdelay = syncer_mask + 1;
}

/*
 * The workitem queue.
 * 
 * It is useful to delay writes of file data and filesystem metadata
 * for tens of seconds so that quickly created and deleted files need
 * not waste disk bandwidth being created and removed. To realize this,
 * we append vnodes to a "workitem" queue. When running with a soft
 * updates implementation, most pending metadata dependencies should
 * not wait for more than a few seconds. Thus, mounted on block devices
 * are delayed only about a half the time that file data is delayed.
 * Similarly, directory updates are more critical, so are only delayed
 * about a third the time that file data is delayed. Thus, there are
 * SYNCER_MAXDELAY queues that are processed round-robin at a rate of
 * one each second (driven off the filesystem syncer process). The
 * syncer_delayno variable indicates the next queue that is to be processed.
 * Items that need to be processed soon are placed in this queue:
 *
 *	syncer_workitem_pending[syncer_delayno]
 *
 * A delay of fifteen seconds is done by placing the request fifteen
 * entries later in the queue:
 *
 *	syncer_workitem_pending[(syncer_delayno + 15) & syncer_mask]
 *
 */

/*
 * Add an item to the syncer work queue.
 */
void
vn_syncer_add_to_worklist(struct vnode *vp, int delay)
{
	int slot;

	crit_enter();

	if (vp->v_flag & VONWORKLST) {
		LIST_REMOVE(vp, v_synclist);
	}

	if (delay > syncer_maxdelay - 2)
		delay = syncer_maxdelay - 2;
	slot = (syncer_delayno + delay) & syncer_mask;

	LIST_INSERT_HEAD(&syncer_workitem_pending[slot], vp, v_synclist);
	vp->v_flag |= VONWORKLST;
	crit_exit();
}

struct  thread *updatethread;
static void sched_sync (void);
static struct kproc_desc up_kp = {
	"syncer",
	sched_sync,
	&updatethread
};
SYSINIT(syncer, SI_SUB_KTHREAD_UPDATE, SI_ORDER_FIRST, kproc_start, &up_kp)

/*
 * System filesystem synchronizer daemon.
 */
void 
sched_sync(void)
{
	struct synclist *slp;
	struct vnode *vp;
	long starttime;
	struct thread *td = curthread;

	EVENTHANDLER_REGISTER(shutdown_pre_sync, shutdown_kproc, td,
	    SHUTDOWN_PRI_LAST);   

	for (;;) {
		kproc_suspend_loop();

		starttime = time_second;

		/*
		 * Push files whose dirty time has expired.  Be careful
		 * of interrupt race on slp queue.
		 */
		crit_enter();
		slp = &syncer_workitem_pending[syncer_delayno];
		syncer_delayno += 1;
		if (syncer_delayno == syncer_maxdelay)
			syncer_delayno = 0;
		crit_exit();

		while ((vp = LIST_FIRST(slp)) != NULL) {
			if (vget(vp, LK_EXCLUSIVE | LK_NOWAIT) == 0) {
				VOP_FSYNC(vp, MNT_LAZY);
				vput(vp);
			}
			crit_enter();

			/*
			 * If the vnode is still at the head of the list
			 * we were not able to completely flush it.  To
			 * give other vnodes a fair shake we move it to
			 * a later slot.
			 *
			 * Note that v_tag VT_VFS vnodes can remain on the
			 * worklist with no dirty blocks, but sync_fsync()
			 * moves it to a later slot so we will never see it
			 * here.
			 */
			if (LIST_FIRST(slp) == vp) {
				if (RB_EMPTY(&vp->v_rbdirty_tree) &&
				    !vn_isdisk(vp, NULL)) {
					panic("sched_sync: fsync failed vp %p tag %d", vp, vp->v_tag);
				}
				vn_syncer_add_to_worklist(vp, syncdelay);
			}
			crit_exit();
		}

		/*
		 * Do soft update processing.
		 */
		if (bioops.io_sync)
			(*bioops.io_sync)(NULL);

		/*
		 * The variable rushjob allows the kernel to speed up the
		 * processing of the filesystem syncer process. A rushjob
		 * value of N tells the filesystem syncer to process the next
		 * N seconds worth of work on its queue ASAP. Currently rushjob
		 * is used by the soft update code to speed up the filesystem
		 * syncer process when the incore state is getting so far
		 * ahead of the disk that the kernel memory pool is being
		 * threatened with exhaustion.
		 */
		if (rushjob > 0) {
			rushjob -= 1;
			continue;
		}
		/*
		 * If it has taken us less than a second to process the
		 * current work, then wait. Otherwise start right over
		 * again. We can still lose time if any single round
		 * takes more than two seconds, but it does not really
		 * matter as we are just trying to generally pace the
		 * filesystem activity.
		 */
		if (time_second == starttime)
			tsleep(&lbolt_syncer, 0, "syncer", 0);
	}
}

/*
 * Request the syncer daemon to speed up its work.
 * We never push it to speed up more than half of its
 * normal turn time, otherwise it could take over the cpu.
 *
 * YYY wchan field protected by the BGL.
 */
int
speedup_syncer(void)
{
	/*
	 * Don't bother protecting the test.  unsleep_and_wakeup_thread()
	 * will only do something real if the thread is in the right state.
	 */
	wakeup(&lbolt_syncer);
	if (rushjob < syncdelay / 2) {
		rushjob += 1;
		stat_rush_requests += 1;
		return (1);
	}
	return(0);
}

/*
 * Routine to create and manage a filesystem syncer vnode.
 */
#define sync_close ((int (*) (struct  vop_close_args *))nullop)
static int	sync_fsync (struct  vop_fsync_args *);
static int	sync_inactive (struct  vop_inactive_args *);
static int	sync_reclaim  (struct  vop_reclaim_args *);
#define sync_lock ((int (*) (struct  vop_lock_args *))vop_stdlock)
#define sync_unlock ((int (*) (struct  vop_unlock_args *))vop_stdunlock)
static int	sync_print (struct vop_print_args *);
#define sync_islocked ((int(*) (struct vop_islocked_args *))vop_stdislocked)

static struct vop_ops sync_vnode_vops = {
	.vop_default =	vop_eopnotsupp,
	.vop_close =	sync_close,
	.vop_fsync =	sync_fsync,
	.vop_inactive =	sync_inactive,
	.vop_reclaim =	sync_reclaim,
	.vop_lock =	sync_lock,
	.vop_unlock =	sync_unlock,
	.vop_print =	sync_print,
	.vop_islocked =	sync_islocked
};

static struct vop_ops *sync_vnode_vops_p = &sync_vnode_vops;

VNODEOP_SET(sync_vnode_vops);

/*
 * Create a new filesystem syncer vnode for the specified mount point.
 * This vnode is placed on the worklist and is responsible for sync'ing
 * the filesystem.
 *
 * NOTE: read-only mounts are also placed on the worklist.  The filesystem
 * sync code is also responsible for cleaning up vnodes.
 */
int
vfs_allocate_syncvnode(struct mount *mp)
{
	struct vnode *vp;
	static long start, incr, next;
	int error;

	/* Allocate a new vnode */
	error = getspecialvnode(VT_VFS, mp, &sync_vnode_vops_p, &vp, 0, 0);
	if (error) {
		mp->mnt_syncer = NULL;
		return (error);
	}
	vp->v_type = VNON;
	/*
	 * Place the vnode onto the syncer worklist. We attempt to
	 * scatter them about on the list so that they will go off
	 * at evenly distributed times even if all the filesystems
	 * are mounted at once.
	 */
	next += incr;
	if (next == 0 || next > syncer_maxdelay) {
		start /= 2;
		incr /= 2;
		if (start == 0) {
			start = syncer_maxdelay / 2;
			incr = syncer_maxdelay;
		}
		next = start;
	}
	vn_syncer_add_to_worklist(vp, syncdelay > 0 ? next % syncdelay : 0);
	mp->mnt_syncer = vp;
	vx_unlock(vp);
	return (0);
}

/*
 * Do a lazy sync of the filesystem.
 *
 * sync_fsync { struct vnode *a_vp, struct ucred *a_cred, int a_waitfor,
 *		struct thread *a_td }
 */
static int
sync_fsync(struct vop_fsync_args *ap)
{
	struct vnode *syncvp = ap->a_vp;
	struct mount *mp = syncvp->v_mount;
	int asyncflag;

	/*
	 * We only need to do something if this is a lazy evaluation.
	 */
	if (ap->a_waitfor != MNT_LAZY)
		return (0);

	/*
	 * Move ourselves to the back of the sync list.
	 */
	vn_syncer_add_to_worklist(syncvp, syncdelay);

	/*
	 * Walk the list of vnodes pushing all that are dirty and
	 * not already on the sync list, and freeing vnodes which have
	 * no refs and whos VM objects are empty.  vfs_msync() handles
	 * the VM issues and must be called whether the mount is readonly
	 * or not.
	 */
	if (vfs_busy(mp, LK_NOWAIT) != 0)
		return (0);
	if (mp->mnt_flag & MNT_RDONLY) {
		vfs_msync(mp, MNT_NOWAIT);
	} else {
		asyncflag = mp->mnt_flag & MNT_ASYNC;
		mp->mnt_flag &= ~MNT_ASYNC;	/* ZZZ hack */
		vfs_msync(mp, MNT_NOWAIT);
		VFS_SYNC(mp, MNT_LAZY);
		if (asyncflag)
			mp->mnt_flag |= MNT_ASYNC;
	}
	vfs_unbusy(mp);
	return (0);
}

/*
 * The syncer vnode is no referenced.
 *
 * sync_inactive { struct vnode *a_vp, struct proc *a_p }
 */
static int
sync_inactive(struct vop_inactive_args *ap)
{
	vgone(ap->a_vp);
	return (0);
}

/*
 * The syncer vnode is no longer needed and is being decommissioned.
 *
 * Modifications to the worklist must be protected with a critical
 * section.
 *
 *	sync_reclaim { struct vnode *a_vp }
 */
static int
sync_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;

	crit_enter();
	vp->v_mount->mnt_syncer = NULL;
	if (vp->v_flag & VONWORKLST) {
		LIST_REMOVE(vp, v_synclist);
		vp->v_flag &= ~VONWORKLST;
	}
	crit_exit();

	return (0);
}

/*
 * Print out a syncer vnode.
 *
 *	sync_print { struct vnode *a_vp }
 */
static int
sync_print(struct vop_print_args *ap)
{
	struct vnode *vp = ap->a_vp;

	printf("syncer vnode");
	lockmgr_printinfo(&vp->v_lock);
	printf("\n");
	return (0);
}

