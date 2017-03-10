/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#include <libdivecomputer/shearwater_predator.h>
#include <libdivecomputer/shearwater_petrel.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser)	( \
	dc_parser_isinstance((parser), &shearwater_predator_parser_vtable) || \
	dc_parser_isinstance((parser), &shearwater_petrel_parser_vtable))

#define SZ_BLOCK   0x80
#define SZ_SAMPLE_PREDATOR  0x10
#define SZ_SAMPLE_PETREL    0x20

#define GASSWITCH     0x01
#define PPO2_EXTERNAL 0x02
#define SETPOINT_HIGH 0x04
#define SC            0x08
#define OC            0x10

#define METRIC   0
#define IMPERIAL 1

#define NGASMIXES 10

typedef struct shearwater_predator_parser_t shearwater_predator_parser_t;

struct shearwater_predator_parser_t {
	dc_parser_t base;
	unsigned int petrel;
	unsigned int samplesize;
	// Cached fields.
	unsigned int cached;
	unsigned int headersize;
	unsigned int footersize;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int serial;
	dc_divemode_t mode;
	unsigned int sensor_cal_value[3];
	signed char sensor_adc_offset[3];
};

static dc_status_t shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t shearwater_predator_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PREDATOR,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const dc_parser_vtable_t shearwater_petrel_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


static unsigned int
shearwater_predator_find_gasmix (shearwater_predator_parser_t *parser, unsigned int o2, unsigned int he)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->oxygen[i] && he == parser->helium[i])
			break;
		i++;
	}

	return i;
}


dc_status_t
shearwater_common_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial, unsigned int petrel)
{
	shearwater_predator_parser_t *parser = NULL;
	const dc_parser_vtable_t *vtable = NULL;
	unsigned int samplesize = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (petrel) {
		vtable = &shearwater_petrel_parser_vtable;
		samplesize = SZ_SAMPLE_PETREL;
	} else {
		vtable = &shearwater_predator_parser_vtable;
		samplesize = SZ_SAMPLE_PREDATOR;
	}

	// Allocate memory.
	parser = (shearwater_predator_parser_t *) dc_parser_allocate (context, vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser->petrel = petrel;
	parser->serial = serial;
	if (petrel) {
		parser->samplesize = SZ_SAMPLE_PETREL;
	} else {
		parser->samplesize = SZ_SAMPLE_PREDATOR;
	}

	// Set the default values.
	parser->cached = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->mode = DC_DIVEMODE_OC;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, serial, 0);
}


dc_status_t
shearwater_petrel_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, serial, 1);
}


static dc_status_t
shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->mode = DC_DIVEMODE_OC;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2 * SZ_BLOCK)
		return DC_STATUS_DATAFORMAT;

	unsigned int ticks = array_uint32_be (data + 12);

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


#define BUFLEN 16

