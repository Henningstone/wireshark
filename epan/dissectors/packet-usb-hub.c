/* packet-usb-hub.c
 * Routines for USB HUB dissection
 * Copyright 2009, Marton Nemeth <nm127@freemail.hu>
 *
 * $Id$
 *
 * USB HUB Specification can be found in the Universal Serial Bus
 * Specification 2.0, Chapter 11 Hub Specification.
 * http://www.usb.org/developers/docs/usb_20_052709.zip
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <epan/packet.h>
#include <string.h>
#include "packet-usb.h"

/* protocols and header fields */
static int proto_usb_hub = -1;

/* USB 2.0, Chapter 11.24.2 Class-Specific Requests */
static int hf_usb_hub_request = -1;
static int hf_usb_hub_value = -1;
static int hf_usb_hub_index = -1;
static int hf_usb_hub_length = -1;

static int hf_usb_hub_hub_feature_selector = -1;
static int hf_usb_hub_port_feature_selector = -1;
static int hf_usb_hub_dev_addr = -1;
static int hf_usb_hub_ep_num = -1;
static int hf_usb_hub_descriptor_type = -1;
static int hf_usb_hub_descriptor_index = -1;
static int hf_usb_hub_zero = -1;
static int hf_usb_hub_tt_flags = -1;
static int hf_usb_hub_tt_port = -1;
static int hf_usb_hub_tt_state_length = -1;
static int hf_usb_hub_port = -1;
static int hf_usb_hub_port_selector = -1;
static int hf_usb_hub_descriptor_length = -1;

static gint ett_usb_hub_wValue = -1;
static gint ett_usb_hub_wIndex = -1;
static gint ett_usb_hub_wLength = -1;

/* Table 11-16. Hub Class Request Codes */
#define USB_HUB_REQUEST_GET_STATUS           0
#define USB_HUB_REQUEST_CLEAR_FEATURE        1
#define USB_HUB_REQUEST_SET_FEATURE          3
#define USB_HUB_REQUEST_GET_DESCRIPTOR       6
#define USB_HUB_REQUEST_SET_DESCRIPTOR       7
#define USB_HUB_REQUEST_CLEAR_TT_BUFFER      8
#define USB_HUB_REQUEST_RESET_TT             9
#define USB_HUB_REQUEST_GET_TT_STATE         10
#define USB_HUB_REQUEST_STOP_TT              11

static const value_string setup_request_names_vals[] = {
	{ USB_HUB_REQUEST_GET_STATUS, "GET_STATUS" },
	{ USB_HUB_REQUEST_CLEAR_FEATURE, "CLEAR_FEATURE" },
	{ USB_HUB_REQUEST_SET_FEATURE, "SET_FEATURE" },
	{ USB_HUB_REQUEST_GET_DESCRIPTOR, "GET_DESCRIPTOR" },
	{ USB_HUB_REQUEST_SET_DESCRIPTOR, "SET_DESCRIPTOR" },
	{ USB_HUB_REQUEST_CLEAR_TT_BUFFER, "CLEAR_TT_BUFFER" },
	{ USB_HUB_REQUEST_GET_TT_STATE, "GET_TT_STATE" },
	{ USB_HUB_REQUEST_STOP_TT, "STOP_TT" },
	{ 0, NULL }
};


/* Table 11-17 Hub Class Feature Selectors */
#define USB_HUB_FEATURE_C_HUB_LOCAL_POWER     0
#define USB_HUB_FEATURE_C_HUB_OVER_CURRENT    1

#define USB_HUB_FEATURE_PORT_CONNECTION       0
#define USB_HUB_FEATURE_PORT_ENABLE           1
#define USB_HUB_FEATURE_PORT_SUSPEND          2
#define USB_HUB_FEATURE_PORT_OVER_CURRENT     3
#define USB_HUB_FEATURE_PORT_RESET            4
#define USB_HUB_FEATURE_PORT_POWER            8
#define USB_HUB_FEATURE_PORT_LOW_SPEED        9
#define USB_HUB_FEATURE_C_PORT_CONNECTION     16
#define USB_HUB_FEATURE_C_PORT_ENABLE         17
#define USB_HUB_FEATURE_C_PORT_SUSPEND        18
#define USB_HUB_FEATURE_C_PORT_OVER_CURRENT   19
#define USB_HUB_FEATURE_C_PORT_RESET          20
#define USB_HUB_FEATURE_PORT_TEST             21
#define USB_HUB_FEATURE_PORT_INDICATOR        22

