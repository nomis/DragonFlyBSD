/*
 * Copyright (c) 1982, 1986, 1989, 1993
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
 *	@(#)vfs_lookup.c	8.4 (Berkeley) 2/16/94
 * $FreeBSD: src/sys/kern/vfs_lookup.c,v 1.38.2.3 2001/08/31 19:36:49 dillon Exp $
 * $DragonFly: src/sys/kern/vfs_lookup.c,v 1.9 2003/11/09 20:29:59 dillon Exp $
 */

#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/sysctl.h>

#ifdef KTRACE
#include <sys/ktrace.h>
#endif

#include <vm/vm_zone.h>

static int varsym_enable = 0;
SYSCTL_INT(_vfs, OID_AUTO, varsym_enable, CTLFLAG_RW, &varsym_enable, 0,
	    "Enable Variant Symlinks");

/*
 * Convert a pathname into a pointer to a locked inode.
 *
 * The CNP_FOLLOW flag is set when symbolic links are to be followed
 * when they occur at the end of the name translation process.
 * Symbolic links are always followed for all other pathname
 * components other than the last.
 *
 * The segflg defines whether the name is to be copied from user
 * space or kernel space.
 *
 * Overall outline of namei:
 *
 *	copy in name
 *	get starting directory
 *	while (!done && !error) {
 *		call lookup to search path.
 *		if symbolic link, massage name in buffer and continue
 *	}
 */
