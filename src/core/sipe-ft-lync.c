/**
 * @file sipe-ft-lync.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2014 SIPE Project <http://sipe.sourceforge.net/>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "sip-transport.h"
#include "sipe-backend.h"
#include "sipe-common.h"
#include "sipe-core.h"
#include "sipe-core-private.h"
#include "sipe-ft-lync.h"
#include "sipe-media.h"
#include "sipe-mime.h"
#include "sipe-nls.h"
#include "sipe-session.h"
#include "sipe-utils.h"
#include "sipe-xml.h"
#include "sipmsg.h"

struct sipe_file_transfer_lync {
	struct sipe_file_transfer public;

	gchar *sdp;
	gchar *file_name;
	gchar *id;
	gsize file_size;
	guint request_id;

	guint expecting_len;

	struct sipe_core_private *sipe_private;
	struct sipe_media_call_private *call_private;
	struct sip_dialog *dialog;

	gboolean was_cancelled;

	int write_source_id;

	void (*call_reject_parent_cb)(struct sipe_media_call *call,
				      gboolean local);
};
#define SIPE_FILE_TRANSFER         ((struct sipe_file_transfer *) ft_private)
#define SIPE_FILE_TRANSFER_PRIVATE ((struct sipe_file_transfer_lync *) ft)

static void
ft_lync_deallocate(struct sipe_file_transfer *ft);

static void
sipe_file_transfer_lync_free(struct sipe_file_transfer_lync *ft_private)
{
	g_free(ft_private->file_name);
	g_free(ft_private->sdp);
	g_free(ft_private->id);

	if (ft_private->write_source_id) {
		g_source_remove(ft_private->write_source_id);
	}

	g_free(ft_private);
}

static void
send_ms_filetransfer_msg(char *body, struct sipe_file_transfer_lync *ft_private,
			 TransCallback callback)
{
	sip_transport_info(ft_private->sipe_private,
			   "Content-Type: application/ms-filetransfer+xml\r\n",
			   body,
			   ft_private->dialog,
			   callback);

	g_free(body);
}

static gboolean
request_download_file(struct sipe_file_transfer_lync *ft_private)
{
	static const gchar *SUCCESS_RESPONSE =
		"<response xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%d\" code=\"success\"/>";

	static const gchar *DOWNLOAD_FILE_REQUEST =
		"<request xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%d\">"
			"<downloadFile>"
				"<fileInfo>"
					"<id>%s</id>"
					"<name>%s</name>"
				"</fileInfo>"
			"</downloadFile>"
		"</request>";

	send_ms_filetransfer_msg(g_strdup_printf(SUCCESS_RESPONSE,
						 ft_private->request_id),
				 ft_private, NULL);

	send_ms_filetransfer_msg(g_strdup_printf(DOWNLOAD_FILE_REQUEST,
						 ++ft_private->request_id,
						 ft_private->id,
						 ft_private->file_name),
				 ft_private, NULL);

	return FALSE;
}

static void
mime_mixed_cb(gpointer user_data, const GSList *fields, const gchar *body,
	      gsize length)
{
	struct sipe_file_transfer_lync *ft_private = user_data;
	const gchar *ctype = sipe_utils_nameval_find(fields, "Content-Type");

	/* Lync 2010 file transfer */
	if (g_str_has_prefix(ctype, "application/ms-filetransfer+xml")) {
		sipe_xml *xml = sipe_xml_parse(body, length);
		const sipe_xml *node;

		const gchar *request_id_str = sipe_xml_attribute(xml, "requestId");
		if (request_id_str) {
			ft_private->request_id = atoi(request_id_str);
		}

		node = sipe_xml_child(xml, "publishFile/fileInfo/name");
		if (node) {
			ft_private->file_name = sipe_xml_data(node);
		}

		node = sipe_xml_child(xml, "publishFile/fileInfo/id");
		if (node) {
			ft_private->id = sipe_xml_data(node);
		}

		node = sipe_xml_child(xml, "publishFile/fileInfo/size");
		if (node) {
			gchar *size_str = sipe_xml_data(node);
			if (size_str) {
				ft_private->file_size = atoi(size_str);
				g_free(size_str);
			}
		}
	} else if (g_str_has_prefix(ctype, "application/sdp")) {
		ft_private->sdp = g_strndup(body, length);
	}
}

