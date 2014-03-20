/*
 * Copyright (c) 2011-2014 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 * by Venkatesh Srinivas <vsrinivas@dragonflybsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * HAMMER2 IN-MEMORY CACHE OF MEDIA STRUCTURES
 *
 * This header file contains structures used internally by the HAMMER2
 * implementation.  See hammer2_disk.h for on-disk structures.
 *
 * There is an in-memory representation of all on-media data structure.
 * Basically everything is represented by a hammer2_chain structure
 * in-memory and other higher-level structures map to chains.
 *
 * A great deal of data is accessed simply via its buffer cache buffer,
 * which is mapped for the duration of the chain's lock.  However, because
 * chains may represent blocks smaller than the 16KB minimum we impose
 * on buffer cache buffers, we cannot hold related buffer cache buffers
 * locked for smaller blocks.  In these situations we kmalloc() a copy
 * of the block.
 *
 * When modifications are made to a chain a new filesystem block must be
 * allocated.  Multiple modifications do not necessarily allocate new
 * blocks.  However, when a flush occurs a flush synchronization point
 * is created and any new modifications made after this point will allocate
 * a new block even if the chain is already in a modified state.
 *
 * The in-memory representation may remain cached (for example in order to
 * placemark clustering locks) even after the related data has been
 * detached.
 *
 *				CORE SHARING
 *
 * In order to support concurrent flushes a flush synchronization point
 * is created represented by a transaction id.  Among other things,
 * operations may move filesystem objects from one part of the topology
 * to another (for example, if you rename a file or when indirect blocks
 * are created or destroyed, and a few other things).  When this occurs
 * across a flush synchronization point the flusher needs to be able to
 * recurse down BOTH the 'before' version of the topology and the 'after'
 * version.
 *
 * To facilitate this modifications to chains do what is called a
 * DELETE-DUPLICATE operation.  Chains are not actually moved in-memory.
 * Instead the chain we wish to move is deleted and a new chain is created
 * at the target location in the topology.  ANY SUBCHAINS PLACED UNDER THE
 * CHAIN BEING MOVED HAVE TO EXIST IN BOTH PLACES.  To make this work
 * all sub-chains are managed by the hammer2_chain_core structure.  This
 * structure can be multi-homed, meaning that it can have more than one
 * chain as its parent.  When a chain is delete-duplicated the chain's core
 * becomes shared under both the old and new chain.
 *
 *				STALE CHAINS
 *
 * When a chain is delete-duplicated the old chain typically becomes stale.
 * This is detected via the HAMMER2_CHAIN_DUPLICATED flag in chain->flags.
 * To avoid executing live filesystem operations on stale chains, the inode
 * locking code will follow stale chains via core->ownerq until it finds
 * the live chain.  The lock prevents ripups by other threads.  Lookups
 * must properly order locking operations to prevent other threads from
 * racing the lookup operation and will also follow stale chains when
 * required.
 */

#ifndef _VFS_HAMMER2_HAMMER2_H_
#define _VFS_HAMMER2_HAMMER2_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/tree.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/mountctl.h>
#include <sys/priv.h>
#include <sys/stat.h>
#include <sys/thread.h>
#include <sys/globaldata.h>
#include <sys/lockf.h>
#include <sys/buf.h>
#include <sys/queue.h>
#include <sys/limits.h>
#include <sys/signal2.h>
#include <sys/dmsg.h>
#include <sys/mutex.h>
#include <sys/kern_syscall.h>

#include <sys/buf2.h>
#include <sys/mutex2.h>

#include "hammer2_disk.h"
#include "hammer2_mount.h"
#include "hammer2_ioctl.h"
#include "hammer2_ccms.h"

struct hammer2_chain;
struct hammer2_cluster;
struct hammer2_inode;
struct hammer2_mount;
struct hammer2_pfsmount;
struct hammer2_span;
struct hammer2_state;
struct hammer2_msg;

/*
 * The chain structure tracks a portion of the media topology from the
 * root (volume) down.  Chains represent volumes, inodes, indirect blocks,
 * data blocks, and freemap nodes and leafs.
 *
 * The chain structure can be multi-homed and its topological recursion
 * (chain->core) can be shared amongst several chains.  Chain structures
 * are topologically stable once placed in the in-memory topology (they
 * don't move around).  Modifications which cross flush synchronization
 * boundaries, renames, resizing, or any move of the chain to elsewhere
 * in the topology is accomplished via the DELETE-DUPLICATE mechanism.
 *
 * Deletions and delete-duplicates:
 *
 *	Any movement of chains within the topology utilize a delete-duplicate
 *	operation instead of a simple rename.  That is, the chain must be
 *	deleted from its original location and then duplicated to the new
 *	location.  A new chain structure is allocated while the old is
 *	deleted.  Deleted chains are removed from the above chain_core's
 *	rbtree but remain linked via the shadow topology for flush
 *	synchronization purposes.
 *
 *	delete_bmap is allocated and a bit set if the chain was originally
 *	loaded via the blockmap.
 *
 * Flush synchronization:
 *
 *	Flushes must synchronize chains up through the root.  To do this
 *	the in-memory topology would normally have to be frozen during the
 *	flush.  To avoid freezing the topology and to allow concurrent
 *	foreground / flush activity, any new modifications made while a
 *	flush is in progress retains the original chain in a shadow topology
 *	that is only visible to the flush code.  Only one flush can be
 *	running at a time so the shadow hierarchy can be implemented with
 *	just a few link fields in our in-memory data structures.
 *
 * Advantages:
 *
 *	(1) Fully coherent snapshots can be taken without requiring
 *	    a pre-flush, resulting in extremely fast (sub-millisecond)
 *	    snapshots.
 *
 *	(2) Multiple synchronization points can be in-flight at the same
 *	    time, representing multiple snapshots or flushes.
 *
 *	(3) The algorithms needed to keep track of everything are actually
 *	    not that complex.
 *
 * Special Considerations:
 *
 *	A chain is ref-counted on a per-chain basis, but the chain's lock
 *	is associated with the shared chain_core and is not per-chain.
 *
 *	The power-of-2 nature of the media radix tree ensures that there
 *	will be no overlaps which straddle edges.
 */
