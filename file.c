/* file.c
 * File I/O routines
 *
 * $Id: file.c,v 1.154 2000/01/18 08:37:55 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
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
# include "config.h"
#endif

#include <gtk/gtk.h>

#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <time.h>

#ifdef HAVE_IO_H
#include <io.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#ifdef NEED_SNPRINTF_H
# ifdef HAVE_STDARG_H
#  include <stdarg.h>
# else
#  include <varargs.h>
# endif
# include "snprintf.h"
#endif

#ifdef NEED_STRERROR_H
#include "strerror.h"
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#include "gtk/main.h"
#include "column.h"
#include "packet.h"
#include "print.h"
#include "file.h"
#include "menu.h"
#include "util.h"
#include "simple_dialog.h"
#include "ui_util.h"
#include "gtk/proto_draw.h"
#include "dfilter.h"
#include "conversation.h"
#include "globals.h"

#include "plugins.h"

extern GtkWidget *packet_list, *prog_bar, *info_bar, *byte_view, *tree_view;
extern guint      file_ctx;

gboolean auto_scroll_live = FALSE;

static guint32 firstsec, firstusec;
static guint32 prevsec, prevusec;

static void wtap_dispatch_cb(u_char *, const struct wtap_pkthdr *, int,
    const u_char *);

static void freeze_clist(capture_file *cf);
static void thaw_clist(capture_file *cf);

static char *file_rename_error_message(int err);
static char *file_close_error_message(int err);

/* Update the progress bar this many times when reading a file. */
#define N_PROGBAR_UPDATES	100

int
open_cap_file(char *fname, gboolean is_tempfile, capture_file *cf)
{
  wtap       *wth;
  int         err;
  FILE_T      fh;
  int         fd;
  struct stat cf_stat;

  wth = wtap_open_offline(fname, &err);
  if (wth == NULL)
    goto fail;

  /* Find the size of the file. */
  fh = wtap_file(wth);
  fd = wtap_fd(wth);
  if (fstat(fd, &cf_stat) < 0) {
    err = errno;
    wtap_close(wth);
    goto fail;
  }

  /* The open succeeded.  Close whatever capture file we had open,
     and fill in the information for this file. */
  close_cap_file(cf, info_bar);

  /* Initialize the table of conversations. */
  conversation_init();

  /* Initialize protocol-specific variables */
  init_all_protocols();

  cf->wth = wth;
  cf->fh = fh;
  cf->filed = fd;
  cf->f_len = cf_stat.st_size;

  /* Set the file name because we need it to set the follow stream filter.
     XXX - is that still true?  We need it for other reasons, though,
     in any case. */
  cf->filename = g_strdup(fname);

  /* Indicate whether it's a permanent or temporary file. */
  cf->is_tempfile = is_tempfile;

  /* If it's a temporary capture buffer file, mark it as not saved. */
  cf->user_saved = !is_tempfile;

  cf->cd_t      = wtap_file_type(cf->wth);
  cf->count     = 0;
  cf->drops     = 0;
  cf->esec      = 0;
  cf->eusec     = 0;
  cf->snap      = wtap_snapshot_length(cf->wth);
  cf->update_progbar = FALSE;
  cf->progbar_quantum = 0;
  cf->progbar_nextstep = 0;
  firstsec = 0, firstusec = 0;
  prevsec = 0, prevusec = 0;
 
  return (0);

fail:
  simple_dialog(ESD_TYPE_WARN, NULL,
			file_open_error_message(err, FALSE), fname);
  return (err);
}

/* Reset everything to a pristine state */
void
close_cap_file(capture_file *cf, void *w)
{
  frame_data *fd, *fd_next;

  if (cf->fh) {
    file_close(cf->fh);
    cf->fh = NULL;
  }
  if (cf->wth) {
    wtap_close(cf->wth);
    cf->wth = NULL;
  }
  /* We have no file open... */
  if (cf->filename != NULL) {
    /* If it's a temporary file, remove it. */
    if (cf->is_tempfile)
      unlink(cf->filename);
    g_free(cf->filename);
    cf->filename = NULL;
  }
  /* ...which means we have nothing to save. */
  cf->user_saved = FALSE;

  for (fd = cf->plist; fd != NULL; fd = fd_next) {
    fd_next = fd->next;
    g_free(fd);
  }
  if (cf->rfcode != NULL) {
    dfilter_destroy(cf->rfcode);
    cf->rfcode = NULL;
  }
  cf->plist = NULL;
  cf->plist_end = NULL;
  unselect_packet(cf);	/* nothing to select */

  /* Clear the packet list. */
  gtk_clist_freeze(GTK_CLIST(packet_list));
  gtk_clist_clear(GTK_CLIST(packet_list));
  gtk_clist_thaw(GTK_CLIST(packet_list));

  /* Clear any file-related status bar messages.
     XXX - should be "clear *ALL* file-related status bar messages;
     will there ever be more than one on the stack? */
  gtk_statusbar_pop(GTK_STATUSBAR(w), file_ctx);

  /* Restore the standard title bar message. */
  set_main_window_name("The Ethereal Network Analyzer");

  /* Disable all menu items that make sense only if you have a capture. */
  set_menus_for_capture_file(FALSE);
  set_menus_for_unsaved_capture_file(FALSE);
  set_menus_for_captured_packets(FALSE);
  set_menus_for_selected_packet(FALSE);
}

/* Set the file name in the status line, in the name for the main window,
   and in the name for the main window's icon. */
static void
set_display_filename(capture_file *cf)
{
  gchar  *name_ptr;
  size_t  msg_len;
  gchar  *done_fmt = " File: %s  Drops: %u";
  gchar  *done_msg;
  gchar  *win_name_fmt = "%s - Ethereal";
  gchar  *win_name;

  if (!cf->is_tempfile) {
    /* Get the last component of the file name, and put that in the
       status bar. */
    if ((name_ptr = (gchar *) strrchr(cf->filename, '/')) == NULL)
      name_ptr = cf->filename;
    else
      name_ptr++;
  } else {
    /* The file we read is a temporary file from a live capture;
       we don't mention its name in the status bar. */
    name_ptr = "<capture>";
  }

  msg_len = strlen(name_ptr) + strlen(done_fmt) + 64;
  done_msg = g_malloc(msg_len);
  snprintf(done_msg, msg_len, done_fmt, name_ptr, cf->drops);
  gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, done_msg);
  g_free(done_msg);

  msg_len = strlen(name_ptr) + strlen(win_name_fmt) + 1;
  win_name = g_malloc(msg_len);
  snprintf(win_name, msg_len, win_name_fmt, name_ptr);
  set_main_window_name(win_name);
  g_free(win_name);
}

