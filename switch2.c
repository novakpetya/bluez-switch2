// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BlueZ - Nintendo Joy-Con 2 GATT transport plugin
 *
 * This plugin deliberately owns transport mechanics only.  It waits until
 * BlueZ has completed ordinary GATT service resolution, identifies the
 * measured Joy-Con 2 service/characteristics, opens the switch2 kernel GATT
 * transport, and copies packets in both directions.
 *
 * Controller identity, initialization, decoding, compatibility devices and
 * rumble policy remain in the kernel. This plugin owns BLE/GATT mechanics only.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>

#include "lib/bluetooth/bluetooth.h"
#include "lib/bluetooth/uuid.h"

#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"
#include "src/plugin.h"
#include "src/shared/att.h"
#include "src/shared/gatt-client.h"
#include "src/shared/gatt-db.h"

#define SWITCH2_SERVICE_UUID "ab7de9be-89fe-49ad-828f-118f09df7fd0"
#define SWITCH2_COMMON_INPUT_UUID "ab7de9be-89fe-49ad-828f-118f09df7fd2"
#define SWITCH2_REPORT_RATE_UUID   "679d5510-5a24-4dee-9557-95df80486ecb"
#define SWITCH2_CONTROL_UUID "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
#define SWITCH2_REPLY_UUID   "c765a961-d9d8-4d36-a20a-5315b111836a"
#define SWITCH2_LEFT_INPUT_UUID    "cc1bbbb5-7354-4d32-a716-a81cb241a32a"
#define SWITCH2_RIGHT_INPUT_UUID   "d5a9e01e-2ffc-4cca-b20c-8b67142bf442"
#define SWITCH2_LEFT_VIBRATION_UUID  "289326cb-a471-485d-a8f4-240c14f18241"
#define SWITCH2_RIGHT_VIBRATION_UUID "fa19b0fb-cd1f-46a7-84a1-bbb09e00c149"
#define SWITCH2_BOOTSTRAP_UUID "00c5af5d-1964-4e30-8f51-1956f96bd280"

#define SWITCH2_GATT_BOOTSTRAP_START 0x0001
#define SWITCH2_GATT_BOOTSTRAP_END   0x0007
#define SWITCH2_GATT_SERVICE_START   0x0008
#define SWITCH2_GATT_SERVICE_END     0x002a
#define SWITCH2_GATT_COMMON_DECL     0x0009
#define SWITCH2_GATT_COMMON_VALUE    0x000a
#define SWITCH2_GATT_COMMON_CCC      0x000b
#define SWITCH2_GATT_COMMON_RATE     0x000c
#define SWITCH2_COMMON_SWITCH_MS 2500
#define SWITCH2_COMMON_SETTLE_MS 250
#define SWITCH2_GATT_SIDE_DECL       0x000d
#define SWITCH2_GATT_SIDE_VALUE      0x000e
#define SWITCH2_GATT_SIDE_CCC        0x000f
#define SWITCH2_GATT_SIDE_RATE       0x0010
#define SWITCH2_GATT_VIBRATION_DECL  0x0011
#define SWITCH2_GATT_VIBRATION_VALUE 0x0012
#define SWITCH2_GATT_CONTROL_DECL    0x0013
#define SWITCH2_GATT_CONTROL_VALUE   0x0014
#define SWITCH2_GATT_REPLY_DECL      0x0019
#define SWITCH2_GATT_REPLY_VALUE     0x001a
#define SWITCH2_GATT_REPLY_CCC       0x001b
#define SWITCH2_GATT_GAP_START       0x002b
#define SWITCH2_GATT_GAP_END         0x002f
#define SWITCH2_GATT_GATT_START      0x0030
#define SWITCH2_GATT_GATT_END        0x0030

#define SWITCH2_VENDOR 0x057e
#define SWITCH2_LEFT_PRODUCT 0x2067
#define SWITCH2_RIGHT_PRODUCT 0x2066
#define SWITCH2_MANUFACTURER 0x0553

#define SWITCH2_REPORT_COMMON 0x05
#define SWITCH2_REPORT_LEFT 0x07
#define SWITCH2_REPORT_RIGHT 0x08

#define SWITCH2_INPUT_SIZE 63
#define SWITCH2_MAX_CONTROL 255
#define SWITCH2_COMMAND_TIMEOUT_MS 2000

#define SWITCH2_TRANSPORT_PATH "/dev/switch2-gatt"
#define SWITCH2_TRANSPORT_ABI 1
#define SWITCH2_TRANSPORT_MAX_PAYLOAD 255
#define SWITCH2_TRANSPORT_HEADER_SIZE 4

enum switch2_transport_rx_type {
	SWITCH2_TRANSPORT_RX_ATTACH = 0x01,
	SWITCH2_TRANSPORT_RX_STATE = 0x02,
	SWITCH2_TRANSPORT_RX_REPLY = 0x03,
};

enum switch2_transport_tx_type {
	SWITCH2_TRANSPORT_TX_COMMAND = 0x81,
	SWITCH2_TRANSPORT_TX_RAW = 0x82,
	SWITCH2_TRANSPORT_TX_OUTPUT = 0x83,
};

enum switch2_input_subscription {
	SWITCH2_INPUT_SIDE,
	SWITCH2_INPUT_SWITCHING,
	SWITCH2_INPUT_COMMON,
};

struct __attribute__((packed)) switch2_transport_header {
	uint8_t abi;
	uint8_t type;
	uint16_t length;
};

struct __attribute__((packed)) switch2_transport_attach {
	uint16_t product;
	uint8_t report_id;
	char address[18];
};

struct switch2_handles {
	uint16_t control;
	uint16_t reply;
	uint16_t side_input;
	uint16_t vibration;
	uint8_t report_id;
	uint16_t product;
};

