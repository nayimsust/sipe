/**
 * @file sipe-media.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2011-2015 SIPE Project <http://sipe.sourceforge.net/>
 * Copyright (C) 2010 Jakub Adam <jakub.adam@ktknet.cz>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "sipe-common.h"
#include "sipmsg.h"
#include "sip-transport.h"
#include "sipe-backend.h"
#include "sdpmsg.h"
#include "sipe-chat.h"
#include "sipe-conf.h"
#include "sipe-core.h"
#include "sipe-core-private.h"
#include "sipe-dialog.h"
#include "sipe-media.h"
#include "sipe-ocs2007.h"
#include "sipe-session.h"
#include "sipe-utils.h"
#include "sipe-nls.h"
#include "sipe-schedule.h"
#include "sipe-xml.h"

struct sipe_media_call_private {
	struct sipe_media_call public;

	/* private part starts here */
	struct sipe_core_private	*sipe_private;

	struct sip_session		*session;

	GSList				*streams;

	struct sipmsg			*invitation;
	SipeIceVersion			 ice_version;
	gboolean			 encryption_compatible;
	gchar				*extra_invite_section;
	gchar				*invite_content_type;

	struct sdpmsg			*smsg;
	GSList				*failed_media;
};
#define SIPE_MEDIA_CALL         ((struct sipe_media_call *) call_private)
#define SIPE_MEDIA_CALL_PRIVATE ((struct sipe_media_call_private *) call)

struct sipe_media_stream_private {
	struct sipe_media_stream public;

	guchar *encryption_key;
	int encryption_key_id;
	gboolean remote_candidates_and_codecs_set;

	GSList *extra_sdp;

	/* Arbitrary data associated with the stream. */
	gpointer data;
	GDestroyNotify data_free_func;
};
#define SIPE_MEDIA_STREAM         ((struct sipe_media_stream *) stream_private)
#define SIPE_MEDIA_STREAM_PRIVATE ((struct sipe_media_stream_private *) stream)

static void sipe_media_codec_list_free(GList *codecs)
{
	for (; codecs; codecs = g_list_delete_link(codecs, codecs))
		sipe_backend_codec_free(codecs->data);
}

static void sipe_media_candidate_list_free(GList *candidates)
{
	for (; candidates; candidates = g_list_delete_link(candidates, candidates))
		sipe_backend_candidate_free(candidates->data);
}

static void
remove_stream(struct sipe_media_call* call,
	      struct sipe_media_stream_private *stream_private)
{
	struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;

	sipe_media_stream_set_data(SIPE_MEDIA_STREAM, NULL, NULL);

	call_private->streams =
			g_slist_remove(call_private->streams, stream_private);
	sipe_backend_media_stream_free(SIPE_MEDIA_STREAM->backend_private);
	g_free(SIPE_MEDIA_STREAM->id);
	g_free(stream_private->encryption_key);
	sipe_utils_nameval_free(stream_private->extra_sdp);
	g_free(stream_private);
}

static gboolean
call_private_equals(SIPE_UNUSED_PARAMETER const gchar *callid,
		    struct sipe_media_call_private *call_private1,
		    struct sipe_media_call_private *call_private2)
{
	return call_private1 == call_private2;
}

static void
sipe_media_call_free(struct sipe_media_call_private *call_private)
{
	if (call_private) {
		g_hash_table_foreach_remove(call_private->sipe_private->media_calls,
					    (GHRFunc) call_private_equals, call_private);

		while (call_private->streams) {
			remove_stream(SIPE_MEDIA_CALL,
				      call_private->streams->data);
		}

		sipe_backend_media_free(call_private->public.backend_private);

		if (call_private->session) {
			sipe_session_remove(call_private->sipe_private,
					    call_private->session);
		}

		if (call_private->invitation)
			sipmsg_free(call_private->invitation);

		sipe_media_add_extra_invite_section(SIPE_MEDIA_CALL, NULL, NULL);

		sdpmsg_free(call_private->smsg);
		sipe_utils_slist_free_full(call_private->failed_media,
				  (GDestroyNotify)sdpmedia_free);
		g_free(SIPE_MEDIA_CALL->with);
		g_free(call_private);
	}
}

static gint
candidate_sort_cb(struct sdpcandidate *c1, struct sdpcandidate *c2)
{
	int cmp = sipe_strcompare(c1->foundation, c2->foundation);
	if (cmp == 0) {
		cmp = sipe_strcompare(c1->username, c2->username);
		if (cmp == 0)
			cmp = c1->component - c2->component;
	}

	return cmp;
}

static GSList *
backend_candidates_to_sdpcandidate(GList *candidates)
{
	GSList *result = NULL;
	GList *i;

	for (i = candidates; i; i = i->next) {
		struct sipe_backend_candidate *candidate = i->data;
		struct sdpcandidate *c;

		gchar *ip = sipe_backend_candidate_get_ip(candidate);
		gchar *base_ip = sipe_backend_candidate_get_base_ip(candidate);
		if (is_empty(ip) || strchr(ip, ':') ||
		    (base_ip && strchr(base_ip, ':'))) {
			/* Ignore IPv6 candidates. */
			g_free(ip);
			g_free(base_ip);
			continue;
		}

		c = g_new(struct sdpcandidate, 1);
		c->foundation = sipe_backend_candidate_get_foundation(candidate);
		c->component = sipe_backend_candidate_get_component_type(candidate);
		c->type = sipe_backend_candidate_get_type(candidate);
		c->protocol = sipe_backend_candidate_get_protocol(candidate);
		c->ip = ip;
		c->port = sipe_backend_candidate_get_port(candidate);
		c->base_ip = base_ip;
		c->base_port = sipe_backend_candidate_get_base_port(candidate);
		c->priority = sipe_backend_candidate_get_priority(candidate);
		c->username = sipe_backend_candidate_get_username(candidate);
		c->password = sipe_backend_candidate_get_password(candidate);

		result = g_slist_insert_sorted(result, c,
					       (GCompareFunc)candidate_sort_cb);
	}

	return result;
}

static void
get_stream_ip_and_ports(GSList *candidates,
			gchar **ip, guint *rtp_port, guint *rtcp_port,
			SipeCandidateType type)
{
	*ip = 0;
	*rtp_port = 0;
	*rtcp_port = 0;

	for (; candidates; candidates = candidates->next) {
		struct sdpcandidate *candidate = candidates->data;

		if (type == SIPE_CANDIDATE_TYPE_ANY || candidate->type == type) {
			if (!*ip) {
				*ip = g_strdup(candidate->ip);
			} else if (!sipe_strequal(*ip, candidate->ip)) {
				continue;
			}

			if (candidate->component == SIPE_COMPONENT_RTP) {
				*rtp_port = candidate->port;
			} else if (candidate->component == SIPE_COMPONENT_RTCP)
				*rtcp_port = candidate->port;
		}

		if (*rtp_port != 0 && *rtcp_port != 0)
			return;
	}
}

static gint
sdpcodec_compare(gconstpointer a, gconstpointer b)
{
	return ((const struct sdpcodec *)a)->id -
	       ((const struct sdpcodec *)b)->id;
}

static GList *
remove_wrong_farstream_0_1_tcp_candidates(GList *candidates)
{
	GList *i = candidates;
	GHashTable *foundation_to_candidate = g_hash_table_new_full(g_str_hash,
								    g_str_equal,
								    g_free,
								    NULL);

	while (i) {
		GList *next = i->next;
		struct sipe_backend_candidate *c1 = i->data;

		if (sipe_backend_candidate_get_protocol(c1) == SIPE_NETWORK_PROTOCOL_UDP) {
			gchar *foundation                 = sipe_backend_candidate_get_foundation(c1);
			struct sipe_backend_candidate *c2 = g_hash_table_lookup(foundation_to_candidate,
										foundation);

			if (c2) {
				g_free(foundation);

				if (sipe_backend_candidate_get_port(c1) ==
				    sipe_backend_candidate_get_port(c2) ||
				    (sipe_backend_candidate_get_type(c1) !=
				     SIPE_CANDIDATE_TYPE_HOST &&
				     sipe_backend_candidate_get_base_port(c1) ==
				     sipe_backend_candidate_get_base_port(c2))) {
					/*
					 * We assume that RTP+RTCP UDP pairs
					 * that share the same port are
					 * actually mistagged TCP candidates.
					 */
					candidates = g_list_remove(candidates, c2);
					candidates = g_list_delete_link(candidates, i);
					sipe_backend_candidate_free(c1);
					sipe_backend_candidate_free(c2);
				}
			} else
				/* hash table takes ownership of "foundation" */
				g_hash_table_insert(foundation_to_candidate, foundation, c1);
		}

		i = next;
	}

	g_hash_table_destroy(foundation_to_candidate);

	return candidates;
}

