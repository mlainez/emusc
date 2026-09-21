/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/wave.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

static void test_descriptor(void)
{
  static const uint8_t raw[20] = {
    0x00, 0x01, 0xee, 0x60, 0x00, 0x20, 0x24, 0x02, 0x6f, 0x9c,
    0x00, 0x02, 0x92, 0x0d, 0x00, 0x00, 0x0a, 0x00, 0xf4, 0x30
  };
  struct sc88_wave_descriptor desc;
  struct sc88_wave_registers registers;
  enum sc88_wave_loop_type mode;

  assert(wave_descriptor_parse(&SC88_PROFILE, raw, sizeof raw, &desc));
  assert(desc.bank_select == 0);
  assert(desc.address_a == 0x01ee60);
  assert(desc.address_b == 0x026f9c);
  assert(desc.address_c == 0x02920d);
  assert(desc.root_key == 0x24);
  assert(desc.base_pitch_correction == 0x20);
  assert(desc.alternate_pitch_correction == 0);
  assert(desc.start_offset == 0x0a00);
  assert(desc.state_a == -3024);
  assert(wave_descriptor_loop_type(&desc, &mode));
  assert(mode == XP_WAVE_FORWARD_LOOP);
  assert(wave_prepare_registers(&SC88_PROFILE, &desc, false, &registers));
  assert(registers.bank_flags == 0x8000);
  assert(registers.start == 0x01f860);
  assert(registers.loop == 0x026f9c);
  assert(registers.end == 0x02920d);
  assert(registers.state_a == 0x0003f430);
  assert(registers.initial_state == 0x18);
}

/* The tuning of a zone is BOTH of the descriptor's pitch corrections, and
   both of them are signed and wrap in a 16-bit accumulator. The descriptor
   here is the ROM's own 0x3b4ac - the `SITAR` zone `Charang` sounds at C5 -
   whose `+4` is -559 and whose `+14` is +132; -427 is what puts its 45-frame
   loop on F5 to a fifth of a cent, where -559 alone leaves it 9.8 cents
   flat. */
static void test_pitch_correction(void)
{
  static const uint8_t raw[20] = {
    0x31, 0x0b, 0x04, 0x40, 0xfd, 0xd1, 0x4d, 0x0b, 0x09, 0x88,
    0x00, 0x0b, 0x09, 0xb4, 0x00, 0x84, 0x04, 0x5b, 0x09, 0xf8
  };
  struct sc88_wave_descriptor desc;

  assert(wave_descriptor_parse(&SC88_PROFILE, raw, sizeof raw, &desc));
  assert(desc.root_key == 77);
  assert(desc.base_pitch_correction == -559);
  assert(desc.alternate_pitch_correction == 132);
  assert(wave_pitch_correction(&desc, false) == -559);
  assert(wave_pitch_correction(&desc, true) == -427);

  desc.base_pitch_correction = -1365;
  desc.alternate_pitch_correction = -442;
  assert(wave_pitch_correction(&desc, true) == -1807);

  /* Neither case above actually leaves the int16 range - the header
     comment's wraparound claim needs a sum that does. Two positives
     summing past +32767 land negative; two negatives summing past
     -32768 land positive - a clamping implementation gives +32767 or
     -32768 here instead, and a naive 32-bit sum truncated the wrong way
     gives neither. */
  desc.base_pitch_correction = 20000;
  desc.alternate_pitch_correction = 20000;
  assert(wave_pitch_correction(&desc, true) == -25536);
  desc.base_pitch_correction = -30000;
  desc.alternate_pitch_correction = -10000;
  assert(wave_pitch_correction(&desc, true) == 25536);
}

static void test_decoder(void)
{
  uint8_t *bank = (uint8_t *)calloc(SC88_PROFILE.waveBankSize, 1);
  struct sc88_fce_decoder decoder;
  int32_t pcm;

  assert(bank);
  bank[0x400] = 0x21;
  bank[0x8000] = 1;
  bank[0x8001] = 0xff;
  assert(fce_decoder_reset(&SC88_PROFILE, &decoder, 0x8000));
  assert(fce_decoder_read(&decoder, bank, SC88_PROFILE.waveBankSize, &pcm));
  assert(pcm == 256);
  assert(fce_decoder_read(&decoder, bank, SC88_PROFILE.waveBankSize, &pcm));
  assert(pcm == 0);

  decoder.accumulator = INT64_C(0x10000);
  assert(fce_decoder_read(&decoder, bank, SC88_PROFILE.waveBankSize, &pcm));
  assert(pcm == 0x7fffff);
  free(bank);
}

