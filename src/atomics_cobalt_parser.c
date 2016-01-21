/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include <libdivecomputer/atomics_cobalt.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &atomics_cobalt_parser_vtable)

#define SZ_HEADER       228
#define SZ_GASMIX       18
#define SZ_GASSWITCH    6
#define SZ_SEGMENT      16

typedef struct atomics_cobalt_parser_t atomics_cobalt_parser_t;

struct atomics_cobalt_parser_t {
	dc_parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
};

static dc_status_t atomics_cobalt_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t atomics_cobalt_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t atomics_cobalt_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t atomics_cobalt_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t atomics_cobalt_parser_vtable = {
	sizeof(atomics_cobalt_parser_t),
	DC_FAMILY_ATOMICS_COBALT,
	atomics_cobalt_parser_set_data, /* set_data */
	atomics_cobalt_parser_get_datetime, /* datetime */
	atomics_cobalt_parser_get_field, /* fields */
	atomics_cobalt_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
atomics_cobalt_parser_create (dc_parser_t **out, dc_context_t *context)
{
	atomics_cobalt_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (atomics_cobalt_parser_t *) dc_parser_allocate (context, &atomics_cobalt_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->atmospheric = 0.0;
	parser->hydrostatic = 1025.0 * GRAVITY;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}


dc_status_t
atomics_cobalt_parser_set_calibration (dc_parser_t *abstract, double atmospheric, double hydrostatic)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = array_uint16_le (p + 0x14);
		datetime->month  = p[0x16];
		datetime->day    = p[0x17];
		datetime->hour   = p[0x18];
		datetime->minute = p[0x19];
		datetime->second = 0;
	}

	return DC_STATUS_SUCCESS;
}


#define BUFLEN 16