struct switch2_session {
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct bt_gatt_client *gatt;
	struct switch2_handles handles;
	char address[18];
	int transport_fd;
	guint transport_source;
	unsigned int reply_notify_id;
	unsigned int input_notify_id;
	bool reply_notify_ready;
	bool side_notify_ready;
	bool kernel_command_seen;
	bool kernel_attached;
	enum switch2_input_subscription input_subscription;
	bool stopping;
	guint stop_source;
	guint common_switch_source;

	bool pending;
	guint pending_timeout;
	uint8_t pending_command;
	uint8_t pending_transport;
	uint8_t pending_subcommand;

};

struct switch2_autoconnect {
	struct btd_adapter *adapter;
	struct btd_device *device;
	char address[18];
};

static GSList *sessions;
static GSList *autoconnect_devices;

static struct switch2_session *switch2_find_session(struct btd_device *device)
{
	GSList *l;

	for (l = sessions; l; l = l->next) {
		struct switch2_session *session = l->data;

		if (session->device == device)
			return session;
	}

	return NULL;
}

static bool switch2_uuid_equal(const bt_uuid_t *uuid, const char *expected)
{
	bt_uuid_t uuid128;
	char value[MAX_LEN_UUID_STR];

	bt_uuid_to_uuid128(uuid, &uuid128);
	if (bt_uuid_to_string(&uuid128, value, sizeof(value)) < 0)
		return false;

	return !strcasecmp(value, expected);
}

static void switch2_scan_characteristic(struct gatt_db_attribute *attr,
					 void *user_data)
{
	struct switch2_handles *handles = user_data;
	uint16_t handle, value_handle, ext_prop;
	uint8_t properties;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, &handle, &value_handle,
					     &properties, &ext_prop, &uuid))
		return;
	(void) handle;
	(void) properties;
	(void) ext_prop;

	if (switch2_uuid_equal(&uuid, SWITCH2_CONTROL_UUID))
		handles->control = value_handle;
	else if (switch2_uuid_equal(&uuid, SWITCH2_REPLY_UUID))
		handles->reply = value_handle;
	else if (switch2_uuid_equal(&uuid, SWITCH2_LEFT_VIBRATION_UUID) ||
		 switch2_uuid_equal(&uuid, SWITCH2_RIGHT_VIBRATION_UUID))
		handles->vibration = value_handle;
	else if (switch2_uuid_equal(&uuid, SWITCH2_LEFT_INPUT_UUID)) {
		handles->side_input = value_handle;
		handles->report_id = SWITCH2_REPORT_LEFT;
		handles->product = SWITCH2_LEFT_PRODUCT;
	} else if (switch2_uuid_equal(&uuid, SWITCH2_RIGHT_INPUT_UUID)) {
		handles->side_input = value_handle;
		handles->report_id = SWITCH2_REPORT_RIGHT;
		handles->product = SWITCH2_RIGHT_PRODUCT;
	}
}

static void switch2_scan_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct switch2_handles *handles = user_data;
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
						&uuid))
		return;
	(void) start;
	(void) end;
	(void) primary;

	if (!switch2_uuid_equal(&uuid, SWITCH2_SERVICE_UUID))
		return;

	gatt_db_service_foreach_char(attr, switch2_scan_characteristic, handles);
}

static bool switch2_find_handles(struct btd_device *device,
				 struct switch2_handles *handles)
{
	struct gatt_db *db;

	memset(handles, 0, sizeof(*handles));
	db = btd_device_get_gatt_db(device);
	if (!db)
		return false;

	gatt_db_foreach_service(db, NULL, switch2_scan_service, handles);

	return handles->control && handles->reply && handles->side_input &&
		handles->vibration && handles->report_id && handles->product;
}

static struct gatt_db_attribute *switch2_gatt_insert_service(
					struct gatt_db *db, uint16_t start,
					uint16_t end, const char *uuid_str)
{
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;

	if (bt_string_to_uuid(&uuid, uuid_str) < 0)
		return NULL;

	service = gatt_db_insert_service(db, start, &uuid, true,
					 end - start + 1);
	if (!service)
		return NULL;

	return service;
}

static bool switch2_gatt_insert_characteristic(
					struct gatt_db_attribute *service,
					uint16_t decl_handle,
					uint16_t value_handle,
					const char *uuid_str,
					uint8_t properties)
{
	bt_uuid_t uuid;

	if (bt_string_to_uuid(&uuid, uuid_str) < 0)
		return false;

	return gatt_db_service_insert_characteristic(service, decl_handle,
						      value_handle, &uuid, 0,
						      properties, NULL, NULL,
						      NULL) != NULL;
}

static bool switch2_gatt_insert_ccc(struct gatt_db_attribute *service,
				    uint16_t handle)
{
	bt_uuid_t uuid;

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	return gatt_db_service_insert_descriptor(service, handle, &uuid, 0,
						 NULL, NULL, NULL) != NULL;
}

static bool switch2_gatt_insert_descriptor(
					struct gatt_db_attribute *service,
					uint16_t handle,
					const char *uuid_str)
{
	bt_uuid_t uuid;

	if (bt_string_to_uuid(&uuid, uuid_str) < 0)
		return false;

	return gatt_db_service_insert_descriptor(service, handle, &uuid, 0,
						 NULL, NULL, NULL) != NULL;
}

