/* packet-ansi_tcap-template.c
 * Routines for ANSI TCAP
 * Copyright 2007 Anders Broman <anders.broman@ericsson.com>
 * Built from the gsm-map dissector Copyright 2004 - 2005, Anders Broman <anders.broman@ericsson.com>
 *
 * $Id$
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
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
 * References: T1.114
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/conversation.h>
#include <epan/oid_resolv.h>
#include <epan/emem.h>
#include <epan/asn1.h>

#include <stdio.h>
#include <string.h>
#include "packet-ber.h"
#include "packet-tcap.h"
#include "packet-ansi_tcap.h"
#include "epan/tcap-persistentdata.h"

#define PNAME  "ANSI Transaction Capabilities Application Part"
#define PSNAME "ANSI_TCAP"
#define PFNAME "ansi_tcap"

/* Initialize the protocol and registered fields */
int proto_ansi_tcap = -1;
static int hf_ansi_tcap_tag = -1;
static int hf_ansi_tcap_length = -1;
static int hf_ansi_tcap_data = -1;
static int hf_ansi_tcap_tid = -1;

int hf_ansi_tcapsrt_SessionId=-1;
int hf_ansi_tcapsrt_Duplicate=-1;
int hf_ansi_tcapsrt_BeginSession=-1;
int hf_ansi_tcapsrt_EndSession=-1;
int hf_ansi_tcapsrt_SessionTime=-1;

#include "packet-ansi_tcap-hf.c"

/* Initialize the subtree pointers */
static gint ett_tcap = -1;
static gint ett_param = -1;

static gint ett_otid = -1;
static gint ett_dtid = -1;
gint ett_ansi_tcap_stat = -1;

static struct tcapsrt_info_t * gp_tcapsrt_info;
static gboolean tcap_subdissector_used=FALSE;
static dissector_handle_t requested_subdissector_handle = NULL;

static struct tcaphash_context_t * gp_tcap_context=NULL;

#include "packet-ansi_tcap-ett.c"

#define MAX_SSN 254
static range_t *global_ssn_range;
static range_t *ssn_range;

gboolean g_ansi_tcap_HandleSRT=FALSE;
extern gboolean gtcap_PersistentSRT;
extern gboolean gtcap_DisplaySRT;
extern guint gtcap_RepetitionTimeout;
extern guint gtcap_LostTimeout;

/* static dissector_handle_t	tcap_handle = NULL; */
static dissector_table_t ber_oid_dissector_table=NULL;
static const char * cur_oid;
static const char * tcapext_oid;
static proto_tree * tcap_top_tree=NULL;
static proto_tree * tcap_stat_tree=NULL;
static proto_item * tcap_stat_item=NULL;

static dissector_handle_t data_handle;
static dissector_handle_t ansi_map_handle;

static dissector_table_t sccp_ssn_table;

static GHashTable* ansi_sub_dissectors = NULL;
static GHashTable* itu_sub_dissectors = NULL;

struct ansi_tcap_private_t ansi_tcap_private;

static void ansi_tcap_ctx_init(struct ansi_tcap_private_t *a_tcap_ctx) {
  memset(a_tcap_ctx, '\0', sizeof(*a_tcap_ctx));
  a_tcap_ctx->signature = ANSI_TCAP_CTX_SIGNATURE;
  ansi_tcap_private.oid_is_present = FALSE;
}

static void dissect_ansi_tcap(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree);
/*
extern void add_ansi_tcap_subdissector(guint32 ssn, dissector_handle_t dissector) {
    g_hash_table_insert(ansi_sub_dissectors,GUINT_TO_POINTER(ssn),dissector);
    dissector_add("sccp.ssn",ssn,tcap_handle);
}

extern void delete_ansi_tcap_subdissector(guint32 ssn, dissector_handle_t dissector _U_) {
    g_hash_table_remove(ansi_sub_dissectors,GUINT_TO_POINTER(ssn));
    dissector_delete("sccp.ssn",ssn,tcap_handle);
}

dissector_handle_t get_ansi_tcap_subdissector(guint32 ssn) {
    return g_hash_table_lookup(ansi_sub_dissectors,GUINT_TO_POINTER(ssn));
}
*/

