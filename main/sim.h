#ifndef SIM_H
#define SIM_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// #ifdef CONFIG_HAS_SIM
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>
// #endif // CONFIG_HAS_SIM

// SIM Module Communication Mode States
typedef enum {
  SIM_MODE_INIT,
  SIM_MODE_INTERNET,
  SIM_MODE_SMS, // Kept for compatibility but not used
  SIM_MODE_FAILED
} sim_mode_t;

// GPIO and Hardware Configuration
// #define SIM_GPIO GPIO_NUM_46 // PWRKEY pin for SIM7600 (try GPIO 4 or 5)
// #define SIM_GPIO GPIO_NUM_33 // PWRKEY pin for SIM7600 (try GPIO 4 or 5)
#define SIM_GPIO -1 // PWRKEY pin for SIM7600 (try GPIO 4 or 5)
// #define SIM_GPIO GPIO_NUM_5 // PWRKEY pin for SIM7600 (try GPIO 4 or 5)


// #ifdef CONFIG_HAS_SIM

// DTR and RI pins for power management
#define SIM_DTR_PIN GPIO_NUM_45  // DTR: ESP32 output to module (LOW=awake, HIGH=sleep)
#define SIM_RI_PIN  GPIO_NUM_40  // RI: Module output to ESP32 (ring indicator)

// GSM Hardware Configuration
#define GSM_UART_NUM UART_NUM_2
// PPPOS/Internet Hardware Configuration
#define MODEM_UART_BAUD_RATE 115200
#define MODEM_UART_TX_PIN GPIO_NUM_18 // ESP32 TX -> SIM7600 RX
#define MODEM_UART_RX_PIN GPIO_NUM_17 // ESP32 RX <- SIM7600 TX
#define MODEM_UART_RTS_PIN 0
#define MODEM_UART_CTS_PIN 0
#define MODEM_UART_NUM UART_NUM_2
#define MODEM_UART_RX_BUFFER_SIZE 512
#define MODEM_UART_TX_BUFFER_SIZE 512
#define MODEM_UART_EVENT_QUEUE_SIZE 10
#define MODEM_UART_EVENT_TASK_STACK_SIZE 4096
#define MODEM_UART_EVENT_TASK_PRIORITY 10
#define MODEM_PPP_APN "internet"

// Connection timeout configurations
#define PPP_CONNECTION_TIMEOUT_MS (15 * 1000) // 15 seconds
#define MODEM_INIT_RETRY_COUNT 3
#define CONNECTION_RETRY_COUNT 3

// SNTP configuration
#define SNTP_SERVER_PRIMARY "pool.ntp.org"
#define SNTP_SERVER_SECONDARY "time.google.com"
#define TIME_SYNC_CHECK_INTERVAL_MS 1000

// #endif // CONFIG_HAS_SIM

// Public API Functions (always available)
esp_err_t sim_init(void);
esp_err_t sim_establish_internet(void);
sim_mode_t sim_get_current_mode(void);
bool sim_is_connected(void);
void power_cycle_gsm(void);

// Utility functions for compatibility
const char *sim_get_status(void);
const char *sim_get_imsi(void);
esp_err_t sim_disconnect(void);

// SMS functionality
esp_err_t sim_send_sms(const char *phone_number, const char *message);

// Legacy compatibility functions for existing code
bool isPPPConnected(void);

// Global variables for compatibility
extern bool sim_init_success;
extern bool is_internet_connected;
extern bool is_sms_connected;

#endif // SIM_H
