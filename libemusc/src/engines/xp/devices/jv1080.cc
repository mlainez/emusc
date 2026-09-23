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
  /* Boot image 2's CRAM, where the permanent chorus/reverb/output program's
     coefficients are loaded from at power-on (`08_effects/dsp_program.md`,
     the boot-image table). The value that was here, 0x03F77C, is inside the
     PRAM-shaped band that same document lists as NOT YET ATTRIBUTED
     (`U-R5-04`), so the eight tap gains were being read out of a smooth
     monotonic ramp - 0.053, 0.046, 0.054, 0.018, ... - instead of off the
     reverb's own coefficients, and every tap ran about 19 times too quiet.
     Nothing caught it because those values pass the range check a gain has
     to pass; only comparing the tail against the machine does. */
  .reverbImage0Cram = 0x03FEBCu,
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

  /* --- The chorus, as this device's own tables describe it -----------
     MODULATOR: a rising triangle, measured (see profile.h). One-sided, so
     the delay runs upward from the pre-delay and never below it - which is
     what the pre-delay measurement itself shows, since a take with depth 0
     sits exactly on the table's own value.

     PRE-DELAY IS THE WHOLE NOMINAL DELAY. `0x038EC8`'s parameter-127 entry
     is 3296 samples and `effects/chorus_pre_delay_127` measures the dry-to-
     wet gap at 103.00 ms against the 103.000 that is at 32 kHz, so there is
     no further fixed delay to add.

     RATE: the table entry is a per-control-period increment on a 16-bit
     accumulator, so the modulation is `table[rate] / 65536` cycles per
     control period - `table * 32000 / 2^24` hertz, the control period
     being 256 samples at 32 kHz.

     Measured on the system chorus at values 32, 64, 96 and 127: 1.6191,
     3.2842, 4.8658 and 11.0000 Hz against the law's 1.6498, 3.2482,
     4.8485 and 10.998. Every deviation is inside that point's own
     counting resolution, which is half a cycle in the 9.5, 19.5, 29 and
     66 cycles the take contains: 1.9 % against 5.3, 1.1 against 2.6,
     0.36 against 1.7, and 0.02 against 0.76.

     The same constant is measured independently on the EFX chorus, whose
     rate reads the same table through a different DSP program: values 20
     and 45 give 1.0490 and 2.2983 Hz against 1.0490 and 2.2984.

     The rate is read by COUNTING the delay's own reversals, because the
     instantaneous frequency of a carrier through this chorus cannot be
     peak-picked. A dry-plus-wet comb notches at delays of 1.911, 5.734
     and 9.556 ms for a 261.63 Hz carrier, the triangle crosses every
     notch inside its excursion twice per period, and the resulting
     glitches put a strong component at (2 x crossings) times the rate.
     The excursion is set by the depth, so the multiple is too: depth 32
     spans one notch and reads 2x, depth 80 spans two and reads 4x, depth
     127 spans three and reads 6x.

     The law also reproduces the field's published range. Table entry 0 is
     26 and entry 127 is 5766, so the parameter spans 0.0496 to 10.998 Hz
     against the specified 0.05 to 10.0.

     DEPTH: the peak-to-peak sweep, 12.1 ms at the top of the field from a
     direct cepstral reading of the delay, scaled by the table's own shape.
     Measured at four depths against four rates, the sweep is a function of
     DEPTH ALONE - 1.69, 3.33, 6.65 and 9.38 ms at depths 32, 64, 96 and
     127, varying by 2.4 to 4.8 % across a twelvefold range of rate.
     THE TABLE'S IDENTITY IS NOT PINNED: three of the five unidentified
     128-entry monotonic tables in the chorus handler's literal pool share
     one shape and this is one of them, so what is used here is that shape
     and not a claim about which table the firmware reads. Normalised, the
     measured points are 0.180, 0.355, 0.709, 1.000 against the shape's
     0.191, 0.418, 0.689, 1.000 - the top two within 3 %, the bottom two
     low in the direction a measurement floor pushes, the depth-32 sweep
     being only 5.3 cents of excursion where `M-043` found its own depth-0
     reading of 1.5 cents to be that floor.

     FEEDBACK: the level table read unshifted (jv1080_engine.cc), taken
     from the left tap alone and written back ahead of the pre-delay
     (`M-112`). What is not recovered is an extra loss of about 0.0017 dB
     per pass that hardware shows at feedback 127 beyond the table's own
     8191/8192; it is left out rather than fitted. */
  .chorusMaxMs = 128.0,
  .chorusModulator = XP_CHORUS_MOD_TRIANGLE_UP,
  .chorusFeedbackTap = XP_CHORUS_FB_TAP_LEFT,   /* M-112 */
  .chorusPreDelayTable = 0x038EC8u,
  .chorusRateTable = 0x038B2Eu,
  .chorusRateAccumulator = 65536.0,
  .chorusDepthTable = 0x03856Cu,
  .chorusDepthMaxMs = 12.1,
  .chorusLevelTable = 0x03856Cu,

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
    /* The rhythm sets each group carries, two to a bank, 2699 bytes to a
       set (`02_rom/rhythm_data.md`, all HIGH, bases and stride supplied by
       the rhythm loader `0x0A019D34` itself). Index 15 is the user
       memory's factory contents, which stand in for battery RAM this
       implementation does not have, exactly as bank 7 does for patches. */
    { 0x0b7de0u,   2u, 2699u, 8u, 9u, 64u },   /* 11 rhythm PR-A */
    { 0x0b92f6u,   2u, 2699u, 8u, 9u, 64u },   /* 12 rhythm PR-B */
    { 0x0ba80cu,   2u, 2699u, 8u, 9u, 64u },   /* 13 rhythm PR-C */
    { 0x0bbd22u,   2u, 2699u, 8u, 9u, 64u },   /* 14 rhythm GM */
    { 0x06e720u,   2u, 2699u, 8u, 9u, 64u },   /* 15 rhythm USER default */
  },
  .packedBankCount = 16u,

  /* The four banks a melodic program change reaches, in the firmware's own
     selector order (2, 3, 4, 5 at 0x0A019AE4), and the rhythm set the
     rhythm part reads. The four preset rhythm banks are in the image but
     their bases are not asserted by this project's own extractor yet, so
     only the INIT set is listed rather than guessed at. */
  .packedMelodicBanks = { 3u, 4u, 5u, 6u },
  .packedMelodicBankCount = 4u,
  .packedRhythmBanks = { 11u, 12u, 13u, 14u, 15u, 10u },
  .packedRhythmBankCount = 6u,

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
    /* msb, lsb, patch source, RHYTHM source, group - one group, two
       sources, and the part's own rhythm flag picks between them. The
       pairs are also what the reverse map `0x0A014EBE` (PRG `0x046C1B` /
       `0x046C27`) gives back for each group, which is how a part's latch
       is seeded from its record. */
    { 0x51u, 0x00u, 3u, 11u, 2u },   /* PR-A */
    { 0x51u, 0x01u, 4u, 12u, 3u },   /* PR-B */
    { 0x51u, 0x02u, 5u, 13u, 4u },   /* PR-C */
    { 0x51u, 0x03u, 6u, 14u, 5u },   /* GM */
    { 0x50u, 0x00u, 7u, 15u, 0u },   /* USER, factory contents */
    /* The rest of `0x0A014EF6`'s tree: groups with no image here. They are
       listed because a pair that names an unheld group is REJECTED, where
       a pair that names no group at all falls back to the part record.
       The odd XP LSBs are the same board's second 128. */
    { 0x52u, 0x00u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 1u },  /* CARD */
    { 0x53u, 0x00u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 6u },  /* PCM-A */
    { 0x53u, 0x01u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 7u },  /* PCM-B */
    { 0x54u, 0x00u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 8u },  /* XP-A */
    { 0x54u, 0x01u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 8u },
    { 0x54u, 0x02u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 9u },  /* XP-B */
    { 0x54u, 0x03u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 9u },
    { 0x54u, 0x04u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 10u }, /* XP-C */
    { 0x54u, 0x05u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 10u },
    { 0x54u, 0x06u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 11u }, /* XP-D */
    { 0x54u, 0x07u, XP_PACKED_BANK_NONE, XP_PACKED_BANK_NONE, 11u },
  },
  .packedBankSelectCount = 16u,

  /* FW-EXACT. The per-part program change `0x0A018A0C` resolves the
     part's latched CC0/CC32 through `0x0A014EF6`; when that pair names no
     group it takes the group from the part record's own type (+2) and id
     (+3) through `0x0A014984`, keeping the program number, and after
     loading writes type, id and number back. For type 0 the id indexes
     PRG `0x046C0C` = `00 00 01 02 03 04 05`, so ids 0 and 1 are USER and
     2..6 are CARD, PR-A, PR-B, PR-C, GM; the reverse, `0x0A014ACE`,
     writes group + 1. Types 1 and 2 name installed PCM cards and
     expansion boards. The three demo songs agree: fifteen of the eighteen
     parts they set to ids 3, 4 or 5 are then sent a patch whose name is
     that bank's record at that number, and the other three carry the
     song's own edits. */
  .partFieldPatchGroupType = 2u,
  .partFieldPatchGroupId = 3u,
  .partFieldPatchNumber = 4u,
  .partGroupIdTable = 0x046C0Cu,
  .partGroupIdCount = 7u,

  /* FW-EXACT. GM System On (`F0 7E 7F 09 01`, dispatched at `0x0A016F8C`)
     runs `0x0A00E0A8`, which fills every part from the 20-byte record at
     PRG `0x04575F` with its receive channel set to the part's number -
     receive on, type 0 id 6 (GM) patch 0, level 127, pan 64, reverb send
     100, chorus send 0, full key range - sets the mode byte to 2, and
     loads each part from its record, so part 10 gets GM rhythm set 0. In
     mode 2 every program change is forced to group 5 with its latch set
     to 81/3 (`0x0A018AB6`). The CC7 volume is FW-STRUCT: the controller
     reset `0x0A0137C0` skips volume in mode 2, and both mode-aware writers
     of the volume table (`0x0A014818`, `0x0A0192BC`, the second directly
     after it calls GM System On) write 100; which of the two a MIDI GM
     System On leaves behind is not traced. */
  .gmPartTemplate = 0x04575Fu,
  .gmBankSelect = 3u,
  .gmVolume = 100u,

  /* FW-EXACT, and confirmed by the owner on his own unit: a JV-1080 at
     factory settings powers on in PATCH mode on USER:001 "Symphonique",
     patch receive channel 1. The system-area image at PRG `0x0BE0DA`
     stores USER:014, but its field 47 (Power Up Mode) is 0 = DEFAULT, and
     the boot routine `0x0A0191AE` then rewrites the selection fields to
     PATCH, USER (type 0, id 1), number 0, performance 0. The hardware's
     own patch-selection readbacks read USER / id 1 / #0.

     Also in that image and not modelled, because this engine has nothing
     that reads them: control channel 16 (performance select), and the
     EFX, chorus and reverb switches, all on. Nor is PATCH mode's gating:
     every part here sounds whatever the mode, which is what a song that
     configures a performance by DT1 needs. */
  .powerOnMode = XP_POWER_ON_PATCH,
  .powerOnPatchGroupId = 1u,
  .powerOnPatchNumber = 0u,
  .powerOnPatchChannel = 0u,

  /* NOT A HARDWARE BEHAVIOUR: a choice for playback. Patch mode plays one
     patch on one channel, and a MIDI file that sends no GM System On of
     its own expects a multitimbral GM device. This device has no GS mode,
     so its GM mode is the one multitimbral state it defines for a file it
     knows nothing about (manual p.76). The factory demo songs were
     recorded in Performance mode and configure it by DT1; they are
     compared with no host reset. */
  .hostResetEntersGm = true,

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
    .panKeyFollow = 0x78u,
    .randomPanDepth = 0x79u,
    .alternatePanDepth = 0x7au,
    .coarseTune = 0x3du,
    .fineTune = 0x3eu,
    .randomPitchDepth = 0x3fu,
    .benderSwitch = 0x13u,
    .holdSwitch = 0x12u,
    .benderRange = XP_VOICE_FIELD_NONE,
    .ampVelocitySens = 0x6au,
    .ampVelocityCurve = 0x69u,
    .lfoFirst = { 0x2du, 0x35u },
    .pitchLfoDepth = 0x4eu,
    .filterLfoDepth = 0x63u,
    .ampLfoDepth = 0x75u,
    .panLfoDepth = 0x7bu,
    .matrixFirst = 0x15u,
    .envelopeMode = XP_VOICE_FIELD_NONE,
    .toneDelayMode = 0x09u,
    .toneDelayTime = 0x0au,
    .pitchKeyFollow = 0x40u,
    .cutoffKeyFollow = 0x52u,
    .sourceKey = XP_VOICE_FIELD_NONE,
    /* MIX / EFX / OUTPUT1 / OUTPUT2 (`05_data_model/partial_schema.md`,
       PRG 0x057158). A tone has no PATCH value - only a part does. */
    .outputAssign = 0x7du,
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
    .pitchEnvDepth = 0x41u,
    .pitchEnvVelSens = 0x42u,
    .pitchEnvVelTime1 = 0x43u,
    .pitchEnvVelTime4 = 0x44u,
    .pitchEnvTimeKeyFollow = 0x45u,
    .pitchEnvTime1 = 0x46u,
    .pitchEnvLevel1 = 0x4au,
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
    .panKeyFollow = XP_VOICE_FIELD_NONE,
    .randomPanDepth = 0x34u,
    .alternatePanDepth = 0x35u,
    .coarseTune = XP_VOICE_FIELD_NONE,
    .fineTune = 0x0du,
    .randomPitchDepth = 0x0eu,
    .benderSwitch = XP_VOICE_FIELD_NONE,
    .holdSwitch = 0x0au,
    .benderRange = 0x06u,
    .ampVelocitySens = 0x2au,
    .ampVelocityCurve = XP_VOICE_FIELD_NONE,
    .lfoFirst = { XP_VOICE_FIELD_NONE, XP_VOICE_FIELD_NONE },
    .pitchLfoDepth = XP_VOICE_FIELD_NONE,
    .filterLfoDepth = XP_VOICE_FIELD_NONE,
    .ampLfoDepth = XP_VOICE_FIELD_NONE,
    .panLfoDepth = XP_VOICE_FIELD_NONE,
    .matrixFirst = XP_VOICE_FIELD_NONE,
    .envelopeMode = 0x08u,
    .toneDelayMode = XP_VOICE_FIELD_NONE,
    .toneDelayTime = XP_VOICE_FIELD_NONE,
    .pitchKeyFollow = XP_VOICE_FIELD_NONE,
    .cutoffKeyFollow = XP_VOICE_FIELD_NONE,
    .sourceKey = 0x0cu,
    .outputAssign = 0x36u,       /* the rhythm note's own, same four values */
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
    .pitchEnvDepth = 0x0fu,
    .pitchEnvVelSens = 0x10u,
    .pitchEnvVelTime1 = 0x11u,
    .pitchEnvVelTime4 = XP_VOICE_FIELD_NONE,
    .pitchEnvTimeKeyFollow = XP_VOICE_FIELD_NONE,
    .pitchEnvTime1 = 0x12u,
    .pitchEnvLevel1 = 0x16u,
    .ampTime1 = 0x2cu,
    .ampLevel1 = 0x30u,
    .keyRangeLow = XP_VOICE_FIELD_NONE,
    .keyRangeHigh = XP_VOICE_FIELD_NONE,
    .velocityRangeLow = XP_VOICE_FIELD_NONE,
    .velocityRangeHigh = XP_VOICE_FIELD_NONE,
    .muteGroup = 0x07u,
  },

  /* PITCH KEY FOLLOW. The tone's field 0x40 indexes the sixteen-entry list
     at PRG `0x057541` - `-100-70 -50 -30 -10 0   +10 ... +200`, four
     characters each, the panel's own strings - and the displayed
     percentage is the law: MEASURED (`M-014`) exact at indices 0, 5, 12
     and 15 as -100, 0, +100 and +200 cents per key, and at index 10 (+50)
     on the first factory song's part 6, whose tone 1 sounds 5 semitones
     under its unison at key 70 on the hardware take (TASK-356). That
     reading also puts the pivot at key 60, as `M-077` has it for every
     key-scaling field on this machine.

     CUTOFF KEY FOLLOW, the tone's field 0x52, indexes the same list, and
     the displayed percentage is again the law: MEASURED (`M-077`) at
     indices 0, 5, 12 and 15 as -1.003, +0.002, +0.991 and +2.03 octaves of
     corner per octave of key about key 60. The rhythm note has neither
     field. */
  .keyFollowTable = 0x057541u,
  .keyFollowCount = 16u,
  .keyFollowWidth = 4u,

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
  /* MEASURED (`M-014`): bend is linear in the 14-bit value, scaled by the
     range on its own side - +-200 cents at 2, +-1200 at 12, and -2400 at
     half of an asymmetric down range of 48. */
  .patchFieldBendUp = 0x31u,
  .patchFieldBendDown = 0x32u,
  .patchFieldControlSource2 = 0x3au,
  .patchFieldControlSource3 = 0x3bu,
  .patchFieldVelocityRangeSwitch = 0x40u,

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
  /* Field 10 of the performance part, five values MIX / EFX / OUTPUT1 /
     OUTPUT2 / PATCH (PRG 0x057158). In the factory demo songs the parts
     that read PATCH are exactly the ones whose tones then say EFX, which
     is the law being self-consistent across sixteen parts and three
     songs. */
  .partFieldOutputAssign = 10u,
  .partOutputAssignPatch = 4u,
  .partFieldReverbSend = 0x0du,
  .partFieldChorusSend = 0x0cu,
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
  /* THE INSERT EFFECT'S PROGRAM BANK. 32 slots at 0x0400BC on a 0x270
     stride, each 104 u32 BE program words then 104 u16 BE coefficients -
     0x1A0 + 0xD0 = 0x270, tiling the stride exactly - and the bank ends at
     0x044EBC, which IS where the type table starts, to the byte. The table
     is 46 rows of two pointers, indexed by the 0-based type; row 46 reads
     0x7F7F7F7D, which is not an address, and ends it. All 46 rows resolve
     to a slot base with the coefficient pointer exactly 0x1A0 above the
     program one, they reach 31 distinct slots, and all 3328 program words
     have top nibble zero - the 28 bits the RAM test implies
     (`08_effects/dsp_program.md`, FW-EXACT). */
  .efxBankBase = 0x0400BCu,
  .efxSlotStride = 0x270u,
  .efxSlotCount = 32u,
  .efxTypeTable = 0x044EBCu,
  .efxTypeCount = 46u,
  .efxPointerBase = 0x0A000000u,
  .efxLevelTable = 0x03856Cu,

  /* THE PARAMETER CONVERSION TABLES, as this device's own extraction lists
     them (`02_rom/extracted/dsp/conversion_tables.json`). Reading them back
     out of the ROM with the geometry below reproduces that extraction
     byte for byte on all nineteen.

     TWO OF THE COUNTS OVERRUN THE TABLE THAT FOLLOWS: 0x038A32's 128
     entries reach two words into 0x038B2E, and 0x038E12's reach 37 words
     into 0x038EC8. The counts are kept as the extraction has them, because
     a parameter running 0..127 does index that far and the firmware packs
     these tight; it is recorded here rather than trimmed away. */
  .efxTables = {
    { 0x03856Cu, 178u, 1u },     /* 0  master level / gain, 0..0x1FFF */
    { 0x038732u, 128u, 1u },     /* 1  level family */
    { 0x038832u, 128u, 1u },     /* 2  level family */
    { 0x038932u, 128u, 1u },     /* 3  level family */
    { 0x038A32u, 128u, 1u },     /* 4  non-monotonic coefficient list */
    { 0x038B2Eu, 128u, 1u },     /* 5  LFO rate / short time */
    { 0x038C2Eu, 126u, 1u },     /* 6  rotary speed family */
    { 0x038D2Au, 116u, 1u },     /* 7  */
    { 0x038E12u, 128u, 1u },     /* 8  bit-15 tagged delay/offset */
    { 0x038EC8u, 128u, 1u },     /* 9  pre-delay, 1..3296 = 103 ms */
    { 0x038FC8u, 127u, 1u },     /* 10 delay, 1..16000 = 500 ms */
    { 0x0390C6u, 116u, 1u },     /* 11 long delay, 200..1000 ms */
    { 0x0391AEu, 121u, 1u },     /* 12 long delay, 200..1000 ms */
    { 0x0392A0u, 128u, 2u },     /* 13 pan, (L, R) */
    { 0x0394A0u, 128u, 2u },     /* 14 balance, (wet, dry) */
    { 0x039700u,  18u, 2u },     /* 15 HF damp one-pole pairs */
    { 0x03D014u, 129u, 1u },     /* 16 auto-wah family */
    { 0x03EBB8u, 128u, 1u },     /* 17 compressor family */
    { 0x03ECD8u, 113u, 1u },     /* 18 compressor family */
  },
  .efxTableCount = 19u,

  .voiceEngine = &JV1080_VOICE_ENGINE,
};

}  // extern "C"
