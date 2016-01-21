/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
 * Copyright (C) 2014 Anton Lundin
 * Copyright (C) 2015 Claudiu Olteanu
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stdio.h>  // FILE, fopen

#include <libdivecomputer/hw_ostc3.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "array.h"
#include "aes.h"

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#define ISINSTANCE(device) dc_device_isinstance((device), &hw_ostc3_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define SZ_DISPLAY    16
#define SZ_CUSTOMTEXT 60
#define SZ_VERSION    (SZ_CUSTOMTEXT + 4)
#define SZ_HARDWARE   1
#define SZ_MEMORY     0x400000
#define SZ_CONFIG     4
#define SZ_FIRMWARE   0x01E000        // 120KB
#define SZ_FIRMWARE_BLOCK    0x1000   //   4KB
#define FIRMWARE_AREA      0x3E0000

#define RB_LOGBOOK_SIZE_COMPACT  16
#define RB_LOGBOOK_SIZE_FULL     256
#define RB_LOGBOOK_COUNT 256

#define S_BLOCK_READ 0x20
#define S_BLOCK_WRITE 0x30
#define S_ERASE    0x42
#define S_READY    0x4C
#define READY      0x4D
#define S_UPGRADE  0x50
#define HEADER     0x61
#define CLOCK      0x62
#define CUSTOMTEXT 0x63
#define DIVE       0x66
#define IDENTITY   0x69
#define HARDWARE   0x6A
#define DISPLAY    0x6E
#define COMPACT    0x6D
#define READ       0x72
#define WRITE      0x77
#define RESET      0x78
#define INIT       0xBB
#define EXIT       0xFF

#define OSTC3      0x0A
#define SPORT      0x12
#define CR         0x05

typedef enum hw_ostc3_state_t {
	OPEN,
	DOWNLOAD,
	SERVICE,
	REBOOTING,
} hw_ostc3_state_t;

typedef struct hw_ostc3_device_t {
	dc_device_t base;
	dc_serial_t *serial;
	unsigned char fingerprint[5];
	hw_ostc3_state_t state;
} hw_ostc3_device_t;

typedef struct hw_ostc3_logbook_t {
	unsigned int size;
	unsigned int profile;
	unsigned int fingerprint;
	unsigned int number;
} hw_ostc3_logbook_t;

typedef struct hw_ostc3_firmware_t {
	unsigned char data[SZ_FIRMWARE];
	unsigned int checksum;
} hw_ostc3_firmware_t;

// This key is used both for the Ostc3 and its cousin,
// the Ostc Sport.
// The Frog uses a similar protocol, and with another key.
static const unsigned char ostc3_key[16] = {
	0xF1, 0xE9, 0xB0, 0x30,
	0x45, 0x6F, 0xBE, 0x55,
	0xFF, 0xE7, 0xF8, 0x31,
	0x13, 0x6C, 0xF2, 0xFE
};

static dc_status_t hw_ostc3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t hw_ostc3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t hw_ostc3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t hw_ostc3_device_close (dc_device_t *abstract);

static const dc_device_vtable_t hw_ostc3_device_vtable = {
	sizeof(hw_ostc3_device_t),
	DC_FAMILY_HW_OSTC3,
	hw_ostc3_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	hw_ostc3_device_dump, /* dump */
	hw_ostc3_device_foreach, /* foreach */
	hw_ostc3_device_close /* close */
};

static const hw_ostc3_logbook_t hw_ostc3_logbook_compact = {
	RB_LOGBOOK_SIZE_COMPACT, /* size */
	0,  /* profile */
	3,  /* fingerprint */
	13, /* number */
};

static const hw_ostc3_logbook_t hw_ostc3_logbook_full = {
	RB_LOGBOOK_SIZE_FULL, /* size */
	9,  /* profile */
	12, /* fingerprint */
	80, /* number */
};


static int
hw_ostc3_strncpy (unsigned char *data, unsigned int size, const char *text)
{
	// Check the maximum length.
	size_t length = (text ? strlen (text) : 0);
	if (length > size) {
		return -1;
	}

	// Copy the text.
	if (length)
		memcpy (data, text, length);

	// Pad with spaces.
	memset (data + length, 0x20, size - length);

	return 0;
}


