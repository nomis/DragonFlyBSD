PROG=	hammer2
SRCS=	main.c subs.c icrc.c
SRCS+=	cmd_remote.c cmd_snapshot.c cmd_pfs.c
SRCS+=	cmd_service.c cmd_leaf.c cmd_debug.c
SRCS+=	cmd_rsa.c cmd_stat.c cmd_setcomp.c cmd_setcheck.c
SRCS+=	cmd_bulkfree.c cmd_cleanup.c cmd_info.c cmd_destroy.c
SRCS+=	print_inode.c
MAN=	hammer2.8

CFLAGS+= -DXXH_NAMESPACE=h2_
CFLAGS+= -I${.CURDIR}/../../sys
CFLAGS+= -pthread
LDADD=	-ldmsg -lm -lutil
DPADD=	${LIBDMSG} ${LIBM} ${LIBUTIL}

DPADD+=		${LIBCRYPTO}
LDADD+=		-lprivate_crypto
LDFLAGS+=	${PRIVATELIB_LDFLAGS}
CFLAGS+=	-I${.CURDIR}/../../crypto/libressl/include

#.PATH: ${.CURDIR}/../../sys/libkern
#SRCS+= crc32.c
.PATH: ${.CURDIR}/../../sys/vfs/hammer2/xxhash
SRCS+= xxhash.c

.include <bsd.prog.mk>
