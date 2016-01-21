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

#include <stdlib.h>
#include <string.h>	// memcmp, strdup
#include <stdio.h>	// snprintf

#include <libdivecomputer/suunto_d9.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &suunto_d9_parser_vtable)

#define MAXPARAMS 3
#define NGASMIXES 11

#define D9       0x0E
#define D6       0x0F
#define VYPER2   0x10
#define COBRA2   0x11
#define D4       0x12
#define VYPERAIR 0x13
#define COBRA3   0x14
#define HELO2    0x15
#define D4i      0x19
#define D6i      0x1A
#define D9tx     0x1B
#define DX       0x1C

#define AIR      0
#define NITROX   1
#define GAUGE    2
#define FREEDIVE 3
#define MIXED    4
#define CCR      5

#define SAFETYSTOP   (1 << 0)
#define DECOSTOP     (1 << 1)
#define DEEPSTOP     (1 << 2)

typedef struct suunto_d9_parser_t suunto_d9_parser_t;

struct suunto_d9_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int serial;
	// Cached fields.
	unsigned int cached;
	unsigned int mode;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int gasmix;
	unsigned int config;
};

typedef struct sample_info_t {
	unsigned int type;
	unsigned int size;
	unsigned int interval;
	unsigned int divisor;
} sample_info_t;

static dc_status_t suunto_d9_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t suunto_d9_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t suunto_d9_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t suunto_d9_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t suunto_d9_parser_vtable = {
	sizeof(suunto_d9_parser_t),
	DC_FAMILY_SUUNTO_D9,
	suunto_d9_parser_set_data, /* set_data */
	suunto_d9_parser_get_datetime, /* datetime */
	suunto_d9_parser_get_field, /* fields */
	suunto_d9_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static unsigned int
suunto_d9_parser_find_gasmix (suunto_d9_parser_t *parser, unsigned int o2, unsigned int he)
{
	// Find the gasmix in the list.
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->oxygen[i] && he == parser->helium[i])
			break;
		i++;
	}

	return i;
}

static dc_status_t
suunto_d9_parser_cache (suunto_d9_parser_t *parser)
{
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	// Gasmix information.
	unsigned int gasmode_offset = 0x19;
	unsigned int gasmix_offset = 0x21;
	unsigned int gasmix_count = 3;
	if (parser->model == HELO2) {
		gasmode_offset = 0x1F;
		gasmix_offset = 0x54;
		gasmix_count = 8;
	} else if (parser->model == D4i) {
		gasmode_offset = 0x1D;
		gasmix_offset = 0x5F;
		gasmix_count = 1;
	} else if (parser->model == D6i) {
		gasmode_offset = 0x1D;
		gasmix_offset = 0x5F;
		if (data[1] == 0x63)
			gasmix_count = 3;
		else
			gasmix_count = 2;
	} else if (parser->model == D9tx) {
		gasmode_offset = 0x1D;
		gasmix_offset = 0x87;
		gasmix_count = 8;
	} else if (parser->model == DX) {
		gasmode_offset = 0x21;
		gasmix_offset = 0xC1;
		gasmix_count = 11;
	}

	// Offset to the configuration data.
	unsigned int config = 0x3A;
	if (parser->model == D4) {
		config += 1;
	} else if (parser->model == HELO2 || parser->model == D4i ||
		parser->model == D6i || parser->model == D9tx ||
		parser->model == DX) {
		config = gasmix_offset + gasmix_count * 6;
	}
	if (config + 1 > size)
		return DC_STATUS_DATAFORMAT;

	// Cache the data for later use.
	parser->mode = data[gasmode_offset];
	parser->gasmix = 0;
	if (parser->mode == GAUGE || parser->mode == FREEDIVE) {
		parser->ngasmixes = 0;
	} else if (parser->mode == AIR) {
		parser->oxygen[0] = 21;
		parser->helium[0] = 0;
		parser->ngasmixes = 1;
	} else {
		parser->ngasmixes = 0;
		for (unsigned int i = 0; i < gasmix_count; ++i) {
			if (parser->model == HELO2 || parser->model == D4i ||
				parser->model == D6i || parser->model == D9tx ||
				parser->model == DX) {
				parser->oxygen[i] = data[gasmix_offset + 6 * i + 1];
				parser->helium[i] = data[gasmix_offset + 6 * i + 2];
			} else {
				unsigned int oxygen = data[gasmix_offset + i];
				if (oxygen == 0x00 || oxygen == 0xFF)
					break;
				parser->oxygen[i] = oxygen;
				parser->helium[i] = 0;
			}
			parser->ngasmixes++;
		}

		// Initial gasmix.
		if (parser->model == HELO2) {
			parser->gasmix = data[0x26];
		} else if (parser->model == D4i || parser->model == D6i ||
			parser->model == D9tx) {
			parser->gasmix = data[0x28];
		}
	}
	parser->config = config;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

dc_status_t
suunto_d9_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int serial)
{
	suunto_d9_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (suunto_d9_parser_t *) dc_parser_allocate (context, &suunto_d9_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->serial = serial;
	parser->cached = 0;
	parser->mode = AIR;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->gasmix = 0;
	parser->config = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_d9_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->mode = AIR;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->gasmix = 0;
	parser->config = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_d9_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t*) abstract;

	unsigned int offset = 0x11;
	if (parser->model == HELO2 || parser->model == DX)
		offset = 0x17;
	else if (parser->model == D4i || parser->model == D6i || parser->model == D9tx)
		offset = 0x13;

	if (abstract->size < offset + 7)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + offset;

	if (datetime) {
		if (parser->model == D4i || parser->model == D6i ||
			parser->model == D9tx || parser->model == DX) {
			datetime->year   = p[0] + (p[1] << 8);
			datetime->month  = p[2];
			datetime->day    = p[3];
			datetime->hour   = p[4];
			datetime->minute = p[5];
			datetime->second = p[6];
		} else {
			datetime->hour   = p[0];
			datetime->minute = p[1];
			datetime->second = p[2];
			datetime->year   = p[3] + (p[4] << 8);
			datetime->month  = p[5];
			datetime->day    = p[6];
		}
	}

	return DC_STATUS_SUCCESS;
}

