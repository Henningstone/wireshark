/* packet-radius.c
 *
 * Routines for RADIUS packet disassembly
 * Copyright 1999 Johan Feyaerts
 * Changed 03/12/2003 Rui Carmo (http://the.taoofmac.com - added all 3GPP VSAs, some parsing)
 * Changed 07/2005 Luis Ontanon <luis.ontanon@gmail.com> - use FreeRADIUS' dictionary
 * Changed 10/2006 Alejandro Vaquero <alejandrovaquero@yahoo.com> - add Conversations support
 *
 * $Id$
 *
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
 *
 * References:
 *
 * RFC 2865 - Remote Authentication Dial In User Service (RADIUS)
 * RFC 2866 - RADIUS Accounting
 * RFC 2867 - RADIUS Accounting Modifications for Tunnel Protocol Support
 * RFC 2868 - RADIUS Attributes for Tunnel Protocol Support
 * RFC 2869 - RADIUS Extensions
 * RFC 3162 - RADIUS and IPv6
 * RFC 3576 - Dynamic Authorization Extensions to RADIUS
 *
 * See also
 *
 *	http://www.iana.org/assignments/radius-types
 */


/*
  TO (re)DO: (see svn rev 14786)
    - dissect_3gpp_ipv6_dns_servers()
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/report_err.h>
#include <epan/prefs.h>
#include <epan/crypt-md5.h>
#include <epan/sminmpec.h>
#include <epan/filesystem.h>
#include <epan/conversation.h>
#include <epan/tap.h>
#include <epan/addr_resolv.h>
#include <epan/emem.h>

#include "packet-radius.h"

typedef struct _e_radiushdr {
	guint8 rh_code;
	guint8 rh_ident;
	guint16 rh_pktlength;
} e_radiushdr;

typedef struct {
	GArray* hf;
	GArray* ett;
	GArray* vend_vs;
} hfett_t;

#define AUTHENTICATOR_LENGTH	16
#define RD_HDR_LENGTH		4
#define HDR_LENGTH		(RD_HDR_LENGTH + AUTHENTICATOR_LENGTH)

#define UDP_PORT_RADIUS		1645
#define UDP_PORT_RADIUS_NEW	1812
#define UDP_PORT_RADACCT	1646
#define UDP_PORT_RADACCT_NEW	1813

static radius_dictionary_t* dict = NULL;

static int proto_radius = -1;

static int hf_radius_req = -1;
static int hf_radius_rsp = -1;
static int hf_radius_req_frame = -1;
static int hf_radius_rsp_frame = -1;
static int hf_radius_time = -1;

static int hf_radius_dup = -1;
static int hf_radius_req_dup = -1;
static int hf_radius_rsp_dup = -1;

static int hf_radius_id = -1;
static int hf_radius_code = -1;
static int hf_radius_length = -1;
static int hf_radius_authenticator = -1;

static int hf_radius_framed_ip_address = -1;
static int hf_radius_login_ip_host = -1;
static int hf_radius_framed_ipx_network = -1;

static int hf_radius_cosine_vpi = -1;
static int hf_radius_cosine_vci = -1;

static gint ett_radius = -1;
static gint ett_radius_avp = -1;
static gint ett_eap = -1;

/*
 * Define the tap for radius
 */
static int radius_tap = -1;

radius_vendor_info_t no_vendor = {"Unknown Vendor",0,NULL,-1};

radius_attr_info_t no_dictionary_entry = {"Unknown-Attribute",0,FALSE,FALSE,radius_octets, NULL, NULL, -1, -1, -1, -1, -1 };

dissector_handle_t eap_handle;
dissector_handle_t radius_handle;

static const gchar* shared_secret = "";
static gboolean show_length = FALSE;
static guint alt_port = 0;
static guint alt_port_pref = 0;

static guint8 authenticator[AUTHENTICATOR_LENGTH];

static const value_string* radius_vendors = NULL;

static radius_info_t rad_info;

static const value_string radius_vals[] =
{
	{RADIUS_ACCESS_REQUEST,		"Access-Request"},
	{RADIUS_ACCESS_ACCEPT,		"Access-Accept"},
	{RADIUS_ACCESS_REJECT,		"Access-Reject"},
	{RADIUS_ACCOUNTING_REQUEST,		"Accounting-Request"},
	{RADIUS_ACCOUNTING_RESPONSE,		"Accounting-Response"},
	{RADIUS_ACCOUNTING_STATUS,		"Accounting-Status"},
	{RADIUS_ACCESS_PASSWORD_REQUEST,	"Access-Password-Request"},
	{RADIUS_ACCESS_PASSWORD_ACK,		"Access-Password-Ack"},
	{RADIUS_ACCESS_PASSWORD_REJECT,	"Access-Password-Reject"},
	{RADIUS_ACCOUNTING_MESSAGE,		"Accounting-Message"},
	{RADIUS_ACCESS_CHALLENGE,		"Access-challenge"},
	{RADIUS_STATUS_SERVER,		"StatusServer"},
	{RADIUS_STATUS_CLIENT,		"StatusClient"},
	{RADIUS_VENDOR_SPECIFIC_CODE,		"Vendor-Specific"},
	{RADIUS_ASCEND_ACCESS_NEXT_CODE,	"Ascend-Access-Next-Code"},
	{RADIUS_ASCEND_ACCESS_NEW_PIN,	"Ascend-Access-New-Pin"},
	{RADIUS_ASCEND_PASSWORD_EXPIRED,	"Ascend-Password-Expired"},
	{RADIUS_ASCEND_ACCESS_EVENT_REQUEST,	"Ascend-Access-Event-Request"},
	{RADIUS_ASCEND_ACCESS_EVENT_RESPONSE,	"Ascend-Access-Event-Response"},
	{RADIUS_DISCONNECT_REQUEST,		"Disconnect-Request"},
	{RADIUS_DISCONNECT_REQUEST_ACK,	"Disconnect-Request ACK"},
	{RADIUS_DISCONNECT_REQUEST_NAK,	"Disconnect-Request NAK"},
	{RADIUS_CHANGE_FILTER_REQUEST,	"Change-Filter-Request"},
	{RADIUS_CHANGE_FILTER_REQUEST_ACK,	"Change-Filter-Request-ACK"},
	{RADIUS_CHANGE_FILTER_REQUEST_NAK,	"Change-Filter-Request-NAK"},
	{RADIUS_RESERVED,			"Reserved"},
	{0, NULL}
};

/*
 * Init Hash table stuff for converation
 */

typedef struct _radius_call_info_key
{
	guint code;
	guint ident;
	conversation_t *conversation;
	nstime_t req_time;
} radius_call_info_key;

static GMemChunk *radius_call_info_key_chunk;
static GMemChunk *radius_call_info_value_chunk;
static GHashTable *radius_calls;