int
namei(struct nameidata *ndp)
{
	struct filedesc *fdp;	/* pointer to file descriptor state */
	char *cp;		/* pointer into pathname argument */
	struct vnode *dp;	/* the directory we are searching */
	struct iovec aiov;		/* uio for reading symbolic links */
	struct uio auio;
	int error, linklen;
	struct componentname *cnp = &ndp->ni_cnd;
	struct proc *p;

	KKASSERT(ndp->ni_cnd.cn_td != NULL);
	p = cnp->cn_td->td_proc;
	KKASSERT(p != NULL);
	KASSERT(cnp->cn_cred, ("namei: bad cred/proc"));
	KKASSERT(cnp->cn_cred == p->p_ucred); /* YYY */
	KASSERT((cnp->cn_nameiop & (~NAMEI_OPMASK)) == 0,
	    ("namei: nameiop contaminated with flags"));
	KASSERT((cnp->cn_flags & NAMEI_OPMASK) == 0,
	    ("namei: flags contaminated with nameiops"));
	fdp = p->p_fd;

	/*
	 * Get a buffer for the name to be translated, and copy the
	 * name into the buffer.
	 */
	if ((cnp->cn_flags & CNP_HASBUF) == 0)
		cnp->cn_pnbuf = zalloc(namei_zone);
	if (ndp->ni_segflg == UIO_SYSSPACE)
		error = copystr(ndp->ni_dirp, cnp->cn_pnbuf,
			    MAXPATHLEN, (size_t *)&ndp->ni_pathlen);
	else
		error = copyinstr(ndp->ni_dirp, cnp->cn_pnbuf,
			    MAXPATHLEN, (size_t *)&ndp->ni_pathlen);

	/*
	 * Don't allow empty pathnames.
	 */
	if (!error && *cnp->cn_pnbuf == '\0')
		error = ENOENT;

	if (error) {
		zfree(namei_zone, cnp->cn_pnbuf);
		ndp->ni_vp = NULL;
		return (error);
	}
	ndp->ni_loopcnt = 0;
#ifdef KTRACE
	if (KTRPOINT(cnp->cn_td, KTR_NAMEI))
		ktrnamei(cnp->cn_td->td_proc->p_tracep, cnp->cn_pnbuf);
#endif

	/*
	 * Get starting point for the translation.
	 */
	ndp->ni_rootdir = fdp->fd_rdir;
	ndp->ni_topdir = fdp->fd_jdir;

	dp = fdp->fd_cdir;
	VREF(dp);
	for (;;) {
		/*
		 * Check if root directory should replace current directory.
		 * Done at start of translation and after symbolic link.
		 */
		cnp->cn_nameptr = cnp->cn_pnbuf;
		if (*(cnp->cn_nameptr) == '/') {
			vrele(dp);
			while (*(cnp->cn_nameptr) == '/') {
				cnp->cn_nameptr++;
				ndp->ni_pathlen--;
			}
			dp = ndp->ni_rootdir;
			VREF(dp);
		}
		ndp->ni_startdir = dp;
		error = lookup(ndp);
		if (error) {
			zfree(namei_zone, cnp->cn_pnbuf);
			return (error);
		}
		/*
		 * Check for symbolic link
		 */
		if ((cnp->cn_flags & CNP_ISSYMLINK) == 0) {
			if ((cnp->cn_flags & (CNP_SAVENAME | CNP_SAVESTART)) == 0)
				zfree(namei_zone, cnp->cn_pnbuf);
			else
				cnp->cn_flags |= CNP_HASBUF;

			if (vn_canvmio(ndp->ni_vp) == TRUE &&
				(cnp->cn_nameiop != NAMEI_DELETE) &&
				((cnp->cn_flags & (CNP_NOOBJ|CNP_LOCKLEAF)) ==
				 CNP_LOCKLEAF))
				vfs_object_create(ndp->ni_vp, ndp->ni_cnd.cn_td);

			return (0);
		}
		if ((cnp->cn_flags & CNP_LOCKPARENT) && ndp->ni_pathlen == 1)
			VOP_UNLOCK(ndp->ni_dvp, 0, cnp->cn_td);
		if (ndp->ni_loopcnt++ >= MAXSYMLINKS) {
			error = ELOOP;
			break;
		}
		if (ndp->ni_pathlen > 1)
			cp = zalloc(namei_zone);
		else
			cp = cnp->cn_pnbuf;
		aiov.iov_base = cp;
		aiov.iov_len = MAXPATHLEN;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_rw = UIO_READ;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_td = NULL;
		auio.uio_resid = MAXPATHLEN;
		error = VOP_READLINK(ndp->ni_vp, &auio, cnp->cn_cred);
		if (error) {
			if (ndp->ni_pathlen > 1)
				zfree(namei_zone, cp);
			break;
		}
		linklen = MAXPATHLEN - auio.uio_resid;
		if (linklen == 0) {
			if (ndp->ni_pathlen > 1)
				zfree(namei_zone, cp);
			error = ENOENT;
			break;
		}
		if (varsym_enable) {
			linklen = varsymreplace(cp, linklen, MAXPATHLEN);
			if (linklen < 0) {
				if (ndp->ni_pathlen > 1)
					zfree(namei_zone, cp);
				error = ENAMETOOLONG;
				break;
			}
		}
		if (linklen + ndp->ni_pathlen >= MAXPATHLEN) {
			if (ndp->ni_pathlen > 1)
				zfree(namei_zone, cp);
			error = ENAMETOOLONG;
			break;
		}
		if (ndp->ni_pathlen > 1) {
			bcopy(ndp->ni_next, cp + linklen, ndp->ni_pathlen);
			zfree(namei_zone, cnp->cn_pnbuf);
			cnp->cn_pnbuf = cp;
		} else {
			cnp->cn_pnbuf[linklen] = '\0';
		}
		ndp->ni_pathlen += linklen;
		vput(ndp->ni_vp);
		dp = ndp->ni_dvp;
	}
	zfree(namei_zone, cnp->cn_pnbuf);
	vrele(ndp->ni_dvp);
	vput(ndp->ni_vp);
	ndp->ni_vp = NULL;
	return (error);
}

/*
 * Search a pathname.
 * This is a very central and rather complicated routine.
 *
 * The pathname is pointed to by ni_ptr and is of length ni_pathlen.
 * The starting directory is taken from ni_startdir. The pathname is
 * descended until done, or a symbolic link is encountered. The variable
 * ni_more is clear if the path is completed; it is set to one if a
 * symbolic link needing interpretation is encountered.
 *
 * The flag argument is NAMEI_LOOKUP, CREATE, RENAME, or DELETE depending on
 * whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * If flag has LOCKPARENT or'ed into it, the parent directory is returned
 * locked. If flag has WANTPARENT or'ed into it, the parent directory is
 * returned unlocked. Otherwise the parent directory is not returned. If
 * the target of the pathname exists and LOCKLEAF is or'ed into the flag
 * the target is returned locked, otherwise it is returned unlocked.
 * When creating or renaming and LOCKPARENT is specified, the target may not
 * be ".".  When deleting and LOCKPARENT is specified, the target may be ".".
 *
 * Overall outline of lookup:
 *
 * dirloop:
 *	identify next component of name at ndp->ni_ptr
 *	handle degenerate case where name is null string
 *	if .. and crossing mount points and on mounted filesys, find parent
 *	call VOP_LOOKUP routine for next component name
 *	    directory vnode returned in ni_dvp, unlocked unless LOCKPARENT set
 *	    component vnode returned in ni_vp (if it exists), locked.
 *	if result vnode is mounted on and crossing mount points,
 *	    find mounted on vnode
 *	if more components of name, do next level at dirloop
 *	return the answer in ni_vp, locked if LOCKLEAF set
 *	    if LOCKPARENT set, return locked parent in ni_dvp
 *	    if WANTPARENT set, return unlocked parent in ni_dvp
 */