static void
fill_zero_tcp_act_ports_from_tcp_pass(GSList *candidates)
{
	GSList *i;
	GHashTable *ip_to_port = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = candidates; i; i = i->next) {
		struct sdpcandidate *c = i->data;
		GSList *j;

		if (c->protocol == SIPE_NETWORK_PROTOCOL_TCP_PASSIVE &&
		    c->type == SIPE_CANDIDATE_TYPE_HOST) {
			g_hash_table_insert(ip_to_port, c->ip,
					&c->port);
		}

		if (c->protocol != SIPE_NETWORK_PROTOCOL_TCP_ACTIVE) {
			continue;
		}

		for (j = candidates; j; j = j->next) {
			struct sdpcandidate *passive = j->data;
			if (passive->protocol != SIPE_NETWORK_PROTOCOL_TCP_PASSIVE ||
			    c->type != passive->type) {
				continue;
			}

			if (sipe_strequal(c->ip, passive->ip) &&
			    sipe_strequal(c->base_ip, passive->base_ip)) {
				if (c->port == 0) {
					c->port = passive->port;
				}

				if (c->base_port == 0) {
					c->base_port = passive->base_port;
				}
				break;
			}
		}
	}

	/* Fill base ports of all TCP relay candidates using what we have
	 * collected from host candidates. */
	for (i = candidates; i; i = i->next) {
		struct sdpcandidate *c = i->data;
		if (c->type == SIPE_CANDIDATE_TYPE_RELAY && c->base_port == 0) {
			guint *base_port = (guint*)g_hash_table_lookup(ip_to_port, c->base_ip);
			if (base_port) {
				c->base_port = *base_port;
			} else {
				SIPE_DEBUG_WARNING("Couldn't determine base port for candidate "
						   "with foundation %s", c->foundation);
			}
		}
	}

	g_hash_table_destroy(ip_to_port);
}

static SipeEncryptionPolicy
get_encryption_policy(struct sipe_core_private *sipe_private)
{
	SipeEncryptionPolicy result =
			sipe_backend_media_get_encryption_policy(SIPE_CORE_PUBLIC);
	if (result == SIPE_ENCRYPTION_POLICY_OBEY_SERVER) {
		result = sipe_private->server_av_encryption_policy;
	}

	return result;
}

static struct sdpmedia *
media_stream_to_sdpmedia(struct sipe_media_call_private *call_private,
			 struct sipe_media_stream_private *stream_private)
{
	struct sdpmedia *sdpmedia = g_new0(struct sdpmedia, 1);
	GList *codecs = sipe_backend_get_local_codecs(SIPE_MEDIA_CALL,
						      SIPE_MEDIA_STREAM);
	SipeEncryptionPolicy encryption_policy =
			get_encryption_policy(call_private->sipe_private);
	guint rtcp_port = 0;
	SipeMediaType type;
	GSList *attributes = NULL;
	GList *candidates;
	GList *i;
	GSList *j;

	sdpmedia->name = g_strdup(SIPE_MEDIA_STREAM->id);

	if (sipe_strequal(sdpmedia->name, "audio"))
		type = SIPE_MEDIA_AUDIO;
	else if (sipe_strequal(sdpmedia->name, "video"))
		type = SIPE_MEDIA_VIDEO;
	else if (sipe_strequal(sdpmedia->name, "data"))
		type = SIPE_MEDIA_APPLICATION;
	else if (sipe_strequal(sdpmedia->name, "applicationsharing"))
		type = SIPE_MEDIA_APPLICATION;
	else {
		// TODO: incompatible media, should not happen here
		g_free(sdpmedia->name);
		g_free(sdpmedia);
		sipe_media_codec_list_free(codecs);
		return(NULL);
	}

	// Process codecs
	for (i = codecs; i; i = i->next) {
		struct sipe_backend_codec *codec = i->data;
		struct sdpcodec *c = g_new0(struct sdpcodec, 1);
		GList *params;

		c->id = sipe_backend_codec_get_id(codec);
		c->name = sipe_backend_codec_get_name(codec);
		c->clock_rate = sipe_backend_codec_get_clock_rate(codec);
		c->type = type;

		params = sipe_backend_codec_get_optional_parameters(codec);
		for (; params; params = params->next) {
			struct sipnameval *param = params->data;
			struct sipnameval *copy = g_new0(struct sipnameval, 1);

			copy->name = g_strdup(param->name);
			copy->value = g_strdup(param->value);

			c->parameters = g_slist_append(c->parameters, copy);
		}

		/* Buggy(?) codecs may report non-unique id (a.k.a. payload
		 * type) that must not appear in SDP messages we send. Thus,
		 * let's ignore any codec having the same id as one we already
		 * have in the converted list. */
		sdpmedia->codecs = sipe_utils_slist_insert_unique_sorted(
				sdpmedia->codecs, c, sdpcodec_compare,
				(GDestroyNotify)sdpcodec_free);
	}

	sipe_media_codec_list_free(codecs);

	// Process local candidates
	// If we have established candidate pairs, send them in SDP response.
	// Otherwise send all available local candidates.
	candidates = sipe_backend_media_get_active_local_candidates(SIPE_MEDIA_CALL,
								    SIPE_MEDIA_STREAM);
	if (!candidates) {
		candidates = sipe_backend_get_local_candidates(SIPE_MEDIA_CALL,
							       SIPE_MEDIA_STREAM);
		candidates = remove_wrong_farstream_0_1_tcp_candidates(candidates);
	}

	sdpmedia->candidates = backend_candidates_to_sdpcandidate(candidates);
	fill_zero_tcp_act_ports_from_tcp_pass(sdpmedia->candidates);

	sipe_media_candidate_list_free(candidates);

	get_stream_ip_and_ports(sdpmedia->candidates, &sdpmedia->ip, &sdpmedia->port,
				&rtcp_port, SIPE_CANDIDATE_TYPE_HOST);
	// No usable HOST candidates, use any candidate
	if (sdpmedia->ip == NULL && sdpmedia->candidates) {
		get_stream_ip_and_ports(sdpmedia->candidates, &sdpmedia->ip, &sdpmedia->port,
					&rtcp_port, SIPE_CANDIDATE_TYPE_ANY);
	}

	if (sipe_backend_stream_is_held(SIPE_MEDIA_STREAM))
		attributes = sipe_utils_nameval_add(attributes, "inactive", "");

	if (rtcp_port) {
		gchar *tmp = g_strdup_printf("%u", rtcp_port);
		attributes  = sipe_utils_nameval_add(attributes, "rtcp", tmp);
		g_free(tmp);
	}

	if (encryption_policy != call_private->sipe_private->server_av_encryption_policy) {
		const gchar *encryption = NULL;
		switch (encryption_policy) {
			case SIPE_ENCRYPTION_POLICY_REJECTED:
				encryption = "rejected";
				break;
			case SIPE_ENCRYPTION_POLICY_OPTIONAL:
				encryption = "optional";
				break;
			case SIPE_ENCRYPTION_POLICY_REQUIRED:
			default:
				encryption = "required";
				break;
		}

		attributes = sipe_utils_nameval_add(attributes, "encryption", encryption);
	}

	// Process remote candidates
	candidates = sipe_backend_media_get_active_remote_candidates(SIPE_MEDIA_CALL,
								     SIPE_MEDIA_STREAM);
	sdpmedia->remote_candidates = backend_candidates_to_sdpcandidate(candidates);
	sipe_media_candidate_list_free(candidates);