/* Compare 2 keys */
static gint radius_call_equal(gconstpointer k1, gconstpointer k2)
{
	const radius_call_info_key* key1 = (const radius_call_info_key*) k1;
	const radius_call_info_key* key2 = (const radius_call_info_key*) k2;

	if (key1->ident == key2->ident && key1->conversation == key2->conversation) {
		nstime_t delta;

		nstime_delta(&delta, &key1->req_time, &key2->req_time);
		if (abs(nstime_to_sec(&delta)) > (double) 5) return 0;

		if (key1->code == key2->code)
			return 1;
		/* check the request and response are of the same code type */
		if (key1->code == RADIUS_ACCESS_REQUEST && ( key2->code == RADIUS_ACCESS_ACCEPT || key2->code == RADIUS_ACCESS_REJECT ) )
			return 1;

		if (key1->code == RADIUS_ACCOUNTING_REQUEST && key2->code == RADIUS_ACCOUNTING_RESPONSE )
			return 1;

		if (key1->code == RADIUS_ACCESS_PASSWORD_REQUEST && ( key2->code == RADIUS_ACCESS_PASSWORD_ACK || key2->code == RADIUS_ACCESS_PASSWORD_REJECT ) )
			return 1;

		if (key1->code == RADIUS_ASCEND_ACCESS_EVENT_REQUEST && key2->code == RADIUS_ASCEND_ACCESS_EVENT_RESPONSE )
			return 1;

		if (key1->code == RADIUS_DISCONNECT_REQUEST && ( key2->code == RADIUS_DISCONNECT_REQUEST_ACK || key2->code == RADIUS_DISCONNECT_REQUEST_NAK ) )
			return 1;

		if (key1->code == RADIUS_CHANGE_FILTER_REQUEST && ( key2->code == RADIUS_CHANGE_FILTER_REQUEST_ACK || key2->code == RADIUS_CHANGE_FILTER_REQUEST_NAK ) )
			return 1;
	} 
	return 0;
}

/* Calculate a hash key */
static guint radius_call_hash(gconstpointer k)
{
	const radius_call_info_key* key = (const radius_call_info_key*) k;

	return key->ident + /*key->code + */ key->conversation->index;
}


static const gchar *dissect_framed_ip_address(proto_tree* tree, tvbuff_t* tvb) {
	int len;
	guint32 ip;
	guint32 ip_h;
	const gchar *str;

	len = tvb_length(tvb);
	if (len != 4)
		return "[wrong length for IP address]";
	
	ip=tvb_get_ipv4(tvb,0);
	ip_h=g_ntohl(ip);

	if (ip_h == 0xFFFFFFFF) {
		str = "Negotiated";
		proto_tree_add_ipv4_format(tree, hf_radius_framed_ip_address,
		    tvb, 0, len, ip, "Framed-IP-Address: %s", str);
	} else if (ip_h == 0xFFFFFFFE) {
		str = "Assigned";
		proto_tree_add_ipv4_format(tree, hf_radius_framed_ip_address,
		    tvb, 0, len, ip, "Framed-IP-Address: %s", str);
	} else {
		str = ip_to_str((guint8 *)&ip);
		proto_tree_add_ipv4_format(tree, hf_radius_framed_ip_address,
		    tvb, 0, len, ip, "Framed-IP-Address: %s (%s)",
		    get_hostname(ip), str);
	}

	return str;
}

static const gchar *dissect_login_ip_host(proto_tree* tree, tvbuff_t* tvb) {
	int len;
	guint32 ip;
	guint32 ip_h;
	const gchar *str;

	len = tvb_length(tvb);
	if (len != 4)
		return "[wrong length for IP address]";
	
	ip=tvb_get_ipv4(tvb,0);
	ip_h=g_ntohl(ip);

	if (ip_h == 0xFFFFFFFF) {
		str = "User-selected";
		proto_tree_add_ipv4_format(tree, hf_radius_login_ip_host,
		    tvb, 0, len, ip, "Login-IP-Host: %s", str);
	} else if (ip_h == 0) {
		str = "NAS-selected";
		proto_tree_add_ipv4_format(tree, hf_radius_login_ip_host,
		    tvb, 0, len, ip, "Login-IP-Host: %s", str);
	} else {
		str = ip_to_str((guint8 *)&ip);
		proto_tree_add_ipv4_format(tree, hf_radius_framed_ip_address,
		    tvb, 0, len, ip, "Login-IP-Host: %s (%s)",
		    get_hostname(ip), str);
	}

	return str;
}

static const gchar *dissect_framed_ipx_network(proto_tree* tree, tvbuff_t* tvb) {
	int len;
	guint32 net;
	const gchar *str;

	len = tvb_length(tvb);
	if (len != 4)
		return "[wrong length for IPX network]";
	
	net=tvb_get_ntohl(tvb,0);

	if (net == 0xFFFFFFFE)
		str = "NAS-selected";
	else
		str = ep_strdup_printf("0x%08X", net);
	proto_tree_add_ipxnet_format(tree, hf_radius_framed_ipx_network, tvb, 0,
	    len, net, "Framed-IPX-Network: %s", str);

	return str;
}

static const gchar* dissect_cosine_vpvc(proto_tree* tree, tvbuff_t* tvb) {
	guint vpi, vci;
	
	if ( tvb_length(tvb) != 4 )
		return "[Wrong Length for VP/VC AVP]";
	
	vpi = tvb_get_ntohs(tvb,0);
	vci = tvb_get_ntohs(tvb,2);
	
	proto_tree_add_uint(tree,hf_radius_cosine_vpi,tvb,0,2,vpi);
	proto_tree_add_uint(tree,hf_radius_cosine_vci,tvb,2,2,vci);

	return ep_strdup_printf("%u/%u",vpi,vci); 
}

static void
radius_decrypt_avp(gchar *dest,int dest_len,tvbuff_t *tvb,int offset,int length)
{
    md5_state_t md_ctx;
    md5_byte_t digest[16];
    int i;
    size_t totlen, returned_length;
    const guint8 *pd;
    guchar c;
	
    DISSECTOR_ASSERT(dest_len > 2);  /* \"\"\0 */
    dest[0] = '"';
    dest[1] = '\0';
    totlen = 1;
    dest_len -= 1; /* Need to add trailing \" */

    md5_init(&md_ctx);
    md5_append(&md_ctx,(const guint8*)shared_secret,strlen(shared_secret));
    md5_append(&md_ctx,authenticator, AUTHENTICATOR_LENGTH);
    md5_finish(&md_ctx,digest);
	
    pd = tvb_get_ptr(tvb,offset,length);
    for( i = 0 ; i < AUTHENTICATOR_LENGTH && i < length ; i++ ) {
		c = pd[i] ^ digest[i];
		if ( isprint(c) ) {
			returned_length = g_snprintf(&dest[totlen], dest_len-totlen,
				"%c",c);
			totlen += MIN(returned_length, dest_len-totlen-1);
		} else {
			returned_length = g_snprintf(&dest[totlen], dest_len-totlen,
				"\\%03o",c);
			totlen += MIN(returned_length, dest_len-totlen-1);
		}
    }
    while(i<length) {
		if ( isprint(pd[i]) ) {
			returned_length = g_snprintf(&dest[totlen], dest_len-totlen,
				"%c", pd[i]);
			totlen += MIN(returned_length, dest_len-totlen-1);
		} else {
			returned_length = g_snprintf(&dest[totlen], dest_len-totlen,
				"\\%03o", pd[i]);
			totlen += MIN(returned_length, dest_len-totlen-1);
		}
		i++;
    }
    g_snprintf(&dest[totlen], dest_len+1-totlen, "%c", '"');
}


