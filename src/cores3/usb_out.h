// Chess System Tal Retro - output to the PC over the ESP32-S3's built-in USB
// Serial/JTAG port, bypassing the Arduino HWCDC driver's transmit path (see
// usb_out.cpp for why). Input still arrives through Serial as usual.
#pragma once
#include <stddef.h>
#include <stdint.h>

void usb_out_begin();
// Queue bytes for the PC. Waits briefly if the queue is full (PC not
// reading), then drops the rest, so a board with no PC attached never hangs.
void usb_write(const void* data, size_t n);
void usb_print(const char* s);
void usb_println(const char* s);
void usb_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Since boot: bytes dropped because the PC wasn't reading, and the longest
// wait for the PC to collect a packet (serial command "info" shows them).
void usb_out_stats(uint32_t* dropped, uint32_t* longest_wait_ms);