static const value_string hub_class_feature_selectors_recipient_hub_vals[] = {
	{ USB_HUB_FEATURE_C_HUB_LOCAL_POWER, "C_HUB_LOCAL_POWER" },
	{ USB_HUB_FEATURE_C_HUB_OVER_CURRENT, "C_HUB_OVER_CURRENT" },
	{ 0, NULL }
};

static const value_string hub_class_feature_selectors_recipient_port_vals[] = {
	{ USB_HUB_FEATURE_PORT_CONNECTION, "PORT_CONNECTION" },
	{ USB_HUB_FEATURE_PORT_ENABLE, "PORT_ENABLE" },
	{ USB_HUB_FEATURE_PORT_SUSPEND, "PORT_SUSPEND" },
	{ USB_HUB_FEATURE_PORT_OVER_CURRENT, "PORT_OVER_CURRENT" },
	{ USB_HUB_FEATURE_PORT_RESET, "PORT_RESET" },
	{ USB_HUB_FEATURE_PORT_POWER, "PORT_POWER" },
	{ USB_HUB_FEATURE_PORT_LOW_SPEED, "PORT_LOW_SPEED" },
	{ USB_HUB_FEATURE_C_PORT_CONNECTION, "C_PORT_CONNECTION" },
	{ USB_HUB_FEATURE_C_PORT_ENABLE, "C_PORT_ENABLE" },
	{ USB_HUB_FEATURE_C_PORT_SUSPEND, "C_PORT_SUSPEND" },
	{ USB_HUB_FEATURE_C_PORT_OVER_CURRENT, "C_PORT_OVER_CURRENT" },
	{ USB_HUB_FEATURE_C_PORT_RESET, "C_PORT_RESET" },
	{ USB_HUB_FEATURE_PORT_TEST, "PORT_TEST" },
	{ USB_HUB_FEATURE_PORT_INDICATOR, "PORT_INDICATOR" },
	{ 0, NULL }
};


/* Dissector for ClearHubFeature, Chapter 11.24.2.1 Clear Hub Feature */
static void
dissect_usb_hub_clear_hub_feature(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_hub_feature_selector, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for ClearPortFeature, Chapter 11.24.2.2 Clear Port Feature */
static void
dissect_usb_hub_clear_port_feature(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_port_feature_selector, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_port, tvb, offset, 1, TRUE);
		offset++;
		proto_tree_add_item(subtree, hf_usb_hub_port_selector, tvb, offset, 1, TRUE);
		offset++;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for ClearTTBuffer, Chapter 11.24.2.3 Clear TT Buffer */
static void
dissect_usb_hub_clear_tt_buffer(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_ep_num, tvb, offset, 1, TRUE);
		offset++;
		proto_tree_add_item(subtree, hf_usb_hub_dev_addr, tvb, offset, 1, TRUE);
		offset++;

		proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_tt_port, tvb, offset, 2, TRUE);
		offset += 2;

		proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for GetHubDescriptor, Chapter 11.24.2.5 Get Hub Descriptor */
static void
dissect_usb_hub_get_hub_descriptor(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_index, tvb, offset, 1, TRUE);
		offset++;
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_type, tvb, offset, 1, TRUE);
		offset++;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_length, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for GetHubStatus, Chapter 11.24.2.6 Get Hub Status */
static void
dissect_usb_hub_get_hub_status(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		/* length shall always contain 4 */
		offset += 2;
	} else {
	}
}

/* Dissector for GetPortStatus, Chapter 11.24.2.7 Get Port Status */
static void
dissect_usb_hub_get_port_status(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_port, tvb, offset, 2, TRUE);
		offset += 2;

		proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		/* length shall always contain 4 */
		offset += 2;
	} else {
	}
}