void radius_integer(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	guint32 uint;

	switch (len) {
		case 2:
			uint = tvb_get_ntohs(tvb,offset);
			break;
		case 3:
			uint = tvb_get_ntoh24(tvb,offset);
			break;
		case 4:
			uint = tvb_get_ntohl(tvb,offset);
			break;
		case 8: {
			guint64 uint64 = tvb_get_ntoh64(tvb,offset);
			proto_tree_add_uint64(tree,a->hf64,tvb,offset,len,uint64);
			proto_item_append_text(avp_item, "%" PRIu64, uint64);
			return;
		}
		default:
			proto_item_append_text(avp_item, "[unhandled integer length(%u)]", len);
			return;
	}

	proto_tree_add_uint(tree,a->hf,tvb,offset,len,uint);

	if (a->vs) {
		proto_item_append_text(avp_item, "%s(%u)", val_to_str(uint, a->vs, "Unknown"),uint);
	} else {
		proto_item_append_text(avp_item, "%u", uint);
	}
}

void radius_string(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	if (a->encrypt) {
		if (*shared_secret == '\0') {
			proto_item_append_text(avp_item, "Encrypted");
			proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
		} else {
			gchar *buffer; 
			buffer=ep_alloc(1024); /* an AVP value can be at most 253 bytes */
			radius_decrypt_avp(buffer,1024,tvb,offset,len);
			proto_item_append_text(avp_item, "Decrypted: %s", buffer);
			proto_tree_add_string(tree, a->hf, tvb, offset, len, buffer);
		}
	} else {
		proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
		proto_item_append_text(avp_item, "%s", tvb_format_text(tvb, offset, len));
	}
}

void radius_octets(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
	proto_item_append_text(avp_item, "%s", tvb_bytes_to_str(tvb, offset, len));
}

void radius_ipaddr(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	guint32 ip;
	gchar buf[16];
	
	if (len != 4) {
		proto_item_append_text(avp_item, "[wrong length for IP address]");
		return;
	}
	
	ip=tvb_get_ipv4(tvb,offset);
	
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);

	ip_to_str_buf((guint8 *)&ip, buf);
	proto_item_append_text(avp_item, "%s", buf);
}

void radius_ipv6addr(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	struct e_in6_addr ipv6_buff;
	gchar txtbuf[256];

	if (len != 16) {
		proto_item_append_text(avp_item, "[wrong length for IPv6 address]");
		return;
	}
	
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
	
	tvb_get_ipv6(tvb, offset, &ipv6_buff);
	ip6_to_str_buf(&ipv6_buff, txtbuf);
	proto_item_append_text(avp_item, "%s", txtbuf);
}

void radius_ipxnet(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	guint32 net;
	
	if (len != 4) {
		proto_item_append_text(avp_item, "[wrong length for IPX network]");
		return;
	}
	
	net=tvb_get_ntohl(tvb,offset);
	
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);

	proto_item_append_text(avp_item, "0x%08X", net);
}

void radius_date(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	nstime_t time_ptr; 

	if (len != 4) {
		proto_item_append_text(avp_item, "[wrong length for timestamp]");
		return;
	}
	time_ptr.secs = tvb_get_ntohl(tvb,offset);
	time_ptr.nsecs = 0;
	
	proto_tree_add_time(tree, a->hf, tvb, offset, len, &time_ptr);
	proto_item_append_text(avp_item, "%s", abs_time_to_str(&time_ptr));
}

void radius_abinary(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
	proto_item_append_text(avp_item, "%s", tvb_bytes_to_str(tvb, offset, len));
}

void radius_ifid(radius_attr_info_t* a, proto_tree* tree, packet_info *pinfo _U_, tvbuff_t* tvb, int offset, int len, proto_item* avp_item) {	
	proto_tree_add_item(tree, a->hf, tvb, offset, len, FALSE);
	proto_item_append_text(avp_item, "%s", tvb_bytes_to_str(tvb, offset, len));
}

static void add_avp_to_tree(proto_tree* avp_tree, proto_item* avp_item, packet_info* pinfo, tvbuff_t* tvb, radius_attr_info_t* dictionary_entry, guint32 avp_length, guint32 offset) {
    proto_item* pi;

    if (dictionary_entry->tagged) {
        guint tag;
        
        if (avp_length < 3) {
            pi = proto_tree_add_text(avp_tree, tvb, offset,
                                0, "AVP too short for tag");
            PROTO_ITEM_SET_GENERATED(pi);
            return;
        }
        
        tag = tvb_get_guint8(tvb, offset);
        
        if (tag <=  0x1f) {
            proto_tree_add_uint(avp_tree,
                                dictionary_entry->hf_tag,
                                tvb, offset, 1, tag);
            
            proto_item_append_text(avp_item,
                                   " Tag=0x%.2x", tag);
            
            offset++;
            avp_length--;
        }
    }
    
    if ( dictionary_entry->dissector ) {
        tvbuff_t* tvb_value;
        const gchar* str;
        
        tvb_value = tvb_new_subset(tvb, offset, avp_length, (gint) avp_length);
        
        str = dictionary_entry->dissector(avp_tree,tvb_value);
        
        proto_item_append_text(avp_item, ": %s",str);
    } else {
        proto_item_append_text(avp_item, ": ");
        
        dictionary_entry->type(dictionary_entry,avp_tree,pinfo,tvb,offset,avp_length,avp_item);
    }
}