static bool switch2_seed_gatt_db(struct btd_device *device, uint16_t product)
{
	struct gatt_db *db;
	struct gatt_db_attribute *bootstrap;
	struct gatt_db_attribute *vendor;
	struct gatt_db_attribute *gap;
	struct gatt_db_attribute *gatt;
	struct switch2_handles handles;
	const char *side_uuid;
	const char *vibration_uuid;
	char address[18];
	bt_uuid_t uuid;

	if (!device)
		return false;

	if (switch2_find_handles(device, &handles))
		return true;

	if (product != SWITCH2_LEFT_PRODUCT &&
	    product != SWITCH2_RIGHT_PRODUCT)
		return false;

	db = btd_device_get_gatt_db(device);
	if (!db || !gatt_db_isempty(db))
		return false;

	/*
	 * Joy-Con 2 primary discovery is reliable, but the controller never
	 * answers the subsequent ATT secondary-service discovery. Seed the fixed
	 * primary-service map as an active remote cache before BlueZ creates its
	 * bt_gatt_client. Primary discovery still runs and validates these ranges;
	 * because every returned primary service is already active,
	 * shared/gatt-client has no pending services and naturally skips its
	 * secondary-service procedure.
	 *
	 * Seed the characteristics used by this plugin plus the controller\'s
	 * common input characteristic and the report-rate descriptors at 0x000c
	 * and 0x0010. The common input is deliberately not forwarded as a native
	 * side report; exposing these fixed remote attributes lets diagnostics use
	 * the normal BlueZ GATT D-Bus APIs.
	 *
	 * The remaining primary services are placeholders whose UUID/range are
	 * validated again by normal primary discovery.
	 */
	bootstrap = switch2_gatt_insert_service(db,
				SWITCH2_GATT_BOOTSTRAP_START,
				SWITCH2_GATT_BOOTSTRAP_END,
				SWITCH2_BOOTSTRAP_UUID);
	vendor = switch2_gatt_insert_service(db, SWITCH2_GATT_SERVICE_START,
					    SWITCH2_GATT_SERVICE_END,
					    SWITCH2_SERVICE_UUID);
	if (!bootstrap || !vendor)
		goto fail;

	side_uuid = product == SWITCH2_RIGHT_PRODUCT ?
		SWITCH2_RIGHT_INPUT_UUID : SWITCH2_LEFT_INPUT_UUID;
	vibration_uuid = product == SWITCH2_RIGHT_PRODUCT ?
		SWITCH2_RIGHT_VIBRATION_UUID : SWITCH2_LEFT_VIBRATION_UUID;

	if (!switch2_gatt_insert_characteristic(vendor,
				SWITCH2_GATT_COMMON_DECL,
				SWITCH2_GATT_COMMON_VALUE,
				SWITCH2_COMMON_INPUT_UUID,
				BT_GATT_CHRC_PROP_READ |
				BT_GATT_CHRC_PROP_NOTIFY) ||
	    !switch2_gatt_insert_ccc(vendor, SWITCH2_GATT_COMMON_CCC) ||
	    !switch2_gatt_insert_descriptor(vendor,
				SWITCH2_GATT_COMMON_RATE,
				SWITCH2_REPORT_RATE_UUID) ||
	    !switch2_gatt_insert_characteristic(vendor,
				SWITCH2_GATT_SIDE_DECL,
				SWITCH2_GATT_SIDE_VALUE,
				side_uuid,
				BT_GATT_CHRC_PROP_READ |
				BT_GATT_CHRC_PROP_NOTIFY) ||
	    !switch2_gatt_insert_ccc(vendor, SWITCH2_GATT_SIDE_CCC) ||
	    !switch2_gatt_insert_descriptor(vendor,
				SWITCH2_GATT_SIDE_RATE,
				SWITCH2_REPORT_RATE_UUID) ||
	    !switch2_gatt_insert_characteristic(vendor,
				SWITCH2_GATT_VIBRATION_DECL,
				SWITCH2_GATT_VIBRATION_VALUE,
				vibration_uuid,
				BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP) ||
	    !switch2_gatt_insert_characteristic(vendor,
				SWITCH2_GATT_CONTROL_DECL,
				SWITCH2_GATT_CONTROL_VALUE,
				SWITCH2_CONTROL_UUID,
				BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP) ||
	    !switch2_gatt_insert_characteristic(vendor,
				SWITCH2_GATT_REPLY_DECL,
				SWITCH2_GATT_REPLY_VALUE,
				SWITCH2_REPLY_UUID,
				BT_GATT_CHRC_PROP_NOTIFY) ||
	    !switch2_gatt_insert_ccc(vendor, SWITCH2_GATT_REPLY_CCC))
		goto fail;

	bt_uuid16_create(&uuid, 0x1800);
	gap = gatt_db_insert_service(db, SWITCH2_GATT_GAP_START, &uuid, true,
				     SWITCH2_GATT_GAP_END -
				     SWITCH2_GATT_GAP_START + 1);
	bt_uuid16_create(&uuid, 0x1801);
	gatt = gatt_db_insert_service(db, SWITCH2_GATT_GATT_START, &uuid, true,
				      SWITCH2_GATT_GATT_END -
				      SWITCH2_GATT_GATT_START + 1);
	if (!gap || !gatt)
		goto fail;

	gatt_db_service_set_active(bootstrap, true);
	gatt_db_service_set_active(vendor, true);
	gatt_db_service_set_active(gap, true);
	gatt_db_service_set_active(gatt, true);

	if (!switch2_find_handles(device, &handles))
		goto fail;

	ba2str(device_get_address(device), address);
	info("switch2: seeded fixed Joy-Con 2 GATT cache for %s (%c)",
	     address, product == SWITCH2_RIGHT_PRODUCT ? 'R' : 'L');
	return true;

fail:
	gatt_db_clear(db);
	return false;
}

static bool switch2_prepare_gatt(struct btd_device *device,
				 uint16_t product_hint)
{
	struct switch2_handles handles;
	uint16_t product;

	if (!device)
		return false;

	if (switch2_find_handles(device, &handles))
		return true;

	product = product_hint ? product_hint : btd_device_get_product(device);
	return switch2_seed_gatt_db(device, product);
}

static struct switch2_autoconnect *
switch2_find_autoconnect(struct btd_device *device)
{
	GSList *l;

	for (l = autoconnect_devices; l; l = l->next) {
		struct switch2_autoconnect *entry = l->data;

		if (entry->device == device)
			return entry;
	}

	return NULL;
}

