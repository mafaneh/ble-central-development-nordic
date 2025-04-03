/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>

#include <dk_buttons_and_leds.h>

// Important defines
#define NAME_LEN      30
#define PERIPHERAL_NAME "PLAYBULB CANDLE II"

// Color Settings
#define COLOR_COUNT 4
#define COLOR_WHITE 0x000000FF
#define COLOR_RED   0x0000FF00
#define COLOR_GREEN 0x00FF0000
#define COLOR_BLUE  0xFF000000

#define COLOR_STRING_MAX_LENGTH 6

typedef struct {
	uint32_t color_value;
	char color_name[COLOR_STRING_MAX_LENGTH];

} color_setting_t;

static color_setting_t color_array[COLOR_COUNT] =
{
	{COLOR_WHITE, "White"},
	{COLOR_RED, "Red"},
	{COLOR_GREEN, "Green"},
	{COLOR_BLUE, "Blue"} };

#define BT_UUID_COLOR_SETTING BT_UUID_DECLARE_16(0xFFFC)

// Button Action Assignments
// - Button 1 ==> Rotate Colors
// - Button 2 ==> Read Battery Level
// - Button 3 ==> Update Connection Parameters
// - Button 4 ==> Disconnect from device
#define BUTTON_COLOR         DK_BTN1_MSK
#define BUTTON_BATTERY_LEVEL DK_BTN2_MSK
#define BUTTON_CONN_PARAMS   DK_BTN3_MSK
#define BUTTON_DISCONNECT    DK_BTN4_MSK

// Data Types
typedef struct adv_data
{
    // Device Name
	uint8_t length;
    char name[NAME_LEN];

} custom_adv_data_t;

// Semaphores
static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_discovered, 0, 1);
static K_SEM_DEFINE(sem_written, 0, 1);
static K_SEM_DEFINE(sem_read_operation, 0, 1);
static K_SEM_DEFINE(sem_disconnected, 0, 1);

// Important global variables
static uint16_t color_attr_handle;
static uint16_t battery_level_value_handle;
static uint32_t current_color_index = 0; // Start with White (at index 0)
static struct bt_conn *default_conn;

static struct bt_uuid_16 discover_uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_read_params read_params;

// Function prototypes
static void start_scan(void);
static int toggle_color(void);

// Functions

static bool data_cb(struct bt_data *data, void *user_data)
{
    custom_adv_data_t *adv_data_struct = user_data;
    uint8_t len;

    switch (data->type) {
    case BT_DATA_NAME_SHORTENED:
    case BT_DATA_NAME_COMPLETE:
        len = MIN(data->data_len, NAME_LEN - 1);
        memcpy(adv_data_struct->name, data->data, len);
        adv_data_struct->name[len] = '\0';
		adv_data_struct->length = len;
        return false;
    default:
        return true;
    }
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	custom_adv_data_t device_ad_data;
	int err;

	if (default_conn) {
		return;
	}

	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_SCAN_RSP)
	{
		return;
	}

    (void)memset(&device_ad_data, 0, sizeof(custom_adv_data_t));
    bt_data_parse(ad, data_cb, &device_ad_data);

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	// Only log and care about devices with a non-empty device name
	if (device_ad_data.length == 0)
	{
		return;
	}

	printk("Device found [%s]: %s with name [%d]: |%s| (RSSI %d)\n", 
			type == BT_GAP_ADV_TYPE_SCAN_RSP ? "Scan Response":"Regular Advertisement",
			addr_str,
			device_ad_data.length,
			device_ad_data.name,
			rssi);

	if (strncmp(PERIPHERAL_NAME, device_ad_data.name, strlen(PERIPHERAL_NAME)) == 0)
	{
		if (bt_le_scan_stop()) {
			return;
		}

		err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
						BT_LE_CONN_PARAM_DEFAULT, &default_conn);
			if (err) {
				printk("Create conn to %s failed (%d)\n", addr_str, err);
				start_scan();
			}
	}	
}

static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	uint8_t *battery_level = ((uint8_t *)data);

	if (!data) {
		printk("[UNSUBSCRIBED]\n");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	printk("Received notification for Battery Level (%u): %u%%\n", length, *battery_level);

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		printk("Discover complete\n");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	printk("Discovered [ATTRIBUTE] with handle %u\n", attr->handle);

	// Discovered the Color Characteristic
	if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_COLOR_SETTING)) {

		printk("Discovery of Color Setting Characteristic Successful\n");
		// color_attr_handle = ((struct bt_gatt_chrc *)attr->user_data)->value_handle;
		color_attr_handle = bt_gatt_attr_value_handle(attr);
		printk("Color Setting Characteristic Handle = %u\n", color_attr_handle);

		// Move on to discovering the Battery Level Characteristic
		memcpy(&discover_uuid, BT_UUID_BAS_BATTERY_LEVEL, sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}
	}
	// Discovered the Battery Level Characteristic
	else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_BAS_BATTERY_LEVEL)) {

		printk("Discovery of Battery Level Characteristic Successful\n");
		// battery_level_value_handle = ((struct bt_gatt_chrc *)attr->user_data)->value_handle;
		battery_level_value_handle = bt_gatt_attr_value_handle(attr);
		printk("Battery Level Characteristic Handle = %u\n", battery_level_value_handle);
		
		// Move on to discovering the Battery Level Characteristic CCCD (to enable notifications)
		memcpy(&discover_uuid, BT_UUID_GATT_CCC, sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}
	}
	else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC))
	{
		printk("Discovery of Battery Level Characteristic CCCD Successful. Subscribing to notifications now.\n");

		subscribe_params.notify = notify_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;
		subscribe_params.ccc_handle = attr->handle;

		err = bt_gatt_subscribe(conn, &subscribe_params);
		if (err && err != -EALREADY) {
			printk("Subscribe failed (err %d)\n", err);
		} else {
			printk("[SUBSCRIBED]\n");
		}

		k_sem_give(&sem_discovered);
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}