static void dissect_attribute_value_pairs(proto_tree *tree, packet_info *pinfo, tvbuff_t *tvb, int offset, guint length) {
    proto_item* item;
    gboolean last_eap = FALSE;
    guint8* eap_buffer = NULL;
    guint eap_seg_num = 0;
    guint eap_tot_len_captured = 0;
    guint eap_tot_len = 0;
    proto_tree* eap_tree = NULL;
    tvbuff_t* eap_tvb = NULL;

    /*
     * In case we throw an exception, clean up whatever stuff we've
     * allocated (if any).
     */
    CLEANUP_PUSH(g_free, eap_buffer);

    while (length > 0) {
        radius_attr_info_t* dictionary_entry = NULL;
        gint tvb_len;
        guint32 avp_type;
        guint32 avp_length;
        guint32 vendor_id;

        proto_item* avp_item;
        proto_item* avp_len_item;
        proto_tree* avp_tree;
        
        if (length < 2) {
            item = proto_tree_add_text(tree, tvb, offset, 0,
                        "Not enough room in packet for AVP header");
            PROTO_ITEM_SET_GENERATED(item);
            return;
        }
        avp_type = tvb_get_guint8(tvb,offset);
        avp_length = tvb_get_guint8(tvb,offset+1);

        if (avp_length < 2) {
            item = proto_tree_add_text(tree, tvb, offset, 0,
                        "AVP too short: length %u < 2", avp_length);
            PROTO_ITEM_SET_GENERATED(item);
            return;
        }

        if (length < avp_length) {
            item = proto_tree_add_text(tree, tvb, offset, 0,
                        "Not enough room in packet for AVP");
            PROTO_ITEM_SET_GENERATED(item);
            return;
        }
        
        length -= avp_length;

        dictionary_entry = g_hash_table_lookup(dict->attrs_by_id,GUINT_TO_POINTER(avp_type));
            
        if (! dictionary_entry ) {
            dictionary_entry = &no_dictionary_entry;
        }
            
        avp_item = proto_tree_add_text(tree, tvb, offset, avp_length,
                                       "AVP: l=%u  t=%s(%u)", avp_length,
                                       dictionary_entry->name, avp_type);
        
        avp_length -= 2;
        offset += 2;

        if (avp_type == RADIUS_VENDOR_SPECIFIC_CODE) {
            radius_vendor_info_t* vendor;
            proto_tree* vendor_tree;
            gint max_offset = offset + avp_length;
            const gchar* vendor_str;

            /* XXX TODO: handle 2 byte codes for USR */
            
            if (avp_length < 4) {
                proto_item_append_text(avp_item, " [AVP too short; no room for vendor ID]");
                offset += avp_length;
                continue;
            }
            vendor_id = tvb_get_ntohl(tvb,offset);
            
            avp_length -= 4;
            offset += 4;
            
            vendor = g_hash_table_lookup(dict->vendors_by_id,GUINT_TO_POINTER(vendor_id));
            if (vendor) {
            	vendor_str = vendor->name;
            } else {
                vendor_str = val_to_str(vendor_id, sminmpec_values, "Unknown");
                vendor = &no_vendor;
            }
            proto_item_append_text(avp_item, " v=%s(%u)", vendor_str,
                                   vendor_id);
            
            vendor_tree = proto_item_add_subtree(avp_item,vendor->ett);
            
            while (offset < max_offset) {
                guint32 avp_vsa_type = tvb_get_guint8(tvb,offset++);
                guint32 avp_vsa_len = tvb_get_guint8(tvb,offset++);
                
                
                if (avp_vsa_len < 2) {
                    proto_tree_add_text(tree, tvb, offset+1, 1,
                                            "[VSA too short]");
                    return;
                }
                
                avp_vsa_len -= 2;
                
                dictionary_entry = g_hash_table_lookup(vendor->attrs_by_id,GUINT_TO_POINTER(avp_vsa_type));
                
                if ( !dictionary_entry ) {
                    dictionary_entry = &no_dictionary_entry;
                }
                
                avp_item = proto_tree_add_text(vendor_tree,tvb,offset-2,avp_vsa_len+2,
                                               "VSA: l=%u t=%s(%u)",
                                               avp_vsa_len+2, dictionary_entry->name, avp_vsa_type);
                
                avp_tree = proto_item_add_subtree(avp_item,dictionary_entry->ett);

                if (show_length) {
                    avp_len_item = proto_tree_add_uint(avp_tree,
                                                       dictionary_entry->hf_len,
                                                       tvb,0,0,avp_length);
                    PROTO_ITEM_SET_GENERATED(avp_len_item);
                }
                
                add_avp_to_tree(avp_tree, avp_item, pinfo, tvb, dictionary_entry, avp_vsa_len, offset);
                
                offset += avp_vsa_len;
            };
            continue;
        }

        avp_tree = proto_item_add_subtree(avp_item,dictionary_entry->ett);
        
        if (show_length) {
            avp_len_item = proto_tree_add_uint(avp_tree,
                                               dictionary_entry->hf_len,
                                               tvb,0,0,avp_length);
            PROTO_ITEM_SET_GENERATED(avp_len_item);
        }
        
        tvb_len = tvb_length_remaining(tvb, offset);
        
        if ((gint)avp_length < tvb_len)
            tvb_len = avp_length;
        
        if (avp_type == RADIUS_EAP_MESSAGE_CODE) {
            eap_seg_num++;

            /* Show this as an EAP fragment. */
            if (tree)
                proto_tree_add_text(avp_tree, tvb, offset, tvb_len,
                                    "EAP fragment");

            if (eap_tvb != NULL) {
                /*
                 * Oops, a non-consecutive EAP-Message
                 * attribute.
                 */
                proto_item_append_text(avp_item, " (non-consecutive)");
            } else {
                /*
                 * RFC 2869 says, in section 5.13, describing
                 * the EAP-Message attribute:
                 *
                 *    The NAS places EAP messages received
                 *    from the authenticating peer into one
                 *    or more EAP-Message attributes and
                 *    forwards them to the RADIUS Server
                 *    within an Access-Request message.
                 *    If multiple EAP-Messages are
                 *    contained within an Access-Request or
                 *    Access-Challenge packet, they MUST be
                 *    in order and they MUST be consecutive
                 *    attributes in the Access-Request or
                 *    Access-Challenge packet.
                 *
                 *        ...
                 *
                 *    The String field contains EAP packets,
                 *    as defined in [3].  If multiple
                 *    EAP-Message attributes are present
                 *    in a packet their values should be
                 *    concatenated; this allows EAP packets
                 *    longer than 253 octets to be passed
                 *    by RADIUS.
                 *
                 * Do reassembly of EAP-Message attributes.
                 * We just concatenate all the attributes,
                 * and when we see either the end of the
                 * attribute list or a non-EAP-Message
                 * attribute, we know we're done.
                 */

                if (eap_buffer == NULL)
                    eap_buffer = g_malloc(eap_tot_len_captured + tvb_len);
                else
                    eap_buffer = g_realloc(eap_buffer,
                                           eap_tot_len_captured + tvb_len);
                tvb_memcpy(tvb, eap_buffer + eap_tot_len_captured, offset,
                           tvb_len);
                eap_tot_len_captured += tvb_len;
                eap_tot_len += avp_length;

                if ( tvb_bytes_exist(tvb, offset + avp_length + 1, 1) ) {
                    guint8 next_type = tvb_get_guint8(tvb, offset + avp_length);
                
                    if ( next_type != RADIUS_EAP_MESSAGE_CODE ) {
                        /* Non-EAP-Message attribute */
                        last_eap = TRUE;
                    }
                } else {
                    /*
                     * No more attributes, either because
                     * we're at the end of the packet or
                     * because we're at the end of the
                     * captured packet data.
                     */
                    last_eap = TRUE;
                }

                if (last_eap && eap_buffer) {
                    gboolean save_writable;

                    proto_item_append_text(avp_item, " Last Segment[%u]",
                                           eap_seg_num);

                    eap_tree = proto_item_add_subtree(avp_item,ett_eap);
                
                    eap_tvb = tvb_new_real_data(eap_buffer,
                                                eap_tot_len_captured,
                                                eap_tot_len);
                    tvb_set_free_cb(eap_tvb, g_free);
                    tvb_set_child_real_data_tvbuff(tvb, eap_tvb);
                    add_new_data_source(pinfo, eap_tvb, "Reassembled EAP");

                    /*
                     * Don't free this when we're done -
                     * it's associated with a tvbuff.
                     */
                    eap_buffer = NULL;
    
                    /*
                     * Set the columns non-writable,
                     * so that the packet list shows
                     * this as an RADIUS packet, not
                     * as an EAP packet.
                     */
                    save_writable = col_get_writable(pinfo->cinfo);
                    col_set_writable(pinfo->cinfo, FALSE);

                    call_dissector(eap_handle, eap_tvb, pinfo, eap_tree);

                    col_set_writable(pinfo->cinfo, save_writable);
                } else {
                    proto_item_append_text(avp_item, " Segment[%u]",
                                           eap_seg_num);
                }
            }
            
            offset += avp_length;
        } else {
            add_avp_to_tree(avp_tree, avp_item, pinfo, tvb, dictionary_entry,
                            avp_length, offset);
            offset += avp_length;
        }
        
    }

    /*
     * Call the cleanup handler to free any reassembled data we haven't
     * attached to a tvbuff, and pop the handler.
     */
    CLEANUP_CALL_AND_POP;
}