static dc_status_t
shearwater_predator_parser_cache (shearwater_predator_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	unsigned int headersize = SZ_BLOCK;
	unsigned int footersize = SZ_BLOCK;
	if (size < headersize + footersize) {
		ERROR (abstract->context, "Invalid data length.");
		return DC_STATUS_DATAFORMAT;
	}

	// Adjust the footersize for the final block.
	if (parser->petrel || array_uint16_be (data + size - footersize) == 0xFFFD) {
		footersize += SZ_BLOCK;
		if (size < headersize + footersize) {
			ERROR (abstract->context, "Invalid data length.");
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Default dive mode.
	dc_divemode_t mode = DC_DIVEMODE_OC;

	// Get the gas mixes.
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	unsigned int helium[NGASMIXES] = {0};
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int offset = headersize;
	unsigned int length = size - footersize;
	while (offset < length) {
		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Status flags.
		unsigned int status = data[offset + 11];
		if ((status & OC) == 0) {
			mode = DC_DIVEMODE_CC;
		}

		// Gaschange.
		unsigned int o2 = data[offset + 7];
		unsigned int he = data[offset + 8];
		if (o2 != o2_previous || he != he_previous) {
			// Find the gasmix in the list.
			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (o2 == oxygen[idx] && he == helium[idx])
					break;
				idx++;
			}

			// Add it to list if not found.
			if (idx >= ngasmixes) {
				if (idx >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_NOMEMORY;
				}
				oxygen[idx] = o2;
				helium[idx] = he;
				ngasmixes = idx + 1;
			}

			o2_previous = o2;
			he_previous = he;
		}

		offset += parser->samplesize;
	}

	// Cache sensor calibration for later use
	parser->sensor_cal_value[0] = array_uint16_be(data + 87);
	parser->sensor_cal_value[1] = array_uint16_be(data + 89);
	parser->sensor_cal_value[2] = array_uint16_be(data + 91);
	// The Predator expects the mV output of the cells to be within 30mV to
	// 70mV in 100% O2 at 1 atmosphere.
	// If we add 1024 (1000?) to the cal value, then the sensors lines up
	// and matches the average
	parser->sensor_cal_value[0] += 1024;
	parser->sensor_cal_value[1] += 1024;
	parser->sensor_cal_value[2] += 1024;

	// Cache sensor adc offset for later use
	// Unit is probably 0.025 mV
	// Is this included in the stored value, or its it "raw"?
	parser->sensor_adc_offset[0] = data[93];
	parser->sensor_adc_offset[1] = data[94];
	parser->sensor_adc_offset[2] = data[95];

	// Cache the data for later use.
	parser->headersize = headersize;
	parser->footersize = footersize;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
		parser->helium[i] = helium[i];
	}
	parser->mode = mode;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Get the offset to the footer record.
	unsigned int footer = size - parser->footersize;

	// Get the unit system.
	unsigned int units = data[8];

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;
	unsigned int density = 0;
	char buf[BUFLEN];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (data + footer + 6) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (units == IMPERIAL)
				*((double *) value) = array_uint16_be (data + footer + 4) * FEET;
			else
				*((double *) value) = array_uint16_be (data + footer + 4);
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			density = array_uint16_be (data + 83);
			if (density == 1000)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = density;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_be (data + 47) / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = parser->mode;
			break;
		case DC_FIELD_STRING:
			switch(flags) {
			case 0: // Battery
				string->desc = "Battery at end";
				snprintf(buf, BUFLEN, "%.1f", data[9] / 10.0);
				break;
			case 1: // Serial
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%08x", parser->serial);
				break;
			case 2: // FW Version
				string->desc = "FW Version";
				snprintf(buf, BUFLEN, "%2x", data[19]);
				break;
			case 3: /* Deco model */
				string->desc = "Deco model";
				switch (data[67]) {
				case 0:
					strncpy(buf, "GF", BUFLEN);
					break;
				case 1:
					strncpy(buf, "VPM-B", BUFLEN);
					break;
				case 2:
					strncpy(buf, "VPM-B/GFS", BUFLEN);
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
				break;
			case 4: /* Deco model info */
				string->desc = "Deco model info";
				switch (data[67]) {
				case 0:
					snprintf(buf, BUFLEN, "GF %u/%u", data[4], data[5]);
					break;
				case 1:
					snprintf(buf, BUFLEN, "VPM-B +%u", data[68]);
					break;
				case 2:
					snprintf(buf, BUFLEN, "VPM-B/GFS +%u %u%%", data[68], data[85]);
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
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
shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Get the unit system.
	unsigned int units = data[8];

	// Previous gas mix.
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int time = 0;
	unsigned int offset = parser->headersize;
	unsigned int length = size - parser->footersize;
	while (offset < length) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Time (seconds).
		time += 10;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m or ft).
		unsigned int depth = array_uint16_be (data + offset);
		if (units == IMPERIAL)
			sample.depth = depth * FEET / 10.0;
		else
			sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (°C or °F).
		int temperature = (signed char) data[offset + 13];
		if (temperature < 0) {
			// Fix negative temperatures.
			temperature += 102;
			if (temperature > 0) {
				temperature = 0;
			}
		}
		if (units == IMPERIAL)
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		else
			sample.temperature = temperature;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// Status flags.
		unsigned int status = data[offset + 11];

		if ((status & OC) == 0) {
			// PPO2 -- only return PPO2 if we are in closed circuit mode
#ifdef SENSOR_AVERAGE
			sample.ppo2 = data[offset + 6] / 100.0;
			if (callback) callback (DC_SAMPLE_PPO2, sample, userdata);
#else
			sample.ppo2 = data[offset + 12] * parser->sensor_cal_value[0] / 100000.0;
			if (callback && (data[86] & 0x01)) callback (DC_SAMPLE_PPO2, sample, userdata);

			sample.ppo2 = data[offset + 14] * parser->sensor_cal_value[1] / 100000.0;
			if (callback && (data[86] & 0x02)) callback (DC_SAMPLE_PPO2, sample, userdata);

			sample.ppo2 = data[offset + 15] * parser->sensor_cal_value[2] / 100000.0;
			if (callback && (data[86] & 0x04)) callback (DC_SAMPLE_PPO2, sample, userdata);
#endif

			// Setpoint
			if (parser->petrel) {
				sample.setpoint = data[offset + 18] / 100.0;
			} else {
				if (status & SETPOINT_HIGH) {
					sample.setpoint = data[18] / 100.0;
				} else {
					sample.setpoint = data[17] / 100.0;
				}
			}
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
		}

		// CNS
		if (parser->petrel) {
			sample.cns = data[offset + 22] / 100.0;
			if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
		}

		// Gaschange.
		unsigned int o2 = data[offset + 7];
		unsigned int he = data[offset + 8];
		if (o2 != o2_previous || he != he_previous) {
			unsigned int idx = shearwater_predator_find_gasmix (parser, o2, he);
			if (idx >= parser->ngasmixes) {
				ERROR (abstract->context, "Invalid gas mix.");
				return DC_STATUS_DATAFORMAT;
			}

			sample.gasmix = idx;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int decostop = array_uint16_be (data + offset + 2);
		if (decostop) {
			sample.deco.type = DC_DECO_DECOSTOP;
			if (units == IMPERIAL)
				sample.deco.depth = decostop * FEET;
			else
				sample.deco.depth = decostop;
		} else {
			sample.deco.type = DC_DECO_NDL;
			sample.deco.depth = 0.0;
		}
		sample.deco.time = data[offset + 9] * 60;
		if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

		offset += parser->samplesize;
	}

	return DC_STATUS_SUCCESS;
}