RB_HEAD(hammer2_chain_tree, hammer2_chain);
TAILQ_HEAD(h2_flush_deferral_list, hammer2_chain);
TAILQ_HEAD(h2_core_list, hammer2_chain);

#define CHAIN_CORE_DELETE_BMAP_ENTRIES	\
	(HAMMER2_PBUFSIZE / sizeof(hammer2_blockref_t) / sizeof(uint32_t))

struct hammer2_chain_core {
	int		good;
	struct ccms_cst	cst;
	struct h2_core_list ownerq;	  /* all chains sharing this core */
	struct hammer2_chain_tree rbtree; /* live chains */
	struct hammer2_chain_tree dbtree; /* bmapped deletions */
	struct h2_core_list dbq;	  /* other deletions */
	int		live_zero;	/* blockref array opt */
	u_int		sharecnt;
	u_int		flags;
	u_int		live_count;	/* live (not deleted) chains in tree */
	u_int		chain_count;	/* live + deleted chains under core */
	int		generation;	/* generation number (inserts only) */
};

typedef struct hammer2_chain_core hammer2_chain_core_t;

#define HAMMER2_CORE_UNUSED0001		0x0001
#define HAMMER2_CORE_COUNTEDBREFS	0x0002

/*
 * H2 is a copy-on-write filesystem.  In order to allow chains to allocate
 * smaller blocks (down to 64-bytes), but improve performance and make
 * clustered I/O possible using larger block sizes, the kernel buffer cache
 * is abstracted via the hammer2_io structure.
 */
RB_HEAD(hammer2_io_tree, hammer2_io);

struct hammer2_io {
	RB_ENTRY(hammer2_io) rbnode;	/* indexed by device offset */
	struct spinlock spin;
	struct hammer2_mount *hmp;
	struct buf	*bp;
	struct bio	*bio;
	off_t		pbase;
	int		psize;
	void		(*callback)(struct hammer2_io *dio,
				    struct hammer2_cluster *cluster,
				    struct hammer2_chain *chain,
				    void *arg1, off_t arg2);
	struct hammer2_cluster *arg_l;		/* INPROG I/O only */
	struct hammer2_chain *arg_c;		/* INPROG I/O only */
	void		*arg_p;			/* INPROG I/O only */
	off_t		arg_o;			/* INPROG I/O only */
	int		refs;
	int		act;			/* activity */
};

typedef struct hammer2_io hammer2_io_t;

/*
 * Primary chain structure keeps track of the topology in-memory.
 */
struct hammer2_chain {
	TAILQ_ENTRY(hammer2_chain) core_entry;	/* contemporary chains */
	RB_ENTRY(hammer2_chain) rbnode;		/* live chain(s) */
	TAILQ_ENTRY(hammer2_chain) db_entry;	/* non bmapped deletions */
	hammer2_blockref_t	bref;
	hammer2_chain_core_t	*core;
	hammer2_chain_core_t	*above;
	struct hammer2_state	*state;		/* if active cache msg */
	struct hammer2_mount	*hmp;
	struct hammer2_pfsmount	*pmp;		/* can be NULL */

	hammer2_blockref_t	dsrc;			/* DEBUG */
	int			ninserts;		/* DEBUG */
	int			nremoves;		/* DEBUG */
	hammer2_tid_t		dsrc_dupfromat;		/* DEBUG */
	uint32_t		dsrc_dupfromflags;	/* DEBUG */
	int			dsrc_reason;		/* DEBUG */
	int			dsrc_ninserts;		/* DEBUG */
	uint32_t		dsrc_flags;		/* DEBUG */
	hammer2_tid_t		dsrc_modify;		/* DEBUG */
	hammer2_tid_t		dsrc_delete;		/* DEBUG */
	hammer2_tid_t		dsrc_update_lo;		/* DEBUG */
	struct hammer2_chain	*dsrc_original;		/* DEBUG */

	hammer2_tid_t	modify_tid;		/* flush filter */
	hammer2_tid_t	delete_tid;		/* flush filter */
	hammer2_tid_t	update_lo;		/* flush propagation */
	hammer2_tid_t	update_hi;		/* setsubmod propagation */
	hammer2_key_t   data_count;		/* delta's to apply */
	hammer2_key_t   inode_count;		/* delta's to apply */
	hammer2_io_t	*dio;			/* physical data buffer */
	u_int		bytes;			/* physical data size */
	u_int		flags;
	u_int		refs;
	u_int		lockcnt;
	hammer2_media_data_t *data;		/* data pointer shortcut */
	TAILQ_ENTRY(hammer2_chain) flush_node;	/* flush deferral list */

	int		inode_reason;
};

typedef struct hammer2_chain hammer2_chain_t;

int hammer2_chain_cmp(hammer2_chain_t *chain1, hammer2_chain_t *chain2);
RB_PROTOTYPE(hammer2_chain_tree, hammer2_chain, rbnode, hammer2_chain_cmp);

/*
 * Special notes on flags:
 *
 * INITIAL - This flag allows a chain to be created and for storage to
 *	     be allocated without having to immediately instantiate the
 *	     related buffer.  The data is assumed to be all-zeros.  It
 *	     is primarily used for indirect blocks.
 *
 * MODIFIED- The chain's media data has been modified.
 */