static void dissect_radius(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	proto_tree *radius_tree = NULL;
	proto_tree *avptree = NULL;
	proto_item *ti;
	proto_item *avptf;
	guint rhlength;
	guint rhcode;
	guint rhident;
	guint avplength;
	e_radiushdr rh;

	conversation_t* conversation;
	radius_call_info_key radius_call_key;
	radius_call_info_key *new_radius_call_key = NULL;
	radius_call_t *radius_call = NULL;
	nstime_t delta;
	static address null_address = { AT_NONE, 0, NULL };

	/* Initialise stat info for passing to tap */
	rad_info.code = 0;
	rad_info.ident = 0;
	rad_info.req_time.secs = 0;
	rad_info.req_time.nsecs = 0;
	rad_info.is_duplicate = FALSE;
	rad_info.request_available = FALSE;
	rad_info.req_num = 0; /* frame number request seen */
	rad_info.rspcode = 0;

	if (check_col(pinfo->cinfo, COL_PROTOCOL))
		col_set_str(pinfo->cinfo, COL_PROTOCOL, "RADIUS");
	if (check_col(pinfo->cinfo, COL_INFO))
		col_clear(pinfo->cinfo, COL_INFO);
	
	tvb_memcpy(tvb,(guint8 *)&rh,0,sizeof(e_radiushdr));
	
	rhcode = rh.rh_code;
	rhident = rh.rh_ident;
	rhlength = g_ntohs(rh.rh_pktlength);
	/* XXX Check for valid length value:
		* Length
		*
		*  The Length field is two octets.  It indicates the length of the
		*  packet including the Code, Identifier, Length, Authenticator and
		*  Attribute fields.  Octets outside the range of the Length field
		*  MUST be treated as padding and ignored on reception.  If the
		*  packet is shorter than the Length field indicates, it MUST be
		*  silently discarded.  The minimum length is 20 and maximum length
		*  is 4096.
		*/
	
	/* tap stat info */
	rad_info.code = rhcode;
	rad_info.ident = rhident;

	if (check_col(pinfo->cinfo, COL_INFO))
	{
		col_add_fstr(pinfo->cinfo,COL_INFO,"%s(%d) (id=%d, l=%d)",
			     val_to_str(rhcode,radius_vals,"Unknown Packet"),
			     rhcode, rhident, rhlength);
	}
	
	if (tree)
	{
		ti = proto_tree_add_item(tree,proto_radius, tvb, 0, rhlength, FALSE);
		
		radius_tree = proto_item_add_subtree(ti, ett_radius);
		
		proto_tree_add_uint(radius_tree,hf_radius_code, tvb, 0, 1, rh.rh_code);
		
		proto_tree_add_uint_format(radius_tree,hf_radius_id, tvb, 1, 1, rh.rh_ident,
								   "Packet identifier: 0x%01x (%d)", rhident,rhident);
	}

	/*
	 * Make sure the length is sane.
	 */
	if (rhlength < HDR_LENGTH)
	{
		if (tree)
		{
			proto_tree_add_uint_format(radius_tree, hf_radius_length,
						   tvb, 2, 2, rhlength,
						   "Length: %u (bogus, < %u)",
						   rhlength, HDR_LENGTH);
		}
		return;
	}
	avplength = rhlength - HDR_LENGTH;
	if (tree)
	{
		proto_tree_add_uint(radius_tree, hf_radius_length, tvb,
				    2, 2, rhlength);
		
		proto_tree_add_item(radius_tree, hf_radius_authenticator, tvb, 4,AUTHENTICATOR_LENGTH,FALSE);
	}
	tvb_memcpy(tvb,authenticator,4,AUTHENTICATOR_LENGTH);

	if (tree) {

		/* Conversation support REQUEST/RESPONSES */
		switch (rhcode)
		{
			case RADIUS_ACCESS_REQUEST:
			case RADIUS_ACCOUNTING_REQUEST:
			case RADIUS_ACCESS_PASSWORD_REQUEST:
			case RADIUS_ASCEND_ACCESS_EVENT_REQUEST:
			case RADIUS_DISCONNECT_REQUEST:
			case RADIUS_CHANGE_FILTER_REQUEST:
				proto_tree_add_boolean_hidden(radius_tree, hf_radius_req, tvb, 0, 0, TRUE);
				/* Keep track of the address and port whence the call came
				 *  so that we can match up requests with replies.
				 * 
				 * Because it is UDP and the reply can come from any IP
				 * and port (not necessarly the request dest), we only 
				 * track the source IP and port of the request to match 
				 * the reply. 
				 */

				/*
				 * XXX - can we just use NO_ADDR_B?  Unfortunately,
				 * you currently still have to pass a non-null
				 * pointer for the second address argument even
				 * if you do that.
				 */
				conversation = find_conversation(pinfo->fd->num, &pinfo->src,
					                                 &null_address, pinfo->ptype, pinfo->srcport,
					                                 pinfo->destport, 0);
				if (conversation == NULL)
				{
					/* It's not part of any conversation - create a new one. */
					conversation = conversation_new(pinfo->fd->num, &pinfo->src,
					                                &null_address, pinfo->ptype, pinfo->srcport,
					                                pinfo->destport, 0);
				}

				/* Prepare the key data */
				radius_call_key.code = rhcode;
				radius_call_key.ident = rhident;
				radius_call_key.conversation = conversation;
				radius_call_key.req_time = pinfo->fd->abs_ts;

				/* Look up the request */
				radius_call = g_hash_table_lookup(radius_calls, &radius_call_key);
				if (radius_call != NULL)
				{
					/* We've seen a request with this ID, with the same 
					   destination, before - but was it *this* request? */
					if (pinfo->fd->num != radius_call->req_num)
					{
						/* No, so it's a duplicate request. Mark it as such. */
						rad_info.is_duplicate = TRUE;
						rad_info.req_num = radius_call->req_num;
						if (check_col(pinfo->cinfo, COL_INFO))
						{
							col_append_fstr(pinfo->cinfo, COL_INFO,
							                ", Duplicate Request ID:%u",
							                rhident);
						}
						if (tree)
						{
							proto_item* item;
							proto_tree_add_uint_hidden(radius_tree, hf_radius_dup, tvb, 0,0, rhident);
							item = proto_tree_add_uint(radius_tree, hf_radius_req_dup, tvb, 0,0, rhident);
							PROTO_ITEM_SET_GENERATED(item);
						}
					}
				}
				else
				{
					/* Prepare the value data.
					   "req_num" and "rsp_num" are frame numbers;
					   frame numbers are 1-origin, so we use 0
					   to mean "we don't yet know in which frame
					   the reply for this call appears". */
					new_radius_call_key = g_mem_chunk_alloc(radius_call_info_key_chunk);
					*new_radius_call_key = radius_call_key;
					radius_call = g_mem_chunk_alloc(radius_call_info_value_chunk);
					radius_call->req_num = pinfo->fd->num;
					radius_call->rsp_num = 0;
					radius_call->ident = rhident;
					radius_call->code = rhcode;
					radius_call->responded = FALSE;
					radius_call->req_time=pinfo->fd->abs_ts;
					radius_call->rspcode = 0;

					/* Store it */
					g_hash_table_insert(radius_calls, new_radius_call_key, radius_call);
				}
				if (radius_call && radius_call->rsp_num)
				{
					proto_item* item = proto_tree_add_uint_format(radius_tree, hf_radius_rsp_frame,
					                                              tvb, 0, 0, radius_call->rsp_num,
					                                              "The response to this request is in frame %u",
					                                              radius_call->rsp_num);
					PROTO_ITEM_SET_GENERATED(item);
				}
				break;
			case RADIUS_ACCESS_ACCEPT:
			case RADIUS_ACCESS_REJECT:
			case RADIUS_ACCOUNTING_RESPONSE:
			case RADIUS_ACCESS_PASSWORD_ACK:
			case RADIUS_ACCESS_PASSWORD_REJECT:
			case RADIUS_ASCEND_ACCESS_EVENT_RESPONSE:
			case RADIUS_DISCONNECT_REQUEST_ACK:
			case RADIUS_DISCONNECT_REQUEST_NAK:
			case RADIUS_CHANGE_FILTER_REQUEST_ACK:
			case RADIUS_CHANGE_FILTER_REQUEST_NAK:
				proto_tree_add_boolean_hidden(radius_tree, hf_radius_rsp, tvb, 0, 0, TRUE);
				/* Check for RADIUS response.  A response must match a call that
				 * we've seen, and the response must be sent to the same
				 * port and address that the call came from.
				 *
				 * Because it is UDP and the reply can come from any IP
				 * and port (not necessarly the request dest), we only 
				 * track the source IP and port of the request to match 
				 * the reply. 
				 */

				/* XXX - can we just use NO_ADDR_B?  Unfortunately,
				 * you currently still have to pass a non-null
				 * pointer for the second address argument even
				 * if you do that.
				 */
				conversation = find_conversation(pinfo->fd->num, &null_address,
					                                 &pinfo->dst, pinfo->ptype, pinfo->srcport,
					                                 pinfo->destport, 0);
				if (conversation != NULL)
				{
					/* Look only for matching request, if
					   matching conversation is available. */
					/* Prepare the key data */
					radius_call_key.code = rhcode;
					radius_call_key.ident = rhident;
					radius_call_key.conversation = conversation;
					radius_call_key.req_time = pinfo->fd->abs_ts;

					radius_call = g_hash_table_lookup(radius_calls, &radius_call_key);
					if (radius_call)
					{
						/* Indicate the frame to which this is a reply. */
						if (radius_call->req_num)
						{
							proto_item* item;
							rad_info.request_available = TRUE;
							rad_info.req_num = radius_call->req_num;
							radius_call->responded = TRUE;

							item = proto_tree_add_uint_format(radius_tree, hf_radius_req_frame,
							                                  tvb, 0, 0, radius_call->req_num,
							                                  "This is a response to a request in frame %u",
							                                  radius_call->req_num);
							PROTO_ITEM_SET_GENERATED(item);
							nstime_delta(&delta, &pinfo->fd->abs_ts, &radius_call->req_time);
							item = proto_tree_add_time(radius_tree, hf_radius_time, tvb, 0, 0, &delta);
							PROTO_ITEM_SET_GENERATED(item);
						}

						if (radius_call->rsp_num == 0)
						{
							/* We have not yet seen a response to that call, so
							   this must be the first response; remember its
							   frame number. */
							radius_call->rsp_num = pinfo->fd->num;
						}
						else
						{
							/* We have seen a response to this call - but was it
							   *this* response? (disregard provisional responses) */
							if ( (radius_call->rsp_num != pinfo->fd->num) && (radius_call->rspcode == rhcode) )
							{
								/* No, so it's a duplicate response. Mark it as such. */
								rad_info.is_duplicate = TRUE;
								if (check_col(pinfo->cinfo, COL_INFO))
								{
									col_append_fstr(pinfo->cinfo, COL_INFO,
									                ", Duplicate Response ID:%u",
									                rhident);
								}
								if (tree)
								{
									proto_item* item;
									proto_tree_add_uint_hidden(radius_tree, hf_radius_dup, tvb, 0,0, rhident);
									item = proto_tree_add_uint(radius_tree, hf_radius_rsp_dup,
									                           tvb, 0, 0, rhident);
									PROTO_ITEM_SET_GENERATED(item);
								}
							}
						}
						/* Now store the response code (after comparison above) */
						radius_call->rspcode = rhcode;
						rad_info.rspcode = rhcode;
					}
				}
				break;
			default:
				break;
		}
		
		if (radius_call)
		{
			rad_info.req_time.secs = radius_call->req_time.secs;
			rad_info.req_time.nsecs = radius_call->req_time.nsecs;
		}

		if (avplength > 0) {
			/* list the attribute value pairs */
			avptf = proto_tree_add_text(radius_tree, tvb, HDR_LENGTH,
							avplength, "Attribute Value Pairs");
			avptree = proto_item_add_subtree(avptf, ett_radius_avp);
				
			dissect_attribute_value_pairs(avptree, pinfo, tvb, HDR_LENGTH,
							  avplength);
		}
	}

	tap_queue_packet(radius_tap, pinfo, &rad_info);
}	