int
lookup(struct nameidata *ndp)
{
	char *cp;			/* pointer into pathname argument */
	struct vnode *dp = NULL;	/* the directory we are searching */
	struct vnode *tdp;		/* saved dp */
	struct mount *mp;		/* mount table entry */
	int docache;			/* == 0 do not cache last component */
	int wantparent;			/* 1 => wantparent or lockparent flag */
	int rdonly;			/* lookup read-only flag bit */
	int trailing_slash;
	int error = 0;
	int dpunlocked = 0;		/* dp has already been unlocked */
	struct componentname *cnp = &ndp->ni_cnd;
	struct thread *td = cnp->cn_td;

	/*
	 * Setup: break out flag bits into variables.
	 */
	wantparent = cnp->cn_flags & (CNP_LOCKPARENT | CNP_WANTPARENT);
	docache = (cnp->cn_flags & CNP_NOCACHE) ^ CNP_NOCACHE;
	if (cnp->cn_nameiop == NAMEI_DELETE ||
	    (wantparent && cnp->cn_nameiop != NAMEI_CREATE &&
	     cnp->cn_nameiop != NAMEI_LOOKUP))
		docache = 0;
	rdonly = cnp->cn_flags & CNP_RDONLY;
	ndp->ni_dvp = NULL;
	cnp->cn_flags &= ~CNP_ISSYMLINK;
	dp = ndp->ni_startdir;
	ndp->ni_startdir = NULLVP;
	vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, td);

dirloop:
	/*
	 * Search a new directory.
	 *
	 * The last component of the filename is left accessible via
	 * cnp->cn_nameptr for callers that need the name. Callers needing
	 * the name set the CNP_SAVENAME flag. When done, they assume
	 * responsibility for freeing the pathname buffer.
	 */
	cnp->cn_consume = 0;
	for (cp = cnp->cn_nameptr; *cp != 0 && *cp != '/'; cp++)
		continue;
	cnp->cn_namelen = cp - cnp->cn_nameptr;
	if (cnp->cn_namelen > NAME_MAX) {
		error = ENAMETOOLONG;
		goto bad;
	}
#ifdef NAMEI_DIAGNOSTIC
	{ char c = *cp;
	*cp = '\0';
	printf("{%s}: ", cnp->cn_nameptr);
	*cp = c; }
