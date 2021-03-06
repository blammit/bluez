/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/uuid.h>

#include <glib.h>
#include <dbus/dbus.h>

#include "log.h"

#include "glib-helper.h"
#include "btio.h"
#include "adapter.h"
#include "device.h"
#include "server.h"
#include "../src/manager.h"
#include "../src/device.h"

#define MAX_ACCEPT_RETRIES 29

static GSList *servers = NULL;

struct input_server {
	bdaddr_t src;
	GIOChannel *ctrl;
	GIOChannel *intr;
	GIOChannel *confirm;

	struct {
		GIOChannel *chan;
		int retries;
		bdaddr_t src;
		bdaddr_t dst;
		int timer;
	} pending_accept;
};

static gint server_cmp(gconstpointer s, gconstpointer user_data)
{
	const struct input_server *server = s;
	const bdaddr_t *src = user_data;

	return bacmp(&server->src, src);
}

static void connect_event_cb(GIOChannel *chan, GError *err, gpointer data)
{
	uint16_t psm;
	bdaddr_t src, dst;
	char address[18];
	GError *gerr = NULL;
	int ret;

	if (err) {
		error("%s", err->message);
		return;
	}

	bt_io_get(chan, BT_IO_L2CAP, &gerr,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_PSM, &psm,
			BT_IO_OPT_INVALID);
	if (gerr) {
		error("%s", gerr->message);
		g_error_free(gerr);
		g_io_channel_shutdown(chan, TRUE, NULL);
		return;
	}

	ba2str(&dst, address);
	DBG("Incoming connection from %s on PSM %d", address, psm);

	ret = input_device_set_channel(&src, &dst, psm, chan);
	if (ret == 0)
		return;

	error("Refusing input device connect: %s (%d)", strerror(-ret), -ret);

	/* Send unplug virtual cable to unknown devices */
	if (ret == -ENOENT && psm == L2CAP_PSM_HIDP_CTRL) {
		unsigned char unplug = 0x15;
		int sk = g_io_channel_unix_get_fd(chan);
		if (write(sk, &unplug, sizeof(unplug)) < 0)
			error("Unable to send virtual cable unplug");
	}

	g_io_channel_shutdown(chan, TRUE, NULL);
}

static struct btd_device *device_for_connection(const bdaddr_t *src,
                                                const bdaddr_t *dst)
{
	struct btd_adapter *adapter;
	struct btd_device *device;
	char sstr[18];
	char dstr[18];

	ba2str(src, sstr);
	ba2str(dst, dstr);

	adapter = manager_find_adapter(src);
	if (adapter == NULL) {
		DBG("No adapter for address %s.", sstr);
		return NULL;
	}
	DBG("Adapter found.");

	device = adapter_find_device(adapter, dstr);
	if (device == NULL) {
		DBG("No device for address %s.", dstr);
		return NULL;
	}

	return device;
}

static gboolean server_accept(struct input_server *server)
{
	GError *err = NULL;

	DBG("");

	if (!bt_io_accept(server->pending_accept.chan, connect_event_cb,
				server, NULL, &err)) {
		error("bt_io_accept: %s", err->message);
		g_error_free(err);
		return FALSE;
	}

	return TRUE;
}

static gboolean retry_server_accept(void *user_data)
{
	struct input_server *server = user_data;
	struct btd_device *device = NULL;

	DBG("");

	device = device_for_connection(&server->pending_accept.src,
				&server->pending_accept.dst);
	if (!device) {
		DBG("No device");
		goto cleanup;
	}

	if (device_has_service_records(device)) {
		DBG("Device has service records");
		if (!server_accept(server))
			DBG("Accept failed");
		goto cleanup;
	}

	if (server->pending_accept.retries >= MAX_ACCEPT_RETRIES) {
		DBG("Retry cap reached.");
		goto cleanup;
	}

	server->pending_accept.retries++;
	return TRUE;
	
cleanup:
	server->pending_accept.timer = 0;
	g_io_channel_unref(server->pending_accept.chan);
	server->pending_accept.chan = NULL;
	return FALSE;
}

