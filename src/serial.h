/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/serial_parameters.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <libdivecomputer/custom_serial.h>

typedef void (*serial_callback_t) (const char *name, void *userdata);

int serial_enumerate (serial_callback_t callback, void *userdata);

int serial_open (serial_t **device, dc_context_t *context, const char* name);

int serial_close (serial_t *device);

int serial_configure (serial_t *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol);

//
// Available read modes:
//
// * Blocking (timeout < 0):
//
// The read function is blocked until all the requested bytes have
// been received. If the requested number of bytes does not arrive,
// the function will block forever.
//
// * Non-blocking (timeout == 0):
//
// The read function returns immediately with the bytes that have already
// been received, even if no bytes have been received.
//
// * Timeout (timeout > 0):
//
// The read function is blocked until all the requested bytes have
// been received. If the requested number of bytes does not arrive
// within the specified amount of time, the function will return
// with the bytes that have already been received.
//

int serial_set_timeout (serial_t *device, long timeout /* milliseconds */);

int serial_set_queue_size (serial_t *device, unsigned int input, unsigned int output);

int serial_set_halfduplex (serial_t *device, int value);

int serial_set_latency (serial_t *device, unsigned int milliseconds);

int serial_read (serial_t *device, void* data, unsigned int size);
int serial_write (serial_t *device, const void* data, unsigned int size);

int serial_flush (serial_t *device, int queue);

int serial_send_break (serial_t *device);

int serial_set_break (serial_t *device, int level);
int serial_set_dtr (serial_t *device, int level);
int serial_set_rts (serial_t *device, int level);

int serial_get_received (serial_t *device);
int serial_get_transmitted (serial_t *device);

int serial_get_line (serial_t *device, int line);

int serial_sleep (serial_t *device, unsigned long timeout /* milliseconds */);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SERIAL_H */
