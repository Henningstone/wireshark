/* emem.h
 * Definitions for ethereal memory management and garbage collection
 * Ronnie Sahlberg 2005
 *
 * $Id$
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef __EMEM_H__
#define __EMEM_H__

/* Functions for handling memory allocation and garbage collection with 
 * a packet lifetime scope.
 * These functions are used to allocate memory that will only remain persistent
 * until ethereal starts dissecting the next packet in the list.
 * Everytime ethereal starts decoding the next packet all memory allocated
 * through these functions will be released back to the free pool.
 *
 * These functions are very fast and offer automatic garbage collection:
 * Everytime a new packet is dissected, all memory allocations done in
 * the previous packet is freed.
 */
/* Initialize packet-lifetime memory allocation pool. This function is called 
 * once when [t]ethereal is initialized to set up the required structures.
 */
void ep_init_chunk(void);

/* Allocate memory with a packet lifetime scope */
void *ep_alloc(size_t size);

/* Duplicate a string with a packet lifetime scope */
gchar* ep_strdup(const gchar* src);

/* Duplicate at most n characters of a string with a packet lifetime scope */
gchar* ep_strndup(const gchar* src, size_t len);

/* Duplicate a buffer with a packet lifetime scope */
guint8* ep_memdup(const guint8* src, size_t len);

/* Create a formated string with a packet lifetime scope */
gchar* ep_strdup_printf(const gchar* fmt, ...);

/* release all memory allocated in the previous packet dissector */
void ep_free_all(void);

#endif /* emem.h */