#define HAMMER2_CHAIN_MODIFIED		0x00000001	/* dirty chain data */
#define HAMMER2_CHAIN_ALLOCATED		0x00000002	/* kmalloc'd chain */
#define HAMMER2_CHAIN_FLUSH_TEMPORARY	0x00000004
#define HAMMER2_CHAIN_FORCECOW		0x00000008	/* force copy-on-wr */
#define HAMMER2_CHAIN_DELETED		0x00000010	/* deleted chain */
#define HAMMER2_CHAIN_INITIAL		0x00000020	/* initial create */
#define HAMMER2_CHAIN_FLUSH_CREATE	0x00000040	/* needs flush blkadd */
#define HAMMER2_CHAIN_FLUSH_DELETE	0x00000080	/* needs flush blkdel */
#define HAMMER2_CHAIN_IOFLUSH		0x00000100	/* bawrite on put */
#define HAMMER2_CHAIN_DEFERRED		0x00000200	/* on a deferral list */
#define HAMMER2_CHAIN_UNLINKED		0x00000400	/* delete on reclaim */
#define HAMMER2_CHAIN_VOLUMESYNC	0x00000800	/* needs volume sync */
#define HAMMER2_CHAIN_ONDBQ		0x00001000	/* !bmapped deletes */
#define HAMMER2_CHAIN_MOUNTED		0x00002000	/* PFS is mounted */
#define HAMMER2_CHAIN_ONRBTREE		0x00004000	/* on parent RB tree */
#define HAMMER2_CHAIN_SNAPSHOT		0x00008000	/* snapshot special */
#define HAMMER2_CHAIN_EMBEDDED		0x00010000	/* embedded data */
#define HAMMER2_CHAIN_RELEASE		0x00020000	/* don't keep around */
#define HAMMER2_CHAIN_BMAPPED		0x00040000	/* in parent blkmap */
#define HAMMER2_CHAIN_ONDBTREE		0x00080000	/* bmapped deletes */
#define HAMMER2_CHAIN_DUPLICATED	0x00100000	/* fwd delete-dup */
#define HAMMER2_CHAIN_PFSROOT		0x00200000	/* in pfs->cluster */

/*
 * Flags passed to hammer2_chain_lookup() and hammer2_chain_next()
 *
 * NOTE: MATCHIND allows an indirect block / freemap node to be returned
 *	 when the passed key range matches the radix.  Remember that key_end
 *	 is inclusive (e.g. {0x000,0xFFF}, not {0x000,0x1000}).
 */
#define HAMMER2_LOOKUP_NOLOCK		0x00000001	/* ref only */
#define HAMMER2_LOOKUP_NODATA		0x00000002	/* data left NULL */
#define HAMMER2_LOOKUP_SHARED		0x00000100
#define HAMMER2_LOOKUP_MATCHIND		0x00000200	/* return all chains */
#define HAMMER2_LOOKUP_UNUSED0400	0x00000400
#define HAMMER2_LOOKUP_ALWAYS		0x00000800	/* resolve data */

/*
 * Flags passed to hammer2_chain_modify() and hammer2_chain_resize()
 *
 * NOTE: OPTDATA allows us to avoid instantiating buffers for INDIRECT
 *	 blocks in the INITIAL-create state.
 */
#define HAMMER2_MODIFY_OPTDATA		0x00000002	/* data can be NULL */
#define HAMMER2_MODIFY_NO_MODIFY_TID	0x00000004
#define HAMMER2_MODIFY_ASSERTNOCOPY	0x00000008	/* assert no del-dup */
#define HAMMER2_MODIFY_NOREALLOC	0x00000010
#define HAMMER2_MODIFY_INPLACE		0x00000020	/* don't del-dup */

/*
 * Flags passed to hammer2_chain_lock()
 */
#define HAMMER2_RESOLVE_NEVER		1
#define HAMMER2_RESOLVE_MAYBE		2
#define HAMMER2_RESOLVE_ALWAYS		3
#define HAMMER2_RESOLVE_MASK		0x0F

#define HAMMER2_RESOLVE_SHARED		0x10	/* request shared lock */
#define HAMMER2_RESOLVE_NOREF		0x20	/* already ref'd on lock */

/*
 * Flags passed to hammer2_chain_delete()
 */
#define HAMMER2_DELETE_UNUSED0001	0x0001

/*
 * Flags passed to hammer2_chain_delete_duplicate()
 */
#define HAMMER2_DELDUP_RECORE		0x0001

/*
 * Cluster different types of storage together for allocations
 */
#define HAMMER2_FREECACHE_INODE		0
#define HAMMER2_FREECACHE_INDIR		1
#define HAMMER2_FREECACHE_DATA		2
#define HAMMER2_FREECACHE_UNUSED3	3
#define HAMMER2_FREECACHE_TYPES		4

/*
 * hammer2_freemap_alloc() block preference
 */
#define HAMMER2_OFF_NOPREF		((hammer2_off_t)-1)

/*
 * BMAP read-ahead maximum parameters
 */
#define HAMMER2_BMAP_COUNT		16	/* max bmap read-ahead */
#define HAMMER2_BMAP_BYTES		(HAMMER2_PBUFSIZE * HAMMER2_BMAP_COUNT)

/*
 * Misc
 */
#define HAMMER2_FLUSH_DEPTH_LIMIT	10	/* stack recursion limit */

/*
 * hammer2_freemap_adjust()
 */
#define HAMMER2_FREEMAP_DORECOVER	1
#define HAMMER2_FREEMAP_DOMAYFREE	2
#define HAMMER2_FREEMAP_DOREALFREE	3

/*
 * HAMMER2 cluster - A set of chains representing the same entity.
 *
 * The hammer2_pfsmount structure embeds a hammer2_cluster.  All other
 * hammer2_cluster use cases use temporary allocations.
 *
 * The cluster API mimics the chain API.  Except as used in the pfsmount,
 * the cluster structure is a temporary 'working copy' of a set of chains
 * representing targets compatible with the operation.  However, for
 * performance reasons the cluster API does not necessarily issue concurrent
 * requests to the underlying chain API for all compatible chains all the
 * time.  This may sometimes necessitate revisiting parent cluster nodes
 * to 'flesh out' (validate more chains).
 *
 * If an insufficient number of chains remain in a working copy, the operation
 * may have to be downgraded, retried, or stall until the requisit number
 * of chains are available.
 */
#define HAMMER2_MAXCLUSTER	8

struct hammer2_cluster {
	int			status;		/* operational status */
	int			refs;		/* track for deallocation */
	struct hammer2_pfsmount	*pmp;
	uint32_t		flags;
	int			nchains;
	hammer2_chain_t		*focus;		/* current focus (or mod) */
	hammer2_chain_t		*array[HAMMER2_MAXCLUSTER];
	int			cache_index[HAMMER2_MAXCLUSTER];
};