#endif
	ndp->ni_pathlen -= cnp->cn_namelen;
	ndp->ni_next = cp;

	/*
	 * Replace multiple slashes by a single slash and trailing slashes
	 * by a null.  This must be done before VOP_LOOKUP() because some
	 * fs's don't know about trailing slashes.  Remember if there were
	 * trailing slashes to handle symlinks, existing non-directories
	 * and non-existing files that won't be directories specially later.
	 */
	trailing_slash = 0;
	while (*cp == '/' && (cp[1] == '/' || cp[1] == '\0')) {
		cp++;
		ndp->ni_pathlen--;
		if (*cp == '\0') {
			trailing_slash = 1;
			*ndp->ni_next = '\0';	/* XXX for direnter() ... */
		}
	}
	ndp->ni_next = cp;

	cnp->cn_flags |= CNP_MAKEENTRY;
	if (*cp == '\0' && docache == 0)
		cnp->cn_flags &= ~CNP_MAKEENTRY;
	if (cnp->cn_namelen == 2 &&
	    cnp->cn_nameptr[1] == '.' && cnp->cn_nameptr[0] == '.')
		cnp->cn_flags |= CNP_ISDOTDOT;
	else
		cnp->cn_flags &= ~CNP_ISDOTDOT;
	if (*ndp->ni_next == 0)
		cnp->cn_flags |= CNP_ISLASTCN;
	else
		cnp->cn_flags &= ~CNP_ISLASTCN;


	/*
	 * Check for degenerate name (e.g. / or "")
	 * which is a way of talking about a directory,
	 * e.g. like "/." or ".".
	 */
	if (cnp->cn_nameptr[0] == '\0') {
		if (dp->v_type != VDIR) {
			error = ENOTDIR;
			goto bad;
		}
		if (cnp->cn_nameiop != NAMEI_LOOKUP) {
			error = EISDIR;
			goto bad;
		}
		if (wantparent) {
			ndp->ni_dvp = dp;
			VREF(dp);
		}
		ndp->ni_vp = dp;
		if (!(cnp->cn_flags & (CNP_LOCKPARENT | CNP_LOCKLEAF)))
			VOP_UNLOCK(dp, 0, cnp->cn_td);
		/* XXX This should probably move to the top of function. */
		if (cnp->cn_flags & CNP_SAVESTART)
			panic("lookup: CNP_SAVESTART");
		return (0);
	}

	/*
	 * Handle "..": two special cases.
	 * 1. If at root directory (e.g. after chroot)
	 *    or at absolute root directory
	 *    then ignore it so can't get out.
	 * 2. If this vnode is the root of a mounted
	 *    filesystem, then replace it with the
	 *    vnode which was mounted on so we take the
	 *    .. in the other file system.
	 * 3. If the vnode is the top directory of
	 *    the jail or chroot, don't let them out.
	 */
	if (cnp->cn_flags & CNP_ISDOTDOT) {
		for (;;) {
			if (dp == ndp->ni_rootdir || 
			    dp == ndp->ni_topdir || 
			    dp == rootvnode) {
				ndp->ni_dvp = dp;
				ndp->ni_vp = dp;
				VREF(dp);
				goto nextname;
			}
			if ((dp->v_flag & VROOT) == 0 ||
			    (cnp->cn_flags & CNP_NOCROSSMOUNT))
				break;
			if (dp->v_mount == NULL) {	/* forced unmount */
				error = EBADF;
				goto bad;
			}
			tdp = dp;
			dp = dp->v_mount->mnt_vnodecovered;
			vput(tdp);
			VREF(dp);
			vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, td);
		}
	}

	/*
	 * We now have a segment name to search for, and a directory to search.
	 */
unionlookup:
	ndp->ni_dvp = dp;
	ndp->ni_vp = NULL;
	cnp->cn_flags &= ~CNP_PDIRUNLOCK;
	ASSERT_VOP_LOCKED(dp, "lookup");
	if ((error = VOP_LOOKUP(dp, NCPNULL, &ndp->ni_vp, NCPPNULL, cnp)) != 0) {
		KASSERT(ndp->ni_vp == NULL, ("leaf should be empty"));
#ifdef NAMEI_DIAGNOSTIC
		printf("not found\n");
#endif
		if ((error == ENOENT) &&
		    (dp->v_flag & VROOT) && (dp->v_mount != NULL) &&
		    (dp->v_mount->mnt_flag & MNT_UNION)) {
			tdp = dp;
			dp = dp->v_mount->mnt_vnodecovered;
			if (cnp->cn_flags & CNP_PDIRUNLOCK)
				vrele(tdp);
			else
				vput(tdp);
			VREF(dp);
			vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, td);
			goto unionlookup;
		}

		if (error != EJUSTRETURN)
			goto bad;
		/*
		 * If creating and at end of pathname, then can consider
		 * allowing file to be created.
		 */
		if (rdonly) {
			error = EROFS;
			goto bad;
		}
		if (*cp == '\0' && trailing_slash &&
		     !(cnp->cn_flags & CNP_WILLBEDIR)) {
			error = ENOENT;
			goto bad;
		}
		/*
		 * We return with ni_vp NULL to indicate that the entry
		 * doesn't currently exist, leaving a pointer to the
		 * (possibly locked) directory inode in ndp->ni_dvp.
		 */
		if (cnp->cn_flags & CNP_SAVESTART) {
			ndp->ni_startdir = ndp->ni_dvp;
			VREF(ndp->ni_startdir);
		}
		return (0);
	}
#ifdef NAMEI_DIAGNOSTIC
	printf("found\n");
