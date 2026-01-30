#pragma once

// Waveshare ESP32-S3 Camera Board Pin Definitions
// OCT PCB Configuration (Octal SPIRAM)

#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1

// I2C pins (same for all SPIRAM modes)
#define CAM_PIN_SIOD 15  // SDA
#define CAM_PIN_SIOC 16  // SCL

// Data pins (same for all SPIRAM modes)
#define CAM_PIN_D7 14 // Y9
#define CAM_PIN_D6 13 // Y8
#define CAM_PIN_D5 12 // Y7
#define CAM_PIN_D4 11 // Y6
#define CAM_PIN_D3 10 // Y5
#define CAM_PIN_D2 9  // Y4
#define CAM_PIN_D1 8  // Y3
#define CAM_PIN_D0 7  // Y2

// OCT PCB: GPIO 33-37 used by SPIRAM octal interface, need alternative pins
#define CAM_PIN_XCLK 39
#define CAM_PIN_VSYNC 42
#define CAM_PIN_HREF 41
#define CAM_PIN_PCLK 46