int
read_cap_file(capture_file *cf)
{
  gchar  *name_ptr, *load_msg, *load_fmt = " Loading: %s...";
  int     success;
  int     err;
  size_t  msg_len;
  char   *errmsg;
  char    errmsg_errno[1024+1];
  gchar   err_str[2048+1];

  if ((name_ptr = (gchar *) strrchr(cf->filename, '/')) == NULL)
    name_ptr = cf->filename;
  else
    name_ptr++;

  msg_len = strlen(name_ptr) + strlen(load_fmt) + 2;
  load_msg = g_malloc(msg_len);
  snprintf(load_msg, msg_len, load_fmt, name_ptr);
  gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, load_msg);
  g_free(load_msg);

  cf->update_progbar = TRUE;
  /* Update the progress bar when it gets to this value. */
  cf->progbar_nextstep = 0;
  /* When we reach the value that triggers a progress bar update,
     bump that value by this amount. */
  cf->progbar_quantum = cf->f_len/N_PROGBAR_UPDATES;

  freeze_clist(cf);
  proto_tree_is_visible = FALSE;
  success = wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf, &err);
  /* Set the file encapsulation type now; we don't know what it is until
     we've looked at all the packets, as we don't know until then whether
     there's more than one type (and thus whether it's
     WTAP_ENCAP_PER_PACKET). */
  cf->lnk_t = wtap_file_encap(cf->wth);
  wtap_close(cf->wth);
  cf->wth = NULL;
  cf->filed = open(cf->filename, O_RDONLY);
  cf->fh = filed_open(cf->filed, "r");
  cf->current_frame = cf->first_displayed;
  thaw_clist(cf);

  gtk_progress_set_activity_mode(GTK_PROGRESS(prog_bar), FALSE);
  gtk_progress_set_value(GTK_PROGRESS(prog_bar), 0);

  gtk_statusbar_pop(GTK_STATUSBAR(info_bar), file_ctx);
  set_display_filename(cf);

  /* Enable menu items that make sense if you have a capture file you've
     finished reading. */
  set_menus_for_capture_file(TRUE);
  set_menus_for_unsaved_capture_file(!cf->user_saved);

  /* Enable menu items that make sense if you have some captured packets. */
  set_menus_for_captured_packets(TRUE);

  /* Make the first row the selected row. */
  gtk_signal_emit_by_name(GTK_OBJECT(packet_list), "select_row", 0);

  if (!success) {
    /* Put up a message box noting that the read failed somewhere along
       the line.  Don't throw out the stuff we managed to read, though,
       if any. */
    switch (err) {

    case WTAP_ERR_CANT_READ:
      errmsg = "An attempt to read from the file failed for"
               " some unknown reason.";
      break;

    case WTAP_ERR_SHORT_READ:
      errmsg = "The capture file appears to have been cut short"
               " in the middle of a packet.";
      break;

    case WTAP_ERR_BAD_RECORD:
      errmsg = "The capture file appears to be damaged or corrupt.";
      break;

    default:
      sprintf(errmsg_errno, "An error occurred while reading the"
                              " capture file: %s.", wtap_strerror(err));
      errmsg = errmsg_errno;
      break;
    }
    snprintf(err_str, sizeof err_str, errmsg);
    simple_dialog(ESD_TYPE_WARN, NULL, err_str);
    return (err);
  } else
    return (0);
}

#ifdef HAVE_LIBPCAP
int
start_tail_cap_file(char *fname, gboolean is_tempfile, capture_file *cf)
{
  int     err;
  int     i;

  err = open_cap_file(fname, is_tempfile, cf);
  if (err == 0) {
    /* Disable menu items that make no sense if you're currently running
       a capture. */
    set_menus_for_capture_in_progress(TRUE);

    /* Enable menu items that make sense if you have some captured
       packets (yes, I know, we don't have any *yet*). */
    set_menus_for_captured_packets(TRUE);

    for (i = 0; i < cf->cinfo.num_cols; i++) {
      if (get_column_resize_type(cf->cinfo.col_fmt[i]) == RESIZE_LIVE)
        gtk_clist_set_column_auto_resize(GTK_CLIST(packet_list), i, TRUE);
      else {
        gtk_clist_set_column_auto_resize(GTK_CLIST(packet_list), i, FALSE);
        gtk_clist_set_column_width(GTK_CLIST(packet_list), i,
				cf->cinfo.col_width[i]);
        gtk_clist_set_column_resizeable(GTK_CLIST(packet_list), i, TRUE);
      }
    }

    /* Yes, "open_cap_file()" set this - but it set it to a file handle
       from Wiretap, which will be closed when we close the file; we
       want it to remain open even after that, so that we can read
       packet data from it. */
    cf->fh = file_open(fname, "r");

    gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, 
		       " <live capture in progress>");
  }
  return err;
}

int
continue_tail_cap_file(capture_file *cf, int to_read)
{
  int err;

  gtk_clist_freeze(GTK_CLIST(packet_list));

  wtap_loop(cf->wth, to_read, wtap_dispatch_cb, (u_char *) cf, &err);

  gtk_clist_thaw(GTK_CLIST(packet_list));
  if (auto_scroll_live && cf->plist_end != NULL)
    gtk_clist_moveto(GTK_CLIST(packet_list), 
		       cf->plist_end->row, -1, 1.0, 1.0);
  return err;
}

int
finish_tail_cap_file(capture_file *cf)
{
  int err;

  gtk_clist_freeze(GTK_CLIST(packet_list));

  wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf, &err);

  thaw_clist(cf);
  if (auto_scroll_live && cf->plist_end != NULL)
    gtk_clist_moveto(GTK_CLIST(packet_list), 
		       cf->plist_end->row, -1, 1.0, 1.0);

  /* Set the file encapsulation type now; we don't know what it is until
     we've looked at all the packets, as we don't know until then whether
     there's more than one type (and thus whether it's
     WTAP_ENCAP_PER_PACKET). */
  cf->lnk_t = wtap_file_encap(cf->wth);

  /* There's nothing more to read from the capture file - close it. */
  wtap_close(cf->wth);
  cf->wth = NULL;

  /* Pop the "<live capture in progress>" message off the status bar. */
  gtk_statusbar_pop(GTK_STATUSBAR(info_bar), file_ctx);

  set_display_filename(cf);

  /* Enable menu items that make sense if you're not currently running
     a capture. */
  set_menus_for_capture_in_progress(FALSE);

  /* Enable menu items that make sense if you have a capture file
     you've finished reading. */
  set_menus_for_capture_file(TRUE);
  set_menus_for_unsaved_capture_file(!cf->user_saved);

  return err;
}
#endif /* HAVE_LIBPCAP */

typedef struct {
  color_filter_t *colorf;
  proto_tree	*protocol_tree;
  guint8	*pd;
} apply_color_filter_args;