static bool switch2_is_known_device(struct btd_device *device)
{
	struct switch2_handles handles;
	uint16_t vendor;
	uint16_t product;

	if (!device)
		return false;

	/*
	 * Once connected/resolved, the exact live Nintendo GATT identity remains
	 * the strongest classifier.
	 */
	if (switch2_find_handles(device, &handles))
		return true;

	/*
	 * On bluetoothd restart BlueZ restores the persisted General/Services
	 * UUID list before device_added/device_resolved driver callbacks run.
	 * The live GATT DB is not rebuilt yet, but this exact Nintendo service UUID
	 * survives restart and is sufficient to re-enroll the known controller.
	 */
	if (!device_is_temporary(device) &&
	    btd_device_has_uuid(device, SWITCH2_SERVICE_UUID))
		return true;

	vendor = btd_device_get_vendor(device);
	product = btd_device_get_product(device);

	return vendor == SWITCH2_VENDOR &&
		(product == SWITCH2_LEFT_PRODUCT ||
		 product == SWITCH2_RIGHT_PRODUCT);
}

static void switch2_autoconnect_disable(struct btd_adapter *adapter,
					struct btd_device *device)
{
	struct switch2_autoconnect *entry;

	entry = switch2_find_autoconnect(device);
	if (!entry)
		return;

	/*
	 * These are complementary BlueZ paths: userspace passive scanning when
	 * kernel connection control is absent, or MGMT background connection
	 * control when it is present.
	 */
	adapter_connect_list_remove(adapter, device);
	adapter_auto_connect_remove(adapter, device);

	autoconnect_devices = g_slist_remove(autoconnect_devices, entry);
	DBG("switch2: auto-connect disabled for %s", entry->address);
	btd_device_unref(entry->device);
	g_free(entry);
}

static bool switch2_autoconnect_enable(struct btd_adapter *adapter,
				       struct btd_device *device)
{
	struct switch2_autoconnect *entry;
	int ret = 0;

	if (!adapter || !device)
		return false;

	if (switch2_find_autoconnect(device))
		return true;

	entry = g_new0(struct switch2_autoconnect, 1);
	entry->adapter = adapter;
	entry->device = btd_device_ref(device);
	ba2str(device_get_address(device), entry->address);

	/*
	 * Mirror BlueZ's own auto-connect setup: populate the kernel
	 * background-connect path when available; if currently disconnected,
	 * also populate the userspace passive-connect list used otherwise.
	 */
	adapter_auto_connect_add(adapter, device);
	if (!btd_device_is_connected(device))
		ret = adapter_connect_list_add(adapter, device);

	if (ret < 0) {
		error("switch2: %s failed to enable auto-connect: %s",
		      entry->address, strerror(-ret));
		adapter_auto_connect_remove(adapter, device);
		btd_device_unref(entry->device);
		g_free(entry);
		return false;
	}

	autoconnect_devices = g_slist_prepend(autoconnect_devices, entry);
	info("switch2: auto-connect enabled for %s", entry->address);
	return true;
}

static void switch2_autoconnect_consider(struct btd_device *device, void *data)
{
	struct btd_adapter *adapter = data;

	if (!switch2_is_known_device(device))
		return;

	/* A cached/resolved device already has the required handles. For an
	 * uncached known device, seed only when its exact product is known; an
	 * advertisement callback can supply that identity later if necessary. */
	if (!switch2_prepare_gatt(device, btd_device_get_product(device)))
		return;

	/* Equivalent of jc2-load-conn-param.py for every known Joy-Con 2:
	 * 7.5 ms fixed interval, zero peripheral latency, 420 ms supervision.
	 * Use bluetoothd's own MGMT queue before enabling auto-connect so the
	 * kernel connection-parameter cache is updated before the connect request. */
	btd_device_set_conn_param(device, 6, 6, 0, 42);
	switch2_autoconnect_enable(adapter, device);
}

static void switch2_manufacturer_data(struct btd_adapter *adapter,
				      struct btd_device *device,
				      uint16_t company,
				      const uint8_t *data,
				      uint8_t length)
{
	uint16_t vendor;
	uint16_t product;

	if (!adapter || !device || !data ||
	    company != SWITCH2_MANUFACTURER || length < 7)
		return;

	/*
	 * Observed Joy-Con 2 advertising identity:
	 *
	 *   company 0x0553
	 *   data    01 00 03 7e 05 67 20 ...  (left)
	 *   data    01 00 03 7e 05 66 20 ...  (right)
	 *
	 * Require the fixed prefix and Nintendo VID before accepting either
	 * measured Joy-Con 2 PID.
	 */
	if (data[0] != 0x01 || data[1] != 0x00 || data[2] != 0x03)
		return;

	vendor = (uint16_t) data[3] | ((uint16_t) data[4] << 8);
	product = (uint16_t) data[5] | ((uint16_t) data[6] << 8);

	if (vendor != SWITCH2_VENDOR ||
	    (product != SWITCH2_LEFT_PRODUCT &&
	     product != SWITCH2_RIGHT_PRODUCT))
		return;

	if (!switch2_prepare_gatt(device, product)) {
		error("switch2: failed to seed fixed GATT cache for advertised JC2");
		return;
	}

	/* Any normal BlueZ discovery (bluetoothctl or GUI) can discover an
	 * uncached Joy-Con 2 here. Seed the required connection parameters before
	 * making the controller auto-connectable. */
	btd_device_set_conn_param(device, 6, 6, 0, 42);
	switch2_autoconnect_enable(adapter, device);
}

static void switch2_autoconnect_rearm(struct btd_device *device)
{
	struct switch2_autoconnect *entry;
	int ret;

	entry = switch2_find_autoconnect(device);
	if (!entry || btd_device_is_connected(device))
		return;

	/*
	 * Successful userspace LE connects are removed from connect_list.
	 * Re-add after a later disconnect. On kernels with connection control
	 * this call is intentionally a no-op; the MGMT entry remains active.
	 */
	ret = adapter_connect_list_add(entry->adapter, device);
	if (ret < 0) {
		error("switch2: %s failed to re-arm auto-connect: %s",
		      entry->address, strerror(-ret));
		return;
	}

	DBG("switch2: auto-connect re-armed for %s", entry->address);
}