typedef struct hammer2_cluster hammer2_cluster_t;

#define HAMMER2_CLUSTER_PFS	0x00000001	/* embedded in pfsmount */
#define HAMMER2_CLUSTER_INODE	0x00000002	/* embedded in inode */


RB_HEAD(hammer2_inode_tree, hammer2_inode);

/*
 * A hammer2 inode.
 *
 * NOTE: The inode's attribute CST which is also used to lock the inode
 *	 is embedded in the chain (chain.cst) and aliased w/ attr_cst.
 */
struct hammer2_inode {
	RB_ENTRY(hammer2_inode) rbnode;		/* inumber lookup (HL) */
	ccms_cst_t		topo_cst;	/* directory topology cst */
	struct hammer2_pfsmount	*pmp;		/* PFS mount */
	struct hammer2_inode	*pip;		/* parent inode */
	struct vnode		*vp;
	hammer2_cluster_t	cluster;
	struct lockf		advlock;
	hammer2_tid_t		inum;
	u_int			flags;
	u_int			refs;		/* +vpref, +flushref */
	uint8_t			comp_heuristic;
	hammer2_off_t		size;
	uint64_t		mtime;
};

typedef struct hammer2_inode hammer2_inode_t;

#define HAMMER2_INODE_MODIFIED		0x0001
#define HAMMER2_INODE_SROOT		0x0002	/* kmalloc special case */
#define HAMMER2_INODE_RENAME_INPROG	0x0004
#define HAMMER2_INODE_ONRBTREE		0x0008
#define HAMMER2_INODE_RESIZED		0x0010
#define HAMMER2_INODE_MTIME		0x0020

int hammer2_inode_cmp(hammer2_inode_t *ip1, hammer2_inode_t *ip2);
RB_PROTOTYPE2(hammer2_inode_tree, hammer2_inode, rbnode, hammer2_inode_cmp,
		hammer2_tid_t);

/*
 * inode-unlink side-structure
 */
struct hammer2_inode_unlink {
	TAILQ_ENTRY(hammer2_inode_unlink) entry;
	hammer2_inode_t	*ip;
};
TAILQ_HEAD(hammer2_unlk_list, hammer2_inode_unlink);

typedef struct hammer2_inode_unlink hammer2_inode_unlink_t;

/*
 * A hammer2 transaction and flush sequencing structure.
 *
 * This global structure is tied into hammer2_mount and is used
 * to sequence modifying operations and flushes.
 *
 * (a) Any modifying operations with sync_tid >= flush_tid will stall until
 *     all modifying operating with sync_tid < flush_tid complete.
 *
 *     The flush related to flush_tid stalls until all modifying operations
 *     with sync_tid < flush_tid complete.
 *
 * (b) Once unstalled, modifying operations with sync_tid > flush_tid are
 *     allowed to run.  All modifications cause modify/duplicate operations
 *     to occur on the related chains.  Note that most INDIRECT blocks will
 *     be unaffected because the modifications just overload the RBTREE
 *     structurally instead of actually modifying the indirect blocks.
 *
 * (c) The actual flush unstalls and RUNS CONCURRENTLY with (b), but only
 *     utilizes the chain structures with sync_tid <= flush_tid.  The
 *     flush will modify related indirect blocks and inodes in-place
 *     (rather than duplicate) since the adjustments are compatible with
 *     (b)'s RBTREE overloading
 *
 *     SPECIAL NOTE:  Inode modifications have to also propagate along any
 *		      modify/duplicate chains.  File writes detect the flush
 *		      and force out the conflicting buffer cache buffer(s)
 *		      before reusing them.
 *
 * (d) Snapshots can be made instantly but must be flushed and disconnected
 *     from their duplicative source before they can be mounted.  This is
 *     because while H2's on-media structure supports forks, its in-memory
 *     structure only supports very simple forking for background flushing
 *     purposes.
 *
 * TODO: Flush merging.  When fsync() is called on multiple discrete files
 *	 concurrently there is no reason to stall the second fsync.
 *	 The final flush that reaches to root can cover both fsync()s.
 *
 *     The chains typically terminate as they fly onto the disk.  The flush
 *     ultimately reaches the volume header.
 */
struct hammer2_trans {
	TAILQ_ENTRY(hammer2_trans) entry;
	struct hammer2_pfsmount *pmp;		/* might be NULL */
	struct hammer2_mount	*hmp_single;	/* if single-targetted */
	hammer2_tid_t		orig_tid;
	hammer2_tid_t		sync_tid;	/* effective transaction id */
	hammer2_tid_t		inode_tid;
	thread_t		td;		/* pointer */
	int			flags;
	int			blocked;
	uint8_t			inodes_created;
	uint8_t			dummy[7];
};

typedef struct hammer2_trans hammer2_trans_t;

#define HAMMER2_TRANS_ISFLUSH		0x0001	/* formal flush */
#define HAMMER2_TRANS_CONCURRENT	0x0002	/* concurrent w/flush */
#define HAMMER2_TRANS_BUFCACHE		0x0004	/* from bioq strategy write */
#define HAMMER2_TRANS_NEWINODE		0x0008	/* caller allocating inode */
#define HAMMER2_TRANS_ISALLOCATING	0x0010	/* in allocator */
#define HAMMER2_TRANS_PREFLUSH		0x0020	/* preflush state */

#define HAMMER2_FREEMAP_HEUR_NRADIX	4	/* pwr 2 PBUFRADIX-MINIORADIX */
#define HAMMER2_FREEMAP_HEUR_TYPES	8
#define HAMMER2_FREEMAP_HEUR		(HAMMER2_FREEMAP_HEUR_NRADIX * \
					 HAMMER2_FREEMAP_HEUR_TYPES)

#define HAMMER2_CLUSTER_COPY_CHAINS	0x0001	/* copy chains */
#define HAMMER2_CLUSTER_COPY_NOREF	0x0002	/* do not ref chains */

/*
 * Global (per device) mount structure for device (aka vp->v_mount->hmp)
 */
TAILQ_HEAD(hammer2_trans_queue, hammer2_trans);

