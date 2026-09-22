/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
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

const ControlRom::DeviceEntry ControlRom::DEVICES[] = {
  SC55_DEVICE, SC55MKII_DEVICE, SC88_DEVICE, JV880_DEVICE,
};
const int ControlRom::DEVICE_COUNT =
  (int) (sizeof(DEVICES) / sizeof(DEVICES[0]));

}
