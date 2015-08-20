/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 * Copyright (C) 2015 Anton Lundin
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

#ifndef SERIAL_PARAMETERS_H
#define SERIAL_PARAMETERS_H

typedef enum serial_parity_t {
	SERIAL_PARITY_NONE = 0,
	SERIAL_PARITY_EVEN = 1,
	SERIAL_PARITY_ODD = 2
} serial_parity_t;

typedef enum serial_flowcontrol_t {
	SERIAL_FLOWCONTROL_NONE = 0,
	SERIAL_FLOWCONTROL_HARDWARE = 1,
	SERIAL_FLOWCONTROL_SOFTWARE = 2
} serial_flowcontrol_t;

typedef enum serial_queue_t {
	SERIAL_QUEUE_INPUT = 0x01,
	SERIAL_QUEUE_OUTPUT = 0x02,
	SERIAL_QUEUE_BOTH = SERIAL_QUEUE_INPUT | SERIAL_QUEUE_OUTPUT
} serial_queue_t;

typedef enum serial_line_t {
	SERIAL_LINE_DCD = 0, // Data carrier detect
	SERIAL_LINE_CTS = 1, // Clear to send
	SERIAL_LINE_DSR = 2, // Data set ready
	SERIAL_LINE_RNG = 3, // Ring indicator
} serial_line_t;

#endif