/* Dissector for GetTTState, Chapter 11.24.2.8 Get_TT_State */
static void
dissect_usb_hub_get_tt_state(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_tt_flags, tvb, offset, 1, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_tt_port, tvb, offset, 1, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_tt_state_length, tvb, offset, 1, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for ResetTT, Chapter 11.24.2.9 Reset_TT */
static void
dissect_usb_hub_reset_tt(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 1, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_tt_port, tvb, offset, 1, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 1, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for SetHubDescriptor, Chapter 11.24.2.10 Set Hub Descriptor */
static void
dissect_usb_hub_set_hub_descriptor(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_index, tvb, offset, 1, TRUE);
		offset++;
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_type, tvb, offset, 1, TRUE);
		offset++;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_descriptor_length, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for StopTT, Chapter 11.24.2.11 Stop TT */
static void
dissect_usb_hub_stop_tt(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 1, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_tt_port, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for SetHubFeature, Chapter 11.24.2.12 Set Hub Feature */
static void
dissect_usb_hub_set_hub_feature(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_hub_feature_selector, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wIndex);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}

/* Dissector for SetPortFeature, Chapter 11.24.2.13 Set Port Feature */
static void
dissect_usb_hub_set_port_feature(packet_info *pinfo _U_, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info _U_, usb_conv_info_t *usb_conv_info _U_)
{
	proto_item *item = NULL;
	proto_tree *subtree = NULL;

	if (is_request) {
		item = proto_tree_add_item(tree, hf_usb_hub_value, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_port_feature_selector, tvb, offset, 2, TRUE);
		offset += 2;

		item = proto_tree_add_item(tree, hf_usb_hub_index, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wValue);
		proto_tree_add_item(subtree, hf_usb_hub_port, tvb, offset, 1, TRUE);
		offset++;
		proto_tree_add_item(subtree, hf_usb_hub_port_selector, tvb, offset, 1, TRUE);
		offset++;

		item = proto_tree_add_item(tree, hf_usb_hub_length, tvb, offset, 2, TRUE);
		subtree = proto_item_add_subtree(item, ett_usb_hub_wLength);
		proto_tree_add_item(subtree, hf_usb_hub_zero, tvb, offset, 2, TRUE);
		offset += 2;
	} else {
	}
}


typedef void (*usb_setup_dissector)(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, gboolean is_request, usb_trans_info_t *usb_trans_info, usb_conv_info_t *usb_conv_info);

typedef struct _usb_setup_dissector_table_t {
	guint8 request_type;
	guint8 request;
	usb_setup_dissector dissector;
} usb_setup_dissector_table_t;


/* USB 2.0, Table 11-15 Hub Class Requests */
static const usb_setup_dissector_table_t setup_dissectors[] = {
	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_DEVICE,
	  USB_HUB_REQUEST_CLEAR_FEATURE,
	  dissect_usb_hub_clear_hub_feature
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_CLEAR_FEATURE,
	  dissect_usb_hub_clear_port_feature
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_CLEAR_TT_BUFFER,
	  dissect_usb_hub_clear_tt_buffer
	},

	{ USB_DIR_IN | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_DEVICE,
	  USB_HUB_REQUEST_GET_DESCRIPTOR,
	  dissect_usb_hub_get_hub_descriptor
	},

	{ USB_DIR_IN | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_DEVICE,
	  USB_HUB_REQUEST_GET_STATUS,
	  dissect_usb_hub_get_hub_status
	},

	{ USB_DIR_IN | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_GET_STATUS,
	  dissect_usb_hub_get_port_status
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_RESET_TT,
	  dissect_usb_hub_reset_tt
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_DEVICE,
	  USB_HUB_REQUEST_SET_DESCRIPTOR,
	  dissect_usb_hub_set_hub_descriptor
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_DEVICE,
	  USB_HUB_REQUEST_SET_FEATURE,
	  dissect_usb_hub_set_hub_feature
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_SET_FEATURE,
	  dissect_usb_hub_set_port_feature
	},

	{ USB_DIR_IN | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_GET_TT_STATE,
	  dissect_usb_hub_get_tt_state
	},

	{ USB_DIR_OUT | (RQT_SETUP_TYPE_CLASS << 5) | RQT_SETUP_RECIPIENT_OTHER,
	  USB_HUB_REQUEST_STOP_TT,
	  dissect_usb_hub_stop_tt
	},

	{ 0, 0, NULL }
};

/* Dissector for USB HUB class-specific control request as defined in
 * USB 2.0, Chapter 11.24.2 Class-specific Requests
 * Returns TRUE if a class specific dissector was found
 * and FALSE otherwise.
 */