static void register_attrs(gpointer k _U_, gpointer v, gpointer p) {
	radius_attr_info_t* a = v;
	int i;
	gint* ett = &(a->ett);
	gchar* abbrev = g_strdup_printf("radius.%s",a->name);
	hf_register_info hfri[] = {
		{ NULL, { NULL,NULL, FT_NONE, BASE_NONE, NULL, 0x0, "", HFILL }},
		{ NULL, { NULL,NULL, FT_UINT8, BASE_DEC, NULL, 0x0, "", HFILL }},
		{ NULL, { NULL,NULL, FT_NONE, BASE_NONE, NULL, 0x0, "", HFILL }},
		{ NULL, { NULL,NULL, FT_NONE, BASE_NONE, NULL, 0x0, "", HFILL }} 
	};
	guint len_hf = 2;
	hfett_t* ri = p;
	
	for(i=0; abbrev[i]; i++) {
		if(abbrev[i] == '-') abbrev[i] = '_';
        if(abbrev[i] == '/') abbrev[i] = '_';
	}
	
	hfri[0].p_id = &(a->hf);
	hfri[1].p_id = &(a->hf_len);
	
	hfri[0].hfinfo.name = a->name;
	hfri[0].hfinfo.abbrev = abbrev;

	hfri[1].hfinfo.name = "Length";
	hfri[1].hfinfo.abbrev = g_strdup_printf("%s.len",abbrev);
	hfri[1].hfinfo.blurb = g_strdup_printf("%s Length",a->name);
	
	if (a->type == radius_integer) {
		hfri[0].hfinfo.type = FT_UINT32;
		hfri[0].hfinfo.display = BASE_DEC;

		hfri[2].p_id = &(a->hf64);
		hfri[2].hfinfo.name = g_strdup(a->name);
		hfri[2].hfinfo.abbrev = abbrev;
		hfri[2].hfinfo.type = FT_UINT64;
		hfri[2].hfinfo.display = BASE_DEC;
			
		if (a->vs) {
			hfri[0].hfinfo.strings = VALS(a->vs);
		}
		
		len_hf++;
		
	} else if (a->type == radius_string) {
		hfri[0].hfinfo.type = FT_STRING;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_octets) {
		hfri[0].hfinfo.type = FT_BYTES;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_ipaddr) {
		hfri[0].hfinfo.type = FT_IPv4;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_ipv6addr) {
		hfri[0].hfinfo.type = FT_IPv6;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_ipxnet) {
		hfri[0].hfinfo.type = FT_IPXNET;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_date) {
		hfri[0].hfinfo.type = FT_ABSOLUTE_TIME;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_abinary) {
		hfri[0].hfinfo.type = FT_BYTES;
		hfri[0].hfinfo.display = BASE_NONE;
	} else if (a->type == radius_ifid) {
		hfri[0].hfinfo.type = FT_BYTES;
		hfri[0].hfinfo.display = BASE_NONE;
	} else {
		hfri[0].hfinfo.type = FT_BYTES;
		hfri[0].hfinfo.display = BASE_NONE;
	}
	
	if (a->tagged) {
		hfri[len_hf].p_id = &(a->hf_tag);
		hfri[len_hf].hfinfo.name = "Tag";
		hfri[len_hf].hfinfo.abbrev = g_strdup_printf("%s.tag",abbrev);
		hfri[len_hf].hfinfo.blurb = g_strdup_printf("%s Tag",a->name);
		hfri[len_hf].hfinfo.type = FT_UINT8;
		hfri[len_hf].hfinfo.display = BASE_HEX;
		len_hf++;
	}
	
	g_array_append_vals(ri->hf,hfri,len_hf);
	g_array_append_val(ri->ett,ett);
	
}

