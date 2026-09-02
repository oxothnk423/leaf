---
title: Factory Commissioning Follow-up
description: Future hardening notes for factory_interface commissioning and flashing behavior.
---

# Factory Commissioning Follow-up

The factory flash flow currently erases the NVS partition before writing firmware so previously
commissioned units can be recommissioned. The current Leaf partition table uses the standard Arduino
ESP32 NVS range:

- Partition: `nvs`
- Type/subtype: `data`, `nvs`
- Offset: `0x9000`
- Size: `0x5000` (`20K`)

This was verified against both the configured `default_8MB.csv` partition table and the generated
`leaf_3_2_6_release` `partitions.bin` artifact.

Future work:

- Replace the factory_interface hardcoded NVS erase range with partition-table-derived values.
- Prefer parsing the configured/generated partition table and erasing the partition named `nvs`.
- Keep the current hardcoded range acceptable while Leaf continues to use the standard
  `default_8MB.csv` layout.

## Commissioning Feature Backlog

### GPS satellite test: suspend Auto-Off

Status: Pending

Disable Auto-Off for the duration of the long GPS satellite test so the unit cannot shut down
before the test completes.

Acceptance criteria:

- Starting the GPS satellite test prevents Auto-Off from powering down the unit.
- Auto-Off remains suspended until the test completes, fails, or is cancelled.
- Leaving the test restores the Auto-Off behavior that was active before the test started.

### GPS satellite test: mute vario audio

Status: Pending

Mute vario volume for the duration of the GPS satellite test so vario tones do not disrupt the
commissioning session.

Acceptance criteria:

- Starting the GPS satellite test silences vario audio.
- Vario audio remains muted until the test completes, fails, or is cancelled.
- Leaving the test restores the volume and mute state that were active before the test started.
