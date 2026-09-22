# GP/XP SSE1 Performance Plan

## Goal and policy

Gain live-MIDI headroom on Pentium III-class, SSE1-only systems without audibly degrading synthesis. Preserve the existing device-profile architecture: all engine changes remain generic and capability-based, with no device names or device-specific branches added to common DSP code.

Use two optimization tiers:

- **Lossless/default:** output-preserving changes enabled on every platform. Generic scalar implementations remain available; SIMD may be selected where supported only when it preserves the scalar arithmetic order and output.
- **Legacy-fast:** theoretically inaudible approximations enabled by default for 32-bit Windows builds, with an explicit opt-out. Other platforms may opt in. Do not apply this tier automatically to modern targets.

Do not use a blanket `-ffast-math`; any relaxed arithmetic must be narrowly scoped and pass the fidelity gates below.

## Prioritized implementation

### 1. XP output FIR: remove division from the tap loop (lossless/default)

Replace `(position + tap_count - tap) % tap_count` in the 31-tap stereo hold filter with a descending ring index and conditional wrap. Preserve the current tap traversal and per-channel accumulation order exactly.

This removes roughly 2.7 million integer divisions per second at 44.1 kHz and is the highest-confidence first change.

### 2. XP TVA envelope: eliminate per-sample transcendental calls (legacy-fast)

At each 125 Hz control tick, calculate the exact exponential envelope value and a per-sample multiplicative step. Advance the envelope by recurrence between ticks, then resynchronize at the next tick so numerical drift is bounded to one control interval. Preserve stage transitions, event timing, and voice lifetime exactly.

Benchmark and validate this independently because a 64-voice render can currently approach 2.8 million `exp()` calls per second.

### 3. XP oscillator: replace x87 phase work (legacy-fast)

Implement a fixed-point phase accumulator that exposes an integer sample index and fractional position without `double`-to-integer control-word changes. Evaluate the existing cubic B-spline interpolation in `float`, using scalar SSE1 on the 32-bit target. Keep loop behavior and sample addressing unchanged.

Choose sufficient fractional precision that pitch-step quantization remains below the fidelity thresholds. Retain the current double path when legacy-fast mode is disabled.

### 4. GP resampler: simplify phase and ring access

- **Lossless/default:** restructure the stereo/ring traversal so wrap handling is outside the inner multiply-accumulate path, while preserving each channel's tap order. Use a generic implementation on all platforms and exact SIMD packing only where it remains output-identical.
- **Legacy-fast:** replace double/floor phase tracking with fixed-point phase. Four-wide tap reassociation is permitted only if it passes the approximate-output gates.

### 5. GP wave oscillator: reduce interpolation overhead (lossless/default)

Inline the hot interpolation kernel, compute its clamped tap indices once per sample, and preconvert Q12 interpolation coefficients to `float` constants. The conversions are exact because the coefficient integers fit exactly in `float` and the scale is a power of two. Hoist other exact power-of-two scaling, including pitch-bend normalization, without changing evaluation order.

### 6. XP delay and chorus

- **Delay, lossless/default:** decompose each fixed delay tap into whole and fractional offsets when parameters change. At render time, use precomputed offsets and branch-based wrapping while retaining the existing arithmetic precision and interpolation order.
- **Chorus, legacy-fast:** replace the per-frame `sin()` call with a lookup table or recursive oscillator with periodic exact resynchronization. Use float tap-position arithmetic only in legacy-fast mode.

### 7. Stereo filter packing

Add generic paired-channel kernels for independent left/right biquads where lane packing preserves each channel's arithmetic order. Enable an SSE1 implementation when available and retain a scalar fallback everywhere. Treat this as lossless/default only after byte-identical validation; otherwise place the affected kernel in legacy-fast mode.

Apply the same rule to the GP analog-stage biquads: a float-state version belongs only in legacy-fast mode unless it proves byte-identical.

### 8. Link-time optimization

Probe and enable IPO/LTO for supported release toolchains only after the complete reference corpus remains byte-identical. Apply it consistently to the shared library, static library, direct-source tools, and tests. If a supported toolchain changes output, keep LTO out of the default tier and evaluate it under legacy-fast instead.

### Deferred work

After the items above, profile XP TVF coefficient updates. Consider table or polynomial replacements for `exp2`, `asin`, and `sin` only in legacy-fast mode and only if coefficient recomputation remains a meaningful hotspot. Do not pursue thread-parallel voices, reduced interpolation order, reduced effect update rates, or lower polyphony as initial optimizations because they add synchronization overhead or carry greater audible risk.

## Build interface

Add a CMake option named `EMUSC_LEGACY_DSP_FAST`:

- Default `ON` when `WIN32 && CMAKE_SIZEOF_VOID_P == 4`.
- Default `OFF` everywhere else.
- Allow users to override either default explicitly.
- Propagate one compile definition consistently to all targets compiling engine sources, including shared/static libraries and XP tools/tests that compile sources directly.

Keep every lossless optimization independent of this flag and enabled by default on all platforms.

Expose cache variables for the 32-bit MinGW architecture and tuning flags, defaulting to `pentium3` and `generic`. This preserves the SSE1 baseline while allowing builders to select a more specific compatible processor. Do not add runtime or compile-time device-model checks.

## Verification and acceptance gates

Measure every optimization independently before combining it, using dense live-MIDI workloads at maximum practical voice count with chorus, delay, reverb, and output filtering enabled. Record real-time render ratio, worst callback duration, and underruns on actual Pentium III-class hardware; modern-machine throughput is useful for regression detection but is not sufficient acceptance evidence.

### Lossless/default gate

- Byte-identical rendered WAV output for the existing SC-55, SC-55mkII, JV-880, and SC-88 reference corpus at 32, 44.1, and 48 kHz.
- Identical event timing, voice allocation/lifetime, frame count, and deterministic output.
- All existing unit, integration, determinism, and audio comparison tests pass.
- Retain only changes with a repeatable performance improvement or a clearly demonstrated removal of expensive operations from a hot path.

### Legacy-fast fidelity gate

- Event timing, voice allocation/lifetime, stage transitions, and frame count remain identical.
- Converted 16-bit output differs by at most 1 LSB, with no more than 0.1% of samples differing across the full corpus.
- Null residual RMS remains below the 16-bit noise floor and spectral analysis shows no correlated tonal residue, pitch bias, periodic spur, unstable filter behavior, or envelope discontinuity.
- Existing hardware/reference spectral and envelope metrics do not regress.
- Long sustained notes, pitch bends, looping samples, maximum-polyphony passages, effect-heavy material, silence, and parameter-boundary cases are included.

If a candidate misses the legacy-fast gate, reject it or narrow it until it passes; do not weaken the gate.

## Delivery order

Implement and benchmark in this sequence: XP FIR, GP oscillator, XP delay, GP resampler lossless work, stereo filter packing, XP TVA recurrence, XP fixed-point oscillator, chorus approximation, then LTO. Land each logically independent change separately so its performance and fidelity evidence can be reviewed or reverted without affecting the others.

## Assumptions

- The primary constrained target is 32-bit Windows on Pentium III-class SSE1 hardware.
- Live MIDI rendering, rather than offline bulk rendering, determines success.
- Small numerical deviations are acceptable only under `EMUSC_LEGACY_DSP_FAST` and only when they satisfy the stated theoretical-inaudibility tests.
- Lossless optimizations are cross-platform, enabled by default, and never hidden behind the Win32 legacy flag.
- No implementation may weaken runtime device injection or add device-specific knowledge to shared engine code.