#endif

	ASSERT_VOP_LOCKED(ndp->ni_vp, "lookup");

	/*
	 * Take into account any additional components consumed by
	 * the underlying filesystem.
	 */
	if (cnp->cn_consume > 0) {
		cnp->cn_nameptr += cnp->cn_consume;
		ndp->ni_next += cnp->cn_consume;
		ndp->ni_pathlen -= cnp->cn_consume;
		cnp->cn_consume = 0;
	}

	dp = ndp->ni_vp;

	/*
	 * Check to see if the vnode has been mounted on;
	 * if so find the root of the mounted file system.
	 */
	while (dp->v_type == VDIR && (mp = dp->v_mountedhere) &&
	       (cnp->cn_flags & CNP_NOCROSSMOUNT) == 0) {
		if (vfs_busy(mp, 0, 0, td))
			continue;
		VOP_UNLOCK(dp, 0, td);
		error = VFS_ROOT(mp, &tdp);
		vfs_unbusy(mp, td);
		if (error) {
			dpunlocked = 1;
			goto bad2;
		}
		cache_mount(dp, tdp);
		vrele(dp);
		ndp->ni_vp = dp = tdp;
	}

	/*
	 * Check for symbolic link
	 */
	if ((dp->v_type == VLNK) &&
	    ((cnp->cn_flags & CNP_FOLLOW) || trailing_slash ||
	     *ndp->ni_next == '/')) {
		cnp->cn_flags |= CNP_ISSYMLINK;
		if (dp->v_mount == NULL) {
			/* We can't know whether the directory was mounted with
			 * NOSYMFOLLOW, so we can't follow safely. */
			error = EBADF;
			goto bad2;
		}
		if (dp->v_mount->mnt_flag & MNT_NOSYMFOLLOW) {
			error = EACCES;
			goto bad2;
		}
		return (0);
	}

	/*
	 * Check for bogus trailing slashes.
	 */
	if (trailing_slash && dp->v_type != VDIR) {
		error = ENOTDIR;
		goto bad2;
	}

nextname:
	/*
	 * Not a symbolic link.  If more pathname,
	 * continue at next component, else return.
	 */
	if (*ndp->ni_next == '/') {
		cnp->cn_nameptr = ndp->ni_next;
		while (*cnp->cn_nameptr == '/') {
			cnp->cn_nameptr++;
			ndp->ni_pathlen--;
		}
		if (ndp->ni_dvp != ndp->ni_vp)
			ASSERT_VOP_UNLOCKED(ndp->ni_dvp, "lookup");
		vrele(ndp->ni_dvp);
		goto dirloop;
	}
	/*
	 * Disallow directory write attempts on read-only file systems.
	 */
	if (rdonly &&
	    (cnp->cn_nameiop == NAMEI_DELETE || cnp->cn_nameiop == NAMEI_RENAME)) {
		error = EROFS;
		goto bad2;
	}
	if (cnp->cn_flags & CNP_SAVESTART) {
		ndp->ni_startdir = ndp->ni_dvp;
		VREF(ndp->ni_startdir);
	}
	if (!wantparent)
		vrele(ndp->ni_dvp);

	if ((cnp->cn_flags & CNP_LOCKLEAF) == 0)
		VOP_UNLOCK(dp, 0, td);
	return (0);

bad2:
	if ((cnp->cn_flags & (CNP_LOCKPARENT | CNP_PDIRUNLOCK)) == CNP_LOCKPARENT &&
	    *ndp->ni_next == '\0')
		VOP_UNLOCK(ndp->ni_dvp, 0, td);
	vrele(ndp->ni_dvp);
bad:
	if (dpunlocked)
		vrele(dp);
	else
		vput(dp);
	ndp->ni_vp = NULL;
	return (error);
}

/*
 * relookup - lookup a path name component
 *    Used by lookup to re-aquire things.
 */
int
relookup(dvp, vpp, cnp)
	struct vnode *dvp, **vpp;
	struct componentname *cnp;
{
	struct thread *td = cnp->cn_td;
	struct vnode *dp = 0;		/* the directory we are searching */
	int docache;			/* == 0 do not cache last component */
	int wantparent;			/* 1 => wantparent or lockparent flag */
	int rdonly;			/* lookup read-only flag bit */
	int error = 0;
#ifdef NAMEI_DIAGNOSTIC
	int newhash;			/* DEBUG: check name hash */
	char *cp;			/* DEBUG: check name ptr/len */
#endif