static void ctrl_confirm_event_cb(GIOChannel *chan, gpointer user_data)
{
	struct input_server *server = user_data;
	bdaddr_t src, dst;
	GError *err = NULL;
	struct btd_device *device = NULL;

	DBG("");

	bt_io_get(chan, BT_IO_L2CAP, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		goto drop;
	}

	device = device_for_connection(&src, &dst);
	if (!device) {
		DBG("No device.");
		goto drop;
	}

	if (device_has_service_records(device)) {
		DBG("Device has service records");
		server->pending_accept.chan = chan;
		if (server_accept(server))
			return;

		DBG("Accept failed");
		goto drop;
	}

	if (server->pending_accept.timer) {
		DBG("Accept already pending.");
		goto drop;
	}

	DBG("Device has no service records, pending accept.");
	server->pending_accept.chan = chan;
	g_io_channel_ref(server->pending_accept.chan);
	server->pending_accept.retries = 0;
	server->pending_accept.src = src;
	server->pending_accept.dst = dst;
	server->pending_accept.timer = g_timeout_add_seconds(1,
							retry_server_accept,
							server);
	return;

drop:
	g_io_channel_shutdown(chan, TRUE, NULL);
}

static void auth_callback(DBusError *derr, void *user_data)
{
	struct input_server *server = user_data;
	bdaddr_t src, dst;
	GError *err = NULL;

	bt_io_get(server->confirm, BT_IO_L2CAP, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		goto reject;
	}

	if (derr) {
		error("Access denied: %s", derr->message);
		goto reject;
	}

	if (!bt_io_accept(server->confirm, connect_event_cb, server,
				NULL, &err)) {
		error("bt_io_accept: %s", err->message);
		g_error_free(err);
		goto reject;
	}

	g_io_channel_unref(server->confirm);
	server->confirm = NULL;

	return;

reject:
	g_io_channel_shutdown(server->confirm, TRUE, NULL);
	g_io_channel_unref(server->confirm);
	server->confirm = NULL;
	input_device_close_channels(&src, &dst);
}

static void intr_confirm_event_cb(GIOChannel *chan, gpointer user_data)
{
	struct input_server *server = user_data;
	bdaddr_t src, dst;
	GError *err = NULL;
	char addr[18];
	int ret;

	DBG("");

	bt_io_get(chan, BT_IO_L2CAP, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		goto drop;
	}

	if (server->confirm) {
		char address[18];

		ba2str(&dst, address);
		error("Refusing connection from %s: setup in progress",
								address);
		goto drop;
	}

	server->confirm = g_io_channel_ref(chan);

	ret = btd_request_authorization(&src, &dst, HID_UUID,
					auth_callback, server);
	if (ret == 0)
		return;

	ba2str(&src, addr);
	error("input: authorization for %s failed: %s (%d)",
						addr, strerror(-ret), ret);

	g_io_channel_unref(server->confirm);
	server->confirm = NULL;

drop:
	input_device_close_channels(&src, &dst);
	g_io_channel_shutdown(chan, TRUE, NULL);
}

int server_start(const bdaddr_t *src)
{
	struct input_server *server;
	GError *err = NULL;

	server = g_new0(struct input_server, 1);
	bacpy(&server->src, src);

	server->ctrl = bt_io_listen(BT_IO_L2CAP, NULL, ctrl_confirm_event_cb,
				server, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, src,
				BT_IO_OPT_PSM, L2CAP_PSM_HIDP_CTRL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID);
	if (!server->ctrl) {
		error("Failed to listen on control channel");
		g_error_free(err);
		g_free(server);
		return -1;
	}

	server->intr = bt_io_listen(BT_IO_L2CAP, NULL, intr_confirm_event_cb,
				server, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, src,
				BT_IO_OPT_PSM, L2CAP_PSM_HIDP_INTR,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID);
	if (!server->intr) {
		error("Failed to listen on interrupt channel");
		g_io_channel_unref(server->ctrl);
		g_error_free(err);
		g_free(server);
		return -1;
	}

	servers = g_slist_append(servers, server);

	return 0;
}

void server_stop(const bdaddr_t *src)
{
	struct input_server *server;
	GSList *l;

	l = g_slist_find_custom(servers, src, server_cmp);
	if (!l)
		return;

	server = l->data;

	g_io_channel_shutdown(server->intr, TRUE, NULL);
	g_io_channel_unref(server->intr);

	g_io_channel_shutdown(server->ctrl, TRUE, NULL);
	g_io_channel_unref(server->ctrl);

	if (server->pending_accept.timer)
		g_source_remove(server->pending_accept.timer);

	if (server->pending_accept.chan)
		g_io_channel_unref(server->pending_accept.chan);

	servers = g_slist_remove(servers, server);
	g_free(server);
}