/*
 * If no color filter has been applied, apply this one.
 * (The "if no color filter has been applied" is to handle the case where
 * more than one color filter matches the packet.)
 */
static void
apply_color_filter(gpointer filter_arg, gpointer argp)
{
  color_filter_t *colorf = filter_arg;
  apply_color_filter_args *args = argp;

  if (colorf->c_colorfilter != NULL && args->colorf == NULL) {
    if (dfilter_apply(colorf->c_colorfilter, args->protocol_tree, args->pd))
      args->colorf = colorf;
  }
}

static void
add_packet_to_packet_list(frame_data *fdata, capture_file *cf, const u_char *buf)
{
  apply_color_filter_args args;
  gint          i, row;
  proto_tree   *protocol_tree = NULL;

  /* We don't yet have a color filter to apply. */
  args.colorf = NULL;

  /* If we don't have the time stamp of the first packet in the
     capture, it's because this is the first packet.  Save the time
     stamp of this packet as the time stamp of the first packet. */
  if (!firstsec && !firstusec) {
    firstsec  = fdata->abs_secs;
    firstusec = fdata->abs_usecs;
  }

  /* Get the time elapsed between the first packet and this packet. */
  cf->esec = fdata->abs_secs - firstsec;
  if (firstusec <= fdata->abs_usecs) {
    cf->eusec = fdata->abs_usecs - firstusec;
  } else {
    cf->eusec = (fdata->abs_usecs + 1000000) - firstusec;
    cf->esec--;
  }

  fdata->cinfo = &cf->cinfo;
  for (i = 0; i < fdata->cinfo->num_cols; i++) {
    fdata->cinfo->col_data[i][0] = '\0';
  }

  /* Apply the filters */
  if (cf->dfcode != NULL || filter_list != NULL) {
    protocol_tree = proto_tree_create_root();
    dissect_packet(buf, fdata, protocol_tree);
    if (cf->dfcode != NULL)
      fdata->passed_dfilter = dfilter_apply(cf->dfcode, protocol_tree, cf->pd);
    else
      fdata->passed_dfilter = TRUE;

    /* Apply color filters, if we have any. */
    if (filter_list != NULL) {
      args.protocol_tree = protocol_tree;
      args.pd = cf->pd;
      g_slist_foreach(filter_list, apply_color_filter, &args);
    }
    proto_tree_free(protocol_tree);
  }
  else {
#ifdef HAVE_PLUGINS
	if (plugin_list)
	    protocol_tree = proto_tree_create_root();
#endif
	dissect_packet(buf, fdata, protocol_tree);
	fdata->passed_dfilter = TRUE;
#ifdef HAVE_PLUGINS
	if (protocol_tree)
	    proto_tree_free(protocol_tree);
#endif
  }

  if (fdata->passed_dfilter) {
    /* XXX - if a GtkCList's selection mode is GTK_SELECTION_BROWSE, when
       the first entry is added to it by "real_insert_row()", that row
       is selected (see "real_insert_row()", in "gtk/gtkclist.c", in both
       our version and the vanilla GTK+ version).

       This means that a "select-row" signal is emitted; this causes
       "packet_list_select_cb()" to be called, which causes "select_packet()"
       to be called.

       "select_packet()" searches the list of frames for a frame with the
       row number passed into it; however, as "gtk_clist_append()", which
       called "real_insert_row()", hasn't yet returned, we don't know what
       the row number is, so we can't correctly set "fd->row" for that frame
       yet.

       This means that we won't find the frame for that row.

       We can't assume that there's only one frame in the frame list,
       either, as we may be filtering the display.

       Therefore, we set "fdata->row" to 0, under the assumption that
       the row number passed to "select_packet()" will be 0 (as we're
       adding the first row to the list; it gets set to the proper
       value later. */
    fdata->row = 0;

    /* If we don't have the time stamp of the previous displayed packet,
       it's because this is the first displayed packet.  Save the time
       stamp of this packet as the time stamp of the previous displayed
       packet. */
    if (!prevsec && !prevusec) {
      prevsec  = fdata->abs_secs;
      prevusec = fdata->abs_usecs;
    }

    /* Get the time elapsed between the first packet and this packet. */
    fdata->rel_secs = cf->esec;
    fdata->rel_usecs = cf->eusec;
  
    /* Get the time elapsed between the previous displayed packet and
       this packet. */
    fdata->del_secs = fdata->abs_secs - prevsec;
    if (prevusec <= fdata->abs_usecs) {
      fdata->del_usecs = fdata->abs_usecs - prevusec;
    } else {
      fdata->del_usecs = (fdata->abs_usecs + 1000000) - prevusec;
      fdata->del_secs--;
    }
    prevsec = fdata->abs_secs;
    prevusec = fdata->abs_usecs;

    fill_in_columns(fdata);

    row = gtk_clist_append(GTK_CLIST(packet_list), fdata->cinfo->col_data);
    fdata->row = row;

    if (filter_list != NULL && (args.colorf != NULL)) {
        gtk_clist_set_background(GTK_CLIST(packet_list), row,
                   &args.colorf->bg_color);
        gtk_clist_set_foreground(GTK_CLIST(packet_list), row,
                   &args.colorf->fg_color);
    } else {
        gtk_clist_set_background(GTK_CLIST(packet_list), row, &WHITE);
        gtk_clist_set_foreground(GTK_CLIST(packet_list), row, &BLACK);
    }

    /* If we haven't yet seen the first frame, this is it. */
    if (cf->first_displayed == NULL)
      cf->first_displayed = fdata;

    /* This is the last frame we've seen so far. */
    cf->last_displayed = fdata;

    /* If this was the current frame, remember the row it's in, so
       we can arrange that it's on the screen when we're done. */
    if (cf->current_frame == fdata)
      cf->current_row = row;
  } else
    fdata->row = -1;	/* not in the display */
  fdata->cinfo = NULL;
}

