/*
 * Copyright (c) 2003 Matthew Dillon <dillon@backplane.com>
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
 * $DragonFly: src/sys/kern/imgact_resident.c,v 1.2 2004/01/20 21:03:23 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysproto.h>
#include <sys/exec.h>
#include <sys/imgact.h>
#include <sys/imgact_aout.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/inflate.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

int
exec_resident_imgact(struct image_params *imgp)
{
	struct vmresident *vmres;

	/*
	 * resident image activator
	 */
	if ((vmres = imgp->vp->v_resident) == NULL)
	    return(-1);
	exec_new_vmspace(imgp, vmres->vr_vmspace);
	imgp->resident = 1;
	imgp->interpreted = 0;
	imgp->proc->p_sysent = vmres->vr_sysent;
	imgp->entry_addr = vmres->vr_entry_addr;
	return(0);
}

static int exec_res_id;
static TAILQ_HEAD(,vmresident) exec_res_list = TAILQ_HEAD_INITIALIZER(exec_res_list);

static MALLOC_DEFINE(M_EXEC_RES, "vmresident", "resident execs");

/*
 * exec_sys_register(entry)
 *
 * Register ourselves for resident execution.  Only root can do this.  This
 * will snapshot the vmspace and cause future exec's of the specified binary
 * to use the snapshot directly rather then load & relocate a new copy.
 */
int
exec_sys_register(struct exec_sys_register_args *uap)
{
    struct vmresident *vmres;
    struct vnode *vp;
    struct proc *p;
    int error;

    p = curproc;
    if ((error = suser(p->p_thread)) != 0)
	return(error);
    if ((vp = p->p_textvp) == NULL)
	return(ENOENT);
    if (vp->v_resident)
	return(EEXIST);
    vhold(vp);
    vmres = malloc(sizeof(*vmres), M_EXEC_RES, M_WAITOK);
    vp->v_resident = vmres;
    vmres->vr_vnode = vp;
    vmres->vr_sysent = p->p_sysent;
    vmres->vr_id = ++exec_res_id;
    vmres->vr_entry_addr = (intptr_t)uap->entry;
    vmres->vr_vmspace = vmspace_fork(p->p_vmspace); /* XXX order */
    TAILQ_INSERT_TAIL(&exec_res_list, vmres, vr_link);
    return(0);
}

/*
 * exec_sys_unregister(id)
 *
 *	Unregister the specified id.  If an id of -1 is used unregister
 *	the registration associated with the current process.  An id of -2
 *	unregisters everything.
 */
int
exec_sys_unregister(struct exec_sys_unregister_args *uap)
{
    struct vmresident *vmres;
    struct proc *p;
    int error;
    int id;
    int count;

    p = curproc;
    if ((error = suser(p->p_thread)) != 0)
	return(error);

    /*
     * If id is -1, unregister ourselves
     */
    if ((id = uap->id) == -1 && p->p_textvp && p->p_textvp->v_resident)
	id = p->p_textvp->v_resident->vr_id;

    /*
     * Look for the registration
     */
    error = ENOENT;
    count = 0;
restart:
    TAILQ_FOREACH(vmres, &exec_res_list, vr_link) {
	if (id == -2 || vmres->vr_id == id) {
	    TAILQ_REMOVE(&exec_res_list, vmres, vr_link);
	    if (vmres->vr_vnode) {
		vmres->vr_vnode->v_resident = NULL;
		vdrop(vmres->vr_vnode);
		vmres->vr_vnode = NULL;
	    }
	    if (vmres->vr_vmspace) {
		vmspace_free(vmres->vr_vmspace);
		vmres->vr_vmspace = NULL;
	    }
	    free(vmres, M_EXEC_RES);
	    error = 0;
	    ++count;
	    goto restart;
	}
    }
    if (error == 0)
	uap->sysmsg_result = count;
    return(error);
}