/* As currently ANSI MAP is the only possible sub dissector this function
 *  must be improved to handle general cases.
 *
 * 
 *
 * TODO: 
 * 1)Handle national codes
 *     Design option
 *     - Create a ansi.tcap.national dissector table and have dissectors for
 *       national codes register there and let ansi tcap call them.
 * 2)Handle Private codes properly
 *     Design question
 *     Unclear how to differentiate between different private "code sets".
 *     Use SCCP SSN table as before? or a ansi.tcap.private dissector table?
 *
 */
static gboolean
find_tcap_subdisector(tvbuff_t *tvb, asn1_ctx_t *actx, proto_tree *tree){

	/* If "DialoguePortion objectApplicationId ObjectIDApplicationContext
	 * points to the subdissector this code can be used.
	 *
	if(ansi_tcap_private.d.oid_is_present){
		call_ber_oid_callback(ansi_tcap_private.objectApplicationId_oid, tvb, 0, actx-pinfo, tree);
		return TRUE;
	}
	*/
	if(ansi_tcap_private.d.OperationCode == 0){
		/* national */
		proto_tree_add_text(tree, tvb, 0, -1, 
			"Dissector for ANSI TCAP NATIONAL code:%u not implemented. Contact Wireshark developers if you want this supported",
			ansi_tcap_private.d.OperationCode_national);
		return FALSE;
	}else if(ansi_tcap_private.d.OperationCode == 1){
		/* private */
		if((ansi_tcap_private.d.OperationCode_private & 0x0900) != 0x0900){
			proto_tree_add_text(tree, tvb, 0, -1,
				"Dissector for ANSI TCAP PRIVATE code:%u not implemented. Contact Wireshark developers if you want this supported",
				ansi_tcap_private.d.OperationCode_private);
			return FALSE;
		}
	}
	/* This is abit of a hack as it assumes the private codes with a "family" of 0x09 is ANSI MAP
	 * Se TODO above.
	 * N.S0005-0 v 1.0 TCAP Formats and Procedures 5-16 Application Services
	 * 6.3.2 Component Portion
	 * The Operation Code is partitioned into an Operation Family followed by a
	 * Specifier associated with each Operation Family member. For TIA/EIA-41 the
	 * Operation Family is coded as decimal 9. Bit H of the Operation Family is always
	 * coded as 0.
	 */

	call_dissector(ansi_map_handle, tvb, actx->pinfo, tree);

	return FALSE;
}

#include "packet-ansi_tcap-fn.c"




static void
dissect_ansi_tcap(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree)
{
    proto_item		*item=NULL;
    proto_tree		*tree=NULL;
    proto_item		*stat_item=NULL;
    proto_tree		*stat_tree=NULL;
	gint			offset = 0;
    struct tcaphash_context_t * p_tcap_context;
    dissector_handle_t subdissector_handle;
	asn1_ctx_t asn1_ctx;

	asn1_ctx_init(&asn1_ctx, ASN1_ENC_BER, TRUE, pinfo);
	ansi_tcap_ctx_init(&ansi_tcap_private);

    tcap_top_tree = parent_tree;
    if (check_col(pinfo->cinfo, COL_PROTOCOL))
    {
		col_set_str(pinfo->cinfo, COL_PROTOCOL, "ANSI TCAP");
    }

    /* create display subtree for the protocol */
    if(parent_tree){
      item = proto_tree_add_item(parent_tree, proto_ansi_tcap, tvb, 0, -1, FALSE);
      tree = proto_item_add_subtree(item, ett_tcap);
      tcap_stat_item=item;
      tcap_stat_tree=tree;
    }
    cur_oid = NULL;
    tcapext_oid = NULL;

    pinfo->private_data = &ansi_tcap_private;
    gp_tcapsrt_info=tcapsrt_razinfo();
    tcap_subdissector_used=FALSE;
    gp_tcap_context=NULL;
    dissect_ansi_tcap_PackageType(FALSE, tvb, 0, &asn1_ctx, tree, -1);

    if (g_ansi_tcap_HandleSRT && !tcap_subdissector_used ) {
		if (gtcap_DisplaySRT && tree) {
			stat_item = proto_tree_add_text(tree, tvb, 0, 0, "Stat");
			PROTO_ITEM_SET_GENERATED(stat_item);
			stat_tree = proto_item_add_subtree(stat_item, ett_ansi_tcap_stat);
		}
		p_tcap_context=tcapsrt_call_matching(tvb, pinfo, stat_tree, gp_tcapsrt_info);
		ansi_tcap_private.context=p_tcap_context;

		/* If the current message is TCAP only,
		 * save the Application contexte name for the next messages
		 */
		if ( p_tcap_context && cur_oid && !p_tcap_context->oid_present ) {
			/* Save the application context and the sub dissector */
			ber_oid_dissector_table = find_dissector_table("ber.oid");
			strncpy(p_tcap_context->oid,cur_oid, LENGTH_OID);
			if ( (subdissector_handle = dissector_get_string_handle(ber_oid_dissector_table, cur_oid)) ) {
				p_tcap_context->subdissector_handle=subdissector_handle;
				p_tcap_context->oid_present=TRUE;
			}
		}
		if (g_ansi_tcap_HandleSRT && p_tcap_context && p_tcap_context->callback) {
			/* Callback fonction for the upper layer */
			(p_tcap_context->callback)(tvb, pinfo, stat_tree, p_tcap_context);
		}
	}
}


