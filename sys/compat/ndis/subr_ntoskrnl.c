/*
 * Copyright (c) 2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
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
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <sys/callout.h>
#include <sys/kernel.h>

#include <machine/clock.h>
#include <machine/bus_memio.h>
#include <machine/bus_pio.h>
#include <machine/bus.h>
#include <machine/stdarg.h>

#include <sys/bus.h>
#include <sys/rman.h>

#include <compat/ndis/pe_var.h>
#include <compat/ndis/resource_var.h>
#include <compat/ndis/ndis_var.h>
#include <compat/ndis/ntoskrnl_var.h>

#define __stdcall __attribute__((__stdcall__))
#define FUNC void(*)(void)

__stdcall static uint32_t ntoskrnl_unicode_equal(ndis_unicode_string *,
	ndis_unicode_string *, uint32_t);
__stdcall static void *ntoskrnl_iobuildsynchfsdreq(uint32_t, void *,
	void *, uint32_t, uint32_t *, void *, void *);
__stdcall static uint32_t ntoskrnl_iofcalldriver(void *, void *);
__stdcall static uint32_t ntoskrnl_waitforobj(void *, uint32_t,
	uint32_t, uint8_t, void *);
__stdcall static void ntoskrnl_initevent(void *, uint32_t, uint8_t);
__stdcall static void ntoskrnl_writereg_ushort(uint16_t *, uint16_t);
__stdcall static uint16_t ntoskrnl_readreg_ushort(uint16_t *);
__stdcall static void ntoskrnl_writereg_ulong(uint32_t *, uint32_t);
__stdcall static uint32_t ntoskrnl_readreg_ulong(uint32_t *);
__stdcall static void ntoskrnl_writereg_uchar(uint8_t *, uint8_t);
__stdcall static uint8_t ntoskrnl_readreg_uchar(uint8_t *);
__stdcall static int64_t _allmul(int64_t, int64_t);
__stdcall static int64_t _alldiv(int64_t, int64_t);
__stdcall static int64_t _allrem(int64_t, int64_t);
__stdcall static int64_t _allshr(int64_t, int);
__stdcall static int64_t _allshl(int64_t, int);
__stdcall static uint64_t _aullmul(uint64_t, uint64_t);
__stdcall static uint64_t _aulldiv(uint64_t, uint64_t);
__stdcall static uint64_t _aullrem(uint64_t, uint64_t);
__stdcall static uint64_t _aullshr(uint64_t, int);
__stdcall static uint64_t _aullshl(uint64_t, int);
__stdcall static void *ntoskrnl_allocfunc(uint32_t, size_t, uint32_t);
__stdcall static void ntoskrnl_freefunc(void *);
__stdcall static void ntoskrnl_init_lookaside(paged_lookaside_list *,
	lookaside_alloc_func *, lookaside_free_func *,
	uint32_t, size_t, uint32_t, uint16_t);
__stdcall static void ntoskrnl_delete_lookaside(paged_lookaside_list *);
__stdcall static void ntoskrnl_init_nplookaside(npaged_lookaside_list *,
	lookaside_alloc_func *, lookaside_free_func *,
	uint32_t, size_t, uint32_t, uint16_t);
__stdcall static void ntoskrnl_delete_nplookaside(npaged_lookaside_list *);
static slist_entry *ntoskrnl_push_slist(slist_entry *, slist_entry *);
static slist_entry *ntoskrnl_pop_slist(slist_entry *);
__stdcall static void dummy(void);

static struct mtx ntoskrnl_interlock;
static int ntoskrnl_inits = 0;

int
ntoskrnl_libinit()
{
	if (ntoskrnl_inits) {
		ntoskrnl_inits++;
		return(0);
	}

	mtx_init(&ntoskrnl_interlock, "ntoskrnllock", MTX_NETWORK_LOCK,
	    MTX_DEF | MTX_RECURSE);

	ntoskrnl_inits++;
	return(0);
}

int
ntoskrnl_libfini()
{
	if (ntoskrnl_inits != 1) {
		ntoskrnl_inits--;
		return(0);
	}

	mtx_destroy(&ntoskrnl_interlock);
	ntoskrnl_inits--;

	return(0);
}

__stdcall static uint32_t 
ntoskrnl_unicode_equal(str1, str2, casesensitive)
	ndis_unicode_string	*str1;
	ndis_unicode_string	*str2;
	uint32_t		casesensitive;
{
	char		*astr1 = NULL, *astr2 = NULL;
	int		rval = 1;

	ndis_unicode_to_ascii(str1->nus_buf, str2->nus_len, &astr1);
	ndis_unicode_to_ascii(str2->nus_buf, str2->nus_len, &astr2);

	if (casesensitive)
		rval = strcmp(astr1, astr2);
#ifdef notdef
	else
		rval = strcasecmp(astr1, astr2);
#endif

	free(astr1, M_DEVBUF);
	free(astr2, M_DEVBUF);

	return(rval);
}

__stdcall static void *
ntoskrnl_iobuildsynchfsdreq(func, dobj, buf, len, off, event, status)
	uint32_t		func;
	void			*dobj;
	void			*buf;
	uint32_t		len;
	uint32_t		*off;
	void			*event;
	void			*status;
{
	return(NULL);
}
	
__stdcall static uint32_t
ntoskrnl_iofcalldriver(dobj, irp)
	void			*dobj;
	void			*irp;
{
	return(0);
}

__stdcall static uint32_t
ntoskrnl_waitforobj(obj, reason, mode, alertable, timeout)
	void			*obj;
	uint32_t		reason;
	uint32_t		mode;
	uint8_t			alertable;
	void			*timeout;
{
	return(0);
}

__stdcall static void
ntoskrnl_initevent(event, type, state)
	void			*event;
	uint32_t		type;
	uint8_t			state;
{
	return;
}

__stdcall static void
ntoskrnl_writereg_ushort(reg, val)
	uint16_t		*reg;
	uint16_t		val;
{
	bus_space_write_2(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg, val);
	return;
}

__stdcall static uint16_t
ntoskrnl_readreg_ushort(reg)
	uint16_t		*reg;
{
	return(bus_space_read_2(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg));
}

__stdcall static void
ntoskrnl_writereg_ulong(reg, val)
	uint32_t		*reg;
	uint32_t		val;
{
	bus_space_write_4(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg, val);
	return;
}

__stdcall static uint32_t
ntoskrnl_readreg_ulong(reg)
	uint32_t		*reg;
{
	return(bus_space_read_4(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg));
}

__stdcall static uint8_t
ntoskrnl_readreg_uchar(reg)
	uint8_t			*reg;
{
	return(bus_space_read_1(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg));
}

__stdcall static void
ntoskrnl_writereg_uchar(reg, val)
	uint8_t			*reg;
	uint8_t			val;
{
	bus_space_write_1(I386_BUS_SPACE_MEM, 0x0, (uint32_t)reg, val);
	return;
}

__stdcall static int64_t
_allmul(a, b)
	int64_t			a;
	int64_t			b;
{
	return (a * b);
}

__stdcall static int64_t
_alldiv(a, b)
	int64_t			a;
	int64_t			b;
{
	return (a / b);
}

__stdcall static int64_t
_allrem(a, b)
	int64_t			a;
	int64_t			b;
{
	return (a % b);
}

__stdcall static uint64_t
_aullmul(a, b)
	uint64_t		a;
	uint64_t		b;
{
	return (a * b);
}

__stdcall static uint64_t
_aulldiv(a, b)
	uint64_t		a;
	uint64_t		b;
{
	return (a / b);
}

__stdcall static uint64_t
_aullrem(a, b)
	uint64_t		a;
	uint64_t		b;
{
	return (a % b);
}

__stdcall static int64_t
_allshl(a, b)
	int64_t			a;
	int			b;
{
	return (a << b);
}

__stdcall static uint64_t
_aullshl(a, b)
	uint64_t		a;
	int			b;
{
	return (a << b);
}

__stdcall static int64_t
_allshr(a, b)
	int64_t			a;
	int			b;
{
	return (a >> b);
}

__stdcall static uint64_t
_aullshr(a, b)
	uint64_t		a;
	int			b;
{
	return (a >> b);
}

__stdcall static void *
ntoskrnl_allocfunc(pooltype, size, tag)
	uint32_t		pooltype;
	size_t			size;
	uint32_t		tag;
{
	return(malloc(size, M_DEVBUF, M_NOWAIT));
}

__stdcall static void
ntoskrnl_freefunc(buf)
	void			*buf;
{
	free(buf, M_DEVBUF);
}

__stdcall static void
ntoskrnl_init_lookaside(lookaside, allocfunc, freefunc,
    flags, size, tag, depth)
	paged_lookaside_list	*lookaside;
	lookaside_alloc_func	*allocfunc;
	lookaside_free_func	*freefunc;
	uint32_t		flags;
	size_t			size;
	uint32_t		tag;
	uint16_t		depth;
{
	lookaside->nll_l.gl_size = size;
	lookaside->nll_l.gl_tag = tag;
	if (allocfunc == NULL)
		lookaside->nll_l.gl_allocfunc = ntoskrnl_allocfunc;
	else
		lookaside->nll_l.gl_allocfunc = allocfunc;

	if (freefunc == NULL)
		lookaside->nll_l.gl_freefunc = ntoskrnl_freefunc;
	else
		lookaside->nll_l.gl_freefunc = freefunc;

	return;
}

__stdcall static void
ntoskrnl_delete_lookaside(lookaside)
	paged_lookaside_list   *lookaside;
{
	return;
}

__stdcall static void
ntoskrnl_init_nplookaside(lookaside, allocfunc, freefunc,
    flags, size, tag, depth)
	npaged_lookaside_list	*lookaside;
	lookaside_alloc_func	*allocfunc;
	lookaside_free_func	*freefunc;
	uint32_t		flags;
	size_t			size;
	uint32_t		tag;
	uint16_t		depth;
{
	lookaside->nll_l.gl_size = size;
	lookaside->nll_l.gl_tag = tag;
	if (allocfunc == NULL)
		lookaside->nll_l.gl_allocfunc = ntoskrnl_allocfunc;
	else
		lookaside->nll_l.gl_allocfunc = allocfunc;

	if (freefunc == NULL)
		lookaside->nll_l.gl_freefunc = ntoskrnl_freefunc;
	else
		lookaside->nll_l.gl_freefunc = freefunc;

	return;
}

__stdcall static void
ntoskrnl_delete_nplookaside(lookaside)
	npaged_lookaside_list   *lookaside;
{
	return;
}

/*
 * Note: the interlocked slist push and pop routines are
 * declared to be _fastcall in Windows, which means they
 * use the _cdecl calling convention here.
 */