static int switch2_transport_write(struct switch2_session *session,
                                   uint8_t type, const void *payload,
                                   uint16_t length)
{
	uint8_t record[SWITCH2_TRANSPORT_HEADER_SIZE + SWITCH2_TRANSPORT_MAX_PAYLOAD];
	struct switch2_transport_header *header =
		(struct switch2_transport_header *) record;
	ssize_t written;

	if (!session || session->transport_fd < 0 || !payload || !length ||
	    length > SWITCH2_TRANSPORT_MAX_PAYLOAD)
		return -EINVAL;

	header->abi = SWITCH2_TRANSPORT_ABI;
	header->type = type;
	header->length = htole16(length);
	memcpy(record + sizeof(*header), payload, length);

	do {
		written = write(session->transport_fd, record,
				sizeof(*header) + length);
	} while (written < 0 && errno == EINTR);

	if (written < 0)
		return -errno;
	if (written != (ssize_t) (sizeof(*header) + length))
		return -EIO;
	return 0;
}

static int switch2_transport_attach(struct switch2_session *session)
{
	struct switch2_transport_attach attach;

	memset(&attach, 0, sizeof(attach));
	attach.product = htole16(session->handles.product);
	attach.report_id = session->handles.report_id;
	snprintf(attach.address, sizeof(attach.address), "%s", session->address);
	return switch2_transport_write(session, SWITCH2_TRANSPORT_RX_ATTACH,
		&attach, sizeof(attach));
}

static int switch2_transport_state(struct switch2_session *session,
                                   uint8_t report_id,
                                   const uint8_t *value, uint16_t length)
{
	uint8_t report[1 + SWITCH2_INPUT_SIZE];

	if (!value || length != SWITCH2_INPUT_SIZE)
		return -EINVAL;
	report[0] = report_id;
	memcpy(&report[1], value, length);
	return switch2_transport_write(session, SWITCH2_TRANSPORT_RX_STATE,
		report, sizeof(report));
}

static bool switch2_write_control(struct switch2_session *session,
				  const uint8_t *value, uint16_t length)
{
	if (!session->gatt || !value || !length)
		return false;

	return bt_gatt_client_write_without_response(session->gatt,
		session->handles.control, false, (uint8_t *) value, length);
}

static void switch2_stop_session(struct switch2_session *session)
{

	if (!session || session->stopping)
		return;

	session->stopping = true;
	if (session->stop_source) {
		g_source_remove(session->stop_source);
		session->stop_source = 0;
	}
	if (session->pending_timeout) {
		g_source_remove(session->pending_timeout);
		session->pending_timeout = 0;
	}
	if (session->common_switch_source) {
		g_source_remove(session->common_switch_source);
		session->common_switch_source = 0;
	}

	sessions = g_slist_remove(sessions, session);
	session->kernel_command_seen = false;
	session->kernel_attached = false;

	if (session->transport_source) {
		g_source_remove(session->transport_source);
		session->transport_source = 0;
	}

	if (session->gatt && session->reply_notify_id)
		bt_gatt_client_unregister_notify(session->gatt,
			session->reply_notify_id);
	if (session->gatt && session->input_notify_id)
		bt_gatt_client_unregister_notify(session->gatt,
			session->input_notify_id);

	session->reply_notify_id = 0;
	session->input_notify_id = 0;

	/* Closing the per-session fd is the kernel transport detach. */
	if (session->transport_fd >= 0) {
		close(session->transport_fd);
		session->transport_fd = -1;
	}

	if (session->gatt)
		bt_gatt_client_unref(session->gatt);
	if (session->device)
		btd_device_unref(session->device);

	info("switch2: detached %s", session->address);
	g_free(session);
}

static gboolean switch2_stop_idle(gpointer user_data)
{
	struct switch2_session *session = user_data;

	session->stop_source = 0;
	switch2_stop_session(session);
	return G_SOURCE_REMOVE;
}

static void switch2_schedule_stop(struct switch2_session *session)
{
	if (!session || session->stopping || session->stop_source)
		return;

	session->stop_source = g_idle_add(switch2_stop_idle, session);
}

static void switch2_maybe_attach_kernel(struct switch2_session *session)
{
	int ret;

	/* Successful reply/side CCC registration is the transport barrier.
	 * Do not require the controller to emit an input packet before attaching
	 * the kernel: initialization may itself be what starts that stream. */
	if (!session || session->stopping || session->kernel_attached ||
	    session->transport_fd < 0 || !session->reply_notify_ready ||
	    !session->side_notify_ready)
		return;

	ret = switch2_transport_attach(session);
	if (ret < 0) {
		error("switch2: %s failed to attach kernel GATT transport: %s",
		      session->address, strerror(-ret));
		switch2_schedule_stop(session);
		return;
	}

	session->kernel_attached = true;
	info("switch2: %s direct kernel transport attached", session->address);
}

static void switch2_reply_registered(uint16_t att_ecode, void *user_data)
{
	struct switch2_session *session = user_data;

	if (att_ecode) {
		error("switch2: %s reply notification registration failed: 0x%02x",
		      session->address, att_ecode);
		switch2_schedule_stop(session);
		return;
	}

	session->reply_notify_ready = true;
	DBG("switch2: %s reply notify ready handle=0x%04x",
	    session->address, (unsigned int) session->handles.reply);
	switch2_maybe_attach_kernel(session);
}

static void switch2_side_registered(uint16_t att_ecode, void *user_data)
{
	struct switch2_session *session = user_data;

	if (att_ecode) {
		error("switch2: %s side notification registration failed: 0x%02x",
		      session->address, att_ecode);
		switch2_schedule_stop(session);
		return;
	}

	session->side_notify_ready = true;
	DBG("switch2: %s side notify ready handle=0x%04x side=0x%02x",
	    session->address, (unsigned int) session->handles.side_input,
	    (unsigned int) session->handles.report_id);
	switch2_maybe_attach_kernel(session);
}

