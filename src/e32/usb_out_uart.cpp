// Chess System Tal Retro - output to the PC on boards with a USB-serial chip
// (the E32R40T's CH340): the usb_out.h interface over the ordinary UART.
// None of the ESP32-S3 native-USB workarounds of cores3/usb_out.cpp apply;
// the lock keeps lines whole, since the engine task and loop() both print.
#include "../cores3/usb_out.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

static SemaphoreHandle_t lock_;

void usb_out_begin() { lock_ = xSemaphoreCreateMutex(); }

void usb_write(const void* data, size_t n) {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  Serial.write(static_cast<const uint8_t*>(data), n);
  xSemaphoreGive(lock_);
}

void usb_out_stats(uint32_t* dropped, uint32_t* longest_wait_ms) {
  *dropped = 0;  // the UART blocks rather than drops
  *longest_wait_ms = 0;
}

void usb_print(const char* s) { usb_write(s, strlen(s)); }

// One locked write for the line and its ending.
void usb_println(const char* s) {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  Serial.write(reinterpret_cast<const uint8_t*>(s), strlen(s));
  Serial.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
  xSemaphoreGive(lock_);
}

void usb_printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) usb_write(buf, size_t(n) < sizeof buf ? size_t(n) : sizeof buf - 1);
}
