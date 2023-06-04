/**
 * @file log_backend_nus.c
 * @author Yifeng Qiu (yifeng.q@gmail.com)
 * @brief A log backend making use of the Nordic Uart Service. It runs as a NUS server on a peripheral.\
 * And pairs with a central running a NUS client. A potential use case is when UART is used for low-level\
 * communication with another device, in which it cannot be used as a log backend.
 * @version 0.1
 * @date 2023-06-04
 *
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/nus.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_output_dict.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/sys/__assert.h>

#include "log_backend_nus.h"

LOG_MODULE_REGISTER(_log_backend_nus);

/* LOG BACKEND DEFINES */
static uint32_t log_format_current = CONFIG_LOG_BACKEND_NUS_OUTPUT_DEFAULT;
static volatile bool in_panic;
static const char LOG_HEX_SEP[10] = "##ZLOGV1##";

#define STACKSIZE CONFIG_BT_NUS_THREAD_STACK_SIZE
#define PRIORITY 7

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;

uint8_t bt_nus_mtu = 0;
static bool first_enable;
K_SEM_DEFINE(sem_ble_nus_tx, 0, 1);
K_SEM_DEFINE(sem_nus_init_ok, 0, 1);

int ble_nus_connected = 0;

const struct log_backend *log_backend_nus_get(void);

struct ble_data_t
{
    void *fifo_reserved;
    uint8_t data[CONFIG_BT_L2CAP_TX_MTU];
    uint16_t len;
};

K_FIFO_DEFINE(fifo_ble_tx_data);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static int append_data_to_fifo(const struct k_fifo *fifo, const uint8_t *data, size_t length)
{
    struct ble_data_t *buf = k_malloc(sizeof(*buf));
    if (!buf)
    {
        return length;
    }

    memcpy(buf->data, data, length);
    buf->len = length;
    k_fifo_put(fifo, buf);
    return length;
}

/* LOG BACKEND FUNCTIONS */
static int char_out(uint8_t *data, size_t length, void *ctx)
{
    ARG_UNUSED(ctx);

    int processed;
    size_t len_stored = length;

    if (ble_nus_connected > 0)
    {
        do
        {
            processed = append_data_to_fifo(
                &fifo_ble_tx_data,
                data,
                (length > bt_nus_mtu) ? bt_nus_mtu : length);
            length -= processed;
            data += processed;
        } while (length != 0);
    }
    return len_stored;
}

static uint8_t nus_output_buf[BLE_BUF_SIZE];
LOG_OUTPUT_DEFINE(log_output_nus, char_out, nus_output_buf, sizeof(nus_output_buf));

static void panic(const struct log_backend *const backend)
{
    in_panic = true;
    log_backend_std_panic(&log_output_nus);
}

static void log_backend_nus_init(const struct log_backend *const backend)
{
}

static int is_ready(const struct log_backend *const backend)
{
    // return -EACCES;
    return 0;
}

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{

    uint32_t flags = log_backend_std_get_flags();

    log_format_func_t log_output_func = log_format_func_t_get(log_format_current);
    log_output_func(&log_output_nus, &msg->log, flags);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
    ARG_UNUSED(backend);
    if (IS_ENABLED(CONFIG_LOG_BACKEND_UART_OUTPUT_DICTIONARY))
    {
        log_dict_output_dropped_process(&log_output_nus, cnt);
    }
    else
    {
        log_backend_std_dropped(&log_output_nus, cnt);
    }
}

struct log_backend_api log_backend_bt_api = {
    .init = log_backend_nus_init,
    .is_ready = is_ready,
    .dropped = dropped,
    .panic = panic,
    .process = process

};

LOG_BACKEND_DEFINE(log_backend_nus, log_backend_bt_api, true);

const struct log_backend *log_backend_nus_get(void)
{
    return &log_backend_nus;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    if (conn_err)
    {
        LOG_ERR("Connection failed (err %u)", conn_err);
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Connected %s", addr);
    current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Disconnected: %s (reason %u)", addr, reason);

    if (auth_conn)
    {
        bt_conn_unref(auth_conn);
        auth_conn = NULL;
    }

    if (current_conn)
    {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    bt_nus_mtu = 0;
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err)
    {
        LOG_WRN("Security changed: %s level %u", addr, level);
    }
    else
    {
        LOG_WRN("Security failed: %s level %u err %d", addr,
                level, err);
    }
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
    .security_changed = security_changed,
#endif
};

#if defined(CONFIG_BT_NUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Passkey for %s: %06u", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];

    auth_conn = bt_conn_ref(conn);

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Passkey for %s: %06u", addr, passkey);
    LOG_WRN("Press Button 1 to confirm, Button 2 to reject.");
    bt_conn_auth_passkey_confirm(auth_conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_WRN("Pairing failed conn: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .cancel = auth_cancel,

};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif

/* NUS related functions */
static void sent_cb(struct bt_conn *conn)
{
}

static void received_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
}

static void send_enabled_cb(enum bt_nus_send_status status)
{
    if (status == BT_NUS_SEND_STATUS_ENABLED)
    {
        bt_nus_mtu = bt_nus_get_mtu(current_conn);
        LOG_WRN("NUS MTU %d", bt_nus_mtu);
        const uint8_t *msg = "NUS Logger Activated";
        append_data_to_fifo(&fifo_ble_tx_data, msg, 20);
        ble_nus_connected++;
    }
    else
    {
        // Disable the backend
        // log_backend_deactivate(&log_backend_nus);
        ble_nus_connected--;
    }
}

static struct bt_nus_cb nus_cb = {
    .sent = sent_cb,
    .received = received_cb,
    .send_enabled = send_enabled_cb,
};

void register_bt_nus_auth_cbs(void)
{
    int err;
    if (IS_ENABLED(CONFIG_BT_NUS_SECURITY_ENABLED))
    {
        err = bt_conn_auth_cb_register(&conn_auth_callbacks);
        if (err)
        {
            LOG_ERR("Failed to register authorization callbacks.\n");
            return;
        }

        err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
        if (err)
        {
            LOG_ERR("Failed to register authorization info callbacks.\n");
            return;
        }
    }
}

int nus_init(void)
{
    int err = 0;
    err = bt_nus_init(&nus_cb);
    if (err)
    {
        LOG_ERR("Failed to initialize NUS (err: %d)", err);
        return err;
    }

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd,
                          ARRAY_SIZE(sd));
    if (err)
    {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return err;
    }
    k_sem_give(&sem_nus_init_ok);
}

void ble_logger_thread(void)
{
    k_sem_take(&sem_nus_init_ok, K_FOREVER);
    int i = 0;
    for (;;)
    {
        /* Wait indefinitely for data to be sent over bluetooth */
        struct ble_data_t *buf = k_fifo_get(&fifo_ble_tx_data,
                                            K_FOREVER);
        while (i < 3 && 0 != bt_nus_send(NULL, buf->data, buf->len))
            i++;
        i = 0;
        k_free(buf);
    }
}

K_THREAD_DEFINE(ble_write_thread_id, STACKSIZE, ble_logger_thread, NULL, NULL,
                NULL, PRIORITY, 0, 0);