	sdpmedia->encryption_active = stream_private->encryption_key &&
				      call_private->encryption_compatible &&
				      stream_private->remote_candidates_and_codecs_set &&
				      encryption_policy != SIPE_ENCRYPTION_POLICY_REJECTED;

	// Set our key if encryption is enabled.
	if (stream_private->encryption_key &&
	    encryption_policy != SIPE_ENCRYPTION_POLICY_REJECTED) {
		sdpmedia->encryption_key = g_memdup(stream_private->encryption_key,
						    SIPE_SRTP_KEY_LEN);
		sdpmedia->encryption_key_id = stream_private->encryption_key_id;
	}

	// Append extra attributes assigned to the stream.
	for (j = stream_private->extra_sdp; j; j = g_slist_next(j)) {
		struct sipnameval *attr = j->data;
		attributes = sipe_utils_nameval_add(attributes,
						    attr->name, attr->value);
	}

	sdpmedia->attributes = attributes;

	return sdpmedia;
}

static struct sdpmsg *
sipe_media_to_sdpmsg(struct sipe_media_call_private *call_private)
{
	struct sdpmsg *msg = g_new0(struct sdpmsg, 1);
	GSList *streams = call_private->streams;

	for (; streams; streams = streams->next) {
		struct sdpmedia *media = media_stream_to_sdpmedia(call_private,
								  streams->data);
		if (media) {
			msg->media = g_slist_append(msg->media, media);

			if (msg->ip == NULL)
				msg->ip = g_strdup(media->ip);
		}
	}

	msg->media = g_slist_concat(msg->media, call_private->failed_media);
	call_private->failed_media = NULL;

	msg->ice_version = call_private->ice_version;

	return msg;
}

static void
sipe_invite_call(struct sipe_media_call_private *call_private, TransCallback tc)
{
	struct sipe_core_private *sipe_private = call_private->sipe_private;
	gchar *hdr;
	gchar *contact;
	gchar *p_preferred_identity = NULL;
	gchar *body;
	struct sip_dialog *dialog = sipe_media_get_sip_dialog(SIPE_MEDIA_CALL);
	struct sdpmsg *msg;

	contact = get_contact(sipe_private);

	if (sipe_private->uc_line_uri) {
		gchar *self = sip_uri_self(sipe_private);
		p_preferred_identity = g_strdup_printf(
			"P-Preferred-Identity: <%s>, <%s>\r\n",
			self, sipe_private->uc_line_uri);
		g_free(self);
	}

	hdr = g_strdup_printf(
		"ms-keep-alive: UAC;hop-hop=yes\r\n"
		"Contact: %s\r\n"
		"%s"
		"Content-Type: %s%s\r\n",
		contact,
		p_preferred_identity ? p_preferred_identity : "",
		call_private->invite_content_type ?
			  call_private->invite_content_type : "application/sdp",
		call_private->invite_content_type ?
			";boundary=\"----=_NextPart_000_001E_01CB4397.0B5EB570\"" : "");

	g_free(contact);
	g_free(p_preferred_identity);

	msg = sipe_media_to_sdpmsg(call_private);
	body = sdpmsg_to_string(msg);

	if (call_private->extra_invite_section) {
		gchar *tmp;
		tmp = g_strdup_printf(
			"------=_NextPart_000_001E_01CB4397.0B5EB570\r\n"
			"%s"
			"\r\n"
			"------=_NextPart_000_001E_01CB4397.0B5EB570\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Transfer-Encoding: 7bit\r\n"
			"Content-Disposition: session; handling=optional\r\n"
			"\r\n"
			"%s"
			"\r\n"
			"------=_NextPart_000_001E_01CB4397.0B5EB570--\r\n",
			call_private->extra_invite_section, body);
		g_free(body);
		body = tmp;
		sipe_media_add_extra_invite_section(SIPE_MEDIA_CALL, NULL, NULL);
	}

	sdpmsg_free(msg);

	dialog->outgoing_invite = sip_transport_invite(sipe_private,
						       hdr,
						       body,
						       dialog,
						       tc);

	g_free(body);
	g_free(hdr);
}

static struct sip_dialog *
sipe_media_dialog_init(struct sip_session* session, struct sipmsg *msg)
{
	gchar *newTag = gentag();
	const gchar *oldHeader;
	gchar *newHeader;
	struct sip_dialog *dialog;

	oldHeader = sipmsg_find_header(msg, "To");
	newHeader = g_strdup_printf("%s;tag=%s", oldHeader, newTag);
	sipmsg_remove_header_now(msg, "To");
	sipmsg_add_header_now(msg, "To", newHeader);
	g_free(newHeader);

	dialog = sipe_dialog_add(session);
	dialog->callid = g_strdup(sipmsg_find_header(msg, "Call-ID"));
	dialog->with = parse_from(sipmsg_find_header(msg, "From"));
	sipe_dialog_parse(dialog, msg, FALSE);

	return dialog;
}

static void
send_response_with_session_description(struct sipe_media_call_private *call_private, int code, gchar *text)
{
	struct sdpmsg *msg = sipe_media_to_sdpmsg(call_private);
	gchar *body = sdpmsg_to_string(msg);
	sdpmsg_free(msg);
	sipmsg_add_header(call_private->invitation, "Content-Type", "application/sdp");
	sip_transport_response(call_private->sipe_private, call_private->invitation, code, text, body);
	g_free(body);
}

static gboolean
process_invite_call_response(struct sipe_core_private *sipe_private,
								   struct sipmsg *msg,
								   struct transaction *trans);

struct sipe_media_stream *
sipe_core_media_get_stream_by_id(struct sipe_media_call *call, const gchar *id)
{
	GSList *i;
	for (i = SIPE_MEDIA_CALL_PRIVATE->streams; i; i = i->next) {
		struct sipe_media_stream *stream = i->data;
		if (sipe_strequal(stream->id, id))
			return stream;
	}
	return NULL;
}

static gboolean
update_call_from_remote_sdp(struct sipe_media_call_private* call_private,
			    struct sdpmedia *media)
{
	struct sipe_media_stream *stream;
	GList *backend_candidates = NULL;
	GList *backend_codecs = NULL;
	GSList *i;
	gboolean result = TRUE;

	stream = sipe_core_media_get_stream_by_id(SIPE_MEDIA_CALL, media->name);
	if (media->port == 0) {
		if (stream) {
			sipe_backend_media_stream_end(SIPE_MEDIA_CALL, stream);
		}
		return TRUE;
	}

	if (!stream)
		return FALSE;

	if (sipe_utils_nameval_find(media->attributes, "inactive")) {
		sipe_backend_stream_hold(SIPE_MEDIA_CALL, stream, FALSE);
	} else if (sipe_backend_stream_is_held(stream)) {
		sipe_backend_stream_unhold(SIPE_MEDIA_CALL, stream, FALSE);
	}

	if (SIPE_MEDIA_STREAM_PRIVATE->remote_candidates_and_codecs_set) {
		return TRUE;
	}

	for (i = media->codecs; i; i = i->next) {
		struct sdpcodec *c = i->data;
		struct sipe_backend_codec *codec;
		GSList *j;

		codec = sipe_backend_codec_new(c->id,
					       c->name,
					       c->type,
					       c->clock_rate);

		for (j = c->parameters; j; j = j->next) {
			struct sipnameval *attr = j->data;

			sipe_backend_codec_add_optional_parameter(codec,
								  attr->name,
								  attr->value);
		}

		backend_codecs = g_list_append(backend_codecs, codec);
	}

	if (media->encryption_key && SIPE_MEDIA_STREAM_PRIVATE->encryption_key) {
		sipe_backend_media_set_encryption_keys(SIPE_MEDIA_CALL, stream,
				SIPE_MEDIA_STREAM_PRIVATE->encryption_key,
				media->encryption_key);
		SIPE_MEDIA_STREAM_PRIVATE->encryption_key_id = media->encryption_key_id;
	}

	result = sipe_backend_set_remote_codecs(SIPE_MEDIA_CALL, stream,
						backend_codecs);
	sipe_media_codec_list_free(backend_codecs);

