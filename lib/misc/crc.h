/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_CRC_H
#define _LVM_CRC_H

#include "lvm-types.h"

#define INITIAL_CRC 0xf597a6cf

uint32_t calc_crc(uint32_t initial, void *buf, uint32_t size);

#endif
