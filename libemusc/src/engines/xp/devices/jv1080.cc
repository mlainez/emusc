/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Roland JV-1080 device profile for the XP engine (engines/xp/).
 *
 *  The single instance below is this device's XpDeviceProfile - see
 *  profile.h for the struct and the injection mechanism (xp_rom::profile,
 *  xp_engine::profile, xp_profile()), and jv1080.h for why none of it is
 *  firmware-exact.
 *
 *  Every address here is a PRG (external program ROM) offset, read out of
 *  the image; every law is a measurement on the owner's unit with its own
 *  measurement id. The fields this device has no counterpart for - the
 *  SC-88's envelope rate, gain, LFO, pan, EQ, delay and reverb tables -
 *  stay zero, because they are firmware tables and this device's firmware
 *  is in silicon. Their readers check for zero; the voice path reads the
 *  measured laws instead.
 */
#include "jv1080.h"
#include "../rom.h"

extern "C" {

const struct XpDeviceProfile JV1080_PROFILE = {
  /* rom_init()'s own identification data - never referenced past that one
     function.

     The first span is the head of the PRG header, which is a Roland RTOS
     task table rather than an SH vector table but is fixed content either
     way: the sixteen bytes at offset 0 occur exactly once in the whole
     1 MiB image. The second is the head of the firmware banner at
     0x04A1CE, `NU-10B Version 1.02     1994/09/12 00:37` - note the five
     spaces, verified against the image rather than transcribed. */
  .romSize = XP_JV1080_CONTROL_ROM_SIZE,
  .identVectors = {
    0x00, 0x00, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x10,
    0x0f, 0xff, 0xf0, 0x24, 0x0f, 0xff, 0xf0, 0x4c },
  .identFirstDirectory = {
    'N', 'U', '-', '1', '0', 'B', ' ', 'V',
    'e', 'r', 's', 'i', 'o', 'n', ' ', '1' },
  .identSecondOffset = 0x04a1ceu,

  /* Measured: 71 keys held gives exactly 64 sounding (`M-031`). */
  .defaultMaxVoices = 64u,

  /* The SC-88's firmware tables have no counterpart here - this device's
     envelope, gain, LFO, pan, EQ, delay and reverb laws are in the
     undumped internal mask ROM, and what this project has instead is those
     laws measured on the hardware. Left zero deliberately; see the file
     comment. */

  /* --- The reverb, as the ROM describes it ---------------------------
     THE SAME RECORD THE SC-88 CARRIES, at different offsets. Table
     `0x0599C8` holds one whole pointer per type - ROOM1, ROOM2, STAGE1,
     STAGE2, HALL1, HALL2, DELAY, PAN-DELAY - into 114-byte records: an
     input one-pole pair at word 0, the eight allpass coefficient pairs at
     2, the two damping pairs at 18 and the thirty-two delay-memory
     addresses at 22. The SC-88's are at 0, 16 and 20 with no input pair,
     which is the whole difference (`M-075`, `M-094`, both `FW-EXACT`).

     THE ADDRESS LAYOUT IS IDENTICAL, which is the finding rather than an
     assumption. `0x03E588` gives the instruction each address patches, and
     the eight that land on 192..206 are exactly the taps `0x038546`
     patches; pairing what is left by adjacency gives twelve buffers whose
     head and far indices, and whose eight allpass-carrying buffers, come
     out the same numbers this engine already holds for the SC-88. It
     checks against the ROM: under that pairing STAGE1's first six buffers
     measure 917, 377, 110, 76, 665 and 813 samples where the boot image's
     own allpass lines are documented at 918, 378, 111, 77, 666 and 813 -
     five to a single sample, one exact.

     THE COEFFICIENTS ARE THE SAME TWO WORDS: `0x3000` and `0x1000`, which
     the shared law reads as -0.5 and +0.5, the Schroeder pair at g = 0.5.
     Seven of the eight slots are enabled on every reverb type and all
     eight are zero on DELAY and PAN-DELAY, which is a control on the
     reading. THOSE VALUES REST ON THE SHIFT-FIELD HALF OF `U-R5-01`, which
     is corroborated across 33 coefficient images decoding self-
     consistently but is NOT measured against the machine - `FW-STRUCT`,
     not `MEASURED`. What is measured is the other half: bit 15 tags a
     length, confirmed to 0.5 ms on `effects/chorus_pre_delay_127`.

     THE CHAIN ORDER IS NOT RECOVERED and is not guessed here. `M-094`
     argues that is the diffuser's purpose rather than a measurement
     failure - a 3 ms tick produces three arrivals above 5 % in 300 ms
     where a comb network would show one per line - and a control settles
     it: ROOM1's lengths score 68 of 83 against a HALL1 recording where
     HALL1's own score 49. The buffers are therefore run in the record's
     own order, which is an ordering and not a claim about the silicon. */
  .reverbPointers = 0x0599C8u,
  .reverbPage = 0u,
  .reverbMacroTable = 0u,
  .reverbAllpassPairA = 0x3000u,
  .reverbAllpassPairB = 0x1000u,
  .reverbAllpassG = 0.5f,
  .reverbImage0Cram = 0x03F77Cu,
  .headWord = { 0u, 2u, 4u, 6u, 8u, 10u, 14u, 16u, 20u, 22u, 26u, 28u },
  .farWord = { 1u, 3u, 5u, 7u, 9u, 13u, 15u, 19u, 21u, 25u, 27u, 31u },
  .tapWord = { 11u, 17u, 23u, 29u, 12u, 18u, 24u, 30u },
  .allpassBuffer = { 0u, 1u, 2u, 3u, 4u, 6u, 8u, 10u },
  .tapInstruction = { 192u, 194u, 196u, 198u, 200u, 202u, 204u, 206u },
  .reverbCharacters = 8u,
  .reverbRecordWords = 57u,
  .reverbAllpassWord = 2u,
  /* The record's two damping pairs read (0, 8191) on every type, which is
     the neutral default: `M-094` has the HF-damp PARAMETER overwrite these
     two slots, and `M-064` solves that law to the unit. So the character
     carries no damping of its own here and the parameter supplies it. */
  .reverbDampWord = XP_REVERB_WORD_NONE,
  .reverbAddressWord = 22u,
  .reverbTrimWord = XP_REVERB_WORD_NONE,
  .reverbInputWord = 0u,
  .reverbPointerBytes = 4u,
  .reverbPointerBase = 0x0A000000u,
  .reverbDampTable = 0x039700u,
  .reverbLevelTable = 0x03856Cu,

  .waveDescriptorSize = 0u,       /* waves are reached through the
                                     multisample chain below, not through
                                     one flat descriptor table */
  .waveBankSize = 0x100000u,      /* the FCE exponent region is per 1 MiB */
  .waveChipSize = 0x200000u,      /* NEC uPD23C16000JGX, 1M x 16 */

  /* THE WAVE ROMS ARE 16-BIT WORD DEVICES, AND THE BOARD SCRAMBLES THEM AS
     SUCH.
     Service Notes p.11 wires all sixteen outputs O0..O15 of each
     uPD23C16000JGX to the sound chip, so one storage unit is a 16-bit
     word, little-endian in the dump, and the address permutation runs over
     the 20 word-address lines rather than 21 byte-address ones. Applying
     both permutations reveals the plaintext-free headers `INT100A` (W1-W3)
     and `INT100B` (W4) at 0x20 and the date `1994-05-22` at 0x30 in all
     four ROMs, where every competing address candidate produces filler.

     The low byte's own permutation is the byte-wide family's; the high
     byte adds the rest. */
  .waveUnitBytes = 2u,
  .waveDataLineCount = 16u,
  .waveAddressLineCount = 20u,
  .waveDataLinePermutation = {
    0x02u, 0x00u, 0x04u, 0x05u, 0x07u, 0x06u, 0x03u, 0x01u,
    0x0au, 0x08u, 0x0cu, 0x0du, 0x0fu, 0x0eu, 0x0bu, 0x09u },
  .waveAddressLinePermutation = {
    0x03u, 0x01u, 0x02u, 0x00u, 0x0cu, 0x06u, 0x0bu, 0x04u,
    0x09u, 0x0fu, 0x08u, 0x05u, 0x07u, 0x0du, 0x10u, 0x0au,
    0x0eu, 0x11u, 0x12u, 0x13u },

  /* One 32-byte plaintext header per chip - `RolandJV1080O.S. Intwave
     Ver1,00A` - and the board leaves it out of the scrambling. Unlike the
     SC-88's board there is no second header at the 1 MiB mark. */
  .waveHeaderBytes = 0x20u,
  .waveHeaderStride = 0x200000u,

  /* NOT RECOVERED, AND NOTHING ON THIS DEVICE'S PATH PROGRAMS THE CHIP.
     The SC-88's eight entries are its bank-select register's own bytes,
     read from its firmware. This device's firmware is internal, so the
     encoding of its bank field is unknown; these are this engine's own
     bank ordinals standing in for it, which is all the engine's bank
     lookup needs. Anything that comes to program the real register must
     replace them with measured bytes rather than trust these. */
  .selectors = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u },

  /* --- Descriptor-packed preset records -------------------------------

     THE PRESETS ARE BIT-PACKED, AND ONE TABLE SAYS WHERE EVERY FIELD IS.
     A twelve-character patch name occupies 84 bits, not twelve bytes,
     which is why plain-ASCII searches of this image never found one.
     `packed_field_decoder` at PRG 0x019648 walks the table at 0x046D08 -
     468 records of 12 bytes - reading each field as

         ((packed[byte] | packed[byte + 1] << 8) & mask) >> shift) + bias

     little-endian even though the CPU is big-endian. Verified
     independently against the image: all 468 descriptors have shift equal
     to their mask's trailing zero count, a mask that is one contiguous
     run, and a zero in byte 11 (0xff marks the 33-entry panel variant at
     0x0482F8, which this table is not). */
  .packedDescriptorBase = 0x046d08u,
  .packedDescriptorStride = 12u,
  .packedDescriptorCount = 468u,
  .packedMaskOffset = 0x00u,
  .packedByteOffset = 0x02u,
  .packedShiftOffset = 0x03u,
  .packedBiasOffset = 0x04u,
  .packedMinOffset = 0x05u,
  .packedMaxOffset = 0x06u,

  /* The ten groups the 468 descriptors form, in table order. A group ends
     where the byte offset resets to zero; within a group the masks tile
     the record's bits with no hole and no overlap bar one documented alias
     pair. The ten first-indices below were reproduced from the image by
     scanning for that reset: 0, 49, 61, 73, 105, 171, 191, 266, 396, 409.

     In order: system common, part scale tune, patch-mode scale tune, the
     effect block, performance common, performance part, patch common,
     tone, rhythm common, rhythm note. */
  .packedGroups = {
    {   0u,  49u, 20u },
    {  49u,  12u, 11u },
    {  61u,  12u, 11u },
    {  73u,  32u, 26u },
    { 105u,  66u, 53u },
    { 171u,  20u, 12u },
    { 191u,  75u, 49u },
    { 266u, 130u, 88u },
    { 396u,  13u, 11u },
    { 409u,  59u, 42u },
  },
  .packedGroupCount = 10u,

  /* The preset banks, and the loaders that read them: the performance
     loader at 0x0A0198B6 (base + 245*n, then 16 sub-records), the patch
     loader at 0x0A019AE4 (base + 401*n, then 4), the rhythm loader at
     0x0A019D34 (base + 2699*n, then 64). The whole preset area is
     contiguous and all sixteen adjacency computations hold.

     Index 6 is the GM bank, which the panel forces when the system's mode
     byte reads GM; its 128 names are the General MIDI program list in
     order, which nothing in the decode was fitted to. */
  .packedBanks = {
    { 0x080000u,  32u,  245u, 4u, 5u, 16u },   /* 0 performance PR-A */
    { 0x081ea0u,  32u,  245u, 4u, 5u, 16u },   /* 1 performance PR-B */
    { 0x083d40u,  32u,  245u, 4u, 5u, 16u },   /* 2 performance user default */
    { 0x085be0u, 128u,  401u, 6u, 7u,  4u },   /* 3 patch PR-A */
    { 0x092460u, 128u,  401u, 6u, 7u,  4u },   /* 4 patch PR-B */
    { 0x09ece0u, 128u,  401u, 6u, 7u,  4u },   /* 5 patch PR-C */
    { 0x0ab560u, 128u,  401u, 6u, 7u,  4u },   /* 6 patch GM */
    { 0x061ea0u, 128u,  401u, 6u, 7u,  4u },   /* 7 patch user default */
    { 0x0bd3c9u,   1u,  245u, 4u, 5u, 16u },   /* 8 INIT PERFORM */
    { 0x0bd4beu,   1u,  401u, 6u, 7u,  4u },   /* 9 INIT PATCH */
    { 0x0bd64fu,   1u, 2699u, 8u, 9u, 64u },   /* 10 INIT SET (rhythm) */
  },
  .packedBankCount = 11u,

  /* The four banks a melodic program change reaches, in the firmware's own
     selector order (2, 3, 4, 5 at 0x0A019AE4), and the rhythm set the
     rhythm part reads. The four preset rhythm banks are in the image but
     their bases are not asserted by this project's own extractor yet, so
     only the INIT set is listed rather than guessed at. */
  .packedMelodicBanks = { 3u, 4u, 5u, 6u },
  .packedMelodicBankCount = 4u,
  .packedRhythmBanks = { 10u },
  .packedRhythmBankCount = 1u,

  /* Bank select to bank, as `0x0A014EF6` resolves CC0 and CC32
     (`04_protocol/program_bank.md`): MSB 81 with LSB 0..3 reaches the
     three preset patch banks and the GM bank, and MSB 80 reaches the
     battery-backed user memory - whose factory contents are the bank at
     0x061EA0, which is what stands in for it here since this
     implementation has no battery RAM to hold an edit. The CARD, PCM and
     XP groups are left out: no image exists for them, so they select
     nothing rather than selecting something else.

     UNEXERCISED so far. This device's own factory demo songs configure
     every part through temporary-patch parameter writes and send no bank
     select or program change at all, so nothing measured has been through
     this path; it is read off the firmware's own selector and no further.
     The same caveat belongs on .selectors above, whose bytes stand in for
     a register encoding this project has not recovered. */
  .packedBankSelect = {
    { 0x51u, 0x00u, 3u },        /* PR-A */
    { 0x51u, 0x01u, 4u },        /* PR-B */
    { 0x51u, 0x02u, 5u },        /* PR-C */
    { 0x51u, 0x03u, 6u },        /* GM */
    { 0x50u, 0x00u, 7u },        /* USER, factory contents */
  },
  .packedBankSelectCount = 5u,

  /* The two record types a voice can come from. Both index sets are the
     manual's own SysEx offsets, which is the same thing as the descriptor's
     index within its group on this device.

     The rhythm note is not a tone with different numbers: it has no coarse
     tune and no key range, and it carries a SOURCE KEY instead - the key
     its wave is played at, whatever key struck it. That is what keeps a
     kick a kick rather than a sample transposed up the keyboard. It has a
     mute group too, which a tone has no counterpart for. */
  .toneFields = {
    .enable = 0x00u,
    .waveGroup = 0x01u,
    .waveGroupId = 0x02u,
    .waveNumber = 0x03u,
    .waveGain = 0x05u,
    .level = 0x65u,
    .pan = 0x77u,
    .coarseTune = 0x3du,
    .fineTune = 0x3eu,
    .sourceKey = XP_VOICE_FIELD_NONE,
    .cutoff = 0x51u,
    .resonance = 0x53u,
    .filterType = 0x50u,
    .filterEnvDepth = 0x55u,
    .filterEnvVelCurve = 0x56u,
    .filterEnvVelSens = 0x57u,
    .filterEnvVelTime1 = 0x58u,
    .filterEnvVelTime4 = 0x59u,
    .filterEnvTimeKeyFollow = 0x5au,
    .filterEnvTime1 = 0x5bu,
    .filterEnvLevel1 = 0x5fu,
    .ampTime1 = 0x6eu,
    .ampLevel1 = 0x72u,
    .keyRangeLow = 0x0eu,
    .keyRangeHigh = 0x0fu,
    .velocityRangeLow = 0x0cu,
    .velocityRangeHigh = 0x0du,
    .muteGroup = XP_VOICE_FIELD_NONE,
  },
  .rhythmNoteFields = {
    .enable = 0x00u,
    .waveGroup = 0x01u,
    .waveGroupId = 0x02u,
    .waveNumber = 0x03u,
    .waveGain = 0x05u,
    .level = 0x29u,
    .pan = 0x33u,
    .coarseTune = XP_VOICE_FIELD_NONE,
    .fineTune = 0x0du,
    .sourceKey = 0x0cu,
    .cutoff = 0x1bu,
    .resonance = 0x1cu,
    .filterType = 0x1au,
    /* A rhythm note's filter envelope carries ONE velocity-time
       sensitivity where a tone carries two, and carries neither a velocity
       curve nor a time key follow (`rhythm_schema.md`). The one it has is
       given to time 1, which is the segment `M-070` measured. */
    .filterEnvDepth = 0x1eu,
    .filterEnvVelCurve = XP_VOICE_FIELD_NONE,
    .filterEnvVelSens = 0x1fu,
    .filterEnvVelTime1 = 0x20u,
    .filterEnvVelTime4 = XP_VOICE_FIELD_NONE,
    .filterEnvTimeKeyFollow = XP_VOICE_FIELD_NONE,
    .filterEnvTime1 = 0x21u,
    .filterEnvLevel1 = 0x25u,
    .ampTime1 = 0x2cu,
    .ampLevel1 = 0x30u,
    .keyRangeLow = XP_VOICE_FIELD_NONE,
    .keyRangeHigh = XP_VOICE_FIELD_NONE,
    .velocityRangeLow = XP_VOICE_FIELD_NONE,
    .velocityRangeHigh = XP_VOICE_FIELD_NONE,
    .muteGroup = 0x07u,
  },

  /* The rhythm set is groups 8 and 9, addressed as part index 9 - which is
     MIDI part 10 - and holds the 64 keys 35 to 98. */
  .packedRhythmCommonGroup = 8u,
  .packedRhythmNoteGroup = 9u,
  .rhythmPartIndex = 9u,
  .rhythmFirstKey = 35u,
  .rhythmKeyCount = 64u,

  /* Field indices inside the patch-common group.

     MEASURED on the device: the octave shift at 0x41 transposes the whole
     patch by exactly twelve semitones per unit, over its whole field and
     on top of the tone's own coarse tune. On part 10 of the first factory
     song, with the tone's coarse tune left at its -12, stepping this field
     over 2, 3, 4, 5, 6 moves a key-60 note to 65.6, 131.1, 261.5, 523.3
     and 1046.3 Hz - -24, -12, 0, +12 and +24 semitones, with the zero at
     wire 3, which is the descriptor's own bias. The same sweep on part 1
     reads -36, -24, -12, 0 for wire 3..6, and stepping that part's own
     coarse tune and its tone's coarse tune each moved the result by their
     own twelve, so the three terms add. Key sweeps on both parts (keys
     36..64 and 48..66) hold to 0.04 semitones, so the term is a constant
     and not a key follow. */

  .patchFieldName = 0x00u,
  .patchFieldNameLength = 12u,
  .patchFieldLevel = 0x2eu,
  .patchFieldPan = 0x2fu,
  .patchFieldOctaveShift = 0x41u,

  /* The performance-part group is index 5, and these four fields are the
     ones the voice path needs. Level and pan are measured: both index the
     same square-law table the tone and patch levels do, to 0.15 dB
     (`M-009`), and part pan sums with the tone's as an offset from centre
     into one table (`M-002`, `M-015`).

     The receive channel matters as much as either. A part is not its
     channel: in this device's own first factory song part 12 listens on
     channel 16 and so does part 16, which is how two patches layer, and
     nothing listens on channel 12 at all. Keying notes by part index
     instead silently plays the wrong patch for three of that song's
     fourteen channels. */
  .packedPerformanceCommonGroup = 4u,
  .packedPerformancePartGroup = 5u,
  .partFieldReceiveChannel = 0x01u,
  .partFieldLevel = 0x06u,
  .partFieldPan = 0x07u,
  /* The part has a fine tune beside its coarse one, and it is live:
     writing 100 to it - the field's top, +50 cents - moved a key-60 note
     on the device by +50.1 cents, measured against the same note with the
     field at its centre. */
  .partFieldKeyShift = 0x08u,
  .partFieldReverbSend = 0x0du,
  .partFieldFineTune = 0x09u,

  /* --- Wave selection through the multisample directories -------------

     A tone names its wave with three fields, and resolving them is two
     indirections deep. 0x0A014A0C turns (Wave group, Wave group ID) into a
     source through the table at 0x046C0C - `00 00 01 02 03 04 05`, so group
     ID 1 is the INT-A namespace and ID 2 the INT-B one - and 0x0A014CEC
     turns (source, Wave number) into a multisample bank and row through
     that source's own pair of tables. The 255 + 193 = 448 numbers are
     exactly this device's documented internal waveform count, and the 193
     was measured from the other side: sweeping every group ID 2 number on
     the hardware, 0..192 sound and 193 upward are silent, which is the
     firmware's own `n < 0xC1` guard heard rather than read (`M-024`).

     The two namespaces are not the two banks. Of the 255 INT-A numbers 164
     point into bank 0 and 91 into bank 1, and of the 193 INT-B numbers 161
     point into bank 0 and 32 into bank 1, so a row can appear in both
     lists. What the namespace does decide is which physical ROMs the
     addresses in the element records name - see
     elementDirectoryChipBase. */
  .waveGroupSourceTable = 0x046c0cu,
  .waveGroupSourceCount = 7u,
  .waveSources = {
    { 0x000300u, 0x0004feu, 255u, 0u, 0u },   /* INT-A */
    { 0x0005feu, 0x000780u, 193u, 0u, 1u },   /* INT-B */
  },
  .waveSourceCount = 2u,

  /* The two multisample tables. Row 0 of each declares its own geometry -
     bank 0 says 326 rows / 957 elements / directory 0x0A075C70 and bank 1
     says 132 / 430 / 0x0A07C6F8 - and the firmware reads those three words
     back through the XP's metadata protocol, so they are source metadata
     rather than a parse assumption. */
  .multisampleBanks = {
    { 0x071000u, 326u, 0x3cu },
    { 0x07a800u, 132u, 0x3cu },
  },
  .multisampleBankCount = 2u,
  .multisampleLayout = {
    .name = 0x08u, .nameLength = 12u,
    .splitPoints = 0x14u, .splitCount = 16u,
    .elementRefs = 0x24u, .refCount = 12u,
  },

  /* The two wave-element directories. The lookup
     `directory + index * 0x12` is proven by the SH code at 0x001790 and
     0x00187C, and every element reference in both banks is below the
     directory's own size. */
  .elementDirectories = {
    { 0x075c70u, 957u, 0x12u },
    { 0x07c6f8u, 430u, 0x12u },
  },
  .elementDirectoryCount = 2u,
  .elementLayout = {
    .attenuation = 0x00u,
    .start = 0x01u,
    .loop = 0x04u,
    .end = 0x07u,
    .control = 0x0cu,
    .rootKey = 0x0du,
    .fineTune = 0x0eu,
    .loopFineTune = 0x10u,
    /* `& 4` is reverse playback and `& 3` is the loop mode (`M-073`): the
       flag is set on 48 of the 1387 elements, which are every element of
       all 46 multisamples named `REV ...` and exactly one that is not, so
       a name-based rule gets that one wrong and the byte gets all 48
       right. The field takes only 0, 1, 2, 4, 5 and 6 - never 3, never
       above 6. */
    .reverseMask = 0x04u,
    .loopModeMask = 0x03u,
  },

  /* The element addresses are in each namespace's own wave-address space,
     cut into 2 MiB chip-sized slots. Directory 0's addresses occupy three
     disjoint slots and directory 1's one, and no element crosses a slot
     boundary - so slots 0/1/2 of directory 0 are W1/W2/W3 and slot 0 of
     directory 1 is W4, which the descrambled headers (`Ver1,00A` three
     times, `Ver1.00B` once) name from the other side.

     MEASURED, not inferred from the headers: scoring every forward-looping
     element with at least four root-key periods of loop by the loop's own
     normalised autocorrelation at that period, this assignment gives
     medians 0.932 / 0.947 / 0.920 for directory 0's three slots and 0.963
     for directory 1, while all five wrong permutations of W1/W2/W3 drop
     the mis-assigned slot to between -0.25 and -0.03, and W1/W2/W3 read as
     directory 1 give -0.080 / -0.030 / -0.019 against W4's 0.963. */
  .waveSourceSlotSize = 0x200000u,
  .elementDirectoryChipBase = { 0u, 3u },

  /* This device's SysEx identity: Roland model id 6A, and a FOUR-byte
     parameter address where a GS device has three. Its own 129-byte tone
     frames run past an address-byte boundary and the machine relies on
     that, so a write is applied as a block rather than an address at a
     time. */
  .sysexModelId = 0x6au,
  .sysexAddressBytes = 4u,

  /* NOT RECOVERED. ITS ORDER IS, AND THAT IS ALL THIS CLAIMS.
     The mixer is inside the sound chip and the undumped internal ROM:
     "summing precision and clipping" are open, and only the eight 9-bit
     level registers in front of them are read. So how far one voice sits
     below the mix's full scale is not a measured number, and this is not
     fitted to make a render match anything.

     What IS measured bounds it from one side. One to four tones sum
     COHERENTLY, to within 0.02 dB of `20*log10(N)` (`M-037`), so a
     four-tone patch at maximum presents +12 dB over one tone - and the
     machine cannot be driven into distortion through any exposed parameter
     (`M-083`), so the mix accommodates that without clipping. The scale
     must therefore be 1/4 or smaller. One eighth is that bound with a
     further 6 dB and is a power of two; the exact value is unknown, and a
     recovered mixer would replace it rather than refine it.

     Without it a single voice overflows on its own: the level fields at
     maximum give unity, wave gain adds up to +12 dB (`M-035`), and the
     wave data itself peaks near half full scale, so one voice reaches 1.7
     before anything is summed. */
  .voiceMixScale = 0.125,

  /* This device's voice path is its own, because it has to be - see
     jv1080_engine.cc and jv1080.h. */
  .voiceEngine = &JV1080_VOICE_ENGINE,
};

}  // extern "C"