#define BUFLEN 16

static dc_status_t
suunto_d9_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t*) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the gas mix data.
	dc_status_t rc = suunto_d9_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;

	char buf[BUFLEN];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->model == D4)
				*((unsigned int *) value) = array_uint16_le (data + 0x0B);
			else if (parser->model == D4i || parser->model == D6i ||
				parser->model == D9tx || parser->model == DX)
				*((unsigned int *) value) = array_uint16_le (data + 0x0D);
			else if (parser->model == HELO2)
				*((unsigned int *) value) = array_uint16_le (data + 0x0D) * 60;
			else
				*((unsigned int *) value) = array_uint16_le (data + 0x0B) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 0x09) / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->mode) {
			case AIR:
			case NITROX:
			case MIXED:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			case CCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CC;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_STRING:
			switch (flags) {
			case 0: /* serial */
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%08u", parser->serial);
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
			string->value = strdup(buf);
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_d9_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t*) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the gas mix data.
	dc_status_t rc = suunto_d9_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Number of parameters in the configuration data.
	unsigned int nparams = data[parser->config];
	if (nparams == 0 || nparams > MAXPARAMS) {
		ERROR (abstract->context, "Invalid number of parameters.");
		return DC_STATUS_DATAFORMAT;
	}

	// Available divisor values.
	const unsigned int divisors[] = {1, 2, 4, 5, 10, 50, 100, 1000};

	// Get the sample configuration.
	sample_info_t info[MAXPARAMS] = {{0}};
	for (unsigned int i = 0; i < nparams; ++i) {
		unsigned int idx = parser->config + 2 + i * 3;
		info[i].type     = data[idx + 0];
		info[i].interval = data[idx + 1];
		info[i].divisor  = divisors[(data[idx + 2] & 0x1C) >> 2];
		switch (info[i].type) {
		case 0x64: // Depth
		case 0x68: // Pressure
			info[i].size = 2;
			break;
		case 0x74: // Temperature
			info[i].size = 1;
			break;
		default: // Unknown sample type
			ERROR (abstract->context, "Unknown sample type 0x%02x.", info[i].type);
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Offset to the profile data.
	unsigned int profile = parser->config + 2 + nparams * 3;
	if (profile + 5 > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// HelO2 dives can have an additional data block.
	const unsigned char sequence[] = {0x01, 0x00, 0x00};
	if (parser->model == HELO2 && memcmp (data + profile, sequence, sizeof (sequence)) != 0)
		profile += 12;
	if (profile + 5 > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Sample recording interval.
	unsigned int interval_sample_offset = 0x18;
	if (parser->model == HELO2 || parser->model == D4i ||
		parser->model == D6i || parser->model == D9tx)
		interval_sample_offset = 0x1E;
	else if (parser->model == DX)
		interval_sample_offset = 0x22;
	unsigned int interval_sample = data[interval_sample_offset];
	if (interval_sample == 0) {
		ERROR (abstract->context, "Invalid sample interval.");
		return DC_STATUS_DATAFORMAT;
	}

	// Offset to the first marker position.
	unsigned int marker = array_uint16_le (data + profile + 3);

	unsigned int in_deco = 0;
	unsigned int time = 0;
	unsigned int nsamples = 0;
	unsigned int offset = profile + 5;
	while (offset < size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Sample data.
		for (unsigned int i = 0; i < nparams; ++i) {
			if (info[i].interval && (nsamples % info[i].interval) == 0) {
				if (offset + info[i].size > size) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int value = 0;
				switch (info[i].type) {
				case 0x64: // Depth
					value = array_uint16_le (data + offset);
					sample.depth = value / (double) info[i].divisor;
					if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
					break;
				case 0x68: // Pressure
					value = array_uint16_le (data + offset);
					if (value != 0xFFFF) {
						sample.pressure.tank = 0;
						sample.pressure.value = value / (double) info[i].divisor;
						if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
					}
					break;
				case 0x74: // Temperature
					sample.temperature = (signed char) data[offset] / (double) info[i].divisor;
					if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
					break;
				default: // Unknown sample type
					ERROR (abstract->context, "Unknown sample type 0x%02x.", info[i].type);
					return DC_STATUS_DATAFORMAT;
				}

				offset += info[i].size;
			}
		}

		// Initial gasmix.
		if (time == 0 && parser->ngasmixes > 0) {
			if (parser->gasmix >= parser->ngasmixes) {
				ERROR (abstract->context, "Invalid initial gas mix.");
				return DC_STATUS_DATAFORMAT;
			}
			sample.gasmix = parser->gasmix;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
#ifdef ENABLE_DEPRECATED
			unsigned int he = parser->helium[parser->gasmix];
			unsigned int o2 = parser->oxygen[parser->gasmix];
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.value = o2 | (he << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
#endif
		}

		// Events
		if ((nsamples + 1) == marker) {
			while (offset < size) {
				unsigned int event = data[offset++];
				unsigned int seconds, type, unknown, heading;
				unsigned int current, next;
				unsigned int he, o2, idx;
				unsigned int length;

				sample.event.type = SAMPLE_EVENT_NONE;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				switch (event) {
				case 0x01: // Next Event Marker
					if (offset + 4 > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					current = array_uint16_le (data + offset + 0);
					next    = array_uint16_le (data + offset + 2);
					if (marker != current) {
						ERROR (abstract->context, "Unexpected event marker!");
						return DC_STATUS_DATAFORMAT;
					}
					marker += next;
					offset += 4;
					break;
				case 0x02: // Surfaced
					if (offset + 2 > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					unknown = data[offset + 0];
					seconds = data[offset + 1];
					sample.event.type = SAMPLE_EVENT_SURFACE;
					sample.event.time = seconds;
					if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
					offset += 2;
					break;
				case 0x03: // Event
					if (offset + 2 > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					type    = data[offset + 0];
					seconds = data[offset + 1];
					switch (type & 0x7F) {
					case 0x00: // Voluntary Safety Stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_VOLUNTARY;
						if (type & 0x80)
							in_deco &= ~SAFETYSTOP;
						else
							in_deco |= SAFETYSTOP;
						break;
					case 0x01: // Mandatory Safety Stop - odd concept; model as deco stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_MANDATORY;
						if (type & 0x80)
							in_deco &= ~DECOSTOP;
						else
							in_deco |= DECOSTOP;
						break;
					case 0x02: // Deep Safety Stop
						sample.event.type = SAMPLE_EVENT_DEEPSTOP;
						if (type & 0x80)
							in_deco &= ~DEEPSTOP;
						else
							in_deco |= DEEPSTOP;
						break;
					case 0x03: // Deco
						sample.event.type = SAMPLE_EVENT_DECOSTOP;
						if (type & 0x80)
							in_deco &= ~DECOSTOP;
						else
							in_deco |= DECOSTOP;
						break;
					case 0x04: // Ascent Rate Warning
						sample.event.type = SAMPLE_EVENT_ASCENT;
						break;
					case 0x05: // Ceiling Broken
						sample.event.type = SAMPLE_EVENT_CEILING;
						break;
					case 0x06: // Mandatory Safety Stop Ceiling Error
						sample.event.type = SAMPLE_EVENT_CEILING_SAFETYSTOP;
						break;
					case 0x07: // Below Deco Floor
						sample.event.type = SAMPLE_EVENT_FLOOR;
						break;
					case 0x08: // Dive Time
						sample.event.type = SAMPLE_EVENT_DIVETIME;
						break;
					case 0x09: // Depth Alarm
						sample.event.type = SAMPLE_EVENT_MAXDEPTH;
						break;
					case 0x0A: // OLF 80
						sample.event.type = SAMPLE_EVENT_OLF;
						sample.event.value = 80;
						break;
					case 0x0B: // OLF 100
						sample.event.type = SAMPLE_EVENT_OLF;
						sample.event.value = 100;
						break;
					case 0x0C: // PO2
						sample.event.type = SAMPLE_EVENT_PO2;
						break;
					case 0x0D: // Air Time Warning
						sample.event.type = SAMPLE_EVENT_AIRTIME;
						break;
					case 0x0E: // RGBM Warning
						sample.event.type = SAMPLE_EVENT_RGBM;
						break;
					case 0x0F: // PO2 High
					case 0x10: // PO2 Low
						sample.event.type = SAMPLE_EVENT_PO2;
						break;
					case 0x11: // Tissue Level Warning
					case 0x12: // Tissue Calc Overflow
						sample.event.type = SAMPLE_EVENT_TISSUELEVEL;
						break;
					case 0x13: // Deep Safety Stop
						sample.event.type = SAMPLE_EVENT_DEEPSTOP;
						if (type & 0x80)
							in_deco &= ~DEEPSTOP;
						else
							in_deco |= DEEPSTOP;
						break;
					case 0x14: // Mandatory Safety Stop - again, model as deco stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_MANDATORY;
						if (type & 0x80)
							in_deco &= ~DECOSTOP;
						else
							in_deco |= DECOSTOP;
						break;
					default: // Unknown
						WARNING (abstract->context, "Unknown event type 0x%02x.", type);
						break;
					}
					if (type & 0x80)
						sample.event.flags = SAMPLE_FLAGS_END;
					else
						sample.event.flags = SAMPLE_FLAGS_BEGIN;
					sample.event.time = seconds;
					if (sample.event.type != SAMPLE_EVENT_NONE) {
						if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
					}
					offset += 2;
					break;
				case 0x04: // Bookmark/Heading
					if (offset + 4 > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					unknown = data[offset + 0];
					seconds = data[offset + 1];
					heading = array_uint16_le (data + offset + 2);
					if (heading == 0xFFFF) {
						sample.event.type = SAMPLE_EVENT_BOOKMARK;
						sample.event.value = 0;
					} else {
						sample.event.type = SAMPLE_EVENT_HEADING;
						sample.event.value = heading / 2;
					}
					sample.event.time = seconds;
					if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
					offset += 4;
					break;
				case 0x05: // Gas Change
					if (offset + 2 > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					o2 = data[offset + 0];
					seconds = data[offset + 1];
					idx = suunto_d9_parser_find_gasmix(parser, o2, 0);
					if (idx >= parser->ngasmixes) {
						ERROR (abstract->context, "Invalid gas mix.");
						return DC_STATUS_DATAFORMAT;
					}
					sample.gasmix = idx;
					if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
#ifdef ENABLE_DEPRECATED
					sample.event.type = SAMPLE_EVENT_GASCHANGE;
					sample.event.time = seconds;
					sample.event.value = o2;
					if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
#endif
					offset += 2;
					break;
				case 0x06: // Gas Change
					if (parser->model == DX)
						length = 5;
					else
						length = 4;
					if (offset + length > size) {
						ERROR (abstract->context, "Buffer overflow detected!");
						return DC_STATUS_DATAFORMAT;
					}
					unknown = data[offset + 0];
					he = data[offset + 1];
					o2 = data[offset + 2];
					if (parser->model == DX) {
						seconds = data[offset + 4];
					} else {
						seconds = data[offset + 3];
					}
					idx = suunto_d9_parser_find_gasmix(parser, o2, he);
					if (idx >= parser->ngasmixes) {
						ERROR (abstract->context, "Invalid gas mix.");
						return DC_STATUS_DATAFORMAT;
					}
					sample.gasmix = idx;
					if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
#ifdef ENABLE_DEPRECATED
					sample.event.type = SAMPLE_EVENT_GASCHANGE2;
					sample.event.time = seconds;
					sample.event.value = o2 | (he << 16);
					if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
#endif
					offset += length;
					break;
				default:
					WARNING (abstract->context, "Unknown event 0x%02x.", event);
					break;
				}

				if (event == 0x01)
					break;
			}
		}

		if (in_deco & DEEPSTOP) {
			sample.deco.type = DC_DECO_DEEPSTOP;
		} else if (in_deco & DECOSTOP) {
			sample.deco.type = DC_DECO_DECOSTOP;
		} else if (in_deco & SAFETYSTOP) {
			sample.deco.type = DC_DECO_SAFETYSTOP;
		} else {
			sample.deco.type = DC_DECO_NDL;
		}
		sample.deco.time = 0;
		sample.deco.depth = 0.0;
		if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

		time += interval_sample;
		nsamples++;
	}

	return DC_STATUS_SUCCESS;
}
