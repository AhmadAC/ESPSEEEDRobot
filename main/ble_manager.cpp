#include "ble_manager.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "servo_controller.h"
#include "wifi_manager.h"
#include <string.h>

static const char *TAG = "BLE_MGR";

static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0xABF0);
static const ble_uuid16_t uart_rx_uuid = BLE_UUID16_INIT(0xABF1);
static const ble_uuid16_t wifi_creds_uuid = BLE_UUID16_INIT(0xABF2);
static const ble_uuid16_t ip_addr_uuid = BLE_UUID16_INIT(0xABF3);

uint16_t ip_chr_val_handle = 0;
uint16_t active_conn_handle = 0;
char current_ip[32] = "0.0.0.0";

// Original RX Callback logic for Gamepad controller overrides
static int ble_rx_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    char buf[128] = {0};
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 0 && len < sizeof(buf)) {
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        for(int i = 0; i < len; i++) {
            if(buf[i] == '\r' || buf[i] == '\n') buf[i] = '\0';
        }
        servo_set_action(buf);
    }
    return 0;
}

// Intercept incoming credentials from the Android app
static int ble_wifi_rx_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    char buf[128] = {0};
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 0 && len < sizeof(buf)) {
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        char* comma = strchr(buf, ',');
        if (comma) {
            *comma = '\0';
            ESP_LOGI(TAG, "Provisioning via BLE - SSID: %s", buf);
            wifi_save_credentials(buf, comma + 1);
            // Initiate a background Wi-Fi connection. ble_manager_notify_ip is called when successful!
            wifi_manager_connect_async(buf, comma + 1);
        }
    }
    return 0;
}

// Allows Android to explicitly read the IP if it missed the notification
static int ble_ip_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    os_mbuf_append(ctxt->om, current_ip, strlen(current_ip));
    return 0;
}

static const struct ble_gatt_chr_def gatt_chrs[] = {
    {
        .uuid = (const ble_uuid_t *)&uart_rx_uuid,
        .access_cb = ble_rx_cb,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = nullptr
    },
    {
        .uuid = (const ble_uuid_t *)&wifi_creds_uuid,
        .access_cb = ble_wifi_rx_cb,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE,
        .min_key_size = 0,
        .val_handle = nullptr
    },
    {
        .uuid = (const ble_uuid_t *)&ip_addr_uuid,
        .access_cb = ble_ip_cb,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &ip_chr_val_handle
    },
    {}
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&svc_uuid,
        .includes = nullptr,
        .characteristics = gatt_chrs
    },
    {}
};

static void ble_app_on_sync(void);

// Tracks NimBLE internal connection status so we know who to notify
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            active_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE Client Connected!");
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        active_conn_handle = 0;
        ble_app_on_sync();
    }
    return 0;
}

static void ble_app_on_sync(void) {
    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);
    
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"ESPRobot";
    fields.name_len = 8;
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    ESP_LOGI(TAG, "BLE UART/Provisioning Advertising Started");
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_manager_notify_ip(const char* ip) {
    strncpy(current_ip, ip, sizeof(current_ip)-1);
    
    if (active_conn_handle != 0 && ip_chr_val_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(current_ip, strlen(current_ip));
        ble_gatts_notify_custom(active_conn_handle, ip_chr_val_handle, om);
        ESP_LOGI(TAG, "Sent new IP Address '%s' over BLE to connected Client", current_ip);
    }
}

void ble_manager_init() {
    nimble_port_init();
    ble_svc_gap_device_name_set("ESPRobot");
    ble_svc_gap_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE Manager Initialization Finished.");
}