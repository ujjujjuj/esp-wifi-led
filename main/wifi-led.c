#include "config.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "responses.h"
#include "string.h"

#define MAX_RETRIES 10
#define WIFI_FAILURE 1 << 0
#define WIFI_SUCCESS 1 << 1
#define TCP_FAILURE 1 << 0
#define TCP_SUCCESS 1 << 1

#define BUF_SIZE 4096
#define LED_PIN 2
#define SERVER_PORT 80

static EventGroupHandle_t wifi_event_group;

static char* TAG;
static int gpio_state = 0;

static int ap_retries = 0;
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to access point...\n");
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (ap_retries < MAX_RETRIES) {
            ESP_LOGW(TAG, "Reconnecting to access point...\n");
            esp_wifi_connect();
            ap_retries++;
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILURE);
        }
    }
}

static void ip_reg_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Station IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ap_retries = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_SUCCESS);
    }
}

esp_err_t wifi_connect() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t wifi_handler_inst;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_handler_inst));

    esp_event_handler_instance_t ip_handler_inst;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_reg_event_handler, NULL, &ip_handler_inst));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false}},
    };
    memcpy(wifi_cfg.sta.ssid, CFG_WIFI_SSID, sizeof(CFG_WIFI_SSID));
    memcpy(wifi_cfg.sta.password, CFG_WIFI_PASS, sizeof(CFG_WIFI_PASS));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Started interface\n");

    EventBits_t event_bits = xEventGroupWaitBits(wifi_event_group, WIFI_SUCCESS | WIFI_FAILURE, pdFALSE, pdFALSE, portMAX_DELAY);

    esp_err_t status = WIFI_FAILURE;
    if (event_bits & WIFI_SUCCESS) {
        ESP_LOGI(TAG, "Connected to access point successfully\n");
        status = WIFI_SUCCESS;
    } else if (event_bits & WIFI_FAILURE) {
        ESP_LOGE(TAG, "Failed to connect to AP\n");
    } else {
        ESP_LOGE(TAG, "Unexpected event\n");
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_inst));
    vEventGroupDelete(wifi_event_group);

    return status;
}

esp_err_t tcp_run_server() {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket\n");
        return TCP_FAILURE;
    }

    struct sockaddr_in server_addr = {0};

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(sock_fd, (const struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind socket\n");
        close(sock_fd);
        return TCP_FAILURE;
    }

    if ((listen(sock_fd, 0)) != 0) {
        ESP_LOGE(TAG, "Failed to listen\n");

        close(sock_fd);
        exit(0);
    }

    for (;;) {
        struct sockaddr_in client_addr = {0};
        socklen_t client_addr_size = sizeof(client_addr);

        int client_fd = accept(sock_fd, (struct sockaddr*)&client_addr, &client_addr_size);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "Error accepting client");
            return TCP_FAILURE;
        }

        char* read_buf = (char*)malloc(BUF_SIZE);
        read(client_fd, read_buf, BUF_SIZE);
        if (strstr(read_buf, "toggle") != NULL) {
            gpio_state = gpio_state == 0 ? 1 : 0;
            gpio_set_level(LED_PIN, gpio_state);
            write(client_fd, TOGGLE_RESPONSE, strlen(TOGGLE_RESPONSE));
        } else {
            write(client_fd, INDEX_PAGE_RESPONSE, strlen(INDEX_PAGE_RESPONSE));
        }

        free(read_buf);
        close(client_fd);
    }

    return TCP_SUCCESS;
}

void init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void error_state() {
    for (;;) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    TAG = pcTaskGetName(NULL);
    ESP_LOGI(TAG, "Started task\n");

    init_nvs();
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    esp_err_t status;

    status = wifi_connect();
    if (status != WIFI_SUCCESS) {
        ESP_LOGE(TAG, "Cannot connect to WIFI...\n");
        error_state();
    }

    status = tcp_run_server();
    if (status != TCP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create server...\n");
        error_state();
    }

    return;
}
