/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@laposte.net>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _POSIX_SEM_H
#define _POSIX_SEM_H

#include <posix/thread.h>       /* For pse51_current_thread and
                                   pse51_thread_t definition. */
#include <posix/registry.h>     /* For assocq */

#if defined(__KERNEL__) && defined(CONFIG_XENO_OPT_PERVASIVE)

typedef struct {
    u_long uaddr;
    unsigned refcnt;
    pse51_assoc_t assoc;

#define assoc2usem(laddr) \
    ((pse51_usem_t *)((unsigned long) (laddr) - offsetof(pse51_usem_t, assoc)))
} pse51_usem_t;

extern pse51_assocq_t pse51_usems;

#endif /* __KERNEL__ && CONFIG_XENO_OPT_PERVASIVE */    

void pse51_sem_pkg_init(void);

void pse51_sem_pkg_cleanup(void);

#endif /* !_POSIX_SEM_H */