static void register_vendors(gpointer k _U_, gpointer v, gpointer p) {
	radius_vendor_info_t* vnd = v;
	hfett_t* ri = p;
	value_string vnd_vs;
	gint* ett_p = &(vnd->ett);
    
	vnd_vs.value = vnd->code;
	vnd_vs.strptr = vnd->name;
	
	g_array_append_val(ri->vend_vs,vnd_vs);
	g_array_append_val(ri->ett,ett_p);

	g_hash_table_foreach(vnd->attrs_by_id,register_attrs,ri);
    
}

extern void radius_register_avp_dissector(guint32 vendor_id, guint32 attribute_id, radius_avp_dissector_t radius_avp_dissector) {
	radius_vendor_info_t* vendor;
	radius_attr_info_t* dictionary_entry;
	GHashTable* by_id;
	
	g_assert(radius_avp_dissector != NULL);
	
	if (vendor_id) {
		vendor = g_hash_table_lookup(dict->vendors_by_id,GUINT_TO_POINTER(vendor_id));
		
		if ( ! vendor ) {
			vendor = g_malloc(sizeof(radius_vendor_info_t));
			
			vendor->name = g_strdup_printf("%s-%u",val_to_str(vendor_id, sminmpec_values, "Unknown"),vendor_id);
			vendor->code = vendor_id;
			vendor->attrs_by_id = g_hash_table_new(g_direct_hash,g_direct_equal);
			vendor->ett = no_vendor.ett;
            
			g_hash_table_insert(dict->vendors_by_id,GUINT_TO_POINTER(vendor->code),vendor);
			g_hash_table_insert(dict->vendors_by_name,(gpointer)(vendor->name),vendor);
		}
		
		dictionary_entry = g_hash_table_lookup(vendor->attrs_by_id,GUINT_TO_POINTER(attribute_id));
		by_id = vendor->attrs_by_id;
	} else {
		dictionary_entry = g_hash_table_lookup(dict->attrs_by_id,GUINT_TO_POINTER(attribute_id));
		by_id = dict->attrs_by_id;
	}
	
	if (!dictionary_entry) {
		dictionary_entry = g_malloc(sizeof(radius_attr_info_t));;
		
		dictionary_entry->name = g_strdup_printf("Unknown-Attribute-%u",attribute_id);
		dictionary_entry->code = attribute_id;
		dictionary_entry->encrypt = FALSE;
		dictionary_entry->type = NULL;
		dictionary_entry->vs = NULL;
		dictionary_entry->hf = no_dictionary_entry.hf;
		dictionary_entry->hf_len = no_dictionary_entry.hf_len;
		dictionary_entry->ett = no_dictionary_entry.ett;
		
		g_hash_table_insert(by_id,GUINT_TO_POINTER(dictionary_entry->code),dictionary_entry);
	}
	
	dictionary_entry->dissector = radius_avp_dissector;

}

static void reinit_radius(void) {
	if ( alt_port_pref != alt_port ) {
		
		if (alt_port)
			dissector_delete("udp.port", alt_port, radius_handle);
		
		if (alt_port_pref)
			dissector_add("udp.port", alt_port_pref, radius_handle);
		
		alt_port = alt_port_pref;
	}	
}

/* Discard and init any state we've saved */
static void 
radius_init_protocol(void)
{
	if (radius_calls != NULL)
	{
		g_hash_table_destroy(radius_calls);
		radius_calls = NULL;
	}
	if (radius_call_info_key_chunk != NULL)
	{
		g_mem_chunk_destroy(radius_call_info_key_chunk);
		radius_call_info_key_chunk = NULL;
	}
	if (radius_call_info_value_chunk != NULL)
	{
		g_mem_chunk_destroy(radius_call_info_value_chunk);
		radius_call_info_value_chunk = NULL;
	}

	radius_calls = g_hash_table_new(radius_call_hash, radius_call_equal);
	radius_call_info_key_chunk = g_mem_chunk_new("call_info_key_chunk",
	                                           sizeof(radius_call_info_key),
	                                           200 * sizeof(radius_call_info_key),
	                                           G_ALLOC_ONLY);
	radius_call_info_value_chunk = g_mem_chunk_new("call_info_value_chunk",
	                                             sizeof(radius_call_t),
	                                             200 * sizeof(radius_call_t),
	                                             G_ALLOC_ONLY);
}

