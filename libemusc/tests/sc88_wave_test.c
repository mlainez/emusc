/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_wave.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_descriptor(void)
{
  static const uint8_t raw[20] = {
    0x00, 0x01, 0xee, 0x60, 0x00, 0x20, 0x24, 0x02, 0x6f, 0x9c,
    0x00, 0x02, 0x92, 0x0d, 0x00, 0x00, 0x0a, 0x00, 0xf4, 0x30
  };
  struct sc88_wave_descriptor desc;
  struct sc88_wave_registers registers;
  enum sc88_wave_loop_type mode;

  assert(sc88_wave_descriptor_parse(raw, sizeof raw, &desc));
  assert(desc.bank_select == 0);
  assert(desc.address_a == 0x01ee60);
  assert(desc.address_b == 0x026f9c);
  assert(desc.address_c == 0x02920d);
  assert(desc.root_key == 0x24);
  assert(desc.base_pitch_correction == 0x20);
  assert(desc.alternate_pitch_correction == 0);
  assert(desc.start_offset == 0x0a00);
  assert(desc.state_a == -3024);
  assert(sc88_wave_descriptor_loop_type(&desc, &mode));
  assert(mode == SC88_WAVE_FORWARD_LOOP);
  assert(sc88_wave_prepare_registers(&desc, false, &registers));
  assert(registers.bank_flags == 0x8000);
  assert(registers.start == 0x01f860);
  assert(registers.loop == 0x026f9c);
  assert(registers.end == 0x02920d);
  assert(registers.state_a == 0x0003f430);
  assert(registers.initial_state == 0x18);
}

static void test_decoder(void)
{
  uint8_t *bank = (uint8_t *)calloc(SC88_WAVE_BANK_SIZE, 1);
  struct sc88_fce_decoder decoder;
  int32_t pcm;

  assert(bank);
  bank[0x400] = 0x21;
  bank[0x8000] = 1;
  bank[0x8001] = 0xff;
  assert(sc88_fce_decoder_reset(&decoder, 0x8000));
  assert(sc88_fce_decoder_read(&decoder, bank, SC88_WAVE_BANK_SIZE, &pcm));
  assert(pcm == 256);
  assert(sc88_fce_decoder_read(&decoder, bank, SC88_WAVE_BANK_SIZE, &pcm));
  assert(pcm == 0);

  decoder.accumulator = INT64_C(0x10000);
  assert(sc88_fce_decoder_read(&decoder, bank, SC88_WAVE_BANK_SIZE, &pcm));
  assert(pcm == 0x7fffff);
  free(bank);
}

static void test_descramble(void)
{
  uint8_t *raw = (uint8_t *)calloc(SC88_WAVE_CHIP_SIZE, 1);
  uint8_t *decoded = (uint8_t *)calloc(SC88_WAVE_CHIP_SIZE, 1);

  assert(raw && decoded);
  raw[0] = 0x5a;
  raw[0x20] = 0x01;
  raw[SC88_WAVE_BANK_SIZE] = 0xa5;
  assert(sc88_wave_descramble_chip(raw, SC88_WAVE_CHIP_SIZE, decoded,
                                    SC88_WAVE_CHIP_SIZE));
  assert(decoded[0] == 0x5a);
  assert(decoded[0x100] == 0x02);
  assert(decoded[SC88_WAVE_BANK_SIZE] == 0xa5);
  assert(!sc88_wave_descramble_chip(raw, SC88_WAVE_CHIP_SIZE, raw,
                                     SC88_WAVE_CHIP_SIZE));
  free(decoded);
  free(raw);
}

static void test_held_chip(const char *raw_path, const char *decoded_path)
{
  FILE *file;
  uint8_t *raw = (uint8_t *)malloc(SC88_WAVE_CHIP_SIZE);
  uint8_t *decoded = (uint8_t *)malloc(SC88_WAVE_CHIP_SIZE);
  uint8_t *expected = (uint8_t *)malloc(SC88_WAVE_CHIP_SIZE);

  assert(raw && decoded && expected);
  file = fopen(raw_path, "rb");
  assert(file);
  assert(fread(raw, 1, SC88_WAVE_CHIP_SIZE, file) == SC88_WAVE_CHIP_SIZE);
  assert(fgetc(file) == EOF);
  fclose(file);
  file = fopen(decoded_path, "rb");
  assert(file);
  assert(fread(expected, 1, SC88_WAVE_CHIP_SIZE, file) ==
         SC88_WAVE_CHIP_SIZE);
  assert(fgetc(file) == EOF);
  fclose(file);
  assert(sc88_wave_descramble_chip(raw, SC88_WAVE_CHIP_SIZE, decoded,
                                    SC88_WAVE_CHIP_SIZE));
  assert(memcmp(decoded, expected, SC88_WAVE_CHIP_SIZE) == 0);
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
    assert(sc88_wave_cursor_current(cursor, &address));
    assert(address == expected[i]);
    assert(sc88_wave_cursor_advance(cursor));
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

  assert(sc88_wave_cursor_init(&cursor, &registers,
                               SC88_WAVE_FORWARD_LOOP));
  expect_cursor(&cursor, forward, sizeof forward / sizeof forward[0]);

  assert(sc88_wave_cursor_init(&cursor, &registers,
                               SC88_WAVE_PING_PONG_LOOP));
  expect_cursor(&cursor, ping_pong, sizeof ping_pong / sizeof ping_pong[0]);

  assert(sc88_wave_cursor_init(&cursor, &registers,
                               SC88_WAVE_FORWARD_ONE_SHOT));
  expect_cursor(&cursor, one_shot, sizeof one_shot / sizeof one_shot[0]);
  assert(!sc88_wave_cursor_current(&cursor, &address));
  assert(!sc88_wave_cursor_init(&cursor, &registers,
                                SC88_WAVE_REVERSE_ONE_SHOT));
}

int main(int argc, char **argv)
{
  test_descriptor();
  test_descramble();
  test_decoder();
  test_cursors();
  if (argc == 3)
    test_held_chip(argv[1], argv[2]);
  return 0;
}
