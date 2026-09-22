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

/* This device's own voice engine, for XpDeviceProfile::voiceEngine. Its
   state is opaque: the shared device layer holds it as a void * and only
   ever calls through this table. */
extern const struct XpVoiceEngineOps JV1080_VOICE_ENGINE;

/* ONE TONE, SOUNDING. A BEHAVIOURAL MODEL, NOT THE CHIP.
 *
 *   Everything below responds the way the hardware measures - the wave it
 *   reads and the rate it reads it at, the amplitude envelope, the filter's
 *   corner, the pan split - and none of it is the arithmetic the machine
 *   uses, because that arithmetic is in an undumped mask ROM. What IS in
 *   this device's readable ROM is the wave data and the parameter values;
 *   those are exact, and this turns them into sound by the measured
 *   transfer functions listed against each field in jv1080.cc.
 *
 *   What this model does NOT yet include, so that nothing reading it
 *   mistakes silence for a measurement: the two LFOs and all their
 *   destinations, the pitch and filter envelopes, FXM, the booster, the
 *   ten structures (six of which ring-modulate), tone delay, the TVA bias,
 *   every key-follow field, the alternate and random pan depths, velocity
 *   curves 1 to 6 (measured in M-029, not yet transcribed here), the
 *   element record's own attenuation and fine-tune fields (units open,
 *   U-R3-03), and the insert, chorus and reverb effects, which are
 *   bypassed rather than approximated.
 */
/* Everything outside the tone that scales or places one of its voices.
   Each term is a measured one: the patch and part levels index the same
   square law the tone level does (`M-009`), CC7 indexes it with a floor
   (`M-081`), the two pans sum as offsets from centre into one table
   (`M-002`, `M-015`), and the part's key shift moves the pitch. */
struct XpJv1080PartControls {
  unsigned patch_level;
  unsigned patch_pan;
  unsigned part_level;
  unsigned part_pan;
  unsigned volume;
  int key_shift;
};

struct XpJv1080Voice {
  bool active;
  bool releasing;

  /* The decoded element, and where in it the read head is. */
  const int32_t *pcm;
  size_t pcm_count;
  double position;               /* fractional index into pcm */
  double increment;              /* wave samples per output sample */
  size_t loop_first;
  size_t loop_last;
  bool looping;
  bool reverse;

  /* Amplitude. static_gain is everything that does not move: the level
     fields' square law, the velocity curve and the wave gain. */
  double static_gain;
  double gain_left;
  double gain_right;

  /* The four-segment amplitude envelope. level[] is linear amplitude,
     time[] is seconds for that segment. */
  double level[4];
  double time[4];
  unsigned segment;
  double envelope;               /* current linear amplitude */
  double segment_start;
  double segment_total;          /* this segment's own duration, seconds */
  double segment_remaining;      /* seconds left in this segment */
  double sample_period;

  /* The filter, as a two-pole section. */
  int filter_type;
  double b0, b1, b2, a1, a2;
  double x1, x2, y1, y2;
};

#ifdef __cplusplus
}

struct xp_rom;
struct xp_packed_record;

namespace EmuSC { namespace Xp {

/* Set a voice up to play one tone. `tone` is the tone's 130 decoded bytes -
 * the form the packed record decodes to, and the same form the device's own
 * SysEx tone frames carry - and patchLevel/patchPan are the patch common's.
 * `banks` are the eight descrambled 1 MiB wave banks. The element is decoded
 * into `pcm`, which must outlive the voice; false means this tone does not
 * sound for this key and velocity, which is not an error.
 */
bool jv1080_voice_start(const struct xp_rom *rom,
                         const struct XpVoiceFieldMap *fields,
                         const uint8_t *tone,
                         const struct XpJv1080PartControls *controls,
                         unsigned key, unsigned velocity,
                         const uint8_t *const banks[XP_WAVE_BANK_COUNT],
                         const size_t bankSizes[XP_WAVE_BANK_COUNT],
                         int32_t *pcm, size_t capacity,
                         double outputRate, struct XpJv1080Voice *voice);

/* Decode one patch's tone into `tone` (130 bytes) and its patch common's
 * level and pan, ready for jv1080_voice_start. */
bool jv1080_patch_tone(const struct xp_rom *rom,
                        const struct xp_packed_record *patch, unsigned index,
                        uint8_t *tone, unsigned *patchLevel,
                        unsigned *patchPan);

/* How many decoded samples this tone needs at this key, so a caller can
 * size the buffer jv1080_voice_start decodes into. False where the tone
 * does not sound, the same cases jv1080_voice_start refuses. */
bool jv1080_voice_span(const struct xp_rom *rom,
                        const struct XpVoiceFieldMap *fields,
                        const uint8_t *tone, unsigned key, unsigned velocity,
                        size_t *samples);

void jv1080_voice_release(struct XpJv1080Voice *voice);

/* Adds this voice's output into l/r. Returns false once it has finished,
 * having written whatever it had left. */
bool jv1080_voice_render(struct XpJv1080Voice *voice, float *l, float *r,
                          size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