static void start_scan(void)
{
	int err;

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

	printk("Scanning successfully started\n ==> Will only be scanning devices with a NON-EMPTY device name <==\n");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s %u %s\n", addr, err, bt_hci_err_to_str(err));

		bt_conn_unref(default_conn);
		default_conn = NULL;

		start_scan();
		return;
	}

	if (conn != default_conn) {
		return;
	}

	printk("Connected: %s\n", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn != default_conn) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	bt_conn_unref(default_conn);
	default_conn = NULL;
	k_sem_give(&sem_disconnected);
}

void remote_info_available_cb(struct bt_conn *conn, struct bt_conn_remote_info *remote_info)
{
	printk("Remote info from connected device available. We can now discover the GATT database\n");

	k_sem_give(&sem_connected);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
    .remote_info_available = remote_info_available_cb
};

static uint8_t battery_read_func(struct bt_conn *conn, uint8_t err,
	struct bt_gatt_read_params *params,
	const void *data, uint16_t length)
{
	printk("Battery Level = %u%%\n", *((uint8_t *)data));

	return BT_GATT_ITER_STOP;
}

static int read_battery_level(void)
{
	int err;

	read_params.func = battery_read_func;
	read_params.handle_count  = 1;
	read_params.single.handle = battery_level_value_handle;
	read_params.single.offset = 0;

	err = bt_gatt_read(default_conn, &read_params);
	if (err)
	{
		printk("Read not successful!\n");
	}

	return err;
}

static void button_state_changed(uint32_t button_state, uint32_t has_changed)
{
    if ((has_changed & BUTTON_COLOR) && (button_state & BUTTON_COLOR))
	{	
		printk("Changing color to next one in array\n");
		current_color_index = (current_color_index + 1) % COLOR_COUNT;
		toggle_color();
    }
	if ((has_changed & BUTTON_BATTERY_LEVEL) && (button_state & BUTTON_BATTERY_LEVEL))
	{
		printk("Reading the Battery Level\n");
		read_battery_level();
	}
	if ((has_changed & BUTTON_CONN_PARAMS) && (button_state & BUTTON_CONN_PARAMS))
	{
		struct bt_conn_info info;
		printk("Current Connection Parameters\n");
		bt_conn_get_info(default_conn, &info);
		printk("Interval: %0.2f ms, Latency: %u, Timeout: %u ms\n",
			info.le.interval*1.25,
			info.le.latency,
			info.le.timeout*10); 

		printk("Updating Connection Parameters\n");
		struct bt_le_conn_param *param = BT_LE_CONN_PARAM(6, 6, 0, 400);
		bt_conn_le_param_update(default_conn, param);

		printk("Updated Connection Parameters\n");
		printk("Interval: %0.2f ms, Latency: %u, Timeout: %u ms\n", 
			param->interval_min*1.25,
			param->latency,
			param->timeout*10);

		// Changing PHY - Not supported by the PlayBulb Candle device, but can be applied with other devices
		// struct bt_conn_le_phy_param phy_param;
		// phy_param.pref_tx_phy = BT_GAP_LE_PHY_2M;
		// phy_param.pref_tx_phy = BT_GAP_LE_PHY_2M;
		// bt_conn_le_phy_update(default_conn, &phy_param);
	}
	if ((has_changed & BUTTON_DISCONNECT) && (button_state & BUTTON_DISCONNECT))
	{
		printk("Disconnecting from device\n");
		bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static int init_buttons(void)
{
    return dk_buttons_init(button_state_changed);
}

static int toggle_color(void)
{
	int err;

	printk("Setting color to: %s\n", color_array[current_color_index].color_name);

	err = bt_gatt_write_without_response(default_conn,
										 color_attr_handle,
										 &(color_array[current_color_index].color_value),
										 sizeof(uint32_t),
										 false);
	if (err) {
		printk("Write failed (err %d)\n", err);
	}
	else // Write sent successfully
	{
		printk("Write without response sent successfully.\n");
	}

	return err;
}


int main(void)
{
	int err;

	err = init_buttons();
	if (err)
	{
		printk("Cannot init buttons (err: %d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	while (1)
	{
		// Continuously scan
		start_scan();

		// Wait for a connection to happen
		k_sem_take(&sem_connected, K_FOREVER);

		// Once connected, we discover the GATT database of the connected Peripheral
		memcpy(&discover_uuid, BT_UUID_COLOR_SETTING, sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.func = discover_func;
		discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
		discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			printk("Discovery failed (err %d)\n", err);
		}
		else // Discovery started successfully
		{
			printk("Discovery started\n");

			err = k_sem_take(&sem_discovered, K_SECONDS(10));
			if (err) {
				printk("Timed out during GATT discovery\n");
			}
			else // Discovery completed successfully
			{
				printk("Discovered the device's characteristics.\n");

				// Stay in this state until a disconnect occurs.
				printk("Wait here for user button presses as long as we're still connected...\n");

				k_sem_take(&sem_disconnected,  K_FOREVER);
			}
		}

		if (err)
		{
			// In case of an error, disconnect from the device
			err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			if (err) {
				printk("Failed to disconnect (err %d)\n", err);
				return 0;
			}

			k_sem_take(&sem_disconnected,  K_SECONDS(30));	
			printk("Disconnected. Going back to scanning again...\n");
		}
	}
	
	return 0;
}