	/*
	 * Setup: break out flag bits into variables.
	 */
	wantparent = cnp->cn_flags & (CNP_LOCKPARENT|CNP_WANTPARENT);
	docache = (cnp->cn_flags & CNP_NOCACHE) ^ CNP_NOCACHE;
	if (cnp->cn_nameiop == NAMEI_DELETE ||
	    (wantparent && cnp->cn_nameiop != NAMEI_CREATE))
		docache = 0;
	rdonly = cnp->cn_flags & CNP_RDONLY;
	cnp->cn_flags &= ~CNP_ISSYMLINK;
	dp = dvp;
	vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, td);

/* dirloop: */
	/*
	 * Search a new directory.
	 *
	 * The last component of the filename is left accessible via
	 * cnp->cn_nameptr for callers that need the name. Callers needing
	 * the name set the CNP_SAVENAME flag. When done, they assume
	 * responsibility for freeing the pathname buffer.
	 */
#ifdef NAMEI_DIAGNOSTIC
	if (cnp->cn_namelen != cp - cnp->cn_nameptr)
		panic ("relookup: bad len");
	if (*cp != 0)
		panic("relookup: not last component");
	printf("{%s}: ", cnp->cn_nameptr);
#endif

	/*
	 * Check for degenerate name (e.g. / or "")
	 * which is a way of talking about a directory,
	 * e.g. like "/." or ".".
	 */
	if (cnp->cn_nameptr[0] == '\0') {
		if (cnp->cn_nameiop != NAMEI_LOOKUP || wantparent) {
			error = EISDIR;
			goto bad;
		}
		if (dp->v_type != VDIR) {
			error = ENOTDIR;
			goto bad;
		}
		if (!(cnp->cn_flags & CNP_LOCKLEAF))
			VOP_UNLOCK(dp, 0, td);
		*vpp = dp;
		/* XXX This should probably move to the top of function. */
		if (cnp->cn_flags & CNP_SAVESTART)
			panic("lookup: CNP_SAVESTART");
		return (0);
	}

	if (cnp->cn_flags & CNP_ISDOTDOT)
		panic ("relookup: lookup on dot-dot");

	/*
	 * We now have a segment name to search for, and a directory to search.
	 */
	if ((error = VOP_LOOKUP(dp, NCPNULL, vpp, NCPPNULL, cnp)) != 0) {
		KASSERT(*vpp == NULL, ("leaf should be empty"));
		if (error != EJUSTRETURN)
			goto bad;
		/*
		 * If creating and at end of pathname, then can consider
		 * allowing file to be created.
		 */
		if (rdonly) {
			error = EROFS;
			goto bad;
		}
		/* ASSERT(dvp == ndp->ni_startdir) */
		if (cnp->cn_flags & CNP_SAVESTART)
			VREF(dvp);
		/*
		 * We return with ni_vp NULL to indicate that the entry
		 * doesn't currently exist, leaving a pointer to the
		 * (possibly locked) directory inode in ndp->ni_dvp.
		 */
		return (0);
	}
	dp = *vpp;

	/*
	 * Check for symbolic link
	 */
	KASSERT(dp->v_type != VLNK || !(cnp->cn_flags & CNP_FOLLOW),
	    ("relookup: symlink found.\n"));

	/*
	 * Disallow directory write attempts on read-only file systems.
	 */
	if (rdonly &&
	    (cnp->cn_nameiop == NAMEI_DELETE || cnp->cn_nameiop == NAMEI_RENAME)) {
		error = EROFS;
		goto bad2;
	}
	/* ASSERT(dvp == ndp->ni_startdir) */
	if (cnp->cn_flags & CNP_SAVESTART)
		VREF(dvp);
	
	if (!wantparent)
		vrele(dvp);

	if (vn_canvmio(dp) == TRUE &&
		((cnp->cn_flags & (CNP_NOOBJ|CNP_LOCKLEAF)) == CNP_LOCKLEAF))
		vfs_object_create(dp, cnp->cn_td);

	if ((cnp->cn_flags & CNP_LOCKLEAF) == 0)
		VOP_UNLOCK(dp, 0, td);
	return (0);

bad2:
	if ((cnp->cn_flags & CNP_LOCKPARENT) && (cnp->cn_flags & CNP_ISLASTCN))
		VOP_UNLOCK(dvp, 0, td);
	vrele(dvp);
bad:
	vput(dp);
	*vpp = NULL;
	return (error);
}