static void
candidate_pair_established_cb(SIPE_UNUSED_PARAMETER struct sipe_media_call *call,
			      struct sipe_media_stream *stream)
{
	g_return_if_fail(sipe_strequal(stream->id, "data"));

	request_download_file(sipe_media_stream_get_data(stream));
}

static void
read_cb(struct sipe_media_call *call, struct sipe_media_stream *stream)
{
	struct sipe_file_transfer_lync *ft_data =
			sipe_media_stream_get_data(stream);
	guint8 buffer[0x800];

	if (ft_data->was_cancelled) {
		// Just drop the incoming data.
		sipe_backend_media_read(call, stream, buffer, sizeof (buffer),
					FALSE);
		return;
	}

	if (ft_data->expecting_len == 0) {
		guint8 type;
		guint16 size;

		sipe_backend_media_read(call, stream, &type, sizeof (guint8), TRUE);
		sipe_backend_media_read(call, stream, (guint8 *)&size, sizeof (guint16), TRUE);
		size = GUINT16_FROM_BE(size);

		if (type == 0x01) {
			sipe_backend_media_read(call, stream, buffer, size, TRUE);
			buffer[size] = 0;
			SIPE_DEBUG_INFO("Received new stream for requestId : %s", buffer);
			sipe_backend_ft_start(&ft_data->public, NULL, NULL, 0);
		} else if (type == 0x02) {
			sipe_backend_media_read(call, stream, buffer, size, TRUE);
			buffer[size] = 0;

			SIPE_DEBUG_INFO("Received end of stream for requestId : %s", buffer);
			// TODO: finish transfer;
		} else if (type == 0x00) {
			SIPE_DEBUG_INFO("Received new data chunk of size %d", size);
			ft_data->expecting_len = size;
		}
		/* Readable will be called again so we can read the rest of
		 * the buffer or the chunk. */
	} else {
		guint len = MIN(ft_data->expecting_len, sizeof (buffer));
		len = sipe_backend_media_read(call, stream, buffer, len, FALSE);
		ft_data->expecting_len -= len;
		SIPE_DEBUG_INFO("Read %d bytes. %d remaining in chunk",
				len, ft_data->expecting_len);
		sipe_backend_ft_write_file(&ft_data->public, buffer, len);
	}
}

static void
ft_lync_incoming_init(struct sipe_file_transfer *ft,
		      SIPE_UNUSED_PARAMETER const gchar *filename,
		      SIPE_UNUSED_PARAMETER gsize size,
		      SIPE_UNUSED_PARAMETER const gchar *who)
{
	struct sipe_file_transfer_lync *ft_private =
			(struct sipe_file_transfer_lync *)ft;
	struct sipe_media_call *call =
			(struct sipe_media_call *)ft_private->call_private;

	if (call) {
		sipe_backend_media_accept(call->backend_private, TRUE);
	}
}

static struct sipe_file_transfer_lync *
ft_private_form_call(struct sipe_media_call *call)
{
	struct sipe_media_stream *stream =
			sipe_core_media_get_stream_by_id(call, "data");
	g_return_val_if_fail(stream, NULL);

	return sipe_media_stream_get_data(stream);
}

static void
call_reject_cb(struct sipe_media_call *call, gboolean local)
{
	struct sipe_file_transfer_lync *ft_private = ft_private_form_call(call);
	g_return_if_fail(ft_private);

	if (ft_private->call_reject_parent_cb) {
		ft_private->call_reject_parent_cb(call, local);
	}

	if (!local) {
		sipe_backend_ft_cancel_remote(&ft_private->public);
	}
}

