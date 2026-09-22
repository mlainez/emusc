/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Roland JV-1080 constants for the XP engine (engines/xp/).
 *
 *  This device runs the same sound chip as the SC-88 and carries the
 *  identical Roland part number for it, but its firmware is a different
 *  thing entirely: the SC-88's is in an external ROM and disassembles,
 *  while the JV-1080's synthesis engine lives in the SH7034's 64 KB
 *  internal mask ROM, which has never been dumped.
 *
 *  SO NOTHING HERE IS A FIRMWARE PORT, AND NOTHING HERE MAY BE READ AS
 *  FIRMWARE-EXACT. What the external ROM holds - the preset records, the
 *  parameter map, the wave tables, the effect coefficient data - is read
 *  from it and is exact. Everything the voice path does with those values
 *  is a BEHAVIOURAL MODEL fitted to laws measured on the hardware: a
 *  transfer function that responds the way the machine does, not the
 *  arithmetic the machine uses to get there. engines/xp/README.md says the
 *  same thing about this device from the other side, and each law below
 *  carries the measurement it comes from.
 *
 *  The device facts themselves live in struct XpDeviceProfile
 *  (devices/profile.h), populated in jv1080.cc.
 */
#ifndef EMUSC_XP_DEVICES_JV1080_H
#define EMUSC_XP_DEVICES_JV1080_H

#include "profile.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This device's control ROM image size, for the test suite's own buffers;
   XpDeviceProfile::romSize is what the engine reads. */
inline constexpr unsigned XP_JV1080_CONTROL_ROM_SIZE = 0x100000u;

/* Which field of the tone group each role is. The index within a group is
   also the parameter's SysEx address offset on this device, because its
   descriptor table doubles as its parameter address map - so these are the
   manual's own offsets, checked field by field against the descriptors'
   declared ranges for all ten groups. They are here rather than in the
   profile only where a caller needs the whole run of a block; the profile
   carries the ones the shared voice path reads by role. */
inline constexpr unsigned XP_JV1080_TONE_FIELDS = 130u;
inline constexpr unsigned XP_JV1080_PATCH_COMMON_FIELDS = 75u;
inline constexpr unsigned XP_JV1080_TONES_PER_PATCH = 4u;

extern const struct XpDeviceProfile JV1080_PROFILE;

#ifdef __cplusplus
}
#endif

#endif