static void
wtap_dispatch_cb(u_char *user, const struct wtap_pkthdr *phdr, int offset,
  const u_char *buf) {
  frame_data   *fdata;
  capture_file *cf = (capture_file *) user;
  int           passed;
  proto_tree   *protocol_tree;
  frame_data   *plist_end;
  int file_pos;
  float prog_val;

  /* Update the progress bar, but do it only N_PROGBAR_UPDATES times;
     when we update it, we have to run the GTK+ main loop to get it
     to repaint what's pending, and doing so may involve an "ioctl()"
     to see if there's any pending input from an X server, and doing
     that for every packet can be costly, especially on a big file.
     
     Do so only if we were told to do so; when reading a capture file
     being updated by a live capture, we don't do so (as we're not
     "done" until the capture stops, so we don't know how close to
     "done" we are. */

  if (cf->update_progbar && offset >= cf->progbar_nextstep) {
      file_pos = lseek(cf->filed, 0, SEEK_CUR);
      prog_val = (gfloat) file_pos / (gfloat) cf->f_len;
      gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar), prog_val);
      cf->progbar_nextstep += cf->progbar_quantum;
      while (gtk_events_pending())
      gtk_main_iteration();
  }

  /* Allocate the next list entry, and add it to the list. */
  fdata = (frame_data *) g_malloc(sizeof(frame_data));

  fdata->next = NULL;
  fdata->prev = NULL;
  fdata->pkt_len  = phdr->len;
  fdata->cap_len  = phdr->caplen;
  fdata->file_off = offset;
  fdata->lnk_t = phdr->pkt_encap;
  fdata->abs_secs  = phdr->ts.tv_sec;
  fdata->abs_usecs = phdr->ts.tv_usec;
  fdata->encoding = CHAR_ASCII;
  fdata->pseudo_header = phdr->pseudo_header;
  fdata->cinfo = NULL;

  passed = TRUE;
  if (cf->rfcode) {
    protocol_tree = proto_tree_create_root();
    dissect_packet(buf, fdata, protocol_tree);
    passed = dfilter_apply(cf->rfcode, protocol_tree, cf->pd);
    proto_tree_free(protocol_tree);
  }   
  if (passed) {
    plist_end = cf->plist_end;
    fdata->prev = plist_end;
    if (plist_end != NULL)
      plist_end->next = fdata;
    else
      cf->plist = fdata;
    cf->plist_end = fdata;

    cf->count++;
    fdata->num = cf->count;
    add_packet_to_packet_list(fdata, cf, buf);
  } else
    g_free(fdata);
}

int
filter_packets(capture_file *cf, gchar *dftext)
{
  dfilter *dfcode;

  if (dftext == NULL) {
    /* The new filter is an empty filter (i.e., display all packets). */
    dfcode = NULL;
  } else {
    /*
     * We have a filter; try to compile it.
     */
    if (dfilter_compile(dftext, &dfcode) != 0) {
      /* The attempt failed; report an error. */
      simple_dialog(ESD_TYPE_WARN, NULL, dfilter_error_msg);
      return 0;
    }

    /* Was it empty? */
    if (dfcode == NULL) {
      /* Yes - free the filter text, and set it to null. */
      g_free(dftext);
      dftext = NULL;
    }
  }

  /* We have a valid filter.  Replace the current filter. */
  if (cf->dfilter != NULL)
    g_free(cf->dfilter);
  cf->dfilter = dftext;
  if (cf->dfcode != NULL)
    dfilter_destroy(cf->dfcode);
  cf->dfcode = dfcode;

  /* Now go through the list of packets we've read from the capture file,
     applying the current display filter, and, if the packet passes the
     display filter, add it to the summary display, appropriately
     colored.  (That's how we colorize the display - it's like filtering
     the display, only we don't install a new filter.) */
  colorize_packets(cf);
  return 1;
}

void
colorize_packets(capture_file *cf)
{
  frame_data *fd;
  guint32 progbar_quantum;
  guint32 progbar_nextstep;
  int count;

  /* We need to re-initialize all the state information that protocols
     keep, because we're making a fresh pass through all the packets. */

  /* Initialize the table of conversations. */
  conversation_init();

  /* Initialize protocol-specific variables */
  init_all_protocols();

  gtk_progress_set_activity_mode(GTK_PROGRESS(prog_bar), FALSE);

  /* Freeze the packet list while we redo it, so we don't get any
     screen updates while it happens. */
  gtk_clist_freeze(GTK_CLIST(packet_list));

  /* Clear it out. */
  gtk_clist_clear(GTK_CLIST(packet_list));

  /* We don't yet know which will be the first and last frames displayed. */
  cf->first_displayed = NULL;
  cf->last_displayed = NULL;

  /* If a packet was selected, we don't know yet what row, if any, it'll
     get. */
  cf->current_row = -1;

  /* Iterate through the list of packets, calling a routine
     to run the filter on the packet, see if it matches, and
     put it in the display list if so.  */
  firstsec = 0;
  firstusec = 0;
  prevsec = 0;
  prevusec = 0;

  proto_tree_is_visible = FALSE;

  /* Update the progress bar when it gets to this value. */
  progbar_nextstep = 0;
  /* When we reach the value that triggers a progress bar update,
     bump that value by this amount. */
  progbar_quantum = cf->count/N_PROGBAR_UPDATES;
  /* Count of packets at which we've looked. */
  count = 0;

  gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(prog_bar), GTK_PROGRESS_LEFT_TO_RIGHT);

  for (fd = cf->plist; fd != NULL; fd = fd->next) {
    /* Update the progress bar, but do it only N_PROGBAR_UPDATES times;
       when we update it, we have to run the GTK+ main loop to get it
       to repaint what's pending, and doing so may involve an "ioctl()"
       to see if there's any pending input from an X server, and doing
       that for every packet can be costly, especially on a big file. */
    if (count >= progbar_nextstep) {
      /* let's not divide by zero. I should never be started
       * with count == 0, so let's assert that
       */
      g_assert(cf->count > 0);

      gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar),
		(gfloat) count / cf->count);

      progbar_nextstep += progbar_quantum;
      while (gtk_events_pending())
        gtk_main_iteration();
    }

    count++;

    wtap_seek_read (cf->cd_t, cf->fh, fd->file_off, cf->pd, fd->cap_len);

    add_packet_to_packet_list(fd, cf, cf->pd);
  }
 
  gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar), 0);

  if (cf->current_row != -1) {
    /* The current frame passed the filter; make sure it's visible. */
    if (!gtk_clist_row_is_visible(GTK_CLIST(packet_list), cf->current_row))
      gtk_clist_moveto(GTK_CLIST(packet_list), cf->current_row, -1, 0.0, 0.0);
    if (cf->current_frame_is_selected) {
      /* It was selected, so re-select it. */
      gtk_clist_select_row(GTK_CLIST(packet_list), cf->current_row, -1);
    }
    finfo_selected = NULL;
  } else {
    /* The current frame didn't pass the filter; make the first frame
       the current frame, and leave it unselected. */
    unselect_packet(cf);
    cf->current_frame = cf->first_displayed;
  }

  /* Unfreeze the packet list. */
  gtk_clist_thaw(GTK_CLIST(packet_list));
}

#define	MAX_LINE_LENGTH	256