static void test_descramble(void)
{
  uint8_t *raw = (uint8_t *)calloc(SC88_PROFILE.waveChipSize, 1);
  uint8_t *decoded = (uint8_t *)calloc(SC88_PROFILE.waveChipSize, 1);

  assert(raw && decoded);
  raw[0] = 0x5a;
  raw[0x20] = 0x01;
  raw[SC88_PROFILE.waveBankSize] = 0xa5;
  assert(wave_descramble_chip(&SC88_PROFILE, raw, SC88_PROFILE.waveChipSize,
                               decoded, SC88_PROFILE.waveChipSize));
  assert(decoded[0] == 0x5a);
  assert(decoded[0x100] == 0x02);
  assert(decoded[SC88_PROFILE.waveBankSize] == 0xa5);
  assert(!wave_descramble_chip(&SC88_PROFILE, raw, SC88_PROFILE.waveChipSize,
                                raw, SC88_PROFILE.waveChipSize));
  free(decoded);
  free(raw);
}

static void test_held_chip(const char *raw_path, const char *decoded_path)
{
  FILE *file;
  uint8_t *raw = (uint8_t *)malloc(SC88_PROFILE.waveChipSize);
  uint8_t *decoded = (uint8_t *)malloc(SC88_PROFILE.waveChipSize);
  uint8_t *expected = (uint8_t *)malloc(SC88_PROFILE.waveChipSize);

  assert(raw && decoded && expected);
  file = fopen(raw_path, "rb");
  assert(file);
  assert(fread(raw, 1, SC88_PROFILE.waveChipSize, file) ==
         SC88_PROFILE.waveChipSize);
  assert(fgetc(file) == EOF);
  fclose(file);
  file = fopen(decoded_path, "rb");
  assert(file);
  assert(fread(expected, 1, SC88_PROFILE.waveChipSize, file) ==
         SC88_PROFILE.waveChipSize);
  assert(fgetc(file) == EOF);
  fclose(file);
  assert(wave_descramble_chip(&SC88_PROFILE, raw, SC88_PROFILE.waveChipSize,
                               decoded, SC88_PROFILE.waveChipSize));
  assert(memcmp(decoded, expected, SC88_PROFILE.waveChipSize) == 0);
  free(expected);
  free(decoded);
  free(raw);
}

static void expect_cursor(struct sc88_wave_cursor *cursor,
                          const uint32_t *expected, size_t count)
{
  size_t i;
  uint32_t address;
  for (i = 0; i < count; ++i) {
    assert(wave_cursor_current(cursor, &address));
    assert(address == expected[i]);
    assert(wave_cursor_advance(cursor));
  }
}

static void test_cursors(void)
{
  struct sc88_wave_registers registers = {0, 8, 10, 12, 0, 0x18};
  struct sc88_wave_cursor cursor;
  static const uint32_t forward[] = {8, 9, 10, 11, 12, 10, 11};
  static const uint32_t ping_pong[] = {
    8, 9, 10, 11, 12, 12, 11, 10, 10, 11, 12
  };
  static const uint32_t one_shot[] = {8, 9, 10, 11, 12};
  uint32_t address;

  assert(wave_cursor_init(&SC88_PROFILE, &cursor, &registers,
                          XP_WAVE_FORWARD_LOOP));
  expect_cursor(&cursor, forward, sizeof forward / sizeof forward[0]);

  assert(wave_cursor_init(&SC88_PROFILE, &cursor, &registers,
                          XP_WAVE_PING_PONG_LOOP));
  expect_cursor(&cursor, ping_pong, sizeof ping_pong / sizeof ping_pong[0]);

  assert(wave_cursor_init(&SC88_PROFILE, &cursor, &registers,
                          XP_WAVE_FORWARD_ONE_SHOT));
  expect_cursor(&cursor, one_shot, sizeof one_shot / sizeof one_shot[0]);
  assert(!wave_cursor_current(&cursor, &address));
  assert(!wave_cursor_init(&SC88_PROFILE, &cursor, &registers,
                           XP_WAVE_REVERSE_ONE_SHOT));
}

int main(int argc, char **argv)
{
  test_descriptor();
  test_pitch_correction();
  test_descramble();
  test_decoder();
  test_cursors();
  if (argc == 3)
    test_held_chip(argv[1], argv[2]);
  return 0;
}