void
proto_register_radius(void)
{
	hf_register_info base_hf[] = {

	{ &hf_radius_req,
    { "Request", "radius.req", FT_BOOLEAN, BASE_NONE, NULL, 0x0,
		"TRUE if RADIUS request", HFILL }},

	{ &hf_radius_rsp,
	{ "Response", "radius.rsp", FT_BOOLEAN, BASE_NONE, NULL, 0x0,
		"TRUE if RADIUS response", HFILL }},

	{ &hf_radius_req_frame,
	{ "Request Frame", "radius.reqframe", FT_FRAMENUM, BASE_NONE, NULL, 0,
            "Request Frame", HFILL }},

	{ &hf_radius_rsp_frame,
	{ "Response Frame", "radius.rspframe", FT_FRAMENUM, BASE_NONE, NULL, 0,
            "Response Frame", HFILL }},

	{ &hf_radius_time,
	{ "Time from request", "radius.time", FT_RELATIVE_TIME, BASE_NONE, NULL, 0,
			"Timedelta between Request and Response", HFILL }},

	{ &hf_radius_code,
	{ "Code","radius.code", FT_UINT8, BASE_DEC, VALS(radius_vals), 0x0,
		"", HFILL }},
		
	{ &hf_radius_id,
	{ "Identifier",	"radius.id", FT_UINT8, BASE_DEC, NULL, 0x0,
		"", HFILL }},
		
	{ &hf_radius_authenticator,
	{ "Authenticator",	"radius.authenticator", FT_BYTES, BASE_HEX, NULL, 0x0,
		"", HFILL }},
		
	{ &hf_radius_length,
	{ "Length","radius.length", FT_UINT16, BASE_DEC, NULL, 0x0,
		"", HFILL }},
		
	{ &(no_dictionary_entry.hf),
	{ "Unknown-Attribute","radius.Unknown_Attribute", FT_BYTES, BASE_HEX, NULL, 0x0,
		"", HFILL }},

	{ &(no_dictionary_entry.hf_len),
	{ "Unknown-Attribute Length","radius.Unknown_Attribute.length", FT_UINT8, BASE_DEC, NULL, 0x0,
		"", HFILL }},
		
	{ &hf_radius_framed_ip_address,
	{ "Framed-IP-Address","radius.Framed-IP-Address", FT_IPv4, BASE_NONE, NULL, 0x0,
		"", HFILL }},

	{ &hf_radius_login_ip_host,
	{ "Login-IP-Host","radius.Login-IP-Host", FT_IPv4, BASE_NONE, NULL, 0x0,
		"", HFILL }},

	{ &hf_radius_framed_ipx_network,
	{ "Framed-IPX-Network","radius.Framed-IPX-Network", FT_IPXNET, BASE_NONE, NULL, 0x0,
		"", HFILL }},

	{ &hf_radius_cosine_vpi,
	{ "Cosine-VPI","radius.Cosine-Vpi", FT_UINT16, BASE_DEC, NULL, 0x0,
		"", HFILL }},

	{ &hf_radius_cosine_vci,
	{ "Cosine-VCI","radius.Cosine-Vci", FT_UINT16, BASE_DEC, NULL, 0x0,
		"", HFILL }},

	{ &hf_radius_dup,
	{ "Duplicate Message", "radius.dup", FT_UINT32, BASE_DEC, NULL, 0x0,
		"Duplicate Message", HFILL }},

	{ &hf_radius_req_dup,
	{ "Duplicate Request", "radius.req.dup", FT_UINT32, BASE_DEC, NULL, 0x0,
		"Duplicate Request", HFILL }},

	{ &hf_radius_rsp_dup,
	{ "Duplicate Response", "radius.rsp.dup", FT_UINT32, BASE_DEC, NULL, 0x0,
		"Duplicate Response", HFILL }},

	};
	
	gint *base_ett[] = {
		&ett_radius,
		&ett_radius_avp,
		&ett_eap,
		&(no_dictionary_entry.ett),
        &(no_vendor.ett),
	};
	
	module_t *radius_module;
	hfett_t ri;
	char* dir = NULL;
	gchar* dict_err_str = NULL;
	
	ri.hf = g_array_new(FALSE,TRUE,sizeof(hf_register_info));
	ri.ett = g_array_new(FALSE,TRUE,sizeof(gint *));
	ri.vend_vs = g_array_new(TRUE,TRUE,sizeof(value_string));
	
	g_array_append_vals(ri.hf, base_hf, array_length(base_hf));
	g_array_append_vals(ri.ett, base_ett, array_length(base_ett));
	
	dir = get_persconffile_path("radius", FALSE);
	
	if (test_for_directory(dir) != EISDIR) {
		/* Although dir isn't a directory it may still use memory */
		g_free(dir);
		
		dir = get_datafile_path("radius");
		
		if (test_for_directory(dir) != EISDIR) {
			g_free(dir);
			dir = NULL;
		}
	}
	
	if (dir) {
		dict = radius_load_dictionary(dir,"dictionary",&dict_err_str);
	} else {
		dict = NULL;
		dict_err_str = g_strdup("Could not find the radius directory");
	}
	
	g_free(dir);
    
	if (dict_err_str) {
		g_warning("radius: %s",dict_err_str);
		g_free(dict_err_str);
	}
	
	if (dict) {
		g_hash_table_foreach(dict->attrs_by_id,register_attrs,&ri);
		g_hash_table_foreach(dict->vendors_by_id,register_vendors,&ri);
	} else {
		/* XXX: TODO load a default dictionary */
		dict = g_malloc(sizeof(radius_dictionary_t));
		
		dict->attrs_by_id = g_hash_table_new(g_direct_hash,g_direct_equal);
		dict->attrs_by_name = g_hash_table_new(g_str_hash,g_str_equal);
		dict->vendors_by_id = g_hash_table_new(g_direct_hash,g_direct_equal);
		dict->vendors_by_name = g_hash_table_new(g_str_hash,g_str_equal);
	}
	
	radius_vendors = (value_string*) ri.vend_vs->data;
	
	proto_radius = proto_register_protocol("Radius Protocol", "RADIUS", "radius");
	register_dissector("radius", dissect_radius, proto_radius);
	
	proto_register_field_array(proto_radius,(hf_register_info*)(ri.hf->data),ri.hf->len);
	proto_register_subtree_array((gint**)(ri.ett->data), ri.ett->len);

	register_init_routine(&radius_init_protocol);

	g_array_free(ri.hf,FALSE);
	g_array_free(ri.ett,FALSE);
	g_array_free(ri.vend_vs,FALSE);
		
	radius_module = prefs_register_protocol(proto_radius,reinit_radius);
	prefs_register_string_preference(radius_module,"shared_secret","Shared Secret",
                                     "Shared secret used to decode User Passwords",
                                     &shared_secret);
	prefs_register_bool_preference(radius_module,"show_length","Show AVP Lengths",
                                     "Whether to add or not to the tree the AVP's payload length",
                                     &show_length);
    prefs_register_uint_preference(radius_module, "alternate_port","Alternate Port",
                                   "An alternate UDP port to decode as RADIUS", 10, &alt_port_pref);

    no_vendor.attrs_by_id = g_hash_table_new(g_direct_hash,g_direct_equal);
    
	radius_tap = register_tap("radius");
}

void
proto_reg_handoff_radius(void)
{
	
	eap_handle = find_dissector("eap");
	
	radius_handle = create_dissector_handle(dissect_radius, proto_radius);
	
	dissector_add("udp.port", UDP_PORT_RADIUS, radius_handle);
	dissector_add("udp.port", UDP_PORT_RADIUS_NEW, radius_handle);
	dissector_add("udp.port", UDP_PORT_RADACCT, radius_handle);
	dissector_add("udp.port", UDP_PORT_RADACCT_NEW, radius_handle);
	
	radius_register_avp_dissector(0,8,dissect_framed_ip_address);
	radius_register_avp_dissector(0,14,dissect_login_ip_host);
	radius_register_avp_dissector(0,23,dissect_framed_ipx_network);
	radius_register_avp_dissector(VENDOR_COSINE,5,dissect_cosine_vpvc);
	
}