	if (result == FALSE) {
		sipe_backend_media_stream_end(SIPE_MEDIA_CALL, stream);
		return FALSE;
	}

	for (i = media->candidates; i; i = i->next) {
		struct sdpcandidate *c = i->data;
		struct sipe_backend_candidate *candidate;
		candidate = sipe_backend_candidate_new(c->foundation,
						       c->component,
						       c->type,
						       c->protocol,
						       c->ip,
						       c->port,
						       c->username,
						       c->password);
		sipe_backend_candidate_set_priority(candidate, c->priority);

		backend_candidates = g_list_append(backend_candidates, candidate);
	}

	sipe_backend_media_add_remote_candidates(SIPE_MEDIA_CALL, stream,
						 backend_candidates);
	sipe_media_candidate_list_free(backend_candidates);

	SIPE_MEDIA_STREAM_PRIVATE->remote_candidates_and_codecs_set = TRUE;

	return TRUE;
}

static gboolean
apply_remote_message(struct sipe_media_call_private* call_private,
		     struct sdpmsg* msg)
{
	GSList *i;

	sipe_utils_slist_free_full(call_private->failed_media, (GDestroyNotify)sdpmedia_free);
	call_private->failed_media = NULL;
	call_private->encryption_compatible = TRUE;

	for (i = msg->media; i; i = i->next) {
		struct sdpmedia *media = i->data;
		const gchar *enc_level =
				sipe_utils_nameval_find(media->attributes, "encryption");
		if (sipe_strequal(enc_level, "rejected") &&
		    get_encryption_policy(call_private->sipe_private) == SIPE_ENCRYPTION_POLICY_REQUIRED) {
			call_private->encryption_compatible = FALSE;
		}

		if (!update_call_from_remote_sdp(call_private, media)) {
			media->port = 0;
			call_private->failed_media =
				g_slist_append(call_private->failed_media, media);
		}
	}

	/* We need to keep failed medias until response is sent, remove them
	 * from sdpmsg that is to be freed. */
	for (i = call_private->failed_media; i; i = i->next) {
		msg->media = g_slist_remove(msg->media, i->data);
	}

	/* FALSE if all streams failed - call ends. */
	return msg->media != NULL;
}

static gboolean
call_initialized(struct sipe_media_call *call)
{
	GSList *streams = SIPE_MEDIA_CALL_PRIVATE->streams;
	for (; streams; streams = streams->next) {
		if (!sipe_backend_stream_initialized(call, streams->data)) {
			return FALSE;
		}
	}

	return TRUE;
}

// Sends an invite response when the call is accepted and local candidates were
// prepared, otherwise does nothing. If error response is sent, call_private is
// disposed before function returns. Returns true when response was sent.
static gboolean
send_invite_response_if_ready(struct sipe_media_call_private *call_private)
{
	struct sipe_backend_media *backend_media;

	backend_media = call_private->public.backend_private;

	if (!sipe_backend_media_accepted(backend_media) ||
	    !call_initialized(&call_private->public))
		return FALSE;

	if (!call_private->encryption_compatible) {
		struct sipe_core_private *sipe_private = call_private->sipe_private;

		sipmsg_add_header(call_private->invitation, "Warning",
			"308 lcs.microsoft.com \"Encryption Levels not compatible\"");
		sip_transport_response(sipe_private,
			call_private->invitation,
			488, "Encryption Levels not compatible",
			NULL);
		sipe_backend_media_reject(backend_media, FALSE);
		sipe_backend_notify_error(SIPE_CORE_PUBLIC,
					  _("Unable to establish a call"),
					  _("Encryption settings of peer are incompatible with ours."));
	} else {
		send_response_with_session_description(call_private, 200, "OK");
	}

	return TRUE;
}

static void
stream_initialized_cb(struct sipe_media_call *call,
		      struct sipe_media_stream *stream)
{
	if (call_initialized(call)) {
		struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;

		if (sipe_backend_media_is_initiator(call, stream)) {
			sipe_invite_call(call_private,
					 process_invite_call_response);
		} else if (call_private->smsg) {
			struct sdpmsg *smsg = call_private->smsg;
			call_private->smsg = NULL;

			if (apply_remote_message(call_private, smsg)) {
				send_invite_response_if_ready(call_private);
			}
			sdpmsg_free(smsg);
		}
	}
}

static void phone_state_publish(struct sipe_core_private *sipe_private)
{
	if (SIPE_CORE_PRIVATE_FLAG_IS(OCS2007)) {
		sipe_ocs2007_phone_state_publish(sipe_private);
	} else {
		// TODO: OCS 2005 support. Is anyone still using it at all?
	}
}

static void
stream_end_cb(struct sipe_media_call* call, struct sipe_media_stream* stream)
{
	remove_stream(call, SIPE_MEDIA_STREAM_PRIVATE);
}

static void
media_end_cb(struct sipe_media_call *call)
{
	struct sipe_core_private *sipe_private;

	g_return_if_fail(call);

	sipe_private = SIPE_MEDIA_CALL_PRIVATE->sipe_private;

	sipe_media_call_free(SIPE_MEDIA_CALL_PRIVATE);
	phone_state_publish(sipe_private);
}

static void
call_accept_cb(struct sipe_media_call *call, gboolean local)
{
	if (local) {
		send_invite_response_if_ready(SIPE_MEDIA_CALL_PRIVATE);
	}
	phone_state_publish(SIPE_MEDIA_CALL_PRIVATE->sipe_private);
}

static void
call_reject_cb(struct sipe_media_call *call, gboolean local)
{
	if (local) {
		struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;

		sip_transport_response(call_private->sipe_private,
				       call_private->invitation,
				       603, "Decline", NULL);

		if (call_private->session) {
			sipe_session_remove(call_private->sipe_private,
					    call_private->session);
			call_private->session = NULL;
		}
	}
}

static gboolean
sipe_media_send_ack(struct sipe_core_private *sipe_private, struct sipmsg *msg,
					struct transaction *trans);

static void call_hold_cb(struct sipe_media_call *call,
			 gboolean local,
			 SIPE_UNUSED_PARAMETER gboolean state)
{
	if (local) {
		sipe_invite_call(SIPE_MEDIA_CALL_PRIVATE, sipe_media_send_ack);
	}
}

static void call_hangup_cb(struct sipe_media_call *call, gboolean local)
{
	if (local) {
		struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;

		if (call_private->session) {
			sipe_session_close(call_private->sipe_private,
					   call_private->session);
			call_private->session = NULL;
		}
	}
}

static void
error_cb(struct sipe_media_call *call, gchar *message)
{
	struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;
	struct sipe_core_private *sipe_private = call_private->sipe_private;
	gboolean initiator = sipe_backend_media_is_initiator(call, NULL);
	gboolean accepted = sipe_backend_media_accepted(call->backend_private);

	gchar *title = g_strdup_printf("Call with %s failed", call->with);
	sipe_backend_notify_error(SIPE_CORE_PUBLIC, title, message);
	g_free(title);

	if (!initiator && !accepted) {
		sip_transport_response(sipe_private,
				       call_private->invitation,
				       488, "Not Acceptable Here", NULL);
	}

	sipe_backend_media_hangup(call->backend_private, initiator || accepted);
}

static struct sipe_media_call_private *
create_media(struct sipe_core_private *sipe_private, const gchar *callid,
	     const gchar* with, gboolean initiator, SipeIceVersion ice_version,
	     gboolean hidden_from_ui)
{
	struct sipe_media_call_private *call_private = g_new0(struct sipe_media_call_private, 1);
	gchar *cname;

	call_private->sipe_private = sipe_private;
	g_assert(g_hash_table_lookup(sipe_private->media_calls, callid) == NULL);
	g_hash_table_insert(sipe_private->media_calls, g_strdup(callid), call_private);

	cname = g_strdup(sipe_private->contact + 1);
	cname[strlen(cname) - 1] = '\0';

	call_private->public.backend_private = sipe_backend_media_new(SIPE_CORE_PUBLIC,
								      SIPE_MEDIA_CALL,
								      with,
								      initiator,
								      hidden_from_ui);
	sipe_backend_media_set_cname(call_private->public.backend_private, cname);

	call_private->ice_version = ice_version;
	call_private->encryption_compatible = TRUE;

	call_private->public.stream_initialized_cb  = stream_initialized_cb;
	call_private->public.stream_end_cb          = stream_end_cb;
	call_private->public.media_end_cb           = media_end_cb;
	call_private->public.call_accept_cb         = call_accept_cb;
	call_private->public.call_reject_cb         = call_reject_cb;
	call_private->public.call_hold_cb           = call_hold_cb;
	call_private->public.call_hangup_cb         = call_hangup_cb;
	call_private->public.error_cb               = error_cb;

	g_free(cname);

	return call_private;
}