static gboolean
ft_lync_incoming_end(struct sipe_file_transfer *ft)
{
	struct sipe_file_transfer_lync *ft_private =
			(struct sipe_file_transfer_lync *)ft;

	static const gchar *FILETRANSFER_PROGRESS =
			"<notify xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" notifyId=\"%d\">"
				"<fileTransferProgress>"
					"<transferId>%d</transferId>"
					"<bytesReceived>"
						"<from>0</from>"
						"<to>%d</to>"
					"</bytesReceived>"
				"</fileTransferProgress>"
			"</notify>";

	send_ms_filetransfer_msg(g_strdup_printf(FILETRANSFER_PROGRESS,
						 rand(),
						 ft_private->request_id,
						 ft_private->file_size - 1),
				 ft_private, NULL);

	/* We still need our filetransfer structure so don't let backend
	 * deallocate it. */
	ft->deallocate = NULL;

	return TRUE;
}

static gboolean
request_cancelled_cb(struct sipe_core_private *sipe_private, struct sipmsg *msg,
		     SIPE_UNUSED_PARAMETER struct transaction *trans)
{
	struct sipe_media_call_private *call_private =
			g_hash_table_lookup(sipe_private->media_calls,
					    sipmsg_find_header(msg, "Call-ID"));

	struct sipe_file_transfer_lync *ft_private =
			ft_private_form_call((struct sipe_media_call *)call_private);

	ft_lync_deallocate(SIPE_FILE_TRANSFER);

	return TRUE;
}

static gboolean
cancel_transfer_cb(struct sipe_core_private *sipe_private, struct sipmsg *msg,
		   SIPE_UNUSED_PARAMETER struct transaction *trans)
{
	struct sipe_media_call_private *call_private =
			g_hash_table_lookup(sipe_private->media_calls,
					    sipmsg_find_header(msg, "Call-ID"));

	struct sipe_file_transfer_lync *ft_private =
			ft_private_form_call((struct sipe_media_call *)call_private);

	static const gchar *FILETRANSFER_CANCEL_RESPONSE =
			"<response xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%d\" code=\"failure\" reason=\"requestCancelled\"/>";
	send_ms_filetransfer_msg(g_strdup_printf(FILETRANSFER_CANCEL_RESPONSE,
						 ft_private->request_id),
				 ft_private, request_cancelled_cb);

	return TRUE;
}

static void
ft_lync_incoming_cancelled(struct sipe_file_transfer *ft, gboolean local)
{
	static const gchar *FILETRANSFER_CANCEL_REQUEST =
			"<request xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%d\"/>"
				"<cancelTransfer>"
					"<transferId>%d</transferId>"
					"<fileInfo>"
						"<id>%s</id>"
						"<name>%s</name>"
					"</fileInfo>"
				"</cancelTransfer>"
			"</request>";

	if (local) {
		struct sipe_file_transfer_lync *ft_private =
				SIPE_FILE_TRANSFER_PRIVATE;

		send_ms_filetransfer_msg(g_strdup_printf(FILETRANSFER_CANCEL_REQUEST,
							 ft_private->request_id + 1,
							 ft_private->request_id,
							 ft_private->id,
							 ft_private->file_name),
					 ft_private, cancel_transfer_cb);

		SIPE_FILE_TRANSFER_PRIVATE->was_cancelled = TRUE;
		/* We still need our filetransfer structure so don't let backend
		 * deallocate it. */
		ft->deallocate = NULL;
	}
}

static void
ft_lync_deallocate(struct sipe_file_transfer *ft)
{
	struct sipe_file_transfer_lync *ft_private =
			(struct sipe_file_transfer_lync *) ft;
	struct sipe_media_call *call =
			(struct sipe_media_call *)ft_private->call_private;
	if (call) {
		sipe_backend_media_hangup(call->backend_private, TRUE);
	}
	sipe_file_transfer_lync_free(ft_private);
}

