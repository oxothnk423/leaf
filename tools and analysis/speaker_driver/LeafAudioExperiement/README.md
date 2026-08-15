# LeafAudioExperiement

Arduino audio test rig for exploring firmware-only improvements to Leaf's vario
sound on an ESP32-S3 WROOM-1 dev kit.

The sketch name intentionally matches the folder name, including the original
`Experiement` spelling, so the Arduino IDE can open it directly.

## Hardware

- GPIO10: piezo terminal A
- GPIO11: piezo terminal B, complementary/inverted PWM output
- GPIO4: analog climb-rate input

The current voltage mapping is:

- 0 mV: -10.65 m/s
- 3098 mV: +12.02 m/s

This covers Leaf's full pitch clamp range. Keep GPIO4 between GND and 3.3 V.
Use a voltage divider if the source can exceed 3.3 V.

## Experiment History

1. DRV2667 bring-up
   - Created a basic DRV2667 Arduino sketch using I2C on GPIO8/GPIO9.
   - Added serial commands for frequency and volume testing.
   - Confirmed I2C wiring after an initial no-ACK condition caused by a loose wire.
   - Verified the device ACKed at address `0x59`.

2. DRV2667 synth mode
   - Tested the DRV2667 internal waveform/synth path.
   - Some frequencies sounded smooth, while others had audible ringing, overtones,
     or low-frequency clicking artifacts.
   - Around 1000 Hz sounded relatively clean, while nearby tones could sound worse.

3. DRV2667 FIFO mode
   - Tried FIFO playback to see whether streaming sample data could produce a
     smoother sine-like output.
   - Initial FIFO frequency updates were unreliable.
   - Even when tones began smoothly, objectionable ringing or overtone behavior
     appeared after a short time.

4. Sweep and PWM comparison
   - Added sweep-style testing and parallel ESP32 PWM output on GPIO10/GPIO11.
   - Confirmed the ESP32 square-wave piezo drive sounded better than the DRV2667
     output for this prototype setup.
   - Decided to focus on firmware changes to improve the perceived quality of
     Leaf's existing square-wave audio method.

5. Leaf vario simulator
   - Created this `LeafAudioExperiement` sketch.
   - Copied Leaf's climb/sink pitch and timing math into the sketch.
   - Used GPIO10/GPIO11 as complementary PWM outputs, matching Leaf's current
     piezo drive style.
   - Used an analog input to simulate climb-rate slews from a bench supply.

6. Basic pitch latching
   - Tried latching pitch for the duration of a beep.
   - This removed warbly mid-beep pitch changes and sounded cleaner.
   - It also lost too much climb-rate information during fast slews.

7. Slower mid-beep updates
   - Tried allowing pitch changes only every 200-250 ms.
   - This helped a little, but the main problem seemed to be small pitch changes,
     not the exact update interval.

8. Minimum pitch-change threshold
   - Returned to 40 ms updates but allowed mid-beep pitch changes only if the
     requested pitch moved by a larger amount.
   - This reduced hunting near stable climb rates.
   - It still discarded too much information, especially once pitch was quantized.

9. C-major quantization
   - Quantized allowable pitches to notes in the C-major scale.
   - This sounded more stable and musical than arbitrary small pitch changes.
   - Boundaries between adjacent notes could still trill when the analog input
     hovered near a transition.

10. Quantizer hysteresis
    - Added Schmitt-style hysteresis around note boundaries.
    - The selected note is remembered, and switching up/down uses different
      thresholds.
    - This cleaned up most boundary jitter.

11. Timed note-change confirmation
    - Added a 120 ms candidate confirmation delay.
    - This prevented some boundary chatter, but during continuous slews it could
      also prevent note changes from committing.
    - The confirmation delay was removed.

12. Chromatic quantization
    - Replaced the C-major scale with the full chromatic scale.
    - This doubled the number of musical pitch steps compared with C major and
      retained more climb-rate information.

13. Quartertone quantization
    - Replaced chromatic semitones with quartertones, about twice as many
      quantized pitch steps again.
    - Hysteresis is now proportional to adjacent quartertone spacing.

14. Play-length latching
    - Latched the beep play length at the start of each beep.
    - Rest timing remains live, so cadence can still respond between beeps.
    - This keeps each beep envelope from stretching or shrinking mid-beep.

15. Quantized mid-beep pitch segments
    - Allowed pitch changes inside a beep only at predictable segment boundaries.
    - Each pitch segment must be at least one third of the latched beep length,
      rounded down to the nearest 40 ms sample, or 80 ms, whichever is larger.
    - This lets long beeps carry controlled pitch motion while short beeps remain
      single-pitch and crisp.

16. Zero-Hz latch fix
    - Fixed a state-machine bug where entering the quiet zone could latch the
      current beep to `0 Hz` forever.
    - The no-tone path now clears play/rest counters and returns the state machine
      to idle.

## Current Behavior

- Leaf's raw climb/sink note and beep/rest timing are computed from the analog
  climb-rate input.
- Pitch is quantized to quartertones from 131 Hz to 2093 Hz.
- Quartertone transitions use proportional hysteresis:
  `QUARTERTONE_HYSTERESIS_PERCENT = 35`.
- Beep play length is latched when the beep starts.
- Rest length continues to update live.
- Mid-beep pitch changes are allowed only when the current pitch segment has
  played long enough and enough beep time remains for the next segment.
- Serial status reports ADC millivolts, climb rate, raw pitch, pending quantized
  pitch, latched pitch, target play length, latched play length, pitch-segment
  minimum, and rest length.

## Build Check

This sketch has been compile-checked with PlatformIO using Leaf's `TestUtil`
environment.