struct hammer2_mount {
	struct vnode	*devvp;		/* device vnode */
	int		ronly;		/* read-only mount */
	int		pmp_count;	/* PFS mounts backed by us */
	TAILQ_ENTRY(hammer2_mount) mntentry; /* hammer2_mntlist */

	struct malloc_type *mchain;
	int		nipstacks;
	int		maxipstacks;
	struct spinlock	io_spin;	/* iotree access */
	struct hammer2_io_tree iotree;
	int		iofree_count;
	hammer2_chain_t vchain;		/* anchor chain (topology) */
	hammer2_chain_t fchain;		/* anchor chain (freemap) */
	hammer2_inode_t	*sroot;		/* super-root localized to media */
	struct lock	alloclk;	/* lockmgr lock */
	struct lock	voldatalk;	/* lockmgr lock */
	struct hammer2_trans_queue transq; /* all in-progress transactions */
	hammer2_off_t	heur_freemap[HAMMER2_FREEMAP_HEUR];
	int		flushcnt;	/* #of flush trans on the list */

	int		volhdrno;	/* last volhdrno written */
	hammer2_volume_data_t voldata;
	hammer2_volume_data_t volsync;	/* synchronized voldata */
};

typedef struct hammer2_mount hammer2_mount_t;

/*
 * HAMMER2 PFS mount point structure (aka vp->v_mount->mnt_data).
 * This has a 1:1 correspondence to struct mount (note that the
 * hammer2_mount structure has a N:1 correspondence).
 *
 * This structure represents a cluster mount and not necessarily a
 * PFS under a specific device mount (HMP).  The distinction is important
 * because the elements backing a cluster mount can change on the fly.
 *
 * Usually the first element under the cluster represents the original
 * user-requested mount that bootstraps the whole mess.  In significant
 * setups the original is usually just a read-only media image (or
 * representitive file) that simply contains a bootstrap volume header
 * listing the configuration.
 */
struct hammer2_pfsmount {
	struct mount		*mp;
	hammer2_cluster_t	cluster;
	hammer2_inode_t		*iroot;		/* PFS root inode */
	hammer2_inode_t		*ihidden;	/* PFS hidden directory */
	struct lock		lock;		/* PFS lock for certain ops */
	hammer2_off_t		inode_count;	/* copy of inode_count */
	ccms_domain_t		ccms_dom;
	struct netexport	export;		/* nfs export */
	int			ronly;		/* read-only mount */
	struct malloc_type	*minode;
	struct malloc_type	*mmsg;
	kdmsg_iocom_t		iocom;
	struct spinlock		inum_spin;	/* inumber lookup */
	struct hammer2_inode_tree inum_tree;
	long			inmem_inodes;
	long			inmem_dirty_chains;
	int			count_lwinprog;	/* logical write in prog */
	struct spinlock		unlinkq_spin;
	struct hammer2_unlk_list unlinkq;
	thread_t		wthread_td;	/* write thread td */
	struct bio_queue_head	wthread_bioq;	/* logical buffer bioq */
	struct mtx		wthread_mtx;	/* interlock */
	int			wthread_destroy;/* termination sequencing */
};

typedef struct hammer2_pfsmount hammer2_pfsmount_t;

#define HAMMER2_DIRTYCHAIN_WAITING	0x80000000
#define HAMMER2_DIRTYCHAIN_MASK		0x7FFFFFFF

#define HAMMER2_LWINPROG_WAITING	0x80000000
#define HAMMER2_LWINPROG_MASK		0x7FFFFFFF

#if defined(_KERNEL)

MALLOC_DECLARE(M_HAMMER2);

#define VTOI(vp)	((hammer2_inode_t *)(vp)->v_data)
#define ITOV(ip)	((ip)->vp)

/*
 * Currently locked chains retain the locked buffer cache buffer for
 * indirect blocks, and indirect blocks can be one of two sizes.  The
 * device buffer has to match the case to avoid deadlocking recursive
 * chains that might otherwise try to access different offsets within
 * the same device buffer.
 */
static __inline
int
hammer2_devblkradix(int radix)
{
	if (radix <= HAMMER2_LBUFRADIX) {
		return (HAMMER2_LBUFRADIX);
	} else {
		return (HAMMER2_PBUFRADIX);
	}
}

static __inline
size_t
hammer2_devblksize(size_t bytes)
{
	if (bytes <= HAMMER2_LBUFSIZE) {
		return(HAMMER2_LBUFSIZE);
	} else {
		KKASSERT(bytes <= HAMMER2_PBUFSIZE &&
			 (bytes ^ (bytes - 1)) == ((bytes << 1) - 1));
		return (HAMMER2_PBUFSIZE);
	}
}


static __inline
hammer2_pfsmount_t *
MPTOPMP(struct mount *mp)
{
	return ((hammer2_pfsmount_t *)mp->mnt_data);
}

extern struct vop_ops hammer2_vnode_vops;
extern struct vop_ops hammer2_spec_vops;
extern struct vop_ops hammer2_fifo_vops;

extern int hammer2_debug;
extern int hammer2_cluster_enable;
extern int hammer2_hardlink_enable;
extern int hammer2_flush_pipe;
extern int hammer2_synchronous_flush;
extern long hammer2_limit_dirty_chains;
extern long hammer2_iod_file_read;
extern long hammer2_iod_meta_read;
extern long hammer2_iod_indr_read;
extern long hammer2_iod_fmap_read;
extern long hammer2_iod_volu_read;
extern long hammer2_iod_file_write;
extern long hammer2_iod_meta_write;
extern long hammer2_iod_indr_write;
extern long hammer2_iod_fmap_write;
extern long hammer2_iod_volu_write;
extern long hammer2_ioa_file_read;
extern long hammer2_ioa_meta_read;
extern long hammer2_ioa_indr_read;
extern long hammer2_ioa_fmap_read;
extern long hammer2_ioa_volu_read;
extern long hammer2_ioa_file_write;
extern long hammer2_ioa_meta_write;
extern long hammer2_ioa_indr_write;
extern long hammer2_ioa_fmap_write;
extern long hammer2_ioa_volu_write;