static gboolean switch2_command_timeout(gpointer user_data)
{
	struct switch2_session *session = user_data;

	session->pending_timeout = 0;
	if (!session->pending || session->stopping)
		return G_SOURCE_REMOVE;

	error("switch2: %s GATT command timed out cmd=0x%02x transport=0x%02x "
	      "subcmd=0x%02x", session->address,
	      (unsigned int) session->pending_command,
	      (unsigned int) session->pending_transport,
	      (unsigned int) session->pending_subcommand);
	switch2_schedule_stop(session);
	return G_SOURCE_REMOVE;
}

static void switch2_arm_command_timeout(struct switch2_session *session)
{
	if (session->pending_timeout)
		g_source_remove(session->pending_timeout);

	session->pending_timeout = g_timeout_add(
		SWITCH2_COMMAND_TIMEOUT_MS, switch2_command_timeout, session);
}

static void switch2_cancel_command_timeout(struct switch2_session *session)
{
	if (!session->pending_timeout)
		return;

	g_source_remove(session->pending_timeout);
	session->pending_timeout = 0;
}

static void switch2_reply_notify(uint16_t value_handle, const uint8_t *value,
                                 uint16_t length, void *user_data)
{
	struct switch2_session *session = user_data;
	int ret;

	if (value_handle != session->handles.reply || !value || length < 4 ||
	    length > SWITCH2_MAX_CONTROL)
		return;

	/* Ignore unrelated housekeeping records; a kernel command is correlated by
	 * command/transport/subcommand exactly as before. */
	if (!session->pending || value[0] != session->pending_command ||
	    value[2] != session->pending_transport ||
	    value[3] != session->pending_subcommand)
		return;

	DBG("switch2: %s GATT reply cmd=0x%02x transport=0x%02x subcmd=0x%02x len=%u",
	    session->address, (unsigned int) value[0],
	    (unsigned int) value[2], (unsigned int) value[3],
	    (unsigned int) length);

	/* Do not consume the pending command until the kernel has accepted the
	 * reply.  A reconnect can produce a matching but malformed/stale reply;
	 * command/transport/subcommand alone cannot distinguish it from the new
	 * transaction.  Keeping the pending command armed lets a subsequent valid
	 * reply win instead of turning a healthy BLE link into a connected-without-
	 * device state. */
	ret = switch2_transport_write(session, SWITCH2_TRANSPORT_RX_REPLY,
		value, length);
	if (ret == -EINVAL) {
		error("switch2: %s kernel rejected matching GATT reply "
		      "cmd=0x%02x transport=0x%02x subcmd=0x%02x len=%u "
		      "bytes=%02x %02x %02x %02x %02x %02x %02x %02x; "
		      "keeping command pending",
		      session->address, (unsigned int) value[0],
		      (unsigned int) value[2], (unsigned int) value[3],
		      (unsigned int) length,
		      (unsigned int) value[0],
		      (unsigned int) (length > 1 ? value[1] : 0),
		      (unsigned int) (length > 2 ? value[2] : 0),
		      (unsigned int) (length > 3 ? value[3] : 0),
		      (unsigned int) (length > 4 ? value[4] : 0),
		      (unsigned int) (length > 5 ? value[5] : 0),
		      (unsigned int) (length > 6 ? value[6] : 0),
		      (unsigned int) (length > 7 ? value[7] : 0));
		return;
	}
	if (ret < 0) {
		error("switch2: %s failed forwarding GATT reply: %s",
		      session->address, strerror(-ret));
		session->pending = false;
		switch2_cancel_command_timeout(session);
		switch2_schedule_stop(session);
		return;
	}

	session->pending = false;
	switch2_cancel_command_timeout(session);
}

static void switch2_side_notify(uint16_t value_handle, const uint8_t *value,
                                uint16_t length, void *user_data)
{
	struct switch2_session *session = user_data;
	int ret;

	if (value_handle != session->handles.side_input || !value ||
	    length != SWITCH2_INPUT_SIZE)
		return;

	if (!session->kernel_attached)
		return;

	ret = switch2_transport_state(session, session->handles.report_id,
		value, length);
	if (ret < 0) {
		error("switch2: %s failed forwarding side report: %s",
		      session->address, strerror(-ret));
		switch2_schedule_stop(session);
	}
}


static void switch2_common_registered(uint16_t att_ecode, void *user_data)
{
	struct switch2_session *session = user_data;

	if (att_ecode) {
		error("switch2: %s common notification registration failed: 0x%02x",
		      session->address, att_ecode);
		switch2_schedule_stop(session);
		return;
	}

	session->input_subscription = SWITCH2_INPUT_COMMON;
	DBG("switch2: %s common notify ready handle=0x%04x",
	    session->address, (unsigned int) SWITCH2_GATT_COMMON_VALUE);
}

static void switch2_common_notify(uint16_t value_handle, const uint8_t *value,
                                  uint16_t length, void *user_data)
{
	struct switch2_session *session = user_data;
	int ret;

	if (value_handle != SWITCH2_GATT_COMMON_VALUE || !value ||
	    length != SWITCH2_INPUT_SIZE || !session->kernel_attached)
		return;

	ret = switch2_transport_state(session, SWITCH2_REPORT_COMMON, value, length);
	if (ret < 0) {
		error("switch2: %s failed forwarding common input report: %s",
		      session->address, strerror(-ret));
		switch2_schedule_stop(session);
	}
}

