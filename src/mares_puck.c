/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include <libdivecomputer/mares_puck.h>

#include "context-private.h"
#include "device-private.h"
#include "mares_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_puck_device_vtable)

#define NEMOWIDE    1
#define NEMOAIR     4
#define PUCK        7
#define PUCKAIR     19

typedef struct mares_puck_device_t {
	mares_common_device_t base;
	const mares_common_layout_t *layout;
	unsigned char fingerprint[5];
} mares_puck_device_t;

static dc_status_t mares_puck_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_puck_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_puck_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mares_puck_device_close (dc_device_t *abstract);

static const dc_device_vtable_t mares_puck_device_vtable = {
	DC_FAMILY_MARES_PUCK,
	mares_puck_device_set_fingerprint, /* set_fingerprint */
	mares_common_device_read, /* read */
	NULL, /* write */
	mares_puck_device_dump, /* dump */
	mares_puck_device_foreach, /* foreach */
	mares_puck_device_close /* close */
};

static const mares_common_layout_t mares_puck_layout = {
	0x4000, /* memsize */
	0x0070, /* rb_profile_begin */
	0x4000, /* rb_profile_end */
	0x4000, /* rb_freedives_begin */
	0x4000  /* rb_freedives_end */
};

static const mares_common_layout_t mares_nemoair_layout = {
	0x8000, /* memsize */
	0x0070, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0x8000, /* rb_freedives_begin */
	0x8000  /* rb_freedives_end */
};

static const mares_common_layout_t mares_nemowide_layout = {
	0x4000, /* memsize */
	0x0070, /* rb_profile_begin */
	0x3400, /* rb_profile_end */
	0x3400, /* rb_freedives_begin */
	0x4000  /* rb_freedives_end */
};


dc_status_t
mares_puck_device_open (dc_device_t **out, dc_context_t *context, const void *params)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	mares_puck_device_t *device = (mares_puck_device_t *) malloc (sizeof (mares_puck_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	mares_common_device_init (&device->base, context, &mares_puck_device_vtable);

	// Set the default values.
	device->layout = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->base.port, context, params);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->base.port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->base.port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->base.port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->base.port);
		free (device);
		return DC_STATUS_IO;
	}

	// Clear the DTR/RTS lines.
	if (serial_set_dtr (device->base.port, 0) == -1 ||
		serial_set_rts (device->base.port, 0) == -1) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		serial_close (device->base.port);
		free (device);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->base.port, SERIAL_QUEUE_BOTH);

	// Identify the model number.
	unsigned char header[PACKETSIZE] = {0};
	dc_status_t status = mares_common_device_read ((dc_device_t *) device, 0, header, sizeof (header));
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->base.port);
		free (device);
		return status;
	}

	// Override the base class values.
	switch (header[1]) {
	case NEMOWIDE:
		device->layout = &mares_nemowide_layout;
		break;
	case NEMOAIR:
	case PUCKAIR:
		device->layout = &mares_nemoair_layout;
		break;
	case PUCK:
		device->layout = &mares_puck_layout;
		break;
	default: // Unknown, try puck
		device->layout = &mares_puck_layout;
		break;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_puck_device_close (dc_device_t *abstract)
{
	mares_puck_device_t *device = (mares_puck_device_t*) abstract;

	// Close the device.
	if (serial_close (device->base.port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_puck_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_puck_device_t *device = (mares_puck_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_puck_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	mares_puck_device_t *device = (mares_puck_device_t *) abstract;

	assert (device->layout != NULL);

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), PACKETSIZE);
}


static dc_status_t
mares_puck_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	mares_puck_device_t *device = (mares_puck_device_t *) abstract;

	assert (device->layout != NULL);

	dc_buffer_t *buffer = dc_buffer_new (device->layout->memsize);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = mares_puck_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = data[1];
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (data + 8);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = mares_common_extract_dives (abstract->context, device->layout, device->fingerprint, data, callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
mares_puck_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	mares_puck_device_t *device = (mares_puck_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PACKETSIZE)
		return DC_STATUS_DATAFORMAT;

	dc_context_t *context = (abstract ? abstract->context : NULL);
	unsigned char *fingerprint = (device ? device->fingerprint : NULL);

	const mares_common_layout_t *layout = NULL;
	switch (data[1]) {
	case NEMOWIDE:
		layout = &mares_nemowide_layout;
		break;
	case NEMOAIR:
	case PUCKAIR:
		layout = &mares_nemoair_layout;
		break;
	case PUCK:
		layout = &mares_puck_layout;
		break;
	default: // Unknown, try puck
		layout = &mares_puck_layout;
		break;
	}

	if (size < layout->memsize)
		return DC_STATUS_DATAFORMAT;

	return mares_common_extract_dives (context, layout, fingerprint, data, callback, userdata);
}