void
process_incoming_invite_ft_lync(struct sipe_core_private *sipe_private,
				struct sipmsg *msg)
{
	struct sipe_file_transfer_lync *ft_private;
	struct sipe_media_call *call;
	struct sipe_media_stream *stream;

	ft_private = g_new0(struct sipe_file_transfer_lync, 1);
	sipe_mime_parts_foreach(sipmsg_find_header(msg, "Content-Type"),
				msg->body, mime_mixed_cb, ft_private);

	if (!ft_private->file_name || !ft_private->file_size || !ft_private->sdp) {
		sip_transport_response(sipe_private, msg, 488, "Not Acceptable Here", NULL);
		sipe_file_transfer_lync_free(ft_private);
		return;
	}

	g_free(msg->body);
	msg->body = ft_private->sdp;
	msg->bodylen = strlen(msg->body);
	ft_private->sdp = NULL;

	ft_private->call_private = process_incoming_invite_call(sipe_private, msg);
	if (!ft_private->call_private) {
		sip_transport_response(sipe_private, msg, 500, "Server Internal Error", NULL);
		sipe_file_transfer_lync_free(ft_private);
		return;
	}

	call = (struct sipe_media_call *) ft_private->call_private;
	call->candidate_pair_established_cb = candidate_pair_established_cb;
	call->read_cb = read_cb;

	ft_private->call_reject_parent_cb = call->call_reject_cb;
	call->call_reject_cb = call_reject_cb;

	ft_private->sipe_private = sipe_private;

	ft_private->dialog = sipe_media_get_sip_dialog(call);

	ft_private->public.init = ft_lync_incoming_init;
	ft_private->public.end = ft_lync_incoming_end;
	ft_private->public.cancelled = ft_lync_incoming_cancelled;
	ft_private->public.deallocate = ft_lync_deallocate;

	stream = sipe_core_media_get_stream_by_id(call, "data");
	sipe_media_stream_set_data(stream, ft_private, NULL);

	sipe_backend_ft_incoming(SIPE_CORE_PUBLIC,
				 (struct sipe_file_transfer *)ft_private,
				 call->with, ft_private->file_name,
				 ft_private->file_size);
}

static void
process_response(struct sipe_file_transfer_lync *ft_private, sipe_xml *xml)
{
	guint request_id = atoi(sipe_xml_attribute(xml, "requestId"));
	const gchar *code;

	if (request_id != ft_private->request_id) {
		return;
	}

	code = sipe_xml_attribute(xml, "code");
	if (sipe_strequal(code, "success")) {
		/* Don't hang up the call ourselves, we'll receive BYE from the
		 * sender. */
		sipe_file_transfer_lync_free(ft_private);
	} else if (sipe_strequal(code, "failure")) {
		const gchar *reason = sipe_xml_attribute(xml, "reason");
		if (sipe_strequal(reason, "requestCancelled")) {
			sipe_backend_ft_cancel_remote(SIPE_FILE_TRANSFER);
		}
	}
}

static void
write_chunk(struct sipe_media_call *call, struct sipe_media_stream *stream,
	    guint8 type, guint16 len, const gchar *buffer, gboolean blocking)
{
	guint16 len_be = GUINT16_TO_BE(len);

	sipe_backend_media_write(call, stream, &type, sizeof (guint8), blocking);
	sipe_backend_media_write(call, stream, (guint8 *)&len_be, sizeof (guint16), blocking);
	sipe_backend_media_write(call, stream, (guint8 *)buffer, len, blocking);
}