static gboolean switch2_common_subscribe(gpointer user_data)
{
	struct switch2_session *session = user_data;

	session->common_switch_source = 0;
	if (session->stopping || !session->gatt ||
	    session->input_subscription != SWITCH2_INPUT_SWITCHING)
		return G_SOURCE_REMOVE;

	session->input_notify_id = bt_gatt_client_register_notify(session->gatt,
		SWITCH2_GATT_COMMON_VALUE, switch2_common_registered,
		switch2_common_notify, session, NULL);
	if (!session->input_notify_id) {
		error("switch2: %s failed to start common notification registration",
		      session->address);
		switch2_schedule_stop(session);
	} else
		info("switch2: %s subscribed common handle=0x%04x",
		     session->address, (unsigned int) SWITCH2_GATT_COMMON_VALUE);

	return G_SOURCE_REMOVE;
}

static gboolean switch2_common_switch(gpointer user_data)
{
	struct switch2_session *session = user_data;

	session->common_switch_source = 0;
	if (session->stopping || !session->gatt || !session->kernel_command_seen ||
	    session->input_subscription != SWITCH2_INPUT_SIDE)
		return G_SOURCE_REMOVE;

	session->input_subscription = SWITCH2_INPUT_SWITCHING;
	if (session->input_notify_id) {
		info("switch2: %s dropping side notification handle=0x%04x",
		     session->address, (unsigned int) session->handles.side_input);
		bt_gatt_client_unregister_notify(session->gatt,
			session->input_notify_id);
		session->input_notify_id = 0;
	}

	/* Give BlueZ time to complete the 0x000f CCC disable before enabling
	 * the common characteristic's 0x000b CCC. */
	session->common_switch_source = g_timeout_add(
		SWITCH2_COMMON_SETTLE_MS,
		switch2_common_subscribe, session);

	return G_SOURCE_REMOVE;
}

static gboolean switch2_transport_event(gint fd, GIOCondition condition,
                                        gpointer user_data)
{
	struct switch2_session *session = user_data;
	uint8_t record[SWITCH2_TRANSPORT_HEADER_SIZE + SWITCH2_TRANSPORT_MAX_PAYLOAD];
	struct switch2_transport_header *header =
		(struct switch2_transport_header *) record;
	const uint8_t *payload;
	uint16_t payload_size;
	ssize_t len;

	if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		goto failed;

	do {
		len = read(fd, record, sizeof(record));
	} while (len < 0 && errno == EINTR);
	if (len < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return G_SOURCE_CONTINUE;
		goto failed;
	}
	if (len < (ssize_t) sizeof(*header) ||
	    header->abi != SWITCH2_TRANSPORT_ABI)
		goto failed;

	payload_size = le16toh(header->length);
	if (!payload_size || payload_size > SWITCH2_TRANSPORT_MAX_PAYLOAD ||
	    len != (ssize_t) (sizeof(*header) + payload_size))
		goto failed;
	payload = record + sizeof(*header);

	if (header->type == SWITCH2_TRANSPORT_TX_OUTPUT) {
		if (payload_size != 17 || !session->handles.vibration)
			return G_SOURCE_CONTINUE;
		if (!bt_gatt_client_write_without_response(session->gatt,
				session->handles.vibration, false,
				(uint8_t *) payload, payload_size))
			DBG("switch2: %s BLE vibration write failed", session->address);
		return G_SOURCE_CONTINUE;
	}

	if (header->type != SWITCH2_TRANSPORT_TX_COMMAND &&
	    header->type != SWITCH2_TRANSPORT_TX_RAW)
		return G_SOURCE_CONTINUE;

	if (header->type == SWITCH2_TRANSPORT_TX_COMMAND) {
		if (payload_size < 4)
			goto failed;
		if (session->pending) {
			error("switch2: %s kernel issued overlapping GATT command",
			      session->address);
			switch2_schedule_stop(session);
			return G_SOURCE_CONTINUE;
		}
		if (!session->kernel_command_seen) {
			session->kernel_command_seen = true;
			info("switch2: %s kernel command transport active", session->address);
		}

		/* Common input is a runtime transport change, not part of controller
		 * initialization.  Keep its CCC writes away from the command/reply
		 * sequence by requiring a full quiet interval after the latest kernel
		 * command.  Re-arming here also prevents the timer from landing in one
		 * of the short gaps between acknowledged initialization commands. */
		if (session->input_subscription == SWITCH2_INPUT_SIDE) {
			if (session->common_switch_source)
				g_source_remove(session->common_switch_source);
			session->common_switch_source = g_timeout_add(
				SWITCH2_COMMON_SWITCH_MS, switch2_common_switch, session);
			DBG("switch2: %s common-input swap deferred until %u ms "
			    "after latest kernel command", session->address,
			    (unsigned int) SWITCH2_COMMON_SWITCH_MS);
		}

		session->pending = true;
		session->pending_command = payload[0];
		session->pending_transport = payload[2];
		session->pending_subcommand = payload[3];
		DBG("switch2: %s kernel command cmd=0x%02x transport=0x%02x "
		    "subcmd=0x%02x len=%u",
		    session->address, (unsigned int) payload[0],
		    (unsigned int) payload[2], (unsigned int) payload[3],
		    (unsigned int) payload_size);
	}

	if (!switch2_write_control(session, payload, payload_size)) {
		error("switch2: %s GATT control write failed", session->address);
		if (header->type == SWITCH2_TRANSPORT_TX_COMMAND)
			session->pending = false;
		switch2_schedule_stop(session);
		return G_SOURCE_CONTINUE;
	}

	if (header->type == SWITCH2_TRANSPORT_TX_COMMAND)
		switch2_arm_command_timeout(session);
	return G_SOURCE_CONTINUE;

failed:
	session->transport_source = 0;
	switch2_stop_session(session);
	return G_SOURCE_REMOVE;
}

static void switch2_device_resolved(struct btd_adapter *adapter,
                                    struct btd_device *device)
{
	struct switch2_session *session;
	struct bt_gatt_client *gatt;
	struct switch2_handles handles;

	if (!switch2_find_handles(device, &handles))
		return;