int
print_packets(capture_file *cf, print_args_t *print_args)
{
  int         i;
  frame_data *fd;
  guint32     progbar_quantum;
  guint32     progbar_nextstep;
  guint32     count;
  proto_tree *protocol_tree;
  gint       *col_widths = NULL;
  gint        data_width;
  gboolean    print_separator;
  char        line_buf[MAX_LINE_LENGTH+1];	/* static-sized buffer! */
  char        *cp;
  int         sprintf_len;

  cf->print_fh = open_print_dest(print_args->to_file, print_args->dest);
  if (cf->print_fh == NULL)
    return FALSE;	/* attempt to open destination failed */

  print_preamble(cf->print_fh, print_args->format);

  if (print_args->print_summary) {
    /* We're printing packet summaries.

       Find the widths for each of the columns - maximum of the
       width of the title and the width of the data - and print
       the column titles. */
    col_widths = (gint *) g_malloc(sizeof(gint) * cf->cinfo.num_cols);
    cp = &line_buf[0];
    for (i = 0; i < cf->cinfo.num_cols; i++) {
      /* Don't pad the last column. */
      if (i == cf->cinfo.num_cols - 1)
        col_widths[i] = 0;
      else {
        col_widths[i] = strlen(cf->cinfo.col_title[i]);
        data_width = get_column_char_width(get_column_format(i));
        if (data_width > col_widths[i])
          col_widths[i] = data_width;
      }

      /* Right-justify the packet number column. */
      if (cf->cinfo.col_fmt[i] == COL_NUMBER)
        sprintf_len = sprintf(cp, "%*s", col_widths[i], cf->cinfo.col_title[i]);
      else
        sprintf_len = sprintf(cp, "%-*s", col_widths[i], cf->cinfo.col_title[i]);
      cp += sprintf_len;
      if (i == cf->cinfo.num_cols - 1)
        *cp++ = '\n';
      else
        *cp++ = ' ';
    }
    *cp = '\0';
    print_line(cf->print_fh, print_args->format, line_buf);
  }

  print_separator = FALSE;
  proto_tree_is_visible = TRUE;

  /* Update the progress bar when it gets to this value. */
  progbar_nextstep = 0;
  /* When we reach the value that triggers a progress bar update,
     bump that value by this amount. */
  progbar_quantum = cf->count/N_PROGBAR_UPDATES;
  /* Count of packets at which we've looked. */
  count = 0;

  gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(prog_bar), GTK_PROGRESS_LEFT_TO_RIGHT);

  /* Iterate through the list of packets, printing the packets that
     were selected by the current display filter.  */
  for (fd = cf->plist; fd != NULL; fd = fd->next) {
    /* Update the progress bar, but do it only N_PROGBAR_UPDATES times;
       when we update it, we have to run the GTK+ main loop to get it
       to repaint what's pending, and doing so may involve an "ioctl()"
       to see if there's any pending input from an X server, and doing
       that for every packet can be costly, especially on a big file. */
    if (count >= progbar_nextstep) {
      /* let's not divide by zero. I should never be started
       * with count == 0, so let's assert that
       */
      g_assert(cf->count > 0);

      gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar),
        (gfloat) count / cf->count);
      progbar_nextstep += progbar_quantum;
      while (gtk_events_pending())
        gtk_main_iteration();
    }
    count++;

    if (fd->passed_dfilter) {
      wtap_seek_read (cf->cd_t, cf->fh, fd->file_off, cf->pd, fd->cap_len);
      if (print_args->print_summary) {
        /* Fill in the column information, but don't bother creating
           the logical protocol tree. */
        fd->cinfo = &cf->cinfo;
        for (i = 0; i < fd->cinfo->num_cols; i++) {
          fd->cinfo->col_data[i][0] = '\0';
        }
        dissect_packet(cf->pd, fd, NULL);
        fill_in_columns(fd);
        cp = &line_buf[0];
        for (i = 0; i < cf->cinfo.num_cols; i++) {
          /* Right-justify the packet number column. */
          if (cf->cinfo.col_fmt[i] == COL_NUMBER)
            sprintf_len = sprintf(cp, "%*s", col_widths[i], cf->cinfo.col_data[i]);
          else
            sprintf_len = sprintf(cp, "%-*s", col_widths[i], cf->cinfo.col_data[i]);
          cp += sprintf_len;
          if (i == cf->cinfo.num_cols - 1)
            *cp++ = '\n';
          else
            *cp++ = ' ';
        }
        *cp = '\0';
        print_line(cf->print_fh, print_args->format, line_buf);
      } else {
        if (print_separator)
          print_line(cf->print_fh, print_args->format, "\n");

        /* Create the logical protocol tree. */
        protocol_tree = proto_tree_create_root();
        dissect_packet(cf->pd, fd, protocol_tree);

        /* Print the information in that tree. */
        proto_tree_print(FALSE, print_args, (GNode *)protocol_tree,
			cf->pd, fd, cf->print_fh);

        proto_tree_free(protocol_tree);

	if (print_args->print_hex) {
	  /* Print the full packet data as hex. */
	  print_hex_data(cf->print_fh, print_args->format, cf->pd,
			fd->cap_len, fd->encoding);
	}

        /* Print a blank line if we print anything after this. */
        print_separator = TRUE;
      }
    }
  }

  if (col_widths != NULL)
    g_free(col_widths);

  print_finale(cf->print_fh, print_args->format);

  close_print_dest(print_args->to_file, cf->print_fh);
 
  gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar), 0);

  cf->print_fh = NULL;
  return TRUE;
}

/* Scan through the packet list and change all columns that use the
   "command-line-specified" time stamp format to use the current
   value of that format. */
void
change_time_formats(capture_file *cf)
{
  frame_data *fd;
  int i;
  GtkStyle  *pl_style;

  /* Freeze the packet list while we redo it, so we don't get any
     screen updates while it happens. */
  freeze_clist(cf);

  /* Iterate through the list of packets, checking whether the packet
     is in a row of the summary list and, if so, whether there are
     any columns that show the time in the "command-line-specified"
     format and, if so, update that row. */
  for (fd = cf->plist; fd != NULL; fd = fd->next) {
    if (fd->row != -1) {
      /* This packet is in the summary list, on row "fd->row". */

      /* XXX - there really should be a way of checking "cf->cinfo" for this;
         the answer isn't going to change from packet to packet, so we should
         simply skip all the "change_time_formats()" work if we're not
         changing anything. */
      fd->cinfo = &cf->cinfo;
      if (check_col(fd, COL_CLS_TIME)) {
        /* There are columns that show the time in the "command-line-specified"
           format; update them. */
        for (i = 0; i < cf->cinfo.num_cols; i++) {
          if (cf->cinfo.fmt_matx[i][COL_CLS_TIME]) {
            /* This is one of the columns that shows the time in
               "command-line-specified" format; update it. */
            cf->cinfo.col_data[i][0] = '\0';
            col_set_cls_time(fd, i);
            gtk_clist_set_text(GTK_CLIST(packet_list), fd->row, i,
			  cf->cinfo.col_data[i]);
	  }
        }
      }
    }
  }

  /* Set the column widths of those columns that show the time in
     "command-line-specified" format. */
  pl_style = gtk_widget_get_style(packet_list);
  for (i = 0; i < cf->cinfo.num_cols; i++) {
    if (cf->cinfo.fmt_matx[i][COL_CLS_TIME]) {
      gtk_clist_set_column_width(GTK_CLIST(packet_list), i,
        gdk_string_width(pl_style->font, get_column_longest_string(COL_CLS_TIME)));
    }
  }

  /* Unfreeze the packet list. */
  thaw_clist(cf);
}