static gboolean
send_file_chunk(struct sipe_file_transfer_lync *ft_private)
{
	struct sipe_media_call *call =
			(struct sipe_media_call *)ft_private->call_private;
	struct sipe_media_stream *stream =
			sipe_core_media_get_stream_by_id(call, "data");
	//gchar buffer[G_MAXINT16];
	gchar buffer[1024];
	gssize bytes_read;

	bytes_read = sipe_backend_ft_read_file(SIPE_FILE_TRANSFER,
					       (guchar *)&buffer,
					       sizeof (buffer));
	if (bytes_read != 0) {
		write_chunk(call, stream, 0x00, bytes_read, buffer, TRUE);
	}

	if (sipe_backend_ft_is_completed(SIPE_FILE_TRANSFER)) {
		/* End of transfer. */
		gchar *request_id_str =
				g_strdup_printf("%u", ft_private->request_id);
		write_chunk(call, stream, 0x02, strlen(request_id_str),
			    request_id_str, TRUE);
		g_free(request_id_str);
		ft_private->write_source_id = 0;
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static void
start_writing(struct sipe_file_transfer_lync *ft_private)
{
	struct sipe_media_call *call =
			(struct sipe_media_call *)ft_private->call_private;
	struct sipe_media_stream *stream =
			sipe_core_media_get_stream_by_id(call, "data");

	if (stream) {
		gchar *request_id_str =
				g_strdup_printf("%u", ft_private->request_id);

		write_chunk(call, stream, 0x01, strlen(request_id_str),
			    request_id_str, TRUE);

		g_free(request_id_str);

		sipe_backend_ft_start(SIPE_FILE_TRANSFER, 0, NULL, 0);
		ft_private->write_source_id =
				g_idle_add((GSourceFunc)send_file_chunk,
					   ft_private);
	}
}

static void
process_request(struct sipe_file_transfer_lync *ft_private, sipe_xml *xml)
{
	static const gchar *DOWNLOAD_PENDING_RESPONSE =
			"<response xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%u\" code=\"pending\"/>";

	if (sipe_xml_child(xml, "downloadFile")) {
		ft_private->request_id =
				atoi(sipe_xml_attribute(xml, "requestId"));

		send_ms_filetransfer_msg(g_strdup_printf(DOWNLOAD_PENDING_RESPONSE,
							 ft_private->request_id),
					 ft_private, NULL);

		start_writing(ft_private);
	}
}

static void
process_notify(struct sipe_file_transfer_lync *ft_private, sipe_xml *xml)
{
	static const gchar *DOWNLOAD_SUCCESS_RESPONSE =
		"<response xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%u\" code=\"success\"/>";

	const sipe_xml *progress_node = sipe_xml_child(xml, "fileTransferProgress");

	if (progress_node) {
		gchar *to_str = sipe_xml_data(sipe_xml_child(progress_node, "bytesReceived/to"));

		if (atoi(to_str) == (int)(ft_private->file_size - 1)) {
			send_ms_filetransfer_msg(g_strdup_printf(DOWNLOAD_SUCCESS_RESPONSE,
								 ft_private->request_id),
						 ft_private, NULL);
			/* This also hangs up the call and sends BYE to the
			 * other party. */
			ft_lync_deallocate(SIPE_FILE_TRANSFER);
		}
		g_free(to_str);
	}
}

void
process_incoming_info_ft_lync(struct sipe_core_private *sipe_private,
			      struct sipmsg *msg)
{
	struct sipe_file_transfer_lync *ft_private;
	sipe_xml *xml;

	struct sipe_media_call_private *call_private =
			g_hash_table_lookup(sipe_private->media_calls,
					    sipmsg_find_header(msg, "Call-ID"));

	ft_private = ft_private_form_call((struct sipe_media_call *)call_private);
	if (!ft_private) {
		return;
	}

	xml = sipe_xml_parse(msg->body, msg->bodylen);
	if (!xml) {
		return;
	}

	sip_transport_response(sipe_private, msg, 200, "OK", NULL);

	if (sipe_backend_ft_is_incoming(SIPE_FILE_TRANSFER)) {
		if (sipe_strequal(sipe_xml_name(xml), "response")) {
			process_response(ft_private, xml);
		}
	} else {
		if (sipe_strequal(sipe_xml_name(xml), "request")) {
			process_request(ft_private, xml);
		} else if (sipe_strequal(sipe_xml_name(xml), "notify")) {
			process_notify(ft_private, xml);
		}
	}

	sipe_xml_free(xml);
}

static void
append_publish_file_invite(struct sipe_media_call *call,
			   struct sipe_file_transfer_lync *ft_private)
{
	static const gchar *PUBLISH_FILE_REQUEST =
			"Content-Type: application/ms-filetransfer+xml\r\n"
			"Content-Transfer-Encoding: 7bit\r\n"
			"Content-Disposition: render; handling=optional\r\n"
			"\r\n"
			"<request xmlns=\"http://schemas.microsoft.com/rtc/2009/05/filetransfer\" requestId=\"%u\">"
				"<publishFile>"
					"<fileInfo>"
						"<id>{6244F934-2EB1-443F-8E2C-48BA64AF463D}</id>"
						"<name>%s</name>"
						"<size>%u</size>"
					"</fileInfo>"
				"</publishFile>"
			"</request>\r\n";
	gchar *body;

	ft_private->request_id =
			++ft_private->sipe_private->ms_filetransfer_request_id;

	body = g_strdup_printf(PUBLISH_FILE_REQUEST, ft_private->request_id,
			       ft_private->file_name, ft_private->file_size);

	sipe_media_add_extra_invite_section(call, "multipart/mixed", body);
}

static void
ft_lync_outgoing_init(struct sipe_file_transfer *ft, const gchar *filename,
		      gsize size, SIPE_UNUSED_PARAMETER const gchar *who)
{
	struct sipe_core_private *sipe_private =
			SIPE_FILE_TRANSFER_PRIVATE->sipe_private;
	struct sipe_file_transfer_lync *ft_private = SIPE_FILE_TRANSFER_PRIVATE;
	struct sipe_media_call *call;
	struct sipe_media_stream *stream;

	ft_private->file_name = g_strdup(filename);
	ft_private->file_size = size;

	call = sipe_data_session_new_outgoing(sipe_private, who, TRUE,
					      SIPE_ICE_RFC_5245);

	ft_private->dialog = sipe_media_get_sip_dialog(call);
	ft_private->call_private = (struct sipe_media_call_private *) call;

	stream = sipe_media_stream_add(call, "data", SIPE_MEDIA_APPLICATION,
				       SIPE_ICE_RFC_5245, TRUE);
	if (!stream) {
		sipe_backend_notify_error(SIPE_CORE_PUBLIC,
					  _("Error occurred"),
					  _("Error creating data stream"));

		sipe_backend_media_hangup(call->backend_private, FALSE);
		sipe_backend_ft_cancel_local(ft);
		return;
	}

	sipe_media_stream_add_extra_attribute(stream, "sendonly", NULL);
	sipe_media_stream_add_extra_attribute(stream, "mid", "1");
	sipe_media_stream_set_data(stream, ft, NULL);
	append_publish_file_invite(call, ft_private);
}

static gboolean
ft_lync_outgoing_end(SIPE_UNUSED_PARAMETER struct sipe_file_transfer *ft)
{
	/* We still need our filetransfer structure so don't let backend
	 * deallocate it. We'll free it in process_notify(). */
	ft->deallocate = NULL;

	return TRUE;
}

struct sipe_file_transfer *
sipe_core_ft_lync_create_outgoing(struct sipe_core_public *sipe_public)
{
	struct sipe_file_transfer_lync *ft_private =
		g_new0(struct sipe_file_transfer_lync, 1);

	ft_private->sipe_private = SIPE_CORE_PRIVATE;
	ft_private->public.init = ft_lync_outgoing_init;
	ft_private->public.end = ft_lync_outgoing_end;
	ft_private->public.deallocate = ft_lync_deallocate;

	return (struct sipe_file_transfer *)ft_private;
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
