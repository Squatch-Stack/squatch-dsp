# Squatch Tuner: CPU cost

| algorithm | ns/sample playing | of one core @48k | ns/sample quiet | of one core @48k |
|---|---|---|---|---|
| Squatch Tuner (6 partials) | 158.9 | 0.76% | 7.2 | 0.03% |
| Squatch Tuner (3 partials) | 153.4 | 0.74% | 7.0 | 0.03% |
| MPM alone (12k) | 438.6 | 2.11% | 7.1 | 0.03% |
| YIN alone (12k) | 250.6 | 1.20% | 7.5 | 0.04% |
| MPM + phase vocoder | 490.5 | 2.35% | 7.3 | 0.04% |
| Poly strum check (6 strings) | 27.1 | 0.13% | 26.8 | 0.13% |

Object sizes (all of the memory, no heap): Tuner 128176 bytes, PolyTuner 233704 bytes,
StrobeBank 116016 bytes, PartialChannel 14496 bytes.
