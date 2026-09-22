/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  The one place that lists every device this engine knows. Adding a device
 *  means writing its own src/devices/<device>.cc (identification) and
 *  src/engines/<engine>/devices/<device>.cc (synthesis parameters), then
 *  adding one line below - nowhere else in this file, or in control_rom.cc,
 *  ever names a device.
 */
#include "../control_rom.h"

namespace EmuSC
{

extern const ControlRom::DeviceEntry SC55_DEVICE;
extern const ControlRom::DeviceEntry SC55MKII_DEVICE;
extern const ControlRom::DeviceEntry SC88_DEVICE;
extern const ControlRom::DeviceEntry JV880_DEVICE;
extern const ControlRom::DeviceEntry JV1080_DEVICE;

const ControlRom::DeviceEntry ControlRom::DEVICES[] = {
  SC55_DEVICE, SC55MKII_DEVICE, SC88_DEVICE, JV880_DEVICE,
  JV1080_DEVICE,
};
const int ControlRom::DEVICE_COUNT =
  (int) (sizeof(DEVICES) / sizeof(DEVICES[0]));

}