static dc_status_t
atomics_cobalt_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;

	double atmospheric = 0.0;
	char buf[BUFLEN];
	dc_field_string_t *string = (dc_field_string_t *) value;

	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (p + 0x26) * BAR / 1000.0;

	unsigned int workpressure = 0;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (p + 0x58) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (array_uint16_le (p + 0x56) * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
			break;
		case DC_FIELD_GASMIX_COUNT:
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = p[0x2a];
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = p[SZ_HEADER + SZ_GASMIX * flags + 5] / 100.0;
			gasmix->oxygen = p[SZ_HEADER + SZ_GASMIX * flags + 4] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_SURFACE:
			*((double *) value) = (p[0x1B] - 32.0) * (5.0 / 9.0);
			break;
		case DC_FIELD_TANK:
			p += SZ_HEADER + SZ_GASMIX * flags;
			switch (p[2]) {
			case 1: // Cuft at psi
			case 2: // Cuft at bar
				workpressure = array_uint16_le(p + 10);
				if (workpressure == 0)
					return DC_STATUS_DATAFORMAT;
				tank->type = DC_TANKVOLUME_IMPERIAL;
				tank->volume = array_uint16_le(p + 8) * CUFT * 1000.0;
				tank->volume /= workpressure * PSI / ATM;
				tank->workpressure = workpressure * PSI / BAR;
				break;
			case 3: // Wet volume in 1/10 liter
				tank->type = DC_TANKVOLUME_METRIC;
				tank->volume = array_uint16_le(p + 8) / 10.0;
				tank->workpressure = 0.0;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			tank->gasmix = flags;
			tank->beginpressure = array_uint16_le(p + 6) * PSI / BAR;
			tank->endpressure = array_uint16_le(p + 14) * PSI / BAR;
			break;
		case DC_FIELD_DIVEMODE:
			switch(p[0x24]) {
			case 0: // Open Circuit Trimix
			case 2: // Open Circuit Nitrox
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case 1: // Closed Circuit
				*((dc_divemode_t *) value) = DC_DIVEMODE_CC;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_STRING:
			switch(flags) {
			case 0: // Serialnr
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%c%c%c%c-%c%c%c%c", p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
				break;
			case 1: // Program Version
				string->desc = "Program Version";
				snprintf(buf, BUFLEN, "%.2f", array_uint16_le(p + 30) / 100.0);
				break;
			case 2: // Boot Version
				string->desc = "Boot Version";
				snprintf(buf, BUFLEN, "%.2f", array_uint16_le(p + 32) / 100.0);
				break;
			case 3: // Nofly
				string->desc = "NoFly Time";
				snprintf(buf, BUFLEN, "%0u:%02u", p[0x52], p[0x53]);
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
atomics_cobalt_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int interval = data[0x1a];
	unsigned int ngasmixes = data[0x2a];
	unsigned int nswitches = data[0x2b];
	unsigned int nsegments = array_uint16_le (data + 0x50);

	unsigned int header = SZ_HEADER + SZ_GASMIX * ngasmixes +
		SZ_GASSWITCH * nswitches;

	if (size < header + SZ_SEGMENT * nsegments)
		return DC_STATUS_DATAFORMAT;

	double atmospheric = 0.0;
	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (data + 0x26) * BAR / 1000.0;

	// Previous gas mix - initialize with impossible value
	unsigned int gasmix_previous = 0xFFFFFFFF;

	// Get the primary tank.
	unsigned int tank = 0;
	while (tank < ngasmixes) {
		unsigned int sensor = array_uint16_le(data + SZ_HEADER + SZ_GASMIX * tank + 12);
		if (sensor == 1)
			break;
		tank++;
	}
	if (tank >= ngasmixes) {
		ERROR (abstract->context, "Invalid primary tank index.");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int time = 0;
	unsigned int in_deco = 0;
	unsigned int offset = header;
	while (offset + SZ_SEGMENT <= size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/1000 bar).
		unsigned int depth = array_uint16_le (data + offset + 0);
		sample.depth = (depth * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Pressure (1 psi).
		unsigned int pressure = array_uint16_le (data + offset + 2);
		sample.pressure.tank = tank;
		sample.pressure.value = pressure * PSI / BAR;
		if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);

		// Current gas mix
		unsigned int gasmix = data[offset + 4];
		if (gasmix != gasmix_previous) {
			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (data[SZ_HEADER + SZ_GASMIX * idx + 0] == gasmix)
					break;
				idx++;
			}
			if (idx >= ngasmixes) {
				ERROR (abstract->context, "Invalid gas mix index.");
				return DC_STATUS_DATAFORMAT;
			}
			sample.gasmix = idx;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
#ifdef ENABLE_DEPRECATED
			unsigned int o2 = data[SZ_HEADER + SZ_GASMIX * idx + 4];
			unsigned int he = data[SZ_HEADER + SZ_GASMIX * idx + 5];
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = o2 | (he << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
#endif
			gasmix_previous = gasmix;
		}

		// Temperature (1 °F).
		unsigned int temperature = data[offset + 8];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// violation status
		sample.event.type = 0;
		sample.event.time = 0;
		sample.event.value = 0;
		sample.event.flags = 0;
		unsigned int violation = data[offset + 11];
		if (violation & 0x01) {
			sample.event.type = SAMPLE_EVENT_ASCENT;
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}
		if (violation & 0x04) {
			sample.event.type = SAMPLE_EVENT_CEILING;
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}
		if (violation & 0x08) {
			sample.event.type = SAMPLE_EVENT_PO2;
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}

		// NDL & deco
		unsigned int ndl = data[offset + 5] * 60;
		if (ndl > 0)
			in_deco = 0;
		else if (ndl == 0 && (violation & 0x02))
			in_deco = 1;
		if (in_deco)
			sample.deco.type = DC_DECO_DECOSTOP;
		else
			sample.deco.type = DC_DECO_NDL;
		sample.deco.time = ndl;
		sample.deco.depth = 0.0;
		if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

		offset += SZ_SEGMENT;
	}

	return DC_STATUS_SUCCESS;
}