static struct sipe_media_call_private *
sipe_media_call_new(struct sipe_core_private *sipe_private, const gchar *callid,
		    const gchar* with, gboolean initiator,
		    SipeIceVersion ice_version)
{
	return create_media(sipe_private, callid, with, initiator, ice_version, FALSE);
}

static struct sipe_media_call_private *
sipe_data_session_new(struct sipe_core_private *sipe_private,
		      const gchar *callid, const gchar* with,
		      gboolean initiator, SipeIceVersion ice_version)
{
	return create_media(sipe_private, callid, with, initiator, ice_version, TRUE);
}

static struct sipe_media_call_private *
create_media_outgoing(struct sipe_core_private *sipe_private, const gchar* with,
		      gboolean initiator, SipeIceVersion ice_version,
		      gboolean hidden_from_ui)
{
	struct sipe_media_call_private *call_private;
	struct sip_session *session = sipe_session_add_call(sipe_private, with);
	struct sip_dialog *dialog = sipe_dialog_add(session);

	dialog->callid = gencallid();
	dialog->with = g_strdup(session->with);
	dialog->ourtag = gentag();

	call_private = create_media(sipe_private, dialog->callid, with,
				    initiator, ice_version, hidden_from_ui);

	call_private->session = session;
	SIPE_MEDIA_CALL->with = g_strdup(with);

	return call_private;
}

static struct sipe_media_call_private *
sipe_media_call_new_outgoing(struct sipe_core_private *sipe_private,
			     const gchar* with, gboolean initiator,
			     SipeIceVersion ice_version)
{
	return create_media_outgoing(sipe_private, with, initiator, ice_version,
				     FALSE);
}

struct sipe_media_call *
sipe_data_session_new_outgoing(struct sipe_core_private *sipe_private,
			       const gchar* with, gboolean initiator,
			       SipeIceVersion ice_version)
{
	struct sipe_media_call_private *call_private =
			create_media_outgoing(sipe_private, with, initiator,
					      ice_version, TRUE);
	return SIPE_MEDIA_CALL;
}

struct sipe_media_stream *
sipe_media_stream_add(struct sipe_media_call *call, const gchar *id,
		      SipeMediaType type, SipeIceVersion ice_version,
		      gboolean initiator)
{
	struct sipe_media_call_private *call_private = SIPE_MEDIA_CALL_PRIVATE;
	struct sipe_core_private *sipe_private = call_private->sipe_private;
	struct sipe_media_stream_private *stream_private;
	struct sipe_backend_media_stream *backend_stream;
	struct sipe_backend_media_relays *backend_media_relays;
	guint min_port = sipe_private->min_media_port;
	guint max_port = sipe_private->max_media_port;

	backend_media_relays = sipe_backend_media_relays_convert(
						sipe_private->media_relays,
						sipe_private->media_relay_username,
						sipe_private->media_relay_password);

	switch (type) {
		case SIPE_MEDIA_AUDIO:
			min_port = sipe_private->min_audio_port;
			max_port = sipe_private->max_audio_port;
			break;
		case SIPE_MEDIA_VIDEO:
			min_port = sipe_private->min_video_port;
			max_port = sipe_private->max_audio_port;
			break;
		case SIPE_MEDIA_APPLICATION:
			if (sipe_strequal(id, "data")) {
				min_port = sipe_private->min_filetransfer_port;
				max_port = sipe_private->max_filetransfer_port;
			} else if (sipe_strequal(id, "applicationsharing")) {
				min_port = sipe_private->min_appsharing_port;
				max_port = sipe_private->max_appsharing_port;
			}
			break;
	}

	backend_stream = sipe_backend_media_add_stream(SIPE_MEDIA_CALL,
						       id, call->with, type,
						       ice_version, initiator,
						       backend_media_relays,
						       min_port, max_port);

	sipe_backend_media_relays_free(backend_media_relays);

	if (!backend_stream) {
		return NULL;
	}

	stream_private = g_new0(struct sipe_media_stream_private, 1);
	SIPE_MEDIA_STREAM->id = g_strdup(id);
	SIPE_MEDIA_STREAM->backend_private = backend_stream;

#ifdef HAVE_SRTP
	{
		int i;
		stream_private->encryption_key = g_new0(guchar, SIPE_SRTP_KEY_LEN);
		for (i = 0; i != SIPE_SRTP_KEY_LEN; ++i) {
			stream_private->encryption_key[i] = rand() & 0xff;
		}
		stream_private->encryption_key_id = 1;
	}
#endif

	SIPE_MEDIA_CALL_PRIVATE->streams =
			g_slist_append(SIPE_MEDIA_CALL_PRIVATE->streams,
				       stream_private);

	return SIPE_MEDIA_STREAM;
}

static void
append_proxy_fallback_invite_if_needed(struct sipe_media_call_private *call_private)
{
	struct sipe_core_private *sipe_private = call_private->sipe_private;
	const gchar *ip = sipe_backend_network_ip_address(SIPE_CORE_PUBLIC);
	gchar *body;

	if (sipe_media_get_sip_dialog(SIPE_MEDIA_CALL)->cseq != 0 ||
	    call_private->ice_version != SIPE_ICE_RFC_5245 ||
	    sipe_strequal(SIPE_MEDIA_CALL->with, sipe_private->test_call_bot_uri)) {
		return;
	}

	body = g_strdup_printf("Content-Type: application/sdp\r\n"
			       "Content-Transfer-Encoding: 7bit\r\n"
			       "Content-Disposition: session; handling=optional; ms-proxy-2007fallback\r\n"
			       "\r\n"
			       "o=- 0 0 IN IP4 %s\r\n"
			       "s=session\r\n"
			       "c=IN IP4 %s\r\n"
			       "m=audio 0 RTP/AVP\r\n",
			       ip, ip);
	sipe_media_add_extra_invite_section(SIPE_MEDIA_CALL,
					    "multipart/alternative",body);
}

static void
sipe_media_initiate_call(struct sipe_core_private *sipe_private,
			 const char *with, SipeIceVersion ice_version,
			 gboolean with_video)
{
	struct sipe_media_call_private *call_private;

	if (sipe_core_media_get_call(SIPE_CORE_PUBLIC)) {
		return;
	}

	call_private = sipe_media_call_new_outgoing(sipe_private, with, TRUE,
						    ice_version);

	if (!sipe_media_stream_add(SIPE_MEDIA_CALL, "audio", SIPE_MEDIA_AUDIO,
				   call_private->ice_version,
				   TRUE)) {
		sipe_backend_notify_error(SIPE_CORE_PUBLIC,
					  _("Error occured"),
					  _("Error creating audio stream"));
		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private,
					  FALSE);
		return;
	}

	if (with_video &&
	    !sipe_media_stream_add(SIPE_MEDIA_CALL, "video", SIPE_MEDIA_VIDEO,
				   call_private->ice_version,
				   TRUE)) {
		sipe_backend_notify_error(SIPE_CORE_PUBLIC,
					  _("Error occured"),
					  _("Error creating video stream"));
		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private,
					  FALSE);
		return;
	}

	append_proxy_fallback_invite_if_needed(call_private);

	// Processing continues in stream_initialized_cb
}

void
sipe_core_media_initiate_call(struct sipe_core_public *sipe_public,
			      const char *with,
			      gboolean with_video)
{
	sipe_media_initiate_call(SIPE_CORE_PRIVATE, with,
				 SIPE_ICE_RFC_5245, with_video);
}

