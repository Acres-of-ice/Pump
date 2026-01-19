#include "sim.h"

// #ifdef CONFIG_HAS_SIM

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "SIM";

// Global variables for compatibility
bool sim_init_success = false;
bool is_internet_connected = false;
bool is_sms_connected = false;

// Static variables for state management
static sim_mode_t current_mode = SIM_MODE_INIT;
static SemaphoreHandle_t sim_mode_mutex = NULL;
static bool is_time_synchronized = false;
static char imsi_number[16] = {0};
static const char *detected_apn = MODEM_PPP_APN;

// Internet communication variables
static esp_modem_dce_t *dce = NULL;
static esp_netif_t *esp_netif_ppp = NULL;
static SemaphoreHandle_t connection_mutex = NULL;

// Forward declarations for internal functions
static esp_err_t sim_init_internet(void);
static esp_err_t sim_set_mode(sim_mode_t new_mode);
static esp_err_t sim_cleanup(void);

// Utility functions
static bool test_gsm_at_baud(int baud_rate);
static esp_err_t init_gsm_uart_driver(void);
static esp_err_t change_gsm_baud_rate(void);
static bool escape_data_mode_to_command(void);
static void time_sync_notification_cb(struct timeval *tv);
static esp_err_t initialize_sntp_enhanced(void);
static esp_err_t wait_for_time_sync(uint32_t timeout_ms);
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);
static bool send_at_command_debug(const char *command,
                                  const char *expected_response,
                                  uint32_t timeout_ms, int baud_rate);

// Public API Functions

