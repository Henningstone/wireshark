/* packet-dcerpc-reg.c
 * Routines for SMB \\PIPE\\winreg packet disassembly
 * Copyright 2001, Tim Potter <tpot@samba.org>
 *
 * $Id: packet-dcerpc-reg.c,v 1.5 2002/06/21 04:59:04 tpot Exp $
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <epan/packet.h>
#include "packet-dcerpc.h"
#include "packet-dcerpc-reg.h"

/* Registry data types */

const value_string reg_datatypes[] = {
	{ REG_NONE, "REG_NONE" },
	{ REG_SZ, "REG_SZ" },
	{ REG_EXPAND_SZ, "REG_EXPAND_SZ" },
	{ REG_BINARY, "REG_BINARY" },
	{ REG_DWORD, "REG_DWORD" },
	{ REG_DWORD_LE, "REG_DWORD_LE" },
	{ REG_DWORD_BE, "REG_DWORD_BE" },
	{ REG_LINK, "REG_LINK" },
	{ REG_MULTI_SZ, "REG_MULTI_SZ" },
	{ REG_RESOURCE_LIST, "REG_RESOURCE_LIST" },
	{ REG_FULL_RESOURCE_DESCRIPTOR, "REG_FULL_RESOURCE_DESCRIPTOR" },
	{ REG_RESOURCE_REQUIREMENTS_LIST, "REG_RESOURCE_REQUIREMENTS_LIST" },
	{0, NULL }
};

static int proto_dcerpc_reg = -1;
static gint ett_dcerpc_reg = -1;

static e_uuid_t uuid_dcerpc_reg = {
        0x338cd001, 0x2244, 0x31f1,
        { 0xaa, 0xaa, 0x90, 0x00, 0x38, 0x00, 0x10, 0x03 }
};

static guint16 ver_dcerpc_reg = 1;

static dcerpc_sub_dissector dcerpc_reg_dissectors[] = {
        { REG_OPEN_HKCR, "OpenHKCR", NULL, NULL },
        { _REG_UNK_01, "Unknown01", NULL, NULL },
        { REG_OPEN_HKLM, "OpenHKLM", NULL, NULL },
        { _REG_UNK_03, "Unknown03", NULL, NULL },
        { REG_OPEN_HKU, "OpenHKU", NULL, NULL },
        { REG_CLOSE, "Close", NULL, NULL },
        { REG_CREATE_KEY, "CreateKey", NULL, NULL },
        { REG_DELETE_KEY, "DeleteKey", NULL, NULL },
        { REG_DELETE_VALUE, "DeleteValue", NULL, NULL },
        { REG_ENUM_KEY, "EnumKey", NULL, NULL },
        { REG_ENUM_VALUE, "EnumValue", NULL, NULL },
        { REG_FLUSH_KEY, "FlushKey", NULL, NULL },
        { REG_GET_KEY_SEC, "GetKeySecurity", NULL, NULL },
        { _REG_UNK_0D, "Unknown0d", NULL, NULL },
        { _REG_UNK_0E, "Unknown0e", NULL, NULL },
        { REG_OPEN_ENTRY, "OpenEntry", NULL, NULL },
        { REG_QUERY_KEY, "QueryKey", NULL, NULL },
        { REG_INFO, "Info", NULL, NULL },
        { _REG_UNK_12, "Unknown12", NULL, NULL },
        { _REG_UNK_13, "Unknown13", NULL, NULL },
        { _REG_UNK_14, "Unknown14", NULL, NULL },
        { REG_SET_KEY_SEC, "SetKeySecurity", NULL, NULL },
        { REG_CREATE_VALUE, "CreateValue", NULL, NULL },
        { _REG_UNK_17, "Unknown17", NULL, NULL },
        { REG_SHUTDOWN, "Shutdown", NULL, NULL },
        { REG_ABORT_SHUTDOWN, "AbortShutdown", NULL, NULL },
        { REG_UNK_1A, "Unknown1A", NULL, NULL },

        { 0, NULL, NULL,  NULL }
};

void 
proto_register_dcerpc_reg(void)
{
        static gint *ett[] = {
                &ett_dcerpc_reg
        };

        proto_dcerpc_reg = proto_register_protocol(
                "Microsoft Registry", "REG", "reg");

        proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_dcerpc_reg(void)
{
        /* Register protocol as dcerpc */

        dcerpc_init_uuid(proto_dcerpc_reg, ett_dcerpc_reg, &uuid_dcerpc_reg,
                         ver_dcerpc_reg, dcerpc_reg_dissectors);
}
