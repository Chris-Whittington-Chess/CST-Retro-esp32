// Chess System Tal Retro - USB output, see usb_out.h.
//
// Why not Serial.print: the arduino-esp32 2.0.17 HWCDC driver decides the
// port is "disconnected" on a short TX stall or an SOF glitch, re-arms its TX
// interrupt only once per such episode, and refuses to send while its
// unplugged-detection flickers. Output then waits in its ring buffer until
// the PC sends something. In UCI the PC is waiting for "bestmove", so the
// two waited for each other (cutechess: "connection stalls"); its flush()
// also throws pending output away on a timeout.
//
// Here a small task on the UI core moves queued bytes straight into the
// 64-byte hardware FIFO whenever it has room - no interrupts, no connection
// state. The HWCDC interrupt handler still runs for input; finding its own
// TX ring empty, it just switches its TX interrupt off.
#include "usb_out.h"
#include <Arduino.h>
#include <freertos/stream_buffer.h>
#include <hal/usb_serial_jtag_ll.h>
#include <stdarg.h>
#include <string.h>

static StreamBufferHandle_t queue_;
static SemaphoreHandle_t lock_;  // several tasks may print

static void writer(void*) {
  uint8_t chunk[64];
  for (;;) {
    size_t n = xStreamBufferReceive(queue_, chunk, sizeof chunk, portMAX_DELAY);
    size_t done = 0;
    while (done < n) {
      if (!usb_serial_jtag_ll_txfifo_writable()) {
        vTaskDelay(1);  // the PC hasn't collected the last packet yet
        continue;
      }
      done += usb_serial_jtag_ll_write_txfifo(chunk + done, uint32_t(n - done));
      usb_serial_jtag_ll_txfifo_flush();
    }
  }
}

void usb_out_begin() {
  queue_ = xStreamBufferCreate(16384, 1);
  lock_ = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(writer, "usb_out", 3072, nullptr, 2, nullptr, 1);
}

void usb_write(const void* data, size_t n) {
  if (!queue_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (n) {
    size_t k = xStreamBufferSend(queue_, p, n, pdMS_TO_TICKS(50));
    if (!k) break;  // PC not reading: drop the rest
    p += k;
    n -= k;
  }
  xSemaphoreGive(lock_);
}

void usb_print(const char* s) { usb_write(s, strlen(s)); }

void usb_println(const char* s) {
  usb_print(s);
  usb_write("\r\n", 2);
}

void usb_printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) usb_write(buf, size_t(n) < sizeof buf ? size_t(n) : sizeof buf - 1);
}