void sipe_core_media_connect_conference(struct sipe_core_public *sipe_public,
					struct sipe_chat_session *chat_session)
{
	struct sipe_media_call_private *call_private;
	struct sipe_core_private *sipe_private = SIPE_CORE_PRIVATE;
	struct sip_session *session;
	SipeIceVersion ice_version;
	gchar **parts;
	gchar *av_uri;

	if (!sipe_conf_supports_mcu_type(sipe_private, "audio-video")) {
		sipe_backend_notify_error(sipe_public, _("Join conference call"),
				_("Conference calls are not supported on this server."));
		return;
	}

	session = sipe_session_find_chat(sipe_private, chat_session);

	if (sipe_core_media_get_call(sipe_public) || !session) {
		return;
	}

	session->is_call = TRUE;

	parts = g_strsplit(chat_session->id, "app:conf:focus:", 2);
	av_uri = g_strjoinv("app:conf:audio-video:", parts);
	g_strfreev(parts);

	ice_version = SIPE_CORE_PRIVATE_FLAG_IS(LYNC2013) ? SIPE_ICE_RFC_5245 :
							    SIPE_ICE_DRAFT_6;

	call_private = sipe_media_call_new_outgoing(sipe_private, av_uri, TRUE,
						    ice_version);

	g_free(av_uri);

	if (!sipe_media_stream_add(SIPE_MEDIA_CALL, "audio", SIPE_MEDIA_AUDIO,
				   call_private->ice_version, TRUE)) {
		sipe_backend_notify_error(sipe_public,
					  _("Error occured"),
					  _("Error creating audio stream"));

		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private,
					  FALSE);
	}

	// Processing continues in stream_initialized_cb
}

struct sipe_media_call *
sipe_core_media_get_call(struct sipe_core_public *sipe_public)
{
	struct sipe_media_call * result = NULL;
	GList *calls = g_hash_table_get_values(SIPE_CORE_PRIVATE->media_calls);

	for (; calls; calls = g_list_delete_link(calls, calls)) {
		if (sipe_core_media_get_stream_by_id(calls->data, "audio")) {
			result = calls->data;
			break;
		}
	}

	return result;
}

static gboolean phone_number_is_valid(const gchar *phone_number)
{
	if (!phone_number || sipe_strequal(phone_number, "")) {
		return FALSE;
	}

	if (*phone_number == '+') {
		++phone_number;
	}

	while (*phone_number != '\0') {
		if (!g_ascii_isdigit(*phone_number)) {
			return FALSE;
		}
		++phone_number;
	}

	return TRUE;
}

void sipe_core_media_phone_call(struct sipe_core_public *sipe_public,
				const gchar *phone_number)
{
	g_return_if_fail(sipe_public);

	if (phone_number_is_valid(phone_number)) {
		gchar *phone_uri = g_strdup_printf("sip:%s@%s;user=phone",
				phone_number, sipe_public->sip_domain);

		sipe_core_media_initiate_call(sipe_public, phone_uri, FALSE);

		g_free(phone_uri);
	} else {
		sipe_backend_notify_error(sipe_public,
					  _("Unable to establish a call"),
					  _("Invalid phone number"));
	}
}

void sipe_core_media_test_call(struct sipe_core_public *sipe_public)
{
	struct sipe_core_private *sipe_private = SIPE_CORE_PRIVATE;
	if (!sipe_private->test_call_bot_uri) {
		sipe_backend_notify_error(sipe_public,
					  _("Unable to establish a call"),
					  _("Audio Test Service is not available."));
		return;
	}

	sipe_core_media_initiate_call(sipe_public,
				      sipe_private->test_call_bot_uri, FALSE);
}

static struct sipe_media_call_private *
sipe_media_from_sipmsg(struct sipe_core_private *sipe_private,
		       struct sipmsg *msg)
{
	return g_hash_table_lookup(sipe_private->media_calls,
				   sipmsg_find_header(msg, "Call-ID"));
}

struct sipe_media_call_private *
process_incoming_invite_call(struct sipe_core_private *sipe_private,
			     struct sipmsg *msg)
{
	struct sipe_media_call_private *call_private;
	struct sdpmsg *smsg;
	gboolean has_new_media = FALSE;
	GSList *i;

	// Don't allow two voice calls in parallel.
	if (!strstr(msg->body, "m=data") &&
	    !strstr(msg->body, "m=applicationsharing")) {
		struct sipe_media_call *call =
				sipe_core_media_get_call(SIPE_CORE_PUBLIC);
		if (call && !is_media_session_msg(SIPE_MEDIA_CALL_PRIVATE, msg)) {
			sip_transport_response(sipe_private, msg,
					       486, "Busy Here", NULL);
			return NULL;
		}
	}

	call_private = sipe_media_from_sipmsg(sipe_private, msg);

	if (call_private) {
		char *self = sip_uri_self(sipe_private);
		if (sipe_strequal(SIPE_MEDIA_CALL->with, self)) {
			g_free(self);
			sip_transport_response(sipe_private, msg, 488, "Not Acceptable Here", NULL);
			return NULL;
		}
		g_free(self);
	}

	smsg = sdpmsg_parse_msg(msg->body);
	if (!smsg) {
		sip_transport_response(sipe_private, msg,
				       488, "Not Acceptable Here", NULL);
		if (call_private) {
			sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private, FALSE);
		}
		return NULL;
	}

	if (!call_private) {
		gchar *with = parse_from(sipmsg_find_header(msg, "From"));
		const gchar *callid = sipmsg_find_header(msg, "Call-ID");

		if (strstr(msg->body, "m=data") ||
		    strstr(msg->body, "m=applicationsharing")) {
			call_private = sipe_data_session_new(sipe_private, callid,
					with, FALSE, smsg->ice_version);
		} else {
			call_private = sipe_media_call_new(sipe_private, callid,
					with, FALSE, smsg->ice_version);
		}

		call_private->session = sipe_session_add_call(sipe_private, with);
		sipe_media_dialog_init(call_private->session, msg);

		SIPE_MEDIA_CALL->with = g_strdup(call_private->session->with);
		g_free(with);
	}

	if (call_private->invitation)
		sipmsg_free(call_private->invitation);
	call_private->invitation = sipmsg_copy(msg);

	// Create any new media streams
	for (i = smsg->media; i; i = i->next) {
		struct sdpmedia *media = i->data;
		gchar *id = media->name;
		SipeMediaType type;

		if (   media->port != 0
		    && !sipe_core_media_get_stream_by_id(SIPE_MEDIA_CALL, id)) {
			struct sipe_media_stream *stream;

			if (sipe_strequal(id, "audio"))
				type = SIPE_MEDIA_AUDIO;
			else if (sipe_strequal(id, "video"))
				type = SIPE_MEDIA_VIDEO;
			else if (sipe_strequal(id, "data"))
				type = SIPE_MEDIA_APPLICATION;
			else if (sipe_strequal(id, "applicationsharing"))
				type = SIPE_MEDIA_APPLICATION;
			else
				continue;

			stream = sipe_media_stream_add(SIPE_MEDIA_CALL, id, type,
						       smsg->ice_version, FALSE);

			if (sipe_strequal(id, "data")) {
				sipe_media_stream_add_extra_attribute(stream, "recvonly", NULL);
			} else if (sipe_strequal(id, "applicationsharing")) {
				sipe_media_stream_add_extra_attribute(stream,
						"x-applicationsharing-session-id", "1");
				sipe_media_stream_add_extra_attribute(stream,
						"x-applicationsharing-role", "viewer");
				sipe_media_stream_add_extra_attribute(stream,
						"x-applicationsharing-media-type", "rdp");
			}

			has_new_media = TRUE;
		}
	}

	if (has_new_media) {
		sdpmsg_free(call_private->smsg);
		call_private->smsg = smsg;
		sip_transport_response(sipe_private, call_private->invitation,
				       180, "Ringing", NULL);
		// Processing continues in stream_initialized_cb
	} else {
		apply_remote_message(call_private, smsg);
		send_response_with_session_description(call_private, 200, "OK");

		sdpmsg_free(smsg);
	}

	return call_private;
}