static void
clear_tree_and_hex_views(void)
{
  /* Clear the hex dump. */
  gtk_text_freeze(GTK_TEXT(byte_view));
  gtk_text_set_point(GTK_TEXT(byte_view), 0);
  gtk_text_forward_delete(GTK_TEXT(byte_view),
    gtk_text_get_length(GTK_TEXT(byte_view)));
  gtk_text_thaw(GTK_TEXT(byte_view));

  /* Remove all nodes in ctree. This is how it's done in testgtk.c in GTK+ */
  gtk_clist_clear ( GTK_CLIST(tree_view) );

}

gboolean
find_packet(capture_file *cf, dfilter *sfcode)
{
  frame_data *start_fd;
  frame_data *fd;
  frame_data *new_fd = NULL;
  guint32 progbar_quantum;
  guint32 progbar_nextstep;
  int count;
  proto_tree *protocol_tree;

  start_fd = cf->current_frame;
  if (start_fd != NULL)  {
    gtk_progress_set_activity_mode(GTK_PROGRESS(prog_bar), FALSE);

    /* Iterate through the list of packets, starting at the packet we've
       picked, calling a routine to run the filter on the packet, see if
       it matches, and stop if so.  */
    count = 0;
    fd = start_fd;

    proto_tree_is_visible = FALSE;

    /* Update the progress bar when it gets to this value. */
    progbar_nextstep = 0;
    /* When we reach the value that triggers a progress bar update,
       bump that value by this amount. */
    progbar_quantum = cf->count/N_PROGBAR_UPDATES;
    gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(prog_bar), GTK_PROGRESS_LEFT_TO_RIGHT);

    fd = start_fd;
    for (;;) {
      /* Update the progress bar, but do it only N_PROGBAR_UPDATES times;
         when we update it, we have to run the GTK+ main loop to get it
         to repaint what's pending, and doing so may involve an "ioctl()"
         to see if there's any pending input from an X server, and doing
         that for every packet can be costly, especially on a big file. */
      if (count >= progbar_nextstep) {
        /* let's not divide by zero. I should never be started
         * with count == 0, so let's assert that
         */
        g_assert(cf->count > 0);

        gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar),
		(gfloat) count / cf->count);

        progbar_nextstep += progbar_quantum;
        while (gtk_events_pending())
          gtk_main_iteration();
      }

      /* Go past the current frame. */
      if (cf->sbackward) {
        /* Go on to the previous frame. */
        fd = fd->prev;
        if (fd == NULL)
          fd = cf->plist_end;	/* wrap around */
      } else {
        /* Go on to the next frame. */
        fd = fd->next;
        if (fd == NULL)
          fd = cf->plist;	/* wrap around */
      }

      if (fd == start_fd) {
        /* We're back to the frame we were on originally.  The search
           failed. */
        break;
      }

      count++;

      /* Is this packet in the display? */
      if (fd->passed_dfilter) {
        /* Yes.  Does it match the search filter? */
        protocol_tree = proto_tree_create_root();
        wtap_seek_read(cf->cd_t, cf->fh, fd->file_off, cf->pd, fd->cap_len);
        dissect_packet(cf->pd, fd, protocol_tree);
        if (dfilter_apply(sfcode, protocol_tree, cf->pd)) {
          new_fd = fd;
          break;	/* found it! */
        }
      }
    }

    gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar), 0);
  }

  if (new_fd != NULL) {
    /* We found a frame.  Make it visible, and select it. */
    if (!gtk_clist_row_is_visible(GTK_CLIST(packet_list), new_fd->row))
      gtk_clist_moveto(GTK_CLIST(packet_list), new_fd->row, -1, 0.0, 0.0);

    /* XXX - why is there no "gtk_clist_set_focus_row()", so that we
       can make the row for the frame we found the focus row?

       See

   http://www.gnome.org/mailing-lists/archives/gtk-list/2000-January/0038.shtml

       */
    GTK_CLIST(packet_list)->focus_row = new_fd->row;
    gtk_clist_select_row(GTK_CLIST(packet_list), new_fd->row, -1);
    return TRUE;	/* success */
  } else
    return FALSE;	/* failure */
}

goto_result_t
goto_frame(capture_file *cf, guint fnumber)
{
  frame_data *fd;

  for (fd = cf->plist; fd != NULL && fd->num < fnumber; fd = fd->next)
    ;

  if (fd == NULL)
    return NO_SUCH_FRAME;	/* we didn't find that frame */
  if (!fd->passed_dfilter)
    return FRAME_NOT_DISPLAYED;	/* the frame with that number isn't displayed */

  /* We found that frame, and it's currently being displayed.
     Make it visible, and select it. */
  if (!gtk_clist_row_is_visible(GTK_CLIST(packet_list), fd->row))
    gtk_clist_moveto(GTK_CLIST(packet_list), fd->row, -1, 0.0, 0.0);

  /* See above complaint about the lack of "gtk_clist_set_focus_row()". */
  GTK_CLIST(packet_list)->focus_row = fd->row;
  gtk_clist_select_row(GTK_CLIST(packet_list), fd->row, -1);
  return FOUND_FRAME;
}

/* Select the packet on a given row. */
void
select_packet(capture_file *cf, int row)
{
  frame_data *fd;
  int i;

  /* Search through the list of frames to see which one is in
     this row. */
  for (fd = cf->plist, i = 0; fd != NULL; fd = fd->next, i++) {
    if (fd->row == row)
      break;
  }

  g_assert(fd != NULL);

  /* Record that this frame is the current frame, and that it's selected. */
  cf->current_frame = fd;
  cf->current_frame_is_selected = TRUE;

  /* Get the data in that frame. */
  wtap_seek_read (cf->cd_t, cf->fh, fd->file_off, cf->pd, fd->cap_len);

  /* Create the logical protocol tree. */
  if (cf->protocol_tree)
      proto_tree_free(cf->protocol_tree);
  cf->protocol_tree = proto_tree_create_root();
  proto_tree_is_visible = TRUE;
  dissect_packet(cf->pd, cf->current_frame, cf->protocol_tree);

  /* Display the GUI protocol tree and hex dump. */
  clear_tree_and_hex_views();
  proto_tree_draw(cf->protocol_tree, tree_view);
  packet_hex_print(GTK_TEXT(byte_view), cf->pd, cf->current_frame->cap_len,
			-1, -1, cf->current_frame->encoding);

  /* A packet is selected. */
  set_menus_for_selected_packet(TRUE);
}