extern struct objcache *cache_buffer_read;
extern struct objcache *cache_buffer_write;

extern int destroy;
extern int write_thread_wakeup;

extern mtx_t thread_protect;

/*
 * hammer2_subr.c
 */
#define hammer2_icrc32(buf, size)	iscsi_crc32((buf), (size))
#define hammer2_icrc32c(buf, size, crc)	iscsi_crc32_ext((buf), (size), (crc))

hammer2_cluster_t *hammer2_inode_lock_ex(hammer2_inode_t *ip);
hammer2_cluster_t *hammer2_inode_lock_sh(hammer2_inode_t *ip);
void hammer2_inode_unlock_ex(hammer2_inode_t *ip, hammer2_cluster_t *chain);
void hammer2_inode_unlock_sh(hammer2_inode_t *ip, hammer2_cluster_t *chain);
void hammer2_voldata_lock(hammer2_mount_t *hmp);
void hammer2_voldata_unlock(hammer2_mount_t *hmp, int modify);
ccms_state_t hammer2_inode_lock_temp_release(hammer2_inode_t *ip);
void hammer2_inode_lock_temp_restore(hammer2_inode_t *ip, ccms_state_t ostate);
ccms_state_t hammer2_inode_lock_upgrade(hammer2_inode_t *ip);
void hammer2_inode_lock_downgrade(hammer2_inode_t *ip, ccms_state_t ostate);

void hammer2_mount_exlock(hammer2_mount_t *hmp);
void hammer2_mount_shlock(hammer2_mount_t *hmp);
void hammer2_mount_unlock(hammer2_mount_t *hmp);

int hammer2_get_dtype(const hammer2_inode_data_t *ipdata);
int hammer2_get_vtype(const hammer2_inode_data_t *ipdata);
u_int8_t hammer2_get_obj_type(enum vtype vtype);
void hammer2_time_to_timespec(u_int64_t xtime, struct timespec *ts);
u_int64_t hammer2_timespec_to_time(const struct timespec *ts);
u_int32_t hammer2_to_unix_xid(const uuid_t *uuid);
void hammer2_guid_to_uuid(uuid_t *uuid, u_int32_t guid);

hammer2_key_t hammer2_dirhash(const unsigned char *name, size_t len);
int hammer2_getradix(size_t bytes);

int hammer2_calc_logical(hammer2_inode_t *ip, hammer2_off_t uoff,
			hammer2_key_t *lbasep, hammer2_key_t *leofp);
int hammer2_calc_physical(hammer2_inode_t *ip,
			const hammer2_inode_data_t *ipdata,
			hammer2_key_t lbase);
void hammer2_update_time(uint64_t *timep);
void hammer2_adjreadcounter(hammer2_blockref_t *bref, size_t bytes);

/*
 * hammer2_inode.c
 */
struct vnode *hammer2_igetv(hammer2_inode_t *ip, hammer2_cluster_t *cparent,
			int *errorp);
void hammer2_inode_lock_nlinks(hammer2_inode_t *ip);
void hammer2_inode_unlock_nlinks(hammer2_inode_t *ip);
hammer2_inode_t *hammer2_inode_lookup(hammer2_pfsmount_t *pmp,
			hammer2_tid_t inum);
hammer2_inode_t *hammer2_inode_get(hammer2_pfsmount_t *pmp,
			hammer2_inode_t *dip, hammer2_cluster_t *cluster);
void hammer2_inode_free(hammer2_inode_t *ip);
void hammer2_inode_ref(hammer2_inode_t *ip);
void hammer2_inode_drop(hammer2_inode_t *ip);
void hammer2_inode_repoint(hammer2_inode_t *ip, hammer2_inode_t *pip,
			hammer2_cluster_t *cluster);
void hammer2_run_unlinkq(hammer2_trans_t *trans, hammer2_pfsmount_t *pmp);

hammer2_inode_t *hammer2_inode_create(hammer2_trans_t *trans,
			hammer2_inode_t *dip,
			struct vattr *vap, struct ucred *cred,
			const uint8_t *name, size_t name_len,
			hammer2_cluster_t **clusterp, int *errorp);
int hammer2_inode_connect(hammer2_trans_t *trans,
			hammer2_cluster_t **clusterp, int hlink,
			hammer2_inode_t *dip, hammer2_cluster_t *dcluster,
			const uint8_t *name, size_t name_len,
			hammer2_key_t key);
hammer2_inode_t *hammer2_inode_common_parent(hammer2_inode_t *fdip,
			hammer2_inode_t *tdip);
void hammer2_inode_fsync(hammer2_trans_t *trans, hammer2_inode_t *ip,
			hammer2_cluster_t *cparent);
int hammer2_unlink_file(hammer2_trans_t *trans, hammer2_inode_t *dip,
			const uint8_t *name, size_t name_len, int isdir,
			int *hlinkp, struct nchandle *nch);
int hammer2_hardlink_consolidate(hammer2_trans_t *trans,
			hammer2_inode_t *ip, hammer2_cluster_t **clusterp,
			hammer2_inode_t *cdip, hammer2_cluster_t *cdcluster,
			int nlinks);
int hammer2_hardlink_deconsolidate(hammer2_trans_t *trans, hammer2_inode_t *dip,
			hammer2_chain_t **chainp, hammer2_chain_t **ochainp);
int hammer2_hardlink_find(hammer2_inode_t *dip, hammer2_cluster_t *cluster);
void hammer2_inode_install_hidden(hammer2_pfsmount_t *pmp);

/*
 * hammer2_chain.c
 */
void hammer2_modify_volume(hammer2_mount_t *hmp);
hammer2_chain_t *hammer2_chain_alloc(hammer2_mount_t *hmp,
				hammer2_pfsmount_t *pmp,
				hammer2_trans_t *trans,
				hammer2_blockref_t *bref);
void hammer2_chain_core_alloc(hammer2_trans_t *trans, hammer2_chain_t *nchain,
				hammer2_chain_t *ochain);