void process_incoming_cancel_call(struct sipe_media_call_private *call_private,
				  struct sipmsg *msg)
{
	// We respond to the CANCEL request with 200 OK response and
	// with 487 Request Terminated to the remote INVITE in progress.
	sip_transport_response(call_private->sipe_private, msg, 200, "OK", NULL);

	if (call_private->invitation) {
		sip_transport_response(call_private->sipe_private,
				       call_private->invitation,
				       487, "Request Terminated", NULL);
	}

	sipe_backend_media_reject(SIPE_MEDIA_CALL->backend_private, FALSE);
}

static gboolean
sipe_media_send_ack(struct sipe_core_private *sipe_private,
		    struct sipmsg *msg,
		    struct transaction *trans)
{
	struct sipe_media_call_private *call_private;
	struct sip_dialog *dialog;
	int tmp_cseq;

	call_private = sipe_media_from_sipmsg(sipe_private, msg);

	if (!is_media_session_msg(call_private, msg))
		return FALSE;

	dialog = sipe_media_get_sip_dialog(SIPE_MEDIA_CALL);
	if (!dialog)
		return FALSE;

	tmp_cseq = dialog->cseq;

	dialog->cseq = sip_transaction_cseq(trans) - 1;
	sip_transport_ack(sipe_private, dialog);
	dialog->cseq = tmp_cseq;

	dialog->outgoing_invite = NULL;

	return TRUE;
}

static gboolean
sipe_media_send_final_ack(struct sipe_core_private *sipe_private,
			  struct sipmsg *msg,
			  struct transaction *trans)
{
	struct sipe_media_call_private *call_private;

	if (!sipe_media_send_ack(sipe_private, msg, trans))
		return FALSE;

	call_private = sipe_media_from_sipmsg(sipe_private, msg);

	sipe_backend_media_accept(SIPE_MEDIA_CALL->backend_private, FALSE);

	return TRUE;
}

void
sipe_core_media_candidate_pair_established(struct sipe_media_call *call,
					   struct sipe_media_stream *stream)
{
	if (sipe_backend_media_is_initiator(call, stream)) {
		sipe_invite_call(SIPE_MEDIA_CALL_PRIVATE, sipe_media_send_final_ack);
	}

	if (call->candidate_pair_established_cb) {
		call->candidate_pair_established_cb(call, stream);
	}
}

static gboolean
maybe_retry_call_with_ice_version(struct sipe_media_call_private *call_private,
				  SipeIceVersion ice_version,
				  struct transaction *trans)
{
	if (call_private->ice_version != ice_version &&
	    sip_transaction_cseq(trans) == 1) {
		gchar *with = g_strdup(SIPE_MEDIA_CALL->with);
		gboolean with_video = sipe_core_media_get_stream_by_id(SIPE_MEDIA_CALL, "video") != NULL;

		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private, FALSE);
		SIPE_DEBUG_INFO("Retrying call with ICEv%d.",
				ice_version == SIPE_ICE_DRAFT_6 ? 6 : 19);
		sipe_media_initiate_call(call_private->sipe_private, with,
					 ice_version, with_video);

		g_free(with);
		return TRUE;
	}

	return FALSE;
}

static gboolean
process_invite_call_response(struct sipe_core_private *sipe_private,
			     struct sipmsg *msg,
			     struct transaction *trans)
{
	const gchar *with;
	struct sipe_media_call_private *call_private;
	struct sip_dialog *dialog;
	struct sdpmsg *smsg;

	call_private = sipe_media_from_sipmsg(sipe_private,msg);

	if (!is_media_session_msg(call_private, msg))
		return FALSE;

	dialog = sipe_media_get_sip_dialog(SIPE_MEDIA_CALL);

	with = dialog->with;

	dialog->outgoing_invite = NULL;

	if (msg->response >= 400) {
		// Call rejected by remote peer or an error occurred
		const gchar *title;
		GString *desc = g_string_new("");
		gboolean append_responsestr = FALSE;

		switch (msg->response) {
			case 480: {
				title = _("User unavailable");

				if (sipmsg_parse_warning(msg, NULL) == 391) {
					g_string_append_printf(desc, _("%s does not want to be disturbed"), with);
				} else
					g_string_append_printf(desc, _("User %s is not available"), with);
				break;
			}
			case 603:
			case 605:
				title = _("Call rejected");
				g_string_append_printf(desc, _("User %s rejected call"), with);
				break;
			case 415:
				// OCS/Lync really sends response string with 'Mutipart' typo.
				if (sipe_strequal(msg->responsestr, "Mutipart mime in content type not supported by Archiving CDR service") &&
				    maybe_retry_call_with_ice_version(call_private, SIPE_ICE_DRAFT_6, trans)) {
					return TRUE;
				}
				title = _("Unsupported media type");
				break;
			case 488: {
				/* Check for incompatible encryption levels error.
				 *
				 * MS Lync 2010:
				 * 488 Not Acceptable Here
				 * ms-client-diagnostics: 52017;reason="Encryption levels dont match"
				 *
				 * older clients (and SIPE itself):
				 * 488 Encryption Levels not compatible
				 */
				const gchar *ms_diag = sipmsg_find_header(msg, "ms-client-diagnostics");
				SipeIceVersion retry_ice_version = SIPE_ICE_DRAFT_6;

				if (sipe_strequal(msg->responsestr, "Encryption Levels not compatible") ||
				    (ms_diag && g_str_has_prefix(ms_diag, "52017;"))) {
					title = _("Unable to establish a call");
					g_string_append(desc, _("Encryption settings of peer are incompatible with ours."));
					break;
				}

				/* Check if this is failed conference using
				 * ICEv6 with reason "Error parsing SDP" and
				 * retry using ICEv19. */
				ms_diag = sipmsg_find_header(msg, "ms-diagnostics");
				if (ms_diag && g_str_has_prefix(ms_diag, "7008;")) {
					retry_ice_version = SIPE_ICE_RFC_5245;
				}

				if (maybe_retry_call_with_ice_version(call_private, retry_ice_version, trans)) {
					return TRUE;
				}
				// Break intentionally omitted
			}
			default:
				title = _("Error occured");
				g_string_append(desc, _("Unable to establish a call"));
				append_responsestr = TRUE;
				break;
		}

		if (append_responsestr) {
			gchar *reason = sipmsg_get_ms_diagnostics_reason(msg);

			g_string_append_printf(desc, "\n%d %s",
					       msg->response, msg->responsestr);
			if (reason) {
				g_string_append_printf(desc, "\n\n%s", reason);
				g_free(reason);
			}
		}

		sipe_backend_notify_error(SIPE_CORE_PUBLIC, title, desc->str);
		g_string_free(desc, TRUE);

		sipe_media_send_ack(sipe_private, msg, trans);
		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private, FALSE);

		return TRUE;
	}

	sipe_dialog_parse(dialog, msg, TRUE);
	smsg = sdpmsg_parse_msg(msg->body);
	if (!smsg) {
		sip_transport_response(sipe_private, msg,
				       488, "Not Acceptable Here", NULL);
		sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private, FALSE);
		return FALSE;
	}

	apply_remote_message(call_private, smsg);
	sdpmsg_free(smsg);

	sipe_media_send_ack(sipe_private, msg, trans);

	return TRUE;

	// Waits until sipe_core_media_candidate_pair_established() is invoked.
}

gboolean is_media_session_msg(struct sipe_media_call_private *call_private,
			      struct sipmsg *msg)
{
	if (!call_private) {
		return FALSE;
	}

	return sipe_media_from_sipmsg(call_private->sipe_private, msg) == call_private;
}

