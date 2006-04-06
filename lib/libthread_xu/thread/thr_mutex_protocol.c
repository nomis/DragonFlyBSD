/*
 * Copyright (c) 1998 Daniel Eischen <eischen@vigrid.com>.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Daniel Eischen.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY DANIEL EISCHEN AND CONTRIBUTORS ``AS IS'' AND
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
 * $DragonFly: src/lib/libthread_xu/thread/thr_mutex_protocol.c,v 1.3 2006/04/06 13:03:09 davidxu Exp $
 */

#include "namespace.h"
#include <errno.h>
#include <pthread.h>
#include "un-namespace.h"

#include "thr_private.h"

int
_pthread_mutexattr_getprotocol(pthread_mutexattr_t *mattr, int *protocol)
{
	int ret = 0;

	if ((mattr == NULL) || (*mattr == NULL))
		ret = EINVAL;
	else
		*protocol = (*mattr)->m_protocol;

	return(ret);
}

int
_pthread_mutexattr_setprotocol(pthread_mutexattr_t *mattr, int protocol)
{
	int ret = 0;

	if ((mattr == NULL) || (*mattr == NULL) ||
	    (protocol < PTHREAD_PRIO_NONE) || (protocol > PTHREAD_PRIO_PROTECT))
		ret = EINVAL;
	else {
		(*mattr)->m_protocol = protocol;
		(*mattr)->m_ceiling = THR_MAX_PRIORITY;
	}
	return(ret);
}

__strong_reference(_pthread_mutexattr_getprotocol, pthread_mutexattr_getprotocol);
__strong_reference(_pthread_mutexattr_setprotocol, pthread_mutexattr_setprotocol);