	switch2_autoconnect_enable(adapter, device);
	if (switch2_find_session(device))
		return;

	gatt = btd_device_get_gatt_client(device);
	if (!gatt || !bt_gatt_client_is_ready(gatt))
		return;

	/* Joy-Con 2 deliberately operates without an SMP bond.  Once its exact
	 * GATT identity has resolved successfully, persist both the device and the
	 * side-specific USB VID/PID identity derived from the live characteristic.
	 * The service UUID alone survives restart but cannot distinguish 2066 from
	 * 2067, so without DeviceID the cold-start path cannot seed the side-specific
	 * fixed GATT map and therefore cannot arm auto-connect.
	 *
	 * Source 0x0002 is the USB Implementers Forum PnP-ID namespace; 057e is
	 * Nintendo's USB VID and 2066/2067 are the measured physical JC2 PIDs.
	 * Version 0 is intentionally unknown rather than fabricated. */
	btd_device_set_temporary(device, false);
	btd_device_set_pnpid(device, 0x0002, SWITCH2_VENDOR,
			       handles.product, 0x0000);

	session = g_new0(struct switch2_session, 1);
	session->adapter = adapter;
	session->device = btd_device_ref(device);
	session->gatt = bt_gatt_client_ref(gatt);
	session->handles = handles;
	session->transport_fd = -1;
	ba2str(device_get_address(device), session->address);

	session->transport_fd = open(SWITCH2_TRANSPORT_PATH,
		O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (session->transport_fd < 0) {
		error("switch2: %s failed to open %s: %s", session->address,
		      SWITCH2_TRANSPORT_PATH, strerror(errno));
		switch2_stop_session(session);
		return;
	}

	sessions = g_slist_prepend(sessions, session);
	session->transport_source = g_unix_fd_add(session->transport_fd,
		G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
		switch2_transport_event, session);
	if (!session->transport_source) {
		error("switch2: %s failed to watch kernel transport fd",
		      session->address);
		switch2_stop_session(session);
		return;
	}

	session->reply_notify_id = bt_gatt_client_register_notify(session->gatt,
		handles.reply, switch2_reply_registered, switch2_reply_notify,
		session, NULL);
	if (!session->reply_notify_id) {
		error("switch2: %s failed to start reply notification registration",
		      session->address);
		switch2_stop_session(session);
		return;
	}

	session->input_notify_id = bt_gatt_client_register_notify(session->gatt,
		handles.side_input, switch2_side_registered, switch2_side_notify,
		session, NULL);
	if (!session->input_notify_id) {
		error("switch2: %s failed to start side notification registration",
		      session->address);
		switch2_stop_session(session);
		return;
	}

	info("switch2: prepared direct kernel transport for %s Joy-Con 2 (%c)",
	     session->address,
	     handles.report_id == SWITCH2_REPORT_LEFT ? 'L' : 'R');
}

static void switch2_device_added(struct btd_adapter *adapter,
				 struct btd_device *device)
{
	switch2_autoconnect_consider(device, adapter);
}

static void switch2_device_removed(struct btd_adapter *adapter,
				   struct btd_device *device)
{
	struct switch2_session *session = switch2_find_session(device);

	switch2_autoconnect_disable(adapter, device);

	if (session)
		switch2_stop_session(session);
}

static void switch2_disconnected(struct btd_device *device, uint8_t reason)
{
	struct switch2_session *session = switch2_find_session(device);
	struct switch2_autoconnect *entry = switch2_find_autoconnect(device);

	if (session) {
		info("switch2: %s disconnected reason=0x%02x",
		     session->address, reason);
		switch2_stop_session(session);
	} else if (entry) {
		info("switch2: %s disconnected before session attach reason=0x%02x",
		     entry->address, reason);
	}

	if (entry)
		switch2_autoconnect_rearm(device);
}

static int switch2_adapter_probe(struct btd_adapter *adapter)
{
	/* Discovery belongs to normal BlueZ clients (bluetoothctl/GUI). The
	 * manufacturer callback observes those advertisements and enrolls any
	 * previously unknown Joy-Con 2 without keeping discovery active here. */
	btd_adapter_register_msd_cb(adapter, switch2_manufacturer_data);
	btd_adapter_for_each_device(adapter, switch2_autoconnect_consider,
				    adapter);
	return 0;
}

static void switch2_adapter_remove(struct btd_adapter *adapter)
{
	GSList *l = sessions;

	btd_adapter_unregister_msd_cb(adapter, switch2_manufacturer_data);

	while (l) {
		GSList *next = l->next;
		struct switch2_session *session = l->data;

		if (session->adapter == adapter)
			switch2_stop_session(session);
		l = next;
	}

	l = autoconnect_devices;
	while (l) {
		GSList *next = l->next;
		struct switch2_autoconnect *entry = l->data;

		if (entry->adapter == adapter)
			switch2_autoconnect_disable(adapter, entry->device);
		l = next;
	}
}

static struct btd_adapter_driver switch2_driver = {
	.name = "switch2",
	.probe = switch2_adapter_probe,
	.remove = switch2_adapter_remove,
	.device_added = switch2_device_added,
	.device_removed = switch2_device_removed,
	.device_resolved = switch2_device_resolved,
};

static int switch2_init(void)
{
	int ret;

	ret = btd_register_adapter_driver(&switch2_driver);
	if (ret < 0)
		return ret;

	btd_add_disconnect_cb(switch2_disconnected);
	return 0;
}

static void switch2_exit(void)
{
	btd_remove_disconnect_cb(switch2_disconnected);
	btd_unregister_adapter_driver(&switch2_driver);

	while (sessions)
		switch2_stop_session(sessions->data);

	if (autoconnect_devices)
		error("switch2: auto-connect entries remained after adapter teardown");
}

BLUETOOTH_PLUGIN_DEFINE(switch2, VERSION, BLUETOOTH_PLUGIN_PRIORITY_LOW,
			switch2_init, switch2_exit)
