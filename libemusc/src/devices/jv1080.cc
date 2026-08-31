/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland JV-1080 device profile.
 *
 *  Data only. Every offset carries the evidence that fixed it: an offset without
 *  provenance is a guess, and a guess that happens to be in range is the most
 *  expensive kind. See device_profile.h for what each field means.
 */

#include "../device_profile.h"

namespace EmuSC
{

// The JV-1080's waveform and sample tables are located; its patch, performance
// and rhythm banks are not mapped yet, so it plays waveforms and nothing more.
const DeviceProfile JV1080_PROFILE = {
  { 0x071008, 60, 12, 11 },
  { 0x075c7a, 18 },
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {} },
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {} },
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

  nullptr, 0
};

}