static gint
dissect_usb_hub_control(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	gboolean is_request;
	usb_conv_info_t *usb_conv_info;
	usb_trans_info_t *usb_trans_info;
	int offset = 0;
	usb_setup_dissector dissector;
	const usb_setup_dissector_table_t *tmp;

	is_request = (pinfo->srcport==NO_ENDPOINT);

	usb_conv_info = pinfo->usb_conv_info;
	usb_trans_info = usb_conv_info->usb_trans_info;

	/* See if we can find a class specific dissector for this request */
	dissector = NULL;

	/* Check valid values for bmRequestType and bRequest */
	for (tmp = setup_dissectors; tmp->dissector; tmp++) {
		if (tmp->request_type == usb_trans_info->requesttype &&
		    tmp->request == usb_trans_info->request) {
			dissector = tmp->dissector;
			break;
		}
	}
	/* No, we could not find any class specific dissector for this request
	 * return FALSE and let USB try any of the standard requests.
	 */
	if (!dissector) {
		return FALSE;
	}

	col_set_str(pinfo->cinfo, COL_PROTOCOL, "USBHUB");

	if (check_col(pinfo->cinfo, COL_INFO)) {
		col_clear(pinfo->cinfo, COL_INFO);
		col_append_fstr(pinfo->cinfo, COL_INFO, "%s %s",
		val_to_str(usb_trans_info->request, setup_request_names_vals, "Unknown type %x"),
			is_request ? "Request" : "Response");
	}

	if (is_request) {
		proto_tree_add_item(tree, hf_usb_hub_request, tvb, offset, 1, TRUE);
		offset += 1;
	}

	dissector(pinfo, tree, tvb, offset, is_request, usb_trans_info, usb_conv_info);
	return TRUE;
}

void
proto_register_usb_hub(void)
{
	static hf_register_info hf[] = {
		/* USB HUB specific requests */
		{ &hf_usb_hub_request,
		{ "bRequest", "usbhub.setup.bRequest", FT_UINT8, BASE_HEX, VALS(setup_request_names_vals), 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_value,
		{ "wValue", "usbhub.setup.wValue", FT_UINT16, BASE_HEX, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_index,
		{ "wIndex", "usbhub.setup.wIndex", FT_UINT16, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_length,
		{ "wLength", "usbhub.setup.wLength", FT_UINT16, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_hub_feature_selector,
		{ "HubFeatureSelector", "usbhub.setup.HubFeatureSelector", FT_UINT16, BASE_DEC,
		  VALS(hub_class_feature_selectors_recipient_hub_vals), 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_port_feature_selector,
		{ "PortFeatureSelector", "usbhub.setup.PortFeatureSelector", FT_UINT16, BASE_DEC,
		  VALS(hub_class_feature_selectors_recipient_port_vals), 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_dev_addr,
		{ "Dev_Addr", "usbhub.setup.Dev_Addr", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_ep_num,
		{ "EP_Num", "usbhub.setup.EP_Num", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_descriptor_type,
		{ "DescriptorType", "usbhub.setup.DescriptorType", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_descriptor_index,
		{ "DescriptorIndex", "usbhub.setup.DescriptorIndex", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_descriptor_length,
		{ "DescriptorLength", "usbhub.setup.DescriptorLength", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_zero,
		{ "(zero)", "usbhub.setup.zero", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_tt_flags,
		{ "TT_Flags", "usbhub.setup.TT_Flags", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_tt_port,
		{ "TT_Port", "usbhub.setup.TT_Port", FT_UINT16, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_tt_state_length,
		{ "TT State Length", "usbhub.setup.TT_StateLength", FT_UINT16, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_port_selector,
		{ "PortSelector", "usbhub.setup.PortSelector", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }},

		{ &hf_usb_hub_port,
		{ "Port", "usbhub.setup.Port", FT_UINT8, BASE_DEC, NULL, 0x0,
		  NULL, HFILL }}

	};

	static gint *usb_hub_subtrees[] = {
		&ett_usb_hub_wValue,
		&ett_usb_hub_wIndex,
		&ett_usb_hub_wLength
	};
	dissector_handle_t usb_hub_control_handle;

	proto_usb_hub = proto_register_protocol("USB HUB", "USBHUB", "usbhub");
	proto_register_field_array(proto_usb_hub, hf, array_length(hf));
	proto_register_subtree_array(usb_hub_subtrees, array_length(usb_hub_subtrees));

	usb_hub_control_handle = new_create_dissector_handle(dissect_usb_hub_control, proto_usb_hub);
	dissector_add("usb.control", IF_CLASS_HUB, usb_hub_control_handle);
	dissector_add("usb.control", IF_CLASS_UNKNOWN, usb_hub_control_handle);

}