static dc_status_t
hw_ostc3_transfer (hw_ostc3_device_t *device,
                  dc_event_progress_t *progress,
                  unsigned char cmd,
                  const unsigned char input[],
                  unsigned int isize,
                  unsigned char output[],
                  unsigned int osize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Get the correct ready byte for the current state.
	const unsigned char ready = (device->state == SERVICE ? S_READY : READY);

	// Send the command.
	unsigned char command[1] = {cmd};
	int n = device->serial->ops->write (device->serial->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Read the echo.
	unsigned char echo[1] = {0};
	n = device->serial->ops->read (device->serial->port, echo, sizeof (echo));
	if (n != sizeof (echo)) {
		ERROR (abstract->context, "Failed to receive the echo.");
		return EXITCODE (n);
	}

	// Verify the echo.
	if (memcmp (echo, command, sizeof (command)) != 0) {
		if (echo[0] == ready) {
			ERROR (abstract->context, "Unsupported command.");
			return DC_STATUS_UNSUPPORTED;
		} else {
			ERROR (abstract->context, "Unexpected echo.");
			return DC_STATUS_PROTOCOL;
		}
	}

	if (input) {
		// Send the input data packet.
		n = device->serial->ops->write (device->serial->port, input, isize);
		if (n != isize) {
			ERROR (abstract->context, "Failed to send the data packet.");
			return EXITCODE (n);
		}
	}

	if (output) {
		unsigned int nbytes = 0;
		while (nbytes < osize) {
			// Set the minimum packet size.
			unsigned int len = 1024;

			// Increase the packet size if more data is immediately available.
			int available = device->serial->ops->get_received (device->serial->port);
			if (available > len)
				len = available;

			// Limit the packet size to the total size.
			if (nbytes + len > osize)
				len = osize - nbytes;

			// Read the packet.
			n = device->serial->ops->read (device->serial->port, output + nbytes, len);
			if (n != len) {
				ERROR (abstract->context, "Failed to receive the answer.");
				return EXITCODE (n);
			}

			// Update and emit a progress event.
			if (progress) {
				progress->current += len;
				device_event_emit ((dc_device_t *) device, DC_EVENT_PROGRESS, progress);
			}

			nbytes += len;
		}
	}

	if (cmd != EXIT) {
		// Read the ready byte.
		unsigned char answer[1] = {0};
		n = device->serial->ops->read (device->serial->port, answer, sizeof (answer));
		if (n != sizeof (answer)) {
			ERROR (abstract->context, "Failed to receive the ready byte.");
			return EXITCODE (n);
		}

		// Verify the ready byte.
		if (answer[0] != ready) {
			ERROR (abstract->context, "Unexpected ready byte.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (hw_ostc3_device_t *) dc_device_allocate (context, &hw_ostc3_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->serial = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = dc_serial_native_open (&device->serial, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->serial->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		status = DC_STATUS_IO;
		goto error_close;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->serial->port, 3000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		status = DC_STATUS_IO;
		goto error_close;
	}

	// Make sure everything is in a sane state.
	serial_sleep (device->serial->port, 300);
	device->serial->ops->flush (device->serial->port, SERIAL_QUEUE_BOTH);

	device->state = OPEN;

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	device->serial->ops->close (device->serial->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
hw_ostc3_device_init_download (hw_ostc3_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Send the init command.
	dc_status_t status = hw_ostc3_transfer (device, NULL, INIT, NULL, 0, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the command.");
		return status;
	}

	device->state = DOWNLOAD;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_init_service (hw_ostc3_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	unsigned char command[] = {0xAA, 0xAB, 0xCD, 0xEF};
	unsigned char output[5];
	int n = 0;

	// We cant use hw_ostc3_transfer here, due to the different echos
	n = device->serial->ops->write (device->serial->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Give the device some time to enter service mode
	serial_sleep (device->serial->port, 100);

	// Read the response
	n = device->serial->ops->read (device->serial->port, output, sizeof (output));
	if (n != sizeof (output)) {
		ERROR (context, "Failed to receive the echo.");
		return EXITCODE (n);
	}

	// Verify the response to service mode
	if (output[0] != 0x4B || output[1] != 0xAB ||
			output[2] != 0xCD || output[3] != 0xEF ||
			output[4] != S_READY) {
		ERROR (context, "Failed to verify echo.");
		return DC_STATUS_PROTOCOL;
	}

	device->state = SERVICE;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_init (hw_ostc3_device_t *device, hw_ostc3_state_t state)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	if (device->state == state) {
		// No change.
		rc = DC_STATUS_SUCCESS;
	} else if (device->state == OPEN) {
		// Change to download or service mode.
		if (state == DOWNLOAD) {
			rc = hw_ostc3_device_init_download(device);
		} else if (state == SERVICE) {
			rc = hw_ostc3_device_init_service(device);
		} else {
			rc = DC_STATUS_INVALIDARGS;
		}
	} else if (device->state == SERVICE && state == DOWNLOAD) {
		// Switching between service and download mode is not possible.
		// But in service mode, all download commands are supported too,
		// so there is no need to change the state.
		rc = DC_STATUS_SUCCESS;
	} else {
		// Not supported.
		rc = DC_STATUS_INVALIDARGS;
	}

	return rc;
}


static dc_status_t
hw_ostc3_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the exit command
	if (device->state == DOWNLOAD || device->state == SERVICE) {
		rc = hw_ostc3_transfer (device, NULL, EXIT, NULL, 0, NULL, 0);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			dc_status_set_error(&status, rc);
		}
	}

	// Close the device.
	if (device->serial->ops->close (device->serial->port) == -1) {
		dc_status_set_error(&status, DC_STATUS_IO);
	}

	return status;
}


static dc_status_t
hw_ostc3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size != SZ_VERSION)
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, IDENTITY, NULL, 0, data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_hardware (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size != SZ_HARDWARE)
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, HARDWARE, NULL, 0, data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Download the version data.
	unsigned char id[SZ_VERSION] = {0};
	rc = hw_ostc3_device_version (abstract, id, sizeof (id));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the version.");
		return rc;
	}

	// Download the hardware descriptor.
	unsigned char hardware[SZ_HARDWARE] = {0};
	rc = hw_ostc3_device_hardware (abstract, hardware, sizeof (hardware));
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR (abstract->context, "Failed to read the hardware descriptor.");
		return rc;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.firmware = array_uint16_be (id + 2);
	devinfo.serial = array_uint16_le (id + 0);
	devinfo.model = hardware[0];
	if (devinfo.model == 0) {
		// Fallback to the serial number.
		if (devinfo.serial > 10000)
			devinfo.model = SPORT;
		else
			devinfo.model = OSTC3;
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate memory.
	unsigned char *header = (unsigned char *) malloc (RB_LOGBOOK_SIZE_FULL * RB_LOGBOOK_COUNT);
	if (header == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Download the compact logbook headers. If the firmware doesn't support
	// compact headers yet, fallback to downloading the full logbook headers.
	// This is slower, but also works for older firmware versions.
	unsigned int compact = 1;
	rc = hw_ostc3_transfer (device, &progress, COMPACT,
              NULL, 0, header, RB_LOGBOOK_SIZE_COMPACT * RB_LOGBOOK_COUNT);
	if (rc == DC_STATUS_UNSUPPORTED) {
		compact = 0;
		rc = hw_ostc3_transfer (device, &progress, HEADER,
		          NULL, 0, header, RB_LOGBOOK_SIZE_FULL * RB_LOGBOOK_COUNT);
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the header.");
		free (header);
		return rc;
	}

	// Get the correct logbook layout.
	const hw_ostc3_logbook_t *logbook = NULL;
	if (compact) {
		logbook = &hw_ostc3_logbook_compact;
	} else {
		logbook = &hw_ostc3_logbook_full;
	}

	// Locate the most recent dive.
	// The device maintains an internal counter which is incremented for every
	// dive, and the current value at the time of the dive is stored in the
	// dive header. Thus the most recent dive will have the highest value.
	unsigned int count = 0;
	unsigned int latest = 0;
	unsigned int maximum = 0;
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		unsigned int offset = i * logbook->size;

		// Ignore uninitialized header entries.
		if (array_isequal (header + offset, logbook->size, 0xFF))
			continue;

		// Get the internal dive number.
		unsigned int current = array_uint16_le (header + offset + logbook->number);
		if (current > maximum) {
			maximum = current;
			latest = i;
		}

		count++;
	}

	// Calculate the total and maximum size.
	unsigned int ndives = 0;
	unsigned int size = 0;
	unsigned int maxsize = 0;
	for (unsigned int i = 0; i < count; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = idx * logbook->size;

		// Uninitialized header entries should no longer be present at this
		// stage, unless the dives are interleaved with empty entries. But
		// that's something we don't support at all.
		if (array_isequal (header + offset, logbook->size, 0xFF)) {
			WARNING (abstract->context, "Unexpected empty header found.");
			break;
		}

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE_FULL + array_uint24_le (header + offset + logbook->profile) - 3;
		if (!compact) {
			// Workaround for a bug in older firmware versions.
			unsigned int firmware = array_uint16_be (header + offset + 0x30);
			if (firmware < 93)
				length -= 3;
		}

		// Check the fingerprint data.
		if (memcmp (header + offset + logbook->fingerprint, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		if (length > maxsize)
			maxsize = length;
		size += length;
		ndives++;
	}

	// Update and emit a progress event.
	progress.maximum = (logbook->size * RB_LOGBOOK_COUNT) + size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Finish immediately if there are no dives available.
	if (ndives == 0) {
		free (header);
		return DC_STATUS_SUCCESS;
	}

	// Allocate enough memory for the largest dive.
	unsigned char *profile = (unsigned char *) malloc (maxsize);
	if (profile == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		free (header);
		return DC_STATUS_NOMEMORY;
	}

	// Download the dives.
	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = idx * logbook->size;

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE_FULL + array_uint24_le (header + offset + logbook->profile) - 3;
		if (!compact) {
			// Workaround for a bug in older firmware versions.
			unsigned int firmware = array_uint16_be (header + offset + 0x30);
			if (firmware < 93)
				length -= 3;
		}

		// Download the dive.
		unsigned char number[1] = {idx};
		rc = hw_ostc3_transfer (device, &progress, DIVE,
			number, sizeof (number), profile, length);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			free (profile);
			free (header);
			return rc;
		}

		// Verify the header in the logbook and profile are identical.
		if (!compact && memcmp (profile, header + offset, logbook->size) != 0) {
			ERROR (abstract->context, "Unexpected profile header.");
			free (profile);
			free (header);
			return rc;
		}

		if (callback && !callback (profile, length, profile + 12, sizeof (device->fingerprint), userdata))
			break;
	}

	free (profile);
	free (header);

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_clock (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (datetime == NULL) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	unsigned char packet[6] = {
		datetime->hour, datetime->minute, datetime->second,
		datetime->month, datetime->day, datetime->year - 2000};
	rc = hw_ostc3_transfer (device, NULL, CLOCK, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_display (dc_device_t *abstract, const char *text)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_DISPLAY] = {0};
	if (hw_ostc3_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, DISPLAY, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_customtext (dc_device_t *abstract, const char *text)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_CUSTOMTEXT] = {0};
	if (hw_ostc3_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, CUSTOMTEXT, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_read (dc_device_t *abstract, unsigned int config, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size > SZ_CONFIG) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	unsigned char command[1] = {config};
	rc = hw_ostc3_transfer (device, NULL, READ, command, sizeof (command), data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_write (dc_device_t *abstract, unsigned int config, const unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size > SZ_CONFIG) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	unsigned char command[SZ_CONFIG + 1] = {config};
	memcpy(command + 1, data, size);
	rc = hw_ostc3_transfer (device, NULL, WRITE, command, size + 1, NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_reset (dc_device_t *abstract)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, RESET, NULL, 0, NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

// This is a variant of fletcher16 with a 16 bit sum instead of an 8 bit sum,
// and modulo 2^16 instead of 2^16-1
static unsigned int
hw_ostc3_firmware_checksum (hw_ostc3_firmware_t *firmware)
{
	unsigned short low = 0;
	unsigned short high = 0;
	for (unsigned int i = 0; i < SZ_FIRMWARE; i++) {
		low  += firmware->data[i];
		high += low;
	}
	return (((unsigned int)high) << 16) + low;
}

static dc_status_t
hw_ostc3_firmware_readline (FILE *fp, dc_context_t *context, unsigned int addr, unsigned char data[], unsigned int size)
{
	unsigned char ascii[39];
	unsigned char faddr_byte[3];
	unsigned int faddr = 0;
	int n = 0;

	if (size > 16) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Read the start code.
	while (1) {
		n = fread (ascii, 1, 1, fp);
		if (n != 1) {
			ERROR (context, "Failed to read the start code.");
			return DC_STATUS_IO;
		}

		if (ascii[0] == ':')
			break;

		// Ignore CR and LF characters.
		if (ascii[0] != '\n' && ascii[0] != '\r') {
			ERROR (context, "Unexpected character (0x%02x).", ascii[0]);
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Read the payload.
	n = fread (ascii + 1, 1, 6 + size * 2, fp);
	if (n != 6 + size * 2) {
		ERROR (context, "Failed to read the data.");
		return DC_STATUS_IO;
	}

	// Convert the address to binary representation.
	if (array_convert_hex2bin(ascii + 1, 6, faddr_byte, sizeof(faddr_byte)) != 0) {
		ERROR (context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the address.
	faddr = array_uint24_be (faddr_byte);
	if (faddr != addr) {
		ERROR (context, "Unexpected address (0x%06x, 0x%06x).", faddr, addr);
		return DC_STATUS_DATAFORMAT;
	}

	// Convert the payload to binary representation.
	if (array_convert_hex2bin (ascii + 1 + 6, size * 2, data, size) != 0) {
		ERROR (context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_firmware_readfile (hw_ostc3_firmware_t *firmware, dc_context_t *context, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	FILE *fp = NULL;
	unsigned char iv[16] = {0};
	unsigned char tmpbuf[16] = {0};
	unsigned char encrypted[16] = {0};
	unsigned int bytes = 0, addr = 0;
	unsigned char checksum[4];

	if (firmware == NULL) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Initialize the buffers.
	memset (firmware->data, 0xFF, sizeof (firmware->data));
	firmware->checksum = 0;

	fp = fopen (filename, "rb");
	if (fp == NULL) {
		ERROR (context, "Failed to open the file.");
		return DC_STATUS_IO;
	}

	rc = hw_ostc3_firmware_readline (fp, context, 0, iv, sizeof(iv));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to parse header.");
		fclose (fp);
		return rc;
	}
	bytes += 16;

	// Load the iv for AES-FCB-mode
	AES128_ECB_encrypt (iv, ostc3_key, tmpbuf);

	for (addr = 0; addr < SZ_FIRMWARE; addr += 16, bytes += 16) {
		rc = hw_ostc3_firmware_readline (fp, context, bytes, encrypted, sizeof(encrypted));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to parse file data.");
			fclose (fp);
			return rc;
		}

		// Decrypt AES-FCB data
		for (unsigned int i = 0; i < 16; i++)
			firmware->data[addr + i] = encrypted[i] ^ tmpbuf[i];

		// Run the next round of encryption
		AES128_ECB_encrypt (encrypted, ostc3_key, tmpbuf);
	}

	// This file format contains a tail with the checksum in
	rc = hw_ostc3_firmware_readline (fp, context, bytes, checksum, sizeof(checksum));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to parse file tail.");
		fclose (fp);
		return rc;
	}

	fclose (fp);

	firmware->checksum = array_uint32_le (checksum);

	if (firmware->checksum != hw_ostc3_firmware_checksum (firmware)) {
		ERROR (context, "Failed to verify file checksum.");
		return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_firmware_erase (hw_ostc3_device_t *device, unsigned int addr, unsigned int size)
{
	// Convert size to number of pages, rounded up.
	unsigned char blocks = ((size + SZ_FIRMWARE_BLOCK - 1) / SZ_FIRMWARE_BLOCK);

	// Erase just the needed pages.
	unsigned char buffer[4];
	array_uint24_be_set (buffer, addr);
	buffer[3] = blocks;

	return hw_ostc3_transfer (device, NULL, S_ERASE, buffer, sizeof (buffer), NULL, 0);
}

static dc_status_t
hw_ostc3_firmware_block_read (hw_ostc3_device_t *device, unsigned int addr, unsigned char block[], unsigned int block_size)
{
	unsigned char buffer[6];
	array_uint24_be_set (buffer, addr);
	array_uint24_be_set (buffer + 3, block_size);

	return hw_ostc3_transfer (device, NULL, S_BLOCK_READ, buffer, sizeof (buffer), block, block_size);
}

static dc_status_t
hw_ostc3_firmware_block_write (hw_ostc3_device_t *device, unsigned int addr, unsigned char block[], unsigned int block_size)
{
	unsigned char buffer[3 + SZ_FIRMWARE_BLOCK];

	// We currenty only support writing max SZ_FIRMWARE_BLOCK sized blocks.
	if (block_size > SZ_FIRMWARE_BLOCK)
		return DC_STATUS_INVALIDARGS;

	array_uint24_be_set (buffer, addr);
	memcpy (buffer + 3, block, block_size);

	return hw_ostc3_transfer (device, NULL, S_BLOCK_WRITE, buffer, 3 + block_size, NULL, 0);
}

static dc_status_t
hw_ostc3_firmware_upgrade (dc_device_t *abstract, unsigned int checksum)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);
	unsigned char buffer[5];
	array_uint32_le_set (buffer, checksum);

	// Compute a one byte checksum, so the device can validate the firmware image.
	buffer[4] = 0x55;
	for (unsigned int i = 0; i < 4; i++) {
		buffer[4] ^= buffer[i];
		buffer[4]  = (buffer[4]<<1 | buffer[4]>>7);
	}

	rc = hw_ostc3_transfer (device, NULL, S_UPGRADE, buffer, sizeof (buffer), NULL, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send flash firmware command");
		return rc;
	}

	// Now the device resets, and if everything is well, it reprograms.
	device->state = REBOOTING;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_fwupdate (dc_device_t *abstract, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// load, erase, upload FZ, verify FZ, reprogram
	progress.maximum = 3 + SZ_FIRMWARE * 2 / SZ_FIRMWARE_BLOCK;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the firmware data.
	hw_ostc3_firmware_t *firmware = (hw_ostc3_firmware_t *) malloc (sizeof (hw_ostc3_firmware_t));
	if (firmware == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Read the hex file.
	rc = hw_ostc3_firmware_readfile (firmware, context, filename);
	if (rc != DC_STATUS_SUCCESS) {
		free (firmware);
		return rc;
	}

	// Make sure the device is in service mode
	rc = hw_ostc3_device_init (device, SERVICE);
	if (rc != DC_STATUS_SUCCESS) {
		free (firmware);
		return rc;
	}

	// Device open and firmware loaded
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	hw_ostc3_device_display (abstract, " Erasing FW...");

	rc = hw_ostc3_firmware_erase (device, FIRMWARE_AREA, SZ_FIRMWARE);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to erase old firmware");
		free (firmware);
		return rc;
	}

	// Memory erased
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	hw_ostc3_device_display (abstract, " Uploading...");

	for (unsigned int len = 0; len < SZ_FIRMWARE; len += SZ_FIRMWARE_BLOCK) {
		char status[SZ_DISPLAY + 1]; // Status message on the display
		snprintf (status, sizeof(status), " Uploading %2d%%", (100 * len) / SZ_FIRMWARE);
		hw_ostc3_device_display (abstract, status);

		rc = hw_ostc3_firmware_block_write (device, FIRMWARE_AREA + len, firmware->data + len, SZ_FIRMWARE_BLOCK);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to write block to device");
			free(firmware);
			return rc;
		}
		// One block uploaded
		progress.current++;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	hw_ostc3_device_display (abstract, " Verifying...");

	for (unsigned int len = 0; len < SZ_FIRMWARE; len += SZ_FIRMWARE_BLOCK) {
		unsigned char block[SZ_FIRMWARE_BLOCK];
		char status[SZ_DISPLAY + 1]; // Status message on the display
		snprintf (status, sizeof(status), " Verifying %2d%%", (100 * len) / SZ_FIRMWARE);
		hw_ostc3_device_display (abstract, status);

		rc = hw_ostc3_firmware_block_read (device, FIRMWARE_AREA + len, block, sizeof (block));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to read block.");
			free (firmware);
			return rc;
		}
		if (memcmp (firmware->data + len, block, sizeof (block)) != 0) {
			ERROR (context, "Failed verify.");
			hw_ostc3_device_display (abstract, " Verify FAILED");
			free (firmware);
			return DC_STATUS_PROTOCOL;
		}
		// One block verified
		progress.current++;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	hw_ostc3_device_display (abstract, " Programming...");

	rc = hw_ostc3_firmware_upgrade (abstract, firmware->checksum);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to start programing");
		free (firmware);
		return rc;
	}

	// Programing done!
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	free (firmware);

	// Finished!
	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Make sure the device is in service mode
	dc_status_t rc = hw_ostc3_device_init (device, SERVICE);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	unsigned int nbytes = 0;
	while (nbytes < SZ_MEMORY) {
		// packet size. Can be almost arbetary size.
		unsigned int len = SZ_FIRMWARE_BLOCK;

		// Read a block
		rc = hw_ostc3_firmware_block_read (device, nbytes, data + nbytes, len);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read block.");
			return rc;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}