static void
end_call(SIPE_UNUSED_PARAMETER gpointer key,
	 struct sipe_media_call_private *call_private,
	 SIPE_UNUSED_PARAMETER gpointer user_data)
{
	if (!sipe_backend_media_is_initiator(SIPE_MEDIA_CALL, NULL) &&
	    !sipe_backend_media_accepted(SIPE_MEDIA_CALL->backend_private)) {
		sip_transport_response(call_private->sipe_private,
				       call_private->invitation,
				       480, "Temporarily Unavailable", NULL);
	} else if (call_private->session) {
		sipe_session_close(call_private->sipe_private,
				   call_private->session);
		call_private->session = NULL;
	}

	sipe_backend_media_hangup(SIPE_MEDIA_CALL->backend_private, FALSE);
}

void
sipe_media_handle_going_offline(struct sipe_core_private *sipe_private)
{
	g_hash_table_foreach(sipe_private->media_calls, (GHFunc) end_call, NULL);
}

gboolean sipe_media_is_conference_call(struct sipe_media_call_private *call_private)
{
	return g_strstr_len(SIPE_MEDIA_CALL->with, -1, "app:conf:audio-video:") != NULL;
}

struct sipe_core_private *
sipe_media_get_sipe_core_private(struct sipe_media_call *call)
{
	g_return_val_if_fail(call, NULL);

	return SIPE_MEDIA_CALL_PRIVATE->sipe_private;
}

struct sip_dialog *
sipe_media_get_sip_dialog(struct sipe_media_call *call)
{
	struct sip_session *session;

	g_return_val_if_fail(call, NULL);

	session = SIPE_MEDIA_CALL_PRIVATE->session;

	if (!session || !session->dialogs) {
		return NULL;
	}

	return session->dialogs->data;
}

static void
sipe_media_relay_free(struct sipe_media_relay *relay)
{
	g_free(relay->hostname);
	if (relay->dns_query)
		sipe_backend_dns_query_cancel(relay->dns_query);
	g_free(relay);
}

void
sipe_media_relay_list_free(GSList *list)
{
	for (; list; list = g_slist_delete_link(list, list))
		sipe_media_relay_free(list->data);
}

static void
relay_ip_resolved_cb(struct sipe_media_relay* relay,
		     const gchar *ip, SIPE_UNUSED_PARAMETER guint port)
{
	gchar *hostname = relay->hostname;
	relay->dns_query = NULL;

	if (ip && port) {
		relay->hostname = g_strdup(ip);
		SIPE_DEBUG_INFO("Media relay %s resolved to %s.", hostname, ip);
	} else {
		relay->hostname = NULL;
		SIPE_DEBUG_INFO("Unable to resolve media relay %s.", hostname);
	}

	g_free(hostname);
}

static gboolean
process_get_av_edge_credentials_response(struct sipe_core_private *sipe_private,
					 struct sipmsg *msg,
					 SIPE_UNUSED_PARAMETER struct transaction *trans)
{
	g_free(sipe_private->media_relay_username);
	g_free(sipe_private->media_relay_password);
	sipe_media_relay_list_free(sipe_private->media_relays);
	sipe_private->media_relay_username = NULL;
	sipe_private->media_relay_password = NULL;
	sipe_private->media_relays = NULL;

	if (msg->response >= 400) {
		SIPE_DEBUG_INFO_NOFORMAT("process_get_av_edge_credentials_response: SERVICE response is not 200. "
					 "Failed to obtain A/V Edge credentials.");
		return FALSE;
	}

	if (msg->response == 200) {
		sipe_xml *xn_response = sipe_xml_parse(msg->body, msg->bodylen);

		if (sipe_strequal("OK", sipe_xml_attribute(xn_response, "reasonPhrase"))) {
			const sipe_xml *xn_credentials = sipe_xml_child(xn_response, "credentialsResponse/credentials");
			const sipe_xml *xn_relays = sipe_xml_child(xn_response, "credentialsResponse/mediaRelayList");
			const sipe_xml *item;
			GSList *relays = NULL;

			item = sipe_xml_child(xn_credentials, "username");
			sipe_private->media_relay_username = sipe_xml_data(item);
			item = sipe_xml_child(xn_credentials, "password");
			sipe_private->media_relay_password = sipe_xml_data(item);

			for (item = sipe_xml_child(xn_relays, "mediaRelay"); item; item = sipe_xml_twin(item)) {
				struct sipe_media_relay *relay = g_new0(struct sipe_media_relay, 1);
				const sipe_xml *node;
				gchar *tmp;

				node = sipe_xml_child(item, "hostName");
				relay->hostname = sipe_xml_data(node);

				node = sipe_xml_child(item, "udpPort");
				if (node) {
					relay->udp_port = atoi(tmp = sipe_xml_data(node));
					g_free(tmp);
				}

				node = sipe_xml_child(item, "tcpPort");
				if (node) {
					relay->tcp_port = atoi(tmp = sipe_xml_data(node));
					g_free(tmp);
				}

				relays = g_slist_append(relays, relay);

				relay->dns_query = sipe_backend_dns_query_a(
							SIPE_CORE_PUBLIC,
							relay->hostname,
							relay->udp_port,
							(sipe_dns_resolved_cb) relay_ip_resolved_cb,
							relay);

				SIPE_DEBUG_INFO("Media relay: %s TCP: %d UDP: %d",
						relay->hostname,
						relay->tcp_port, relay->udp_port);
			}

			sipe_private->media_relays = relays;
		}

		sipe_xml_free(xn_response);
	}

	return TRUE;
}

void
sipe_media_get_av_edge_credentials(struct sipe_core_private *sipe_private)
{
	// TODO: re-request credentials after duration expires?
	static const char CRED_REQUEST_XML[] =
		"<request requestID=\"%d\" "
		         "from=\"%s\" "
			 "version=\"1.0\" "
			 "to=\"%s\" "
			 "xmlns=\"http://schemas.microsoft.com/2006/09/sip/mrasp\" "
			 "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
			"<credentialsRequest credentialsRequestID=\"%d\">"
				"<identity>%s</identity>"
				"<location>%s</location>"
				"<duration>480</duration>"
			"</credentialsRequest>"
		"</request>";

	int request_id = rand();
	gchar *self;
	gchar *body;

	if (!sipe_private->mras_uri)
		return;

	self = sip_uri_self(sipe_private);

	body = g_strdup_printf(
		CRED_REQUEST_XML,
		request_id,
		self,
		sipe_private->mras_uri,
		request_id,
		self,
		SIPE_CORE_PRIVATE_FLAG_IS(REMOTE_USER) ? "internet" : "intranet");
	g_free(self);

	sip_transport_service(sipe_private,
			      sipe_private->mras_uri,
			      "Content-Type: application/msrtc-media-relay-auth+xml\r\n",
			      body,
			      process_get_av_edge_credentials_response);

	g_free(body);
}

void
sipe_media_add_extra_invite_section(struct sipe_media_call *call,
				    const gchar *invite_content_type,
				    gchar *body)
{
	g_free(SIPE_MEDIA_CALL_PRIVATE->extra_invite_section);
	g_free(SIPE_MEDIA_CALL_PRIVATE->invite_content_type);
	SIPE_MEDIA_CALL_PRIVATE->extra_invite_section = body;
	SIPE_MEDIA_CALL_PRIVATE->invite_content_type =
			g_strdup(invite_content_type);
}

void
sipe_media_stream_add_extra_attribute(struct sipe_media_stream *stream,
				      const gchar *name, const gchar *value)
{
	SIPE_MEDIA_STREAM_PRIVATE->extra_sdp =
			sipe_utils_nameval_add(SIPE_MEDIA_STREAM_PRIVATE->extra_sdp,
					       name, value);
}

void
sipe_media_stream_set_data(struct sipe_media_stream *stream, gpointer data,
			   GDestroyNotify free_func)
{
	struct sipe_media_stream_private *stream_private =
			SIPE_MEDIA_STREAM_PRIVATE;

	g_return_if_fail(stream_private);

	if (stream_private->data && stream_private->data_free_func) {
		stream_private->data_free_func(stream_private->data);
	}

	stream_private->data = data;
	stream_private->data_free_func = free_func;
}

gpointer
sipe_media_stream_get_data(struct sipe_media_stream *stream)
{
	g_return_val_if_fail(stream, NULL);

	return SIPE_MEDIA_STREAM_PRIVATE->data;
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