/* Unselect the selected packet, if any. */
void
unselect_packet(capture_file *cf)
{
  cf->current_frame_is_selected = FALSE;

  /* Destroy the protocol tree for that packet. */
  if (cf->protocol_tree != NULL) {
    proto_tree_free(cf->protocol_tree);
    cf->protocol_tree = NULL;
  }

  finfo_selected = NULL;

  /* Clear out the display of that packet. */
  clear_tree_and_hex_views();

  /* No packet is selected. */
  set_menus_for_selected_packet(FALSE);
}

static void
freeze_clist(capture_file *cf)
{
  int i;

  /* Make the column sizes static, so they don't adjust while
     we're reading the capture file (freezing the clist doesn't
     seem to suffice). */
  for (i = 0; i < cf->cinfo.num_cols; i++)
    gtk_clist_set_column_auto_resize(GTK_CLIST(packet_list), i, FALSE);
  gtk_clist_freeze(GTK_CLIST(packet_list));
}

static void
thaw_clist(capture_file *cf)
{
  int i;

  for (i = 0; i < cf->cinfo.num_cols; i++) {
    if (get_column_resize_type(cf->cinfo.col_fmt[i]) == RESIZE_MANUAL) {
      /* Set this column's width to the appropriate value. */
      gtk_clist_set_column_width(GTK_CLIST(packet_list), i,
				cf->cinfo.col_width[i]);
    } else {
      /* Make this column's size dynamic, so that it adjusts to the
         appropriate size. */
      gtk_clist_set_column_auto_resize(GTK_CLIST(packet_list), i, TRUE);
    }
  }
  gtk_clist_thaw(GTK_CLIST(packet_list));

  /* Hopefully, the columns have now gotten their appropriate sizes;
     make them resizeable - a column that auto-resizes cannot be
     resized by the user, and *vice versa*. */
  for (i = 0; i < cf->cinfo.num_cols; i++)
    gtk_clist_set_column_resizeable(GTK_CLIST(packet_list), i, TRUE);
}

int
save_cap_file(char *fname, capture_file *cf, gboolean save_filtered,
		guint save_format)
{
  gchar        *from_filename;
  gchar        *name_ptr, *save_msg, *save_fmt = " Saving: %s...";
  size_t        msg_len;
  int           err;
  gboolean      do_copy;
  int           from_fd, to_fd, nread, nwritten;
  wtap_dumper  *pdh;
  frame_data   *fd;
  struct wtap_pkthdr hdr;
  guint8        pd[65536];

  if ((name_ptr = (gchar *) strrchr(fname, '/')) == NULL)
    name_ptr = fname;
  else
    name_ptr++;
  msg_len = strlen(name_ptr) + strlen(save_fmt) + 2;
  save_msg = g_malloc(msg_len);
  snprintf(save_msg, msg_len, save_fmt, name_ptr);
  gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, save_msg);
  g_free(save_msg);

  if (!save_filtered && save_format == cf->cd_t) {
    /* We're not filtering packets, and we're saving it in the format
       it's already in, so we can just move or copy the raw data. */

    /* In this branch, we set "err" only if we get an error, so we
       must first clear it. */
    err = 0;
    if (cf->is_tempfile) {
      /* The file being saved is a temporary file from a live
         capture, so it doesn't need to stay around under that name;
	 first, try renaming the capture buffer file to the new name. */
      if (rename(cf->filename, fname) == 0) {
      	/* That succeeded - there's no need to copy the source file. */
      	from_filename = NULL;
	do_copy = FALSE;
      } else {
      	if (errno == EXDEV) {
	  /* They're on different file systems, so we have to copy the
	     file. */
	  do_copy = TRUE;
          from_filename = cf->filename;
	} else {
	  /* The rename failed, but not because they're on different
	     file systems - put up an error message.  (Or should we
	     just punt and try to copy?  The only reason why I'd
	     expect the rename to fail and the copy to succeed would
	     be if we didn't have permission to remove the file from
	     the temporary directory, and that might be fixable - but
	     is it worth requiring the user to go off and fix it?) */
	  err = errno;
	  simple_dialog(ESD_TYPE_WARN, NULL,
				file_rename_error_message(err), fname);
	  goto done;
	}
      }
    } else {
      /* It's a permanent file, so we should copy it, and not remove the
         original. */
      do_copy = TRUE;
      from_filename = cf->filename;
    }

    /* Copy the file, if we haven't moved it. */
    if (do_copy) {
      /* Copy the raw bytes of the file. */
      from_fd = open(from_filename, O_RDONLY);
      if (from_fd < 0) {
      	err = errno;
	simple_dialog(ESD_TYPE_WARN, NULL,
			file_open_error_message(err, TRUE), from_filename);
	goto done;
      }

      to_fd = creat(fname, 0644);
      if (to_fd < 0) {
      	err = errno;
	simple_dialog(ESD_TYPE_WARN, NULL,
			file_open_error_message(err, TRUE), fname);
	close(from_fd);
	goto done;
      }

      while ((nread = read(from_fd, pd, sizeof pd)) > 0) {
	nwritten = write(to_fd, pd, nread);
	if (nwritten < nread) {
	  if (nwritten < 0)
	    err = errno;
	  else
	    err = WTAP_ERR_SHORT_WRITE;
	  simple_dialog(ESD_TYPE_WARN, NULL,
				file_write_error_message(err), fname);
	  close(from_fd);
	  close(to_fd);
	  goto done;
	}
      }
      if (nread < 0) {
      	err = errno;
	simple_dialog(ESD_TYPE_WARN, NULL,
			file_read_error_message(err), from_filename);
	close(from_fd);
	close(to_fd);
	goto done;
      }
      close(from_fd);
      if (close(to_fd) < 0) {
      	err = errno;
	simple_dialog(ESD_TYPE_WARN, NULL,
		file_close_error_message(err), fname);
	goto done;
      }
    }
  } else {
    /* Either we're filtering packets, or we're saving in a different
       format; we can't do that by copying or moving the capture file,
       we have to do it by writing the packets out in Wiretap. */
    pdh = wtap_dump_open(fname, save_format, cf->lnk_t, cf->snap, &err);
    if (pdh == NULL) {
      simple_dialog(ESD_TYPE_WARN, NULL,
			file_open_error_message(err, TRUE), fname);
      goto done;
    }

    /* XXX - have a way to save only the packets currently selected by
       the display filter.

       If we do that, should we make that file the current file?  If so,
       it means we can no longer get at the other packets.  What does
       NetMon do? */
    for (fd = cf->plist; fd != NULL; fd = fd->next) {
      /* XXX - do a progress bar */
      if (!save_filtered || fd->passed_dfilter) {
      	/* Either we're saving all frames, or we're saving filtered frames
	   and this one passed the display filter - save it. */
        hdr.ts.tv_sec = fd->abs_secs;
        hdr.ts.tv_usec = fd->abs_usecs;
        hdr.caplen = fd->cap_len;
        hdr.len = fd->pkt_len;
        hdr.pkt_encap = fd->lnk_t;
        hdr.pseudo_header = fd->pseudo_header;
	wtap_seek_read(cf->cd_t, cf->fh, fd->file_off, pd, fd->cap_len);

        if (!wtap_dump(pdh, &hdr, pd, &err)) {
	    simple_dialog(ESD_TYPE_WARN, NULL,
				file_write_error_message(err), fname);
	    wtap_dump_close(pdh, &err);
	    goto done;
	}
      }
    }

    if (!wtap_dump_close(pdh, &err)) {
      simple_dialog(ESD_TYPE_WARN, NULL,
		file_close_error_message(err), fname);
      goto done;
    }
  }