void
proto_reg_handoff_ansi_tcap(void)
{

	data_handle = find_dissector("data");
	ansi_map_handle = find_dissector("ansi_map");
	ber_oid_dissector_table = find_dissector_table("ber.oid");
}



void
proto_register_ansi_tcap(void)
{

/* Setup list of header fields  See Section 1.6.1 for details*/
    static hf_register_info hf[] = {
	{ &hf_ansi_tcap_tag,
		{ "Tag",           "tcap.msgtype",
		FT_UINT8, BASE_HEX, NULL, 0,
		"", HFILL }
	},
	{ &hf_ansi_tcap_length,
		{ "Length", "tcap.len",
		FT_UINT8, BASE_HEX, NULL, 0,
		"", HFILL }
	},
	{ &hf_ansi_tcap_data,
		{ "Data", "tcap.data",
		FT_BYTES, BASE_HEX, NULL, 0,
		"", HFILL }
	},
		{ &hf_ansi_tcap_tid,
		{ "Transaction Id", "tcap.tid",
		FT_BYTES, BASE_HEX, NULL, 0,
		"", HFILL }
	},
	/* Tcap Service Response Time */
	{ &hf_ansi_tcapsrt_SessionId,
	  { "Session Id",
	    "tcap.srt.session_id",
	    FT_UINT32, BASE_DEC, NULL, 0x0,
	    "", HFILL }
	},
	{ &hf_ansi_tcapsrt_BeginSession,
	  { "Begin Session",
	    "tcap.srt.begin",
	    FT_FRAMENUM, BASE_NONE, NULL, 0x0,
	    "SRT Begin of Session", HFILL }
	},
	{ &hf_ansi_tcapsrt_EndSession,
	  { "End Session",
	    "tcap.srt.end",
	    FT_FRAMENUM, BASE_NONE, NULL, 0x0,
	    "SRT End of Session", HFILL }
	},
	{ &hf_ansi_tcapsrt_SessionTime,
	  { "Session duration",
	    "tcap.srt.sessiontime",
	    FT_RELATIVE_TIME, BASE_NONE, NULL, 0x0,
	    "Duration of the TCAP session", HFILL }
	},
	{ &hf_ansi_tcapsrt_Duplicate,
	  { "Request Duplicate",
	    "tcap.srt.duplicate",
	    FT_UINT32, BASE_DEC, NULL, 0x0,
	    "", HFILL }
	},
#include "packet-ansi_tcap-hfarr.c"
    };

/* Setup protocol subtree array */
    static gint *ett[] = {
	&ett_tcap,
	&ett_param,
	&ett_otid,
	&ett_dtid,
	&ett_ansi_tcap_stat,
	#include "packet-ansi_tcap-ettarr.c"
    };

    /*static enum_val_t tcap_options[] = {
	{ "itu", "ITU",  ITU_TCAP_STANDARD },
	{ "ansi", "ANSI", ANSI_TCAP_STANDARD },
	{ NULL, NULL, 0 }
    };*/


/* Register the protocol name and description */
    proto_ansi_tcap = proto_register_protocol(PNAME, PSNAME, PFNAME);
	register_dissector("ansi_tcap", dissect_ansi_tcap, proto_ansi_tcap);

/* Required function calls to register the header fields and subtrees used */
    proto_register_field_array(proto_ansi_tcap, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));


}













