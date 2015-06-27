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

#ifndef SUUNTO_VYPER2_H
#define SUUNTO_VYPER2_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "context.h"
#include "device.h"

#define SUUNTO_VYPER2_VERSION_SIZE 0x04

dc_status_t
suunto_vyper2_device_open (dc_device_t **device, dc_context_t *context, const void *params);

dc_status_t
suunto_vyper2_device_version (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
suunto_vyper2_device_reset_maxdepth (dc_device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER2_H */