done:

  /* Pop the "Saving:" message off the status bar. */
  gtk_statusbar_pop(GTK_STATUSBAR(info_bar), file_ctx);
  if (err == 0) {
    if (!save_filtered) {
      /* We saved the entire capture, not just some packets from it.
         Open and read the file we saved it to.

	 XXX - this is somewhat of a waste; we already have the
	 packets, all this gets us is updated file type information
	 (which we could just stuff into "cf"), and having the new
	 file be the one we have opened and from which we're reading
	 the data, and it means we have to spend time opening and
	 reading the file, which could be a significant amount of
	 time if the file is large. */
      cf->user_saved = TRUE;

      if ((err = open_cap_file(fname, FALSE, cf)) == 0) {
	/* XXX - report errors if this fails? */
	err = read_cap_file(cf);
	set_menus_for_unsaved_capture_file(FALSE);
      }
    }
  }
  return err;
}

char *
file_open_error_message(int err, int for_writing)
{
  char *errmsg;
  static char errmsg_errno[1024+1];

  switch (err) {

  case WTAP_ERR_NOT_REGULAR_FILE:
    errmsg = "The file \"%s\" is invalid.";
    break;

  case WTAP_ERR_FILE_UNKNOWN_FORMAT:
  case WTAP_ERR_UNSUPPORTED:
    /* Seen only when opening a capture file for reading. */
    errmsg = "The file \"%s\" is not a capture file in a format Ethereal understands.";
    break;

  case WTAP_ERR_UNSUPPORTED_FILE_TYPE:
    /* Seen only when opening a capture file for writing. */
    errmsg = "Ethereal does not support writing capture files in that format.";
    break;

  case WTAP_ERR_UNSUPPORTED_ENCAP:
  case WTAP_ERR_ENCAP_PER_PACKET_UNSUPPORTED:
    /* Seen only when opening a capture file for writing. */
    errmsg = "Ethereal cannot save this capture in that format.";
    break;

  case WTAP_ERR_BAD_RECORD:
    errmsg = "The file \"%s\" appears to be damaged or corrupt.";
    break;

  case WTAP_ERR_CANT_OPEN:
    if (for_writing)
      errmsg = "The file \"%s\" could not be created for some unknown reason.";
    else
      errmsg = "The file \"%s\" could not be opened for some unknown reason.";
    break;

  case WTAP_ERR_SHORT_READ:
    errmsg = "The file \"%s\" appears to have been cut short"
             " in the middle of a packet.";
    break;

  case WTAP_ERR_SHORT_WRITE:
    errmsg = "A full header couldn't be written to the file \"%s\".";
    break;

  case ENOENT:
    if (for_writing)
      errmsg = "The path to the file \"%s\" does not exist.";
    else
      errmsg = "The file \"%s\" does not exist.";
    break;

  case EACCES:
    if (for_writing)
      errmsg = "You do not have permission to create or write to the file \"%s\".";
    else
      errmsg = "You do not have permission to read the file \"%s\".";
    break;

  default:
    sprintf(errmsg_errno, "The file \"%%s\" could not be opened: %s.",
				wtap_strerror(err));
    errmsg = errmsg_errno;
    break;
  }
  return errmsg;
}

static char *
file_rename_error_message(int err)
{
  char *errmsg;
  static char errmsg_errno[1024+1];

  switch (err) {

  case ENOENT:
    errmsg = "The path to the file \"%s\" does not exist.";
    break;

  case EACCES:
    errmsg = "You do not have permission to move the capture file to \"%s\".";
    break;

  default:
    sprintf(errmsg_errno, "The file \"%%s\" could not be moved: %s.",
				wtap_strerror(err));
    errmsg = errmsg_errno;
    break;
  }
  return errmsg;
}

char *
file_read_error_message(int err)
{
  static char errmsg_errno[1024+1];

  sprintf(errmsg_errno, "An error occurred while reading from the file \"%%s\": %s.",
				wtap_strerror(err));
  return errmsg_errno;
}

char *
file_write_error_message(int err)
{
  char *errmsg;
  static char errmsg_errno[1024+1];

  switch (err) {

  case ENOSPC:
    errmsg = "The file \"%s\" could not be saved because there is no space left on the file system.";
    break;

#ifdef EDQUOT
  case EDQUOT:
    errmsg = "The file \"%s\" could not be saved because you are too close to, or over, your disk quota.";
    break;
#endif

  default:
    sprintf(errmsg_errno, "An error occurred while writing to the file \"%%s\": %s.",
				wtap_strerror(err));
    errmsg = errmsg_errno;
    break;
  }
  return errmsg;
}

/* Check for write errors - if the file is being written to an NFS server,
   a write error may not show up until the file is closed, as NFS clients
   might not send writes to the server until the "write()" call finishes,
   so that the write may fail on the server but the "write()" may succeed. */
static char *
file_close_error_message(int err)
{
  char *errmsg;
  static char errmsg_errno[1024+1];

  switch (err) {

  case WTAP_ERR_CANT_CLOSE:
    errmsg = "The file \"%s\" couldn't be closed for some unknown reason.";
    break;

  case WTAP_ERR_SHORT_WRITE:
    errmsg = "Not all the data could be written to the file \"%s\".";
    break;

  case ENOSPC:
    errmsg = "The file \"%s\" could not be saved because there is no space left on the file system.";
    break;

#ifdef EDQUOT
  case EDQUOT:
    errmsg = "The file \"%s\" could not be saved because you are too close to, or over, your disk quota.";
    break;
#endif

  default:
    sprintf(errmsg_errno, "An error occurred while closing the file \"%%s\": %s.",
				wtap_strerror(err));
    errmsg = errmsg_errno;
    break;
  }
  return errmsg;
}