static slist_entry *
ntoskrnl_push_slist(head, entry)
	slist_entry		*head;
	slist_entry		*entry;
{
	slist_entry		*oldhead;
	mtx_lock(&ntoskrnl_interlock);
	oldhead = head->sl_next;
	entry->sl_next = head->sl_next;
	head->sl_next = entry;
	mtx_unlock(&ntoskrnl_interlock);
	return(oldhead);
}

static slist_entry *
ntoskrnl_pop_slist(head)
	slist_entry		*head;
{
	slist_entry		*first;
	mtx_lock(&ntoskrnl_interlock);
	first = head->sl_next;
	if (first != NULL)
		head->sl_next = first->sl_next;
	mtx_unlock(&ntoskrnl_interlock);
	return(first);
}

__stdcall static void
dummy()
{
	printf ("ntoskrnl dummy called...\n");
	return;
}


image_patch_table ntoskrnl_functbl[] = {
	{ "RtlEqualUnicodeString",	(FUNC)ntoskrnl_unicode_equal },
	{ "sprintf",			(FUNC)sprintf },
	{ "DbgPrint",			(FUNC)printf },
	{ "strncmp",			(FUNC)strncmp },
	{ "strcmp",			(FUNC)strcmp },
	{ "strncpy",			(FUNC)strncpy },
	{ "strcpy",			(FUNC)strcpy },
	{ "IofCallDriver",		(FUNC)ntoskrnl_iofcalldriver },
	{ "IoBuildSynchronousFsdRequest", (FUNC)ntoskrnl_iobuildsynchfsdreq },
	{ "KeWaitForSingleObject",	(FUNC)ntoskrnl_waitforobj },
	{ "KeInitializeEvent",		(FUNC)ntoskrnl_initevent },
	{ "_allmul",			(FUNC)_allmul },
	{ "_alldiv",			(FUNC)_alldiv },
	{ "_allrem",			(FUNC)_allrem },
	{ "_allshr",			(FUNC)_allshr },
	{ "_allshl",			(FUNC)_allshl },
	{ "_aullmul",			(FUNC)_aullmul },
	{ "_aulldiv",			(FUNC)_aulldiv },
	{ "_aullrem",			(FUNC)_aullrem },
	{ "_aullushr",			(FUNC)_aullshr },
	{ "_aullshl",			(FUNC)_aullshl },
	{ "WRITE_REGISTER_USHORT",	(FUNC)ntoskrnl_writereg_ushort },
	{ "READ_REGISTER_USHORT",	(FUNC)ntoskrnl_readreg_ushort },
	{ "WRITE_REGISTER_ULONG",	(FUNC)ntoskrnl_writereg_ulong },
	{ "READ_REGISTER_ULONG",	(FUNC)ntoskrnl_readreg_ulong },
	{ "READ_REGISTER_UCHAR",	(FUNC)ntoskrnl_readreg_uchar },
	{ "WRITE_REGISTER_UCHAR",	(FUNC)ntoskrnl_writereg_uchar },
	{ "ExInitializePagedLookasideList", (FUNC)ntoskrnl_init_lookaside },
	{ "ExDeletePagedLookasideList", (FUNC)ntoskrnl_delete_lookaside },
	{ "ExInitializeNPagedLookasideList", (FUNC)ntoskrnl_init_nplookaside },
	{ "ExDeleteNPagedLookasideList", (FUNC)ntoskrnl_delete_nplookaside },
	{ "InterlockedPopEntrySList",	(FUNC)ntoskrnl_pop_slist },
	{ "InterlockedPushEntrySList",	(FUNC)ntoskrnl_push_slist },

	/*
	 * This last entry is a catch-all for any function we haven't
	 * implemented yet. The PE import list patching routine will
	 * use it for any function that doesn't have an explicit match
	 * in this table.
	 */

	{ NULL, (FUNC)dummy },

	/* End of list. */

	{ NULL, NULL },
};