void hammer2_chain_ref(hammer2_chain_t *chain);
void hammer2_chain_drop(hammer2_chain_t *chain);
int hammer2_chain_lock(hammer2_chain_t *chain, int how);
void hammer2_chain_load_async(hammer2_cluster_t *cluster,
				void (*func)(hammer2_io_t *dio,
					     hammer2_cluster_t *cluster,
					     hammer2_chain_t *chain,
					     void *arg_p, off_t arg_o),
				void *arg_p);
void hammer2_chain_moved(hammer2_chain_t *chain);
void hammer2_chain_modify(hammer2_trans_t *trans,
				hammer2_chain_t **chainp, int flags);
void hammer2_chain_resize(hammer2_trans_t *trans, hammer2_inode_t *ip,
				hammer2_chain_t *parent,
				hammer2_chain_t **chainp,
				int nradix, int flags);
void hammer2_chain_unlock(hammer2_chain_t *chain);
void hammer2_chain_wait(hammer2_chain_t *chain);
hammer2_chain_t *hammer2_chain_get(hammer2_chain_t *parent, int generation,
				hammer2_blockref_t *bref);
hammer2_chain_t *hammer2_chain_lookup_init(hammer2_chain_t *parent, int flags);
void hammer2_chain_lookup_done(hammer2_chain_t *parent);
hammer2_chain_t *hammer2_chain_lookup(hammer2_chain_t **parentp,
				hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int *cache_indexp, int flags, int *ddflagp);
hammer2_chain_t *hammer2_chain_next(hammer2_chain_t **parentp,
				hammer2_chain_t *chain,
				hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int *cache_indexp, int flags);
hammer2_chain_t *hammer2_chain_scan(hammer2_chain_t *parent,
				hammer2_chain_t *chain,
				int *cache_indexp, int flags);

int hammer2_chain_create(hammer2_trans_t *trans,
				hammer2_chain_t **parentp,
				hammer2_chain_t **chainp,
				hammer2_key_t key, int keybits,
				int type, size_t bytes);
void hammer2_chain_duplicate(hammer2_trans_t *trans, hammer2_chain_t **parentp,
				hammer2_chain_t **chainp,
				hammer2_blockref_t *bref, int snapshot,
				int duplicate_reason);
int hammer2_chain_snapshot(hammer2_trans_t *trans, hammer2_chain_t **chainp,
				hammer2_ioc_pfs_t *pfs);
void hammer2_chain_delete(hammer2_trans_t *trans, hammer2_chain_t *chain,
				int flags);
void hammer2_chain_delete_duplicate(hammer2_trans_t *trans,
				hammer2_chain_t **chainp, int flags);
void hammer2_flush(hammer2_trans_t *trans, hammer2_chain_t **chainp);
void hammer2_chain_commit(hammer2_trans_t *trans, hammer2_chain_t *chain);
void hammer2_chain_setsubmod(hammer2_trans_t *trans, hammer2_chain_t *chain);
void hammer2_chain_countbrefs(hammer2_chain_t *chain,
				hammer2_blockref_t *base, int count);

void hammer2_pfs_memory_wait(hammer2_pfsmount_t *pmp);
void hammer2_pfs_memory_inc(hammer2_pfsmount_t *pmp);
void hammer2_pfs_memory_wakeup(hammer2_pfsmount_t *pmp);

int hammer2_base_find(hammer2_chain_t *chain,
				hammer2_blockref_t *base, int count,
				int *cache_indexp, hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int delete_filter);
void hammer2_base_delete(hammer2_trans_t *trans, hammer2_chain_t *chain,
				hammer2_blockref_t *base, int count,
				int *cache_indexp, hammer2_chain_t *child);
void hammer2_base_insert(hammer2_trans_t *trans, hammer2_chain_t *chain,
				hammer2_blockref_t *base, int count,
				int *cache_indexp, hammer2_chain_t *child);
void hammer2_chain_refactor(hammer2_chain_t **chainp);

/*
 * hammer2_trans.c
 */
void hammer2_trans_init(hammer2_trans_t *trans, hammer2_pfsmount_t *pmp,
				hammer2_mount_t *hmp, int flags);
void hammer2_trans_done(hammer2_trans_t *trans);

/*
 * hammer2_ioctl.c
 */
int hammer2_ioctl(hammer2_inode_t *ip, u_long com, void *data,
				int fflag, struct ucred *cred);

/*
 * hammer2_io.c
 */
hammer2_io_t *hammer2_io_getblk(hammer2_mount_t *hmp, off_t lbase,
				int lsize, int *ownerp);
void hammer2_io_putblk(hammer2_io_t **diop);
void hammer2_io_cleanup(hammer2_mount_t *hmp, struct hammer2_io_tree *tree);
char *hammer2_io_data(hammer2_io_t *dio, off_t lbase);
int hammer2_io_new(hammer2_mount_t *hmp, off_t lbase, int lsize,
				hammer2_io_t **diop);
int hammer2_io_newnz(hammer2_mount_t *hmp, off_t lbase, int lsize,
				hammer2_io_t **diop);
int hammer2_io_newq(hammer2_mount_t *hmp, off_t lbase, int lsize,
				hammer2_io_t **diop);
int hammer2_io_bread(hammer2_mount_t *hmp, off_t lbase, int lsize,
				hammer2_io_t **diop);
void hammer2_io_breadcb(hammer2_mount_t *hmp, off_t lbase, int lsize,
				void (*callback)(hammer2_io_t *dio,
						 hammer2_cluster_t *arg_l,
						 hammer2_chain_t *arg_c,
						 void *arg_p, off_t arg_o),
				hammer2_cluster_t *arg_l,
				hammer2_chain_t *arg_c,
				void *arg_p, off_t arg_o);
void hammer2_io_bawrite(hammer2_io_t **diop);
void hammer2_io_bdwrite(hammer2_io_t **diop);
int hammer2_io_bwrite(hammer2_io_t **diop);
int hammer2_io_isdirty(hammer2_io_t *dio);
void hammer2_io_setdirty(hammer2_io_t *dio);
void hammer2_io_setinval(hammer2_io_t *dio, u_int bytes);
void hammer2_io_brelse(hammer2_io_t **diop);
void hammer2_io_bqrelse(hammer2_io_t **diop);

