/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  The SC-88's output stage: everything between the last firmware-exact
 *  sample and the connector.
 *
 *  libEmuSC models the digital machine. A real module then puts those
 *  samples through a converter and an analog board, and this file is the
 *  single place for that class of thing on the SC-88 - the same role
 *  analog_stage.cc plays for the devices that run through synth.cc. The
 *  SC-88's voice path is the sc88_* C engine and never reaches synth.cc,
 *  so it needs its own.
 *
 *  Nothing here is [FW-EXACT]. Every section is [MEASURED], carries the
 *  measurement that produced it, and is meant to be deleted the day the
 *  mechanism it stands in for is modelled properly. Keeping such a term
 *  here rather than folded into a ROM-derived law is the whole point:
 *  one labelled place, easy to find and easy to remove.
 */
#ifndef SC88_OUTPUT_H
#define SC88_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sc88_output_section_type {
  SC88_OUTPUT_LOW_SHELF,
  SC88_OUTPUT_PEAKING,
  SC88_OUTPUT_HIGH_SHELF
};

struct sc88_output_section {
  enum sc88_output_section_type type;
  float frequency;
  float gain_db;
  float q;
};

struct sc88_output_biquad {
  float b0, b1, b2, a1, a2;
  float x1[2], x2[2], y1[2], y2[2];
};

#define SC88_OUTPUT_MAX_SECTIONS 4

/* The converter hold, as a symmetric FIR. 31 taps realises the target to
   0.003 dB through 13.9 kHz and 0.05 dB at 15 kHz at the chip's own
   32 kHz, 0.007 dB across the whole band at 44.1 kHz; only the last few
   hundred hertz before Nyquist fall short, where the target is a cusp.
   The cost is a constant group delay of SC88_OUTPUT_HOLD_TAPS / 2
   samples on the whole render, 0.47 ms at 32 kHz. */
#define SC88_OUTPUT_HOLD_TAPS 31

struct sc88_output {
  struct sc88_output_biquad section[SC88_OUTPUT_MAX_SECTIONS];
  unsigned sections;
  /* The DAC's zero-order hold. See sc88_output.c for the measurement. */
  float hold[SC88_OUTPUT_HOLD_TAPS];
  float hold_z[2][SC88_OUTPUT_HOLD_TAPS];
  unsigned hold_taps;
  unsigned hold_pos;
  /* The DC blocker, which lived inline in sc88_device.c until it was
     moved here. It is not cited to any ROM either, so it belongs in the
     labelled stage rather than unmarked in the middle of the render. */
  float dc_pole;
  float dc_x[2], dc_y[2];
  bool enabled;
};

/* Designs the profile at `rate` and clears the state. */
void sc88_output_init(struct sc88_output *out, double rate);
void sc88_output_reset(struct sc88_output *out);
void sc88_output_process(struct sc88_output *out, float *stereo,
                         size_t frames);

/* The profile itself, exposed so a test can assert its response. */
extern const struct sc88_output_section SC88_OUTPUT_RESPONSE[];
extern const unsigned SC88_OUTPUT_RESPONSE_SECTIONS;

/* The converter's hold, in hertz: the rate the DAC is clocked at. */
#define SC88_OUTPUT_DAC_RATE 32000.0

/* The analog board's own poles, as the R and C that make them. See
   sc88_output.c for the schematic they are read from. */
#define SC88_OUTPUT_ANALOG_SECTIONS 5
struct sc88_output_rc { double r_ohm, c_farad; };
extern const struct sc88_output_rc
  SC88_OUTPUT_ANALOG[SC88_OUTPUT_ANALOG_SECTIONS];

#ifdef __cplusplus
}
#endif

#endif