sim_mode_t sim_get_current_mode(void) {
  if (sim_mode_mutex &&
      xSemaphoreTake(sim_mode_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    sim_mode_t mode = current_mode;
    xSemaphoreGive(sim_mode_mutex);
    return mode;
  }
  return current_mode;
}

bool sim_is_connected(void) {
  sim_mode_t mode = sim_get_current_mode();
  return (mode == SIM_MODE_INTERNET) && is_internet_connected;
}

bool isPPPConnected(void) {
  if (current_mode == SIM_MODE_INTERNET) {
    if (xSemaphoreTake(connection_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      bool connected = is_internet_connected;
      xSemaphoreGive(connection_mutex);
      return connected;
    }
    return false;
  }
  return false;
}

const char *sim_get_status(void) {
  sim_mode_t mode = sim_get_current_mode();
  switch (mode) {
  case SIM_MODE_INTERNET:
    return is_time_synchronized ? "Internet & time Synced"
                                : "Internet Connected";
  case SIM_MODE_INIT:
    return "Initializing";
  case SIM_MODE_FAILED:
  default:
    return "Disconnected";
  }
}

const char *sim_get_imsi(void) {
  return imsi_number[0] != '\0' ? imsi_number : "Unknown";
}

// const char* sim_get_imsi(void) {
//   static char imsi[20] = {0};
  
//   // Send AT+CIMI command to get IMSI
//   char *response = sim_send_at_command("AT+CIMI", 2000);
  
//   if (response != NULL) {
//     // Parse IMSI from response (typically 15 digits)
//     char *imsi_start = response;
//     // Skip OK and newlines
//     while (*imsi_start && (*imsi_start == '\r' || *imsi_start == '\n' || 
//            strncmp(imsi_start, "OK", 2) == 0)) {
//       imsi_start++;
//     }
    
//     // Extract digits
//     int i = 0;
//     while (*imsi_start && i < 15 && isdigit(*imsi_start)) {
//       imsi[i++] = *imsi_start++;
//     }
//     imsi[i] = '\0';
    
//     return imsi;
//   }
  
//   return NULL;
// }


esp_err_t sim_disconnect(void) {
  ESP_LOGI(TAG, "Disconnecting SIM module...");
  return sim_cleanup();
}

// Main Initialization Function

esp_err_t sim_init(void) {
  ESP_LOGI(TAG, "=== SIM Module Initialization (Internet-only mode) ===");

  // Clean up any existing resources first
  if (current_mode != SIM_MODE_INIT) {
    ESP_LOGD(TAG,
             "Cleaning up existing SIM resources before reinitializing...");
    sim_cleanup();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Configure DTR pin to keep module awake and responsive
  ESP_LOGI(TAG, "Configuring DTR pin (GPIO %d) for module control",
           SIM_DTR_PIN);
  gpio_config_t dtr_config = {.pin_bit_mask = (1ULL << SIM_DTR_PIN),
                              .mode = GPIO_MODE_OUTPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&dtr_config));
  gpio_set_level(SIM_DTR_PIN, 0); // DTR LOW = module stays awake
  ESP_LOGI(TAG, "✓ DTR set to LOW (module awake)");

  // Configure RI pin as input (for future use - ring indicator)
  ESP_LOGD(TAG, "Configuring RI pin (GPIO %d) as input", SIM_RI_PIN);
  gpio_config_t ri_config = {.pin_bit_mask = (1ULL << SIM_RI_PIN),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&ri_config));

  // Configure PWRKEY for power control (skip if SIM_GPIO is -1)
  if (SIM_GPIO >= 0) {
    ESP_LOGI(TAG, "Configuring PWRKEY (GPIO %d)", SIM_GPIO);
    gpio_config_t pwrkey_config = {.pin_bit_mask = (1ULL << SIM_GPIO),
                                   .mode = GPIO_MODE_OUTPUT,
                                   .pull_up_en = GPIO_PULLUP_DISABLE,
                                   .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                   .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&pwrkey_config));
    gpio_set_level(SIM_GPIO, 1); // PWRKEY HIGH = inactive
    ESP_LOGI(TAG, "✓ PWRKEY set to HIGH (inactive)");

    // Send power-on pulse to wake/reset module after deep sleep
    // SIM7670G: LOW pulse for 500-1000ms = power on/wake
    ESP_LOGI(TAG, "Sending PWRKEY pulse to wake/reset module...");
    gpio_set_level(SIM_GPIO, 0);     // Pull PWRKEY LOW
    vTaskDelay(pdMS_TO_TICKS(1000)); // Hold for 1 second
    gpio_set_level(SIM_GPIO, 1);     // Release PWRKEY to HIGH
  } else {
    ESP_LOGI(TAG, "PWRKEY disabled (SIM_GPIO=-1), assuming module is externally powered");
  }

  ESP_LOGI(TAG, "Waiting for SIM module to boot (10s)...");
  vTaskDelay(pdMS_TO_TICKS(10000));

  // Create mutex for mode management if it doesn't exist
  if (!sim_mode_mutex) {
    sim_mode_mutex = xSemaphoreCreateMutex();
    if (!sim_mode_mutex) {
      ESP_LOGW(TAG, "Failed to create sim mode mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  // Set initial state
  sim_set_mode(SIM_MODE_INIT);
  sim_init_success = false;
  is_internet_connected = false;
  is_time_synchronized = false;

  // Initialize internet hardware
  ESP_LOGI(TAG, "Step 1: Initializing internet hardware...");
  esp_err_t internet_result = sim_init_internet();
  if (internet_result != ESP_OK) {
    ESP_LOGW(TAG, "✗ Internet hardware initialization failed");
    sim_set_mode(SIM_MODE_FAILED);
    sim_init_success = false;
    if (sim_mode_mutex) {
      vSemaphoreDelete(sim_mode_mutex);
      sim_mode_mutex = NULL;
    }
    // gpio_set_level(SIM_GPIO, 1);
    return ESP_FAIL;
  }

  // Establish internet connection
  ESP_LOGI(TAG, "Step 2: Establishing internet connection (60s timeout)...");
  internet_result = sim_establish_internet();
  if (internet_result != ESP_OK) {
    ESP_LOGW(TAG, "✗ Internet connection failed");
    sim_set_mode(SIM_MODE_FAILED);
    sim_init_success = false;
    if (sim_mode_mutex) {
      vSemaphoreDelete(sim_mode_mutex);
      sim_mode_mutex = NULL;
    }
    // gpio_set_level(SIM_GPIO, 1);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "✓ Internet connection established successfully");
  sim_set_mode(SIM_MODE_INTERNET);
  is_internet_connected = true;
  sim_init_success = true;

  return ESP_OK;
}

// Internal State Management Functions

static esp_err_t sim_set_mode(sim_mode_t new_mode) {
  if (sim_mode_mutex &&
      xSemaphoreTake(sim_mode_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    sim_mode_t old_mode = current_mode;
    current_mode = new_mode;
    xSemaphoreGive(sim_mode_mutex);

    if (old_mode != new_mode) {
      ESP_LOGI(TAG, "Mode transition: %d -> %d", old_mode, new_mode);
    }
    return ESP_OK;
  }
  return ESP_FAIL;
}

static esp_err_t sim_cleanup(void) {
  ESP_LOGD(TAG, "Cleaning up SIM module resources");

  // Mark as disconnected
  is_internet_connected = false;

  // Cleanup internet resources - destroy modem DCE first
  if (dce) {
    ESP_LOGI(TAG, "Destroying modem DCE instance");
    esp_modem_destroy(dce);
    dce = NULL;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  if (esp_netif_ppp) {
    ESP_LOGD(TAG, "Destroying PPP netif");
    esp_netif_destroy(esp_netif_ppp);
    esp_netif_ppp = NULL;
  }

  if (connection_mutex) {
    vSemaphoreDelete(connection_mutex);
    connection_mutex = NULL;
  }

  // Delete UART driver if installed
  if (uart_is_driver_installed(GSM_UART_NUM)) {
    ESP_LOGI(TAG, "Deleting UART driver");
    uart_driver_delete(GSM_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Reset state
  is_internet_connected = false;
  is_time_synchronized = false;

  ESP_LOGI(TAG, "SIM module resources cleaned up");
  return ESP_OK;
}

// Internet Initialization Functions

static esp_err_t sim_init_internet(void) {
  ESP_LOGD(TAG, "=== Internet Hardware Init ===");

  // Initialize UART driver first
  if (init_gsm_uart_driver() != ESP_OK) {
    ESP_LOGW(TAG, "Failed to initialize UART driver");
    return ESP_FAIL;
  }

  // Give module some time to stabilize after power-on
  ESP_LOGD(TAG, "Waiting for SIM module to stabilize...");
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Step 1: Try 115200 first (normal default state)
  ESP_LOGD(TAG, "Step 1: Testing if SIM module is at 115200 baud");
  if (!test_gsm_at_baud(115200)) {
    // Step 2: Module not responding - assume it's in data mode, try to escape
    ESP_LOGD(TAG, "Step 2: Module not responding at 115200 baud");
    ESP_LOGD(TAG, "Assuming module is in data mode - attempting to escape to command mode");

    if (escape_data_mode_to_command()) {
      ESP_LOGD(TAG, "✓ Successfully escaped data mode to command mode");

      // Now test again at 115200
      if (!test_gsm_at_baud(115200)) {
        ESP_LOGW(TAG, "✗ Still not responding after escape attempt");

        if (uart_is_driver_installed(MODEM_UART_NUM)) {
          uart_driver_delete(MODEM_UART_NUM);
        }
        return ESP_FAIL;
      }
      ESP_LOGD(TAG, "✓ Module now responding at 115200 baud");
    } else {
      ESP_LOGW(TAG, "✗ Failed to escape data mode or module not responding");
      ESP_LOGW(TAG, "Check module power, connections, and pin configuration");

      if (uart_is_driver_installed(MODEM_UART_NUM)) {
        uart_driver_delete(MODEM_UART_NUM);
      }
      return ESP_FAIL;
    }
  } else {
    ESP_LOGI(TAG, "✓ SIM module at 115200 baud - ready");
  }

  // Configure DTR behavior (AT&D command)
  // AT&D0 = Ignore DTR (module always stays active)
  // AT&D1 = Online command mode when DTR is OFF
  // AT&D2 = Hang up and enter command mode when DTR goes OFF
  ESP_LOGD(TAG, "Configuring DTR pin behavior");
  uart_flush(MODEM_UART_NUM);
  uart_write_bytes(MODEM_UART_NUM, "AT&D0\r\n", 7); // Ignore DTR changes
  vTaskDelay(pdMS_TO_TICKS(500));
  char dtr_response[128] = {0};
  uart_read_bytes(MODEM_UART_NUM, dtr_response, sizeof(dtr_response) - 1,
                  pdMS_TO_TICKS(1000));
  ESP_LOGD(TAG, "AT&D0 response: %s", dtr_response);

  // Get IMSI from SIM card using direct UART
  char response[256] = {0};
  int len;

  ESP_LOGD(TAG, "Retrieving IMSI from SIM card...");
  uart_flush(MODEM_UART_NUM);
  uart_write_bytes(MODEM_UART_NUM, "AT+CIMI\r\n", 9);
  vTaskDelay(pdMS_TO_TICKS(1000));
  len = uart_read_bytes(MODEM_UART_NUM, response, sizeof(response) - 1,
                        pdMS_TO_TICKS(2000));
  if (len > 0) {
    response[len] = '\0';
    ESP_LOGD(TAG, "AT+CIMI response: %s", response);

    // Parse IMSI
    char *imsi_start = response;
    while (*imsi_start &&
           (*imsi_start == '\r' || *imsi_start == '\n' || *imsi_start == ' ')) {
      imsi_start++;
    }

    int digit_count = 0;
    char temp_imsi[16] = {0};
    for (int i = 0; imsi_start[i] && digit_count < 15; i++) {
      if (imsi_start[i] >= '0' && imsi_start[i] <= '9') {
        temp_imsi[digit_count++] = imsi_start[i];
      } else if (digit_count > 0) {
        break;
      }
    }

    if (digit_count >= 14 && digit_count <= 15) {
      temp_imsi[digit_count] = '\0';
      strncpy(imsi_number, temp_imsi, sizeof(imsi_number) - 1);
      imsi_number[sizeof(imsi_number) - 1] = '\0';
      ESP_LOGI(TAG, "IMSI retrieved successfully: %s", imsi_number);
    } else {
      ESP_LOGW(TAG, "Invalid IMSI length: %d digits", digit_count);
    }
  } else {
    ESP_LOGW(TAG, "Failed to retrieve IMSI");
  }

  // Detect SIM provider using IMSI (MCC+MNC) - most reliable method
  ESP_LOGD(TAG, "Detecting SIM provider from IMSI...");

  bool provider_detected = false;

  if (strlen(imsi_number) >= 5) {
    char mcc_mnc[6] = {0};
    strncpy(mcc_mnc, imsi_number, 5);
    mcc_mnc[5] = '\0';

    ESP_LOGD(TAG, "Using IMSI MCC+MNC: %s for provider detection", mcc_mnc);

    // Check for Airtel: 40555, 40400, 404xx, etc.
    if (strcmp(mcc_mnc, "40555") == 0 || strcmp(mcc_mnc, "40400") == 0 ||
        strncmp(mcc_mnc, "4040", 4) == 0 || strcmp(mcc_mnc, "40410") == 0 ||
        strcmp(mcc_mnc, "40416") == 0 || strcmp(mcc_mnc, "40431") == 0 ||
        strcmp(mcc_mnc, "40445") == 0 || strcmp(mcc_mnc, "40449") == 0 ||
        strcmp(mcc_mnc, "40470") == 0 || strncmp(mcc_mnc, "4049", 4) == 0) {
      detected_apn = "airtelgprs.com";
      ESP_LOGD(TAG, "Detected Airtel from IMSI (%s) - using airtelgprs.com APN",
               mcc_mnc);
      provider_detected = true;
    }
    // Check for BSNL: 40462, 40434, 40435, 40437, 40438, 404xx, 407xx
    else if (strcmp(mcc_mnc, "40462") == 0 || strcmp(mcc_mnc, "40434") == 0 ||
             strcmp(mcc_mnc, "40435") == 0 || strcmp(mcc_mnc, "40437") == 0 ||
             strcmp(mcc_mnc, "40438") == 0 ||
             strncmp(mcc_mnc, "4045", 4) == 0 ||
             strcmp(mcc_mnc, "40464") == 0 ||
             strncmp(mcc_mnc, "4047", 4) == 0) {
      detected_apn = "bsnlnet";
      ESP_LOGD(TAG, "Detected BSNL from IMSI (%s) - using bsnlnet APN",
               mcc_mnc);
      provider_detected = true;
    }
    // Check for Jio: 405xx range
    else if (strncmp(mcc_mnc, "4058", 4) == 0) {
      detected_apn = "jionet";
      ESP_LOGD(TAG, "Detected Jio from IMSI (%s) - using jionet APN", mcc_mnc);
      provider_detected = true;
    }
  } else {
    ESP_LOGW(TAG, "IMSI not available or too short for provider detection");
  }

  if (!provider_detected) {
    detected_apn = "internet";
    ESP_LOGI(TAG, "Unable to detect provider, using generic internet APN");
  }

  ESP_LOGD(TAG, "Selected APN: %s", detected_apn);

  // Clean up UART driver before returning - esp_modem will install its own
  if (uart_is_driver_installed(MODEM_UART_NUM)) {
    uart_driver_delete(MODEM_UART_NUM);
  }
  return ESP_OK;
}

esp_err_t sim_establish_internet(void) {
  esp_err_t ret = ESP_OK;

  ESP_LOGD(TAG, "=== Establishing Internet ===");

  // Create mutex for connection state protection
  connection_mutex = xSemaphoreCreateMutex();
  if (!connection_mutex) {
    ESP_LOGW(TAG, "Failed to create connection mutex");
    return ESP_ERR_NO_MEM;
  }

  // Reset state variables
  is_internet_connected = false;
  is_time_synchronized = false;

  // Initialize network interface and event loop
  ret = esp_netif_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
    return ret;
  }

  // Register event handlers
  ret = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event,
                                   NULL);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register IP event handler: %s",
             esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                   &on_ppp_changed, NULL);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register PPP event handler: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // === MODEM LIBRARY INITIALIZATION ===
  ESP_LOGD(TAG, "Step 1: Creating modem DCE instance...");

  // Configure network interface
  esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
  esp_netif_ppp = esp_netif_new(&netif_ppp_config);
  if (!esp_netif_ppp) {
    ESP_LOGE(TAG, "Failed to create PPP netif");
    return ESP_ERR_NO_MEM;
  }

  // Configure DCE with detected APN
  esp_modem_dce_config_t dce_config =
      ESP_MODEM_DCE_DEFAULT_CONFIG(detected_apn);

  // Configure DTE (UART)
  esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
  dte_config.uart_config.tx_io_num = MODEM_UART_TX_PIN;
  dte_config.uart_config.rx_io_num = MODEM_UART_RX_PIN;
  dte_config.uart_config.rts_io_num = MODEM_UART_RTS_PIN;
  dte_config.uart_config.cts_io_num = MODEM_UART_CTS_PIN;
  dte_config.uart_config.port_num = MODEM_UART_NUM;
  dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
  dte_config.uart_config.rx_buffer_size = MODEM_UART_RX_BUFFER_SIZE;
  dte_config.uart_config.tx_buffer_size = MODEM_UART_TX_BUFFER_SIZE;
  dte_config.uart_config.event_queue_size = MODEM_UART_EVENT_QUEUE_SIZE;
  dte_config.task_stack_size = MODEM_UART_EVENT_TASK_STACK_SIZE;
  dte_config.task_priority = MODEM_UART_EVENT_TASK_PRIORITY;
  dte_config.dte_buffer_size = MODEM_UART_RX_BUFFER_SIZE / 2;
  dte_config.uart_config.baud_rate = MODEM_UART_BAUD_RATE;

  // Create modem DCE (SIM7670G uses SIM7600 device type - they share similar AT
  // commands)
  dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config,
                          esp_netif_ppp);
  if (!dce) {
    ESP_LOGE(TAG, "Failed to create modem DCE");
    esp_netif_destroy(esp_netif_ppp);
    esp_netif_ppp = NULL;
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGD(TAG, "✓ Modem DCE created successfully");

  // === SWITCH TO DATA MODE FOR PPP (Skip AT commands) ===
  ESP_LOGD(TAG, "Step 2: Switching to PPP data mode...");

  const int MAX_DATA_MODE_RETRIES = 5;
  bool data_mode_success = false;
  for (int retry = 0; retry < MAX_DATA_MODE_RETRIES; retry++) {
    if (retry > 0) {
      ESP_LOGW(TAG, "Retrying data mode command (attempt %d/%d)", retry + 1,
               MAX_DATA_MODE_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (ret == ESP_OK) {
      ESP_LOGD(TAG, "✓ Data mode command successful");
      data_mode_success = true;
      break;
    } else {
      ESP_LOGW(TAG, "✗ Data mode command failed: %s (attempt %d/%d)",
               esp_err_to_name(ret), retry + 1, MAX_DATA_MODE_RETRIES);
    }
  }

  if (!data_mode_success) {
    ESP_LOGE(TAG, "Failed to set data mode after %d attempts",
             MAX_DATA_MODE_RETRIES);
    esp_modem_destroy(dce);
    dce = NULL;
    esp_netif_destroy(esp_netif_ppp);
    esp_netif_ppp = NULL;
    return ESP_FAIL;
  }

  ESP_LOGD(TAG, "✓ Modem in PPP data mode");
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Wait for PPP connection
  ESP_LOGD(TAG, "Waiting for PPP connection (timeout: %d seconds)...",
           PPP_CONNECTION_TIMEOUT_MS / 1000);

  TickType_t connection_start = xTaskGetTickCount();
  TickType_t connection_timeout = pdMS_TO_TICKS(PPP_CONNECTION_TIMEOUT_MS);

  while (!is_internet_connected) {
    if ((xTaskGetTickCount() - connection_start) > connection_timeout) {
      ESP_LOGW(TAG, "PPP connection timeout after %d ms",
               PPP_CONNECTION_TIMEOUT_MS);
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGD(TAG, "PPP connection established successfully");

  // Always initialize SNTP after internet connection
  ESP_LOGD(TAG, "Attempting SNTP time synchronization...");
  ret = initialize_sntp_enhanced();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SNTP initialization failed: %s", esp_err_to_name(ret));
  } else {
    ret = wait_for_time_sync(30000);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Time synchronization failed: %s", esp_err_to_name(ret));
    } else {
      is_time_synchronized = true;
    }
  }

  ESP_LOGI(TAG, "=== Internet Connection Complete ===\n  PPP: ✓  Time: %s",
           is_time_synchronized ? "✓" : "✗");

  return ESP_OK;
}

// SNTP and Event Handling Functions

static void time_sync_notification_cb(struct timeval *tv) {
  ESP_LOGD(TAG, "Time synchronization event received");
  is_time_synchronized = true;

  time_t now = time(NULL);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char strftime_buf[64];
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGD(TAG, "Current time: %s", strftime_buf);
}

static esp_err_t initialize_sntp_enhanced(void) {
  ESP_LOGD(TAG, "Initializing enhanced SNTP configuration");

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

  esp_sntp_setservername(0, "time.google.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_setservername(2, "time.nist.gov");
  esp_sntp_setservername(3, "time.cloudflare.com");

  setenv("TZ", "IST-5:30", 1);
  tzset();

  esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED); // Sync immediately once

  esp_sntp_init();
  ESP_LOGD(TAG, "SNTP initialized for time synchronization");
  return ESP_OK;
}

static esp_err_t wait_for_time_sync(uint32_t timeout_ms) {
  ESP_LOGD(TAG,
           "Waiting for SNTP time synchronization (timeout: %" PRIu32 " ms)",
           timeout_ms);

  TickType_t start_time = xTaskGetTickCount();
  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

  is_time_synchronized = false;

  time_t time_before_sntp = time(NULL);
  ESP_LOGD(TAG, "Time before SNTP sync: %lld", (long long)time_before_sntp);

  // SNTP already started in initialize_sntp_enhanced(), no need to restart
  vTaskDelay(pdMS_TO_TICKS(3000));

  while (!is_time_synchronized) {
    if ((xTaskGetTickCount() - start_time) > timeout_ticks) {
      ESP_LOGW(TAG, "SNTP synchronization timeout after %" PRIu32 " ms",
               timeout_ms);
      return ESP_ERR_TIMEOUT;
    }

    time_t now = time(NULL);
    if (llabs((long long)(now - time_before_sntp)) > 5) {
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      if (timeinfo.tm_year > (2020 - 1900)) {
        ESP_LOGI(TAG, "Time appears to have been updated by SNTP");
        is_time_synchronized = true;
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_CHECK_INTERVAL_MS));
  }

  if (is_time_synchronized) {
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S",
             &timeinfo);
    ESP_LOGI(TAG, "SNTP synchronization successful: %s", strftime_buf);

    // Stop SNTP after first sync - only sync on boot
    esp_sntp_stop();
    ESP_LOGD(TAG, "SNTP stopped - time synced once on boot");

    return ESP_OK;
  }

  return ESP_ERR_TIMEOUT;
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
  if (event_id != NETIF_PPP_ERRORNONE) {
    ESP_LOGW(TAG, "PPP event: %ld", (long)event_id);
  }
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
  if (connection_mutex &&
      xSemaphoreTake(connection_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
      ESP_LOGD(TAG, "PPP Connected");
      is_internet_connected = true;
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
      ESP_LOGW(TAG, "PPP Lost");
      is_internet_connected = false;
      is_time_synchronized = false;
    }
    xSemaphoreGive(connection_mutex);
  }
}

void power_cycle_gsm(void) {
  if (SIM_GPIO >= 0) {
    ESP_LOGI(TAG, "Power cycling GSM module");
    gpio_set_level(SIM_GPIO, 0);
    vTaskDelay(1000);
    ESP_LOGD(TAG, "Switching off");
    gpio_set_level(SIM_GPIO, 1);
    vTaskDelay(1000);
    ESP_LOGD(TAG, "Switching on");
    gpio_set_level(SIM_GPIO, 0);
    vTaskDelay(1000);
  } else {
    ESP_LOGW(TAG, "Cannot power cycle - SIM_GPIO disabled (set to -1)");
  }
}

// UART and Baud Rate Utility Functions

static bool test_gsm_at_baud(int baud_rate) {
  ESP_LOGD(TAG, "Testing SIM module at %d baud", baud_rate);

  uart_config_t uart_config = {
      .baud_rate = baud_rate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  esp_err_t err = uart_param_config(MODEM_UART_NUM, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  // Try multiple times as module might need to wake up
  for (int attempt = 0; attempt < 3; attempt++) {
    uart_flush(MODEM_UART_NUM);
    uart_write_bytes(MODEM_UART_NUM, "AT\r\n", 4);
    vTaskDelay(pdMS_TO_TICKS(300));

    char response[128] = {0};
    int len = uart_read_bytes(MODEM_UART_NUM, response, sizeof(response) - 1,
                              pdMS_TO_TICKS(1000));

    if (len > 0) {
      response[len] = '\0';
      ESP_LOGD(TAG, "Response at %d baud (attempt %d): '%s'", baud_rate,
               attempt + 1, response);

      // Check for OK - can be with or without echo
      if (strstr(response, "OK") != NULL) {
        ESP_LOGI(TAG, "✓ SIM module responding at %d baud", baud_rate);

        // Disable echo for cleaner communication
        uart_flush(MODEM_UART_NUM);
        uart_write_bytes(MODEM_UART_NUM, "ATE0\r\n", 6);
        vTaskDelay(pdMS_TO_TICKS(300));
        uart_read_bytes(MODEM_UART_NUM, response, sizeof(response) - 1,
                        pdMS_TO_TICKS(500));
        ESP_LOGD(TAG, "Echo disabled");

        return true;
      }

      // If we got ERROR, module is responding but might be in bad state
      if (strstr(response, "ERROR") != NULL) {
        ESP_LOGD(TAG, "Module responding but with ERROR, trying again...");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
    }
  }

  ESP_LOGD(TAG, "✗ No valid response at %d baud", baud_rate);
  return false;
}

static esp_err_t init_gsm_uart_driver(void) {
  bool driver_was_installed = uart_is_driver_installed(MODEM_UART_NUM);

  if (driver_was_installed) {
    ESP_LOGD(TAG, "UART driver already installed, skipping installation");
  } else {
    esp_err_t err = uart_driver_install(MODEM_UART_NUM, 1024, 1024, 0, NULL, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
      return err;
    }
  }

  esp_err_t err =
      uart_set_pin(MODEM_UART_NUM, MODEM_UART_TX_PIN, MODEM_UART_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
    if (!driver_was_installed) {
      uart_driver_delete(MODEM_UART_NUM);
    }
    return err;
  }

  ESP_LOGD(TAG, "UART driver initialized for SIM module testing");
  return ESP_OK;
}

static bool send_at_command_debug(const char *command,
                                  const char *expected_response,
                                  uint32_t timeout_ms, int baud_rate) {
  char response_buffer[512] = {0};
  int response_len = 0;

  ESP_LOGD(TAG, "=== AT Command Debug ===");
  ESP_LOGD(TAG, "Baud Rate: %d", baud_rate);
  ESP_LOGD(TAG, "Sending: '%s'", command);
  ESP_LOGD(TAG, "Expected: '%s'", expected_response);
  ESP_LOGD(TAG, "Timeout: %" PRIu32 " ms", timeout_ms);

  uart_flush(MODEM_UART_NUM);
  ESP_LOGD(TAG, "UART buffer flushed");

  int bytes_written =
      uart_write_bytes(MODEM_UART_NUM, command, strlen(command));
  uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
  ESP_LOGD(TAG, "Command sent (%d bytes)", bytes_written + 2);

  TickType_t start_time = xTaskGetTickCount();
  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

  while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
    int len = uart_read_bytes(
        MODEM_UART_NUM, (uint8_t *)response_buffer + response_len,
        sizeof(response_buffer) - response_len - 1, pdMS_TO_TICKS(200));

    if (len > 0) {
      response_len += len;
      response_buffer[response_len] = '\0';

      ESP_LOGD(TAG, "Raw response so far: '%s'", response_buffer);

      if (strstr(response_buffer, expected_response) != NULL) {
        ESP_LOGD(TAG, "✓ Expected response '%s' found!", expected_response);
        ESP_LOGD(TAG, "Total response time: %" PRIu32 " ms",
                 (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS);
        return true;
      }

      if (strstr(response_buffer, "ERROR") != NULL) {
        ESP_LOGW(TAG, "✗ AT command failed with ERROR");
        return false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGW(TAG, "✗ Timeout after %" PRIu32 " ms", timeout_ms);
  ESP_LOGW(TAG, "Final response (%d bytes): '%s'", response_len,
           response_buffer);
  return false;
}

// SMS send function
esp_err_t sim_send_sms(const char *phone_number, const char *message) {
  if (!phone_number || !message) {
    ESP_LOGE(TAG, "Invalid SMS parameters");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "=== Sending SMS ===");
  ESP_LOGI(TAG, "To: %s", phone_number);
  ESP_LOGI(TAG, "Message: %s", message);

  // Step 1: Switch modem from DATA mode to COMMAND mode
  bool was_in_ppp = (current_mode == SIM_MODE_INTERNET);
  if (was_in_ppp && dce) {
    ESP_LOGI(TAG, "Switching modem from DATA to COMMAND mode...");
    esp_err_t ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to switch to command mode: %s", esp_err_to_name(ret));
      return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(2000)); // Give modem time to fully switch modes
    ESP_LOGI(TAG, "Modem switched to COMMAND mode");

    // Flush UART buffers and test AT communication
    uart_flush(MODEM_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Testing AT communication after mode switch...");
    if (!send_at_command_debug("AT", "OK", 3000, 115200)) {
      ESP_LOGE(TAG, "Modem not responding to AT commands after mode switch");
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "✓ AT communication OK");
  }

  // Step 2: Set SMS text mode
  ESP_LOGI(TAG, "Setting SMS to text mode...");
  if (!send_at_command_debug("AT+CMGF=1", "OK", 3000, 115200)) {
    ESP_LOGE(TAG, "Failed to set SMS text mode");
    return ESP_FAIL;
  }

  // Step 3: Set SMS character set to GSM
  ESP_LOGI(TAG, "Setting character set to GSM...");
  send_at_command_debug("AT+CSCS=\"GSM\"", "OK", 3000, 115200);

  // Step 4: Initiate SMS send
  char at_cmd[64];
  snprintf(at_cmd, sizeof(at_cmd), "AT+CMGS=\"%s\"", phone_number);
  ESP_LOGI(TAG, "Initiating SMS: %s", at_cmd);

  uart_flush(MODEM_UART_NUM);
  uart_write_bytes(MODEM_UART_NUM, at_cmd, strlen(at_cmd));
  uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

  // Wait for '>' prompt
  char response[128] = {0};
  int response_len = 0;
  TickType_t start = xTaskGetTickCount();
  bool got_prompt = false;

  while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
    int len = uart_read_bytes(MODEM_UART_NUM, (uint8_t*)response + response_len,
                              sizeof(response) - response_len - 1, pdMS_TO_TICKS(200));
    if (len > 0) {
      response_len += len;
      response[response_len] = '\0';
      if (strchr(response, '>') != NULL) {
        got_prompt = true;
        ESP_LOGI(TAG, "Got '>' prompt, sending message...");
        break;
      }
    }
  }

  if (!got_prompt) {
    ESP_LOGE(TAG, "Timeout waiting for '>' prompt. Response: %s", response);
    return ESP_FAIL;
  }

  // Step 5: Send message text followed by Ctrl+Z (0x1A)
  uart_write_bytes(MODEM_UART_NUM, message, strlen(message));
  uart_write_bytes(MODEM_UART_NUM, "\x1A", 1); // Ctrl+Z to send

  ESP_LOGI(TAG, "Message text sent, waiting for confirmation...");

  // Step 6: Wait for +CMGS response
  memset(response, 0, sizeof(response));
  response_len = 0;
  start = xTaskGetTickCount();
  bool sms_sent = false;

  while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(30000)) { // 30s timeout for SMS send
    int len = uart_read_bytes(MODEM_UART_NUM, (uint8_t*)response + response_len,
                              sizeof(response) - response_len - 1, pdMS_TO_TICKS(500));
    if (len > 0) {
      response_len += len;
      response[response_len] = '\0';
      ESP_LOGD(TAG, "SMS response: %s", response);

      if (strstr(response, "+CMGS:") != NULL || strstr(response, "OK") != NULL) {
        sms_sent = true;
        ESP_LOGI(TAG, "✓ SMS sent successfully!");
        break;
      }

      if (strstr(response, "ERROR") != NULL) {
        ESP_LOGE(TAG, "✗ SMS send failed: %s", response);
        return ESP_FAIL;
      }
    }
  }

  if (!sms_sent) {
    ESP_LOGE(TAG, "Timeout waiting for SMS confirmation");
    return ESP_FAIL;
  }

  // Step 7: If we were in PPP mode, we DON'T need to reconnect
  // The periodic task will handle deep sleep entry immediately
  ESP_LOGI(TAG, "SMS sent, not reconnecting PPP (entering deep sleep)");

  return ESP_OK;
}

static bool escape_data_mode_to_command(void) {
  ESP_LOGD(TAG, "=== Attempting Hayes Escape Sequence (+++​) ===");

  // Hayes escape sequence requires:
  // 1. Guard time before (1 second of silence)
  // 2. Send "+++" without CR/LF
  // 3. Guard time after (1 second of silence)

  ESP_LOGD(TAG, "Guard time before: waiting 1.5 seconds...");
  vTaskDelay(pdMS_TO_TICKS(1500));

  ESP_LOGD(TAG, "Sending '+++​' escape sequence (without CR/LF)");
  uart_flush(MODEM_UART_NUM);
  uart_write_bytes(MODEM_UART_NUM, "+++", 3); // NO \r\n!

  ESP_LOGD(TAG, "Guard time after: waiting 1.5 seconds...");
  vTaskDelay(pdMS_TO_TICKS(1500));

  // Check for response - some modems send "OK" or "NO CARRIER"
  char response[128] = {0};
  int len = uart_read_bytes(MODEM_UART_NUM, response, sizeof(response) - 1,
                            pdMS_TO_TICKS(1000));

  if (len > 0) {
    response[len] = '\0';
    ESP_LOGD(TAG, "Escape sequence response: '%s'", response);
  } else {
    ESP_LOGD(TAG, "No response to escape sequence (some modules don't respond)");
  }

  // Wait a bit more for mode transition to complete
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Try sending AT command to verify we're in command mode
  ESP_LOGD(TAG, "Verifying command mode with AT test...");
  uart_flush(MODEM_UART_NUM);
  uart_write_bytes(MODEM_UART_NUM, "AT\r\n", 4);
  vTaskDelay(pdMS_TO_TICKS(500));

  len = uart_read_bytes(MODEM_UART_NUM, response, sizeof(response) - 1,
                        pdMS_TO_TICKS(1000));

  if (len > 0) {
    response[len] = '\0';
    ESP_LOGD(TAG, "AT response after escape: '%s'", response);

    if (strstr(response, "OK") != NULL) {
      ESP_LOGD(TAG, "✓ Module is now in command mode");
      return true;
    }
  }

  ESP_LOGW(TAG, "✗ Module did not respond to AT command after escape");
  return false;
}

__attribute__((unused)) static esp_err_t change_gsm_baud_rate(void) {
  ESP_LOGI(TAG, "=== SIM Module Baud Rate Change ===");

  ESP_LOGI(TAG, "Step 1: Configuring ESP32 UART to 9600 baud");

  uart_config_t uart_config = {
      .baud_rate = 9600,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  esp_err_t err = uart_param_config(MODEM_UART_NUM, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "✗ Failed to configure UART: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "✓ ESP32 UART configured to 9600 baud");

  vTaskDelay(pdMS_TO_TICKS(1000));

  ESP_LOGI(TAG, "Step 2: Testing GSM connectivity at 9600 baud");

  if (!send_at_command_debug("AT", "OK", 3000, 9600)) {
    ESP_LOGW(TAG, "✗ GSM not responding at 9600 baud");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "✓ GSM responding at 9600 baud");

  ESP_LOGI(TAG, "Sending baud rate change command");

  if (!send_at_command_debug("AT+IPREX=115200", "OK", 5000, 9600)) {
    ESP_LOGW(TAG, "✗ Failed to send baud rate change command");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "✓ GSM accepted baud rate change command");

  ESP_LOGI(TAG, "Waiting for GSM to apply new baud rate");
  vTaskDelay(pdMS_TO_TICKS(1500));

  ESP_LOGI(TAG, "Reconfiguring ESP32 UART to 115200 baud");

  uart_config.baud_rate = 115200;
  err = uart_param_config(MODEM_UART_NUM, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "✗ Failed to reconfigure UART to 115200: %s",
             esp_err_to_name(err));
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "✓ ESP32 UART reconfigured to 115200 baud");

  vTaskDelay(pdMS_TO_TICKS(1000));

  ESP_LOGI(TAG, "Testing GSM connectivity at 115200 baud");

  if (!send_at_command_debug("AT", "OK", 3000, 115200)) {
    ESP_LOGW(TAG, "✗ GSM not responding at 115200 baud");

    uart_config.baud_rate = 9600;
    uart_param_config(MODEM_UART_NUM, &uart_config);
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (send_at_command_debug("AT", "OK", 3000, 9600)) {
      ESP_LOGI(TAG, "✓ Recovered to 9600 baud");
    }
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "✓ GSM responding at 115200 baud");

  ESP_LOGI(TAG, "Saving GSM configuration");
  send_at_command_debug("AT&W", "OK", 3000, 115200);

  ESP_LOGI(TAG, "=== ✓ SIM Module Baud Rate SUCCESS - 9600 → 115200 baud ====");

  return ESP_OK;
}

//#else // CONFIG_HAS_SIM not defined - provide stub implementations

// Global variables for compatibility
// bool sim_init_success = false;
// bool is_internet_connected = false;
// bool is_sms_connected = false;

// Stub implementations when SIM is disabled
//sim_mode_t sim_get_current_mode(void) { return SIM_MODE_FAILED; }

// bool sim_is_connected(void) { return false; }

// bool isPPPConnected(void) { return false; }

// const char *sim_get_status(void) { return "SIM Disabled"; }

// const char *sim_get_imsi(void) { return "SIM Disabled"; }

// esp_err_t sim_disconnect(void) { return ESP_OK; }

// esp_err_t sim_init(void) {
//   sim_init_success = false;
//   return ESP_ERR_NOT_SUPPORTED;
// }

// esp_err_t sim_establish_internet(void) { return ESP_ERR_NOT_SUPPORTED; }

// esp_err_t sim_send_sms(const char *phone_number, const char *message) {
//   return ESP_ERR_NOT_SUPPORTED;
// }

//#endif // CONFIG_HAS_SIM