/*
 * hammer2_msgops.c
 */
int hammer2_msg_dbg_rcvmsg(kdmsg_msg_t *msg);
int hammer2_msg_adhoc_input(kdmsg_msg_t *msg);

/*
 * hammer2_vfsops.c
 */
void hammer2_clusterctl_wakeup(kdmsg_iocom_t *iocom);
void hammer2_volconf_update(hammer2_pfsmount_t *pmp, int index);
void hammer2_cluster_reconnect(hammer2_pfsmount_t *pmp, struct file *fp);
void hammer2_dump_chain(hammer2_chain_t *chain, int tab, int *countp, char pfx);
void hammer2_bioq_sync(hammer2_pfsmount_t *pmp);
int hammer2_vfs_sync(struct mount *mp, int waitflags);
void hammer2_lwinprog_ref(hammer2_pfsmount_t *pmp);
void hammer2_lwinprog_drop(hammer2_pfsmount_t *pmp);
void hammer2_lwinprog_wait(hammer2_pfsmount_t *pmp);

/*
 * hammer2_freemap.c
 */
int hammer2_freemap_alloc(hammer2_trans_t *trans, hammer2_chain_t *chain,
				size_t bytes);
void hammer2_freemap_adjust(hammer2_trans_t *trans, hammer2_mount_t *hmp,
				hammer2_blockref_t *bref, int how);

/*
 * hammer2_cluster.c
 */
u_int hammer2_cluster_bytes(hammer2_cluster_t *cluster);
uint8_t hammer2_cluster_type(hammer2_cluster_t *cluster);
const hammer2_media_data_t *hammer2_cluster_data(hammer2_cluster_t *cluster);
hammer2_media_data_t *hammer2_cluster_wdata(hammer2_cluster_t *cluster);
hammer2_cluster_t *hammer2_cluster_from_chain(hammer2_chain_t *chain);
int hammer2_cluster_modified(hammer2_cluster_t *cluster);
int hammer2_cluster_unlinked(hammer2_cluster_t *cluster);
int hammer2_cluster_duplicated(hammer2_cluster_t *cluster);
void hammer2_cluster_set_chainflags(hammer2_cluster_t *cluster, uint32_t flags);
void hammer2_cluster_bref(hammer2_cluster_t *cluster, hammer2_blockref_t *bref);
void hammer2_cluster_setsubmod(hammer2_trans_t *trans,
			hammer2_cluster_t *cluster);
hammer2_cluster_t *hammer2_cluster_alloc(hammer2_pfsmount_t *pmp,
			hammer2_trans_t *trans,
			hammer2_blockref_t *bref);
void hammer2_cluster_core_alloc(hammer2_trans_t *trans,
			hammer2_cluster_t *ncluster,
			hammer2_cluster_t *ocluster);
void hammer2_cluster_ref(hammer2_cluster_t *cluster);
void hammer2_cluster_drop(hammer2_cluster_t *cluster);
void hammer2_cluster_wait(hammer2_cluster_t *cluster);
int hammer2_cluster_lock(hammer2_cluster_t *cluster, int how);
void hammer2_cluster_replace(hammer2_cluster_t *dst, hammer2_cluster_t *src);
void hammer2_cluster_replace_locked(hammer2_cluster_t *dst,
			hammer2_cluster_t *src);
hammer2_cluster_t *hammer2_cluster_copy(hammer2_cluster_t *ocluster,
			int with_chains);
void hammer2_cluster_refactor(hammer2_cluster_t *cluster);
void hammer2_cluster_unlock(hammer2_cluster_t *cluster);
void hammer2_cluster_resize(hammer2_trans_t *trans, hammer2_inode_t *ip,
			hammer2_cluster_t *cparent, hammer2_cluster_t *cluster,
			int nradix, int flags);
hammer2_inode_data_t *hammer2_cluster_modify_ip(hammer2_trans_t *trans,
			hammer2_inode_t *ip, hammer2_cluster_t *cluster,
			int flags);
void hammer2_cluster_modify(hammer2_trans_t *trans, hammer2_cluster_t *cluster,
			int flags);
void hammer2_cluster_modsync(hammer2_cluster_t *cluster);
hammer2_cluster_t *hammer2_cluster_lookup_init(hammer2_cluster_t *cparent,
			int flags);
void hammer2_cluster_lookup_done(hammer2_cluster_t *cparent);
hammer2_cluster_t *hammer2_cluster_lookup(hammer2_cluster_t *cparent,
			hammer2_key_t *key_nextp,
			hammer2_key_t key_beg, hammer2_key_t key_end,
			int flags, int *ddflagp);
hammer2_cluster_t *hammer2_cluster_next(hammer2_cluster_t *cparent,
			hammer2_cluster_t *cluster,
			hammer2_key_t *key_nextp,
			hammer2_key_t key_beg, hammer2_key_t key_end,
			int flags);
hammer2_cluster_t *hammer2_cluster_scan(hammer2_cluster_t *cparent,
			hammer2_cluster_t *cluster, int flags);
int hammer2_cluster_create(hammer2_trans_t *trans, hammer2_cluster_t *cparent,
			hammer2_cluster_t **clusterp,
			hammer2_key_t key, int keybits, int type, size_t bytes);
void hammer2_cluster_duplicate(hammer2_trans_t *trans,
			hammer2_cluster_t *cparent, hammer2_cluster_t *cluster,
			hammer2_blockref_t *bref,
			int snapshot, int duplicate_reason);
void hammer2_cluster_delete_duplicate(hammer2_trans_t *trans,
			hammer2_cluster_t *cluster, int flags);
void hammer2_cluster_delete(hammer2_trans_t *trans, hammer2_cluster_t *cluster,
			int flags);
int hammer2_cluster_snapshot(hammer2_trans_t *trans,
			hammer2_cluster_t *ocluster, hammer2_ioc_pfs_t *pfs);

#endif /* !_KERNEL */
#endif /* !_VFS_HAMMER2_HAMMER2_H_ */
