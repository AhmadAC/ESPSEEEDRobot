#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

// Modular Component Headers
#include "wifi_manager.h"
#include "sensor_monitor.h"
#include "servo_controller.h"
#include "claw_controller.h"
#include "web_server.h"
#include "ble_manager.h"
#include "audio_player.h"

static const char *TAG = "MAIN";
bool is_claw_mode = false; // Global flag

// FreeRTOS task to monitor standard input for REPL control characters
static void console_read_task(void *pvParameter) {
    ESP_LOGI("REPL", "==================================================");
    ESP_LOGI("REPL", "Available Device REPL Commands:");
    ESP_LOGI("REPL", "  mode claw  - Switch to Claw Controller Profile");
    ESP_LOGI("REPL", "  mode robot - Switch to ESPRobot Profile");
    ESP_LOGI("REPL", "  ap         - Force AP Mode (Wi-Fi Hotspot)");
    ESP_LOGI("REPL", "  wifi       - Connect to saved Wi-Fi");
    ESP_LOGI("REPL", "  bt         - Switch to Bluetooth Control");
    ESP_LOGI("REPL", "  reset      - Factory Reset NVS (Restores Settings)");
    ESP_LOGI("REPL", "  sound      - Enable Audio UI");
    ESP_LOGI("REPL", "  no sound   - Disable & Hide Audio UI");
    ESP_LOGI("REPL", "  (Ctrl+C)   - Stop Motors / Relax Claw");
    ESP_LOGI("REPL", "  (Ctrl+D)   - Soft Reboot");
    ESP_LOGI("REPL", "==================================================");
    
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    uint8_t buf[64];
    char cmd[64];
    int cmd_idx = 0;
    
    while (1) {
        int len = read(fd, buf, sizeof(buf));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (buf[i] == 0x03) { // Ctrl+C (Interrupt)
                    ESP_LOGW("REPL", "[Sent Ctrl+C - Interrupt]");
                    if (is_claw_mode) claw_execute_command("stop");
                    else servo_set_action("stop");
                } else if (buf[i] == 0x04) { // Ctrl+D (Soft Reboot)
                    ESP_LOGW("REPL", "[Sent Ctrl+D - Soft Reboot]");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else if (buf[i] == '\r' || buf[i] == '\n') {
                    if (cmd_idx > 0) {
                        cmd[cmd_idx] = '\0';
                        if (strcmp(cmd, "reset") == 0) {
                            ESP_LOGW("REPL", "Command 'reset' received. Factory Resetting NVS...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_all(h);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "mode claw") == 0) {
                            ESP_LOGW("REPL", "Command 'mode claw' received. Switching profile...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_str(h, "dev_mode", "claw");
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "mode robot") == 0) {
                            ESP_LOGW("REPL", "Command 'mode robot' received. Switching profile...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_str(h, "dev_mode", "robot");
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "bt") == 0) {
                            ESP_LOGW("REPL", "Command 'bt' received. Switching to Bluetooth Mode...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_str(h, "boot_mode", "bt");
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "wifi") == 0) {
                            ESP_LOGW("REPL", "Command 'wifi' received. Switching to Wi-Fi Mode...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_str(h, "boot_mode", "wifi");
                                nvs_set_u8(h, "force_ap", 0); 
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "ap") == 0) {
                            ESP_LOGW("REPL", "Command 'ap' received. Switching to Forced AP Mode...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_str(h, "boot_mode", "wifi"); 
                                nvs_set_u8(h, "force_ap", 1);        
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "sound") == 0) {
                            ESP_LOGW("REPL", "Command 'sound' received. Audio UI Enabled.");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "sound_en", 1);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "no sound") == 0) {
                            ESP_LOGW("REPL", "Command 'no sound' received. Audio UI Disabled.");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "sound_en", 0);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strncmp(cmd, "angle ", 6) == 0 && is_claw_mode) {
                            int ang = atoi(cmd + 6);
                            claw_set_angle(ang);
                        } else if (is_claw_mode) {
                            claw_execute_command(cmd);
                        }
                        cmd_idx = 0;
                    }
                } else {
                    if (cmd_idx < sizeof(cmd) - 1) {
                        cmd[cmd_idx++] = (char)buf[i];
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void) {
    // 1. Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Setup Base Network Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Detect Saved Device & Boot Mode
    nvs_handle_t my_handle;
    char boot_mode[16] = "wifi";
    char dev_mode[16] = "robot";
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len = sizeof(boot_mode);
        nvs_get_str(my_handle, "boot_mode", boot_mode, &len);
        len = sizeof(dev_mode);
        nvs_get_str(my_handle, "dev_mode", dev_mode, &len);
        nvs_close(my_handle);
    }
    
    is_claw_mode = (strcmp(dev_mode, "claw") == 0);

    // 4. Boot Modular Components
    if (is_claw_mode) {
        ESP_LOGI(TAG, "Booting Device Profile: CLAW");
        claw_controller_init();
        
        ESP_LOGI(TAG, "Booting in WI-FI Mode.");
        wifi_manager_init();
        web_server_init();
    } else {
        ESP_LOGI(TAG, "Booting Device Profile: ROBOT");
        audio_player_init(); 
        sensor_monitor_init();
        servo_controller_init();
        
        if (strcmp(boot_mode, "bt") == 0) {
            ESP_LOGI(TAG, "Booting in BLUETOOTH Mode. (Wi-Fi Disabled)");
            ble_manager_init();
        } else {
            ESP_LOGI(TAG, "Booting in WI-FI Mode.");
            wifi_manager_init();
            web_server_init();
        }
    }

    // 5. Start standard input keyboard listener for REPL commands
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Boot Sequence Complete!");
}