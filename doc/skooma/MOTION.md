# Object motion — index

Recording, replaying and improving how the viewer moves objects between network updates
(physical vehicles, keyframed motion, bullets) and why the camera stutters on them.

| doc | what | status |
|---|---|---|
| `.motion-plan.md` | problem, code map (update path, per-frame interpolation, drawable damping, camera order), candidate causes, 3-step plan, metrics | 2026-09-29 |
| `.motion-recorder-brief.md` | builder brief for `feature-motion/recorder` | 2026-09-29 |
| `.motion-unclamp-brief.md` | builder brief for `feature-motion/unclamp`: ratio-method recovery of velocities clamped to ±128 m/s by the terse encoding | 2026-09-30 |
| `.camera-follow-brief.md` | builder brief for `feature-camera/follow`: alt-cam focus follower (object track filtered, offset after, feed-forward v + ω×r, τ 0.12 s) | 2026-09-30 |
| `.motion-smooth-brief.md` | builder brief for `feature-motion/smooth`: bleed update position errors out exponentially (τ 0.15 s) instead of snapping; oracle `interp_variants.py` V1 | 2026-09-30 |
| Soapstorm `feature-ss-timing-inversion` (`82a34092c8`, `8ec57e07c6`) | the earlier timestamp-inference attempt; no results recorded | lessons only |

## Status

| branch | state |
|---|---|
| `motion/recorder` | **integrated** 2026-09-29, 4 commits (`cc342508f8`..`317267659b`): `SKMotionRecorder`, Develop → Network → Record Object Motion, `SKMotionRecordSeconds` 30 / `Radius` 256 / `MaxObjects` 512, reader `V:\Scratch\motion-recorder\read_skm.py` (`--selftest` passes). Review: 9 fixed. Merge into develop resolved two adjacent-add conflicts by hand (ghost-watch line, CMake lists); F5 tree builds. F5-verified 2026-09-29 on a bullet capture (header fix `310321c502` integrated 2026-09-30). Earlier note: record 30 s alt-cammed on a KFM vehicle, run the reader, confirm non-zero `UPDATE` residuals and an `OBJECT` row per frame for the focus object |
| `motion/unclamp` | **integrated** 2026-09-30, base `motion/recorder`, 4 commits (`a556c24785`..`83c230dacb`): ratio-method recovery in `LLViewerObject` with sign-agreement guard and EWMA fallback, clamped acceleration zeroed, toggle Develop → Network → Recover Clamped Velocities (`SKMotionRecoverClampedVelocity`, on), recorder UPDATE `used_vel`/`recover`, header v2. Review: 3 fixed. F5-verified 2026-09-30 on `bullet-2.skm`: C++ matches the replay exactly (bar the unported region-edge clip); per-bullet snap 3.7 → 1.7 m, cross-track 0.57 → 0.15 m. Follow-ups integrated 2026-09-30 (`5afc9c4085`): `c60f3899ab` axis rule by largest |v_i| ≥ 0.5 m/s (no fallback on either recording); `7448000ef4` per-stage frame timers in FRAME (header v3, stage fields describe the previous frame; reader `stages.py`). Oracle `unclamp.py`: ratio method on 10520/10521 clamped updates, speed error 0.04 m/s median, direction 0°, systematic snap gone; remaining along-track snap is arrival jitter (|p50| 3.5 m at 200 m/s) |

| `camera/follow` | **integrated** 2026-09-30 (`4b6516f10b`), base `motion/unclamp`, 3 commits (`a93590e4b6`..`d9b8f8e8f1`): alt-cam focus follower on the object's track (offset added after), feed-forward v + ω×r, τ 0.12 s, resets on focus change / view reset / > 2 m; toggle Develop → Network → Smooth Camera Follow (`SKCameraFollowEnabled`, `SKCameraFollowSmoothing`); CAMERA record `focus_raw`, header v4. Review: 5 fixed. Seated-on-vehicle site not done: needs one filtered anchor at the top of `updateCamera` read by both `calcCameraPositionTargetGlobal` and the third-person focus. Not F5-verified |
| `motion/smooth` | **integrated** 2026-09-30 (merged sibling `camera/follow` first, `3cd4a45ae7`; ghost-line conflict resolved by hand), base `motion/unclamp`, 3 commits (`865ce5372d`..`7cf1d6038c`): update position errors kept as `mSKPendingCorrection` and bled with the step's dilated dt (τ 0.15), snap on > max(2 m, 3× per-update travel), first update, region change, static, parented, selected, attachment; toggle Develop → Network → Smooth Update Corrections (`SKMotionSmoothCorrections`, `SKMotionSmoothTau`); UPDATE `snapped`/`correction`, OBJECT `pending`, header v5; `replay.py` ports the rule when the header says on. Review: 7 fixed. Port vs oracle: ship within 1.8 cm p99, friend's avatar 1 mm. Rotation still snaps. Not F5-verified. Oracle: V1 exp τ 0.15 → ship per-frame speed 0.82–3.39 → 1.26–1.70 m/s, max discontinuity 0.106 → 0.025 m, bullets 0.40–2.38× → 0.74–1.24×, cross-track rms 0.85 → 0.24 m, lag 4 cm / 18 ms. V2 delayed interpolation rejected (80–220 ms latency, breaks on avatars whose reported velocity flips on a vehicle) |

## Findings so far

- **F1 bullets** (`.motion-plan.md` § Findings): the terse update clamps velocity to ±128 m/s and
  acceleration to ±64 m/s² per axis; 200 m/s tracers arrive as 130 m/s and 3.7° off, so the viewer
  snaps 3.7 m forward (median) on every update. On top, arrival jitter (per object, with the recovered
  speed) 17 ms median / 56 ms p95 against 47 ms update gaps, plus a 2-step jump on same-frame doubles. Replay validates exactly on the real file. Fix direction: position-derived
  velocity when clamped, sim-time inference plus delayed entity interpolation instead of snap.
- **F3 KFM ship** (`kfm-ship.skm`, `kfm-ship-2.skm`): ship snaps 3 cm at a 2.3 cm per-frame step (±30–50 ms arrival jitter), no drawable damping on the ship; avatars standing on it snap 10 cm (4.5–6.5 frames) and their drawable lags 14–16 cm (damped `updateXform`); bullets stall ~250 ms then burst 5 updates in a frame. Periodic +35 ms frame hitches every 60–96 ms in every recording are **not** caused by updates (first reading corrected) and are out of scope; stage timers will name them. Alt-cam: camera velocity change 0.76 m/s median per frame on the clean span; oracle `follower.py`: object-track follower, tau 0.12 s, feed-forward incl. ω×r, offset added after → 0.09, lag 3–4 cm. One-command diagnosis: `report.py <file>`.
- Pending: a ship recording with the stage timers (the two follow-up commits are integrated, `5afc9c4085`). Header fix `310321c502` (unit suffix broke the JSON) integrated 2026-09-30; `read_skm.py` tolerates old files.

## Scratch

`V:\Scratch\motion-recorder\` (README has the loop): `read_skm.py` reader; `replay.py` port of the current interpolator (`Interpolator` with pluggable `step`, `--validate` diffs against recorded OBJECT rows; exact on synthetic files, not yet on a real one); `metrics.py` (inter-arrival, snap along/cross-track, camera-space jerk of the focus object, drawable lag, arc deviation); `plots.py` dark-style PNGs into `out\`; `synth.py` synthetic recordings; `find_fast.py` (fastest objects, flags, coverage), `saturation.py` (clamped components), `timing.py` (arrival gap vs sim-implied gap, per object), `camera.py` (focus spans, focus object per-frame motion), `pacing.py` (frame-interval distribution and alternation), `hitch.py` (long frames vs which objects' updates), `stages.py` (per-stage frame times, v3 files). Recordings: `bullet-up.skm`, `bullet-2.skm`, `kfm-ship.skm`, `kfm-ship-2.skm`. Not ported: Z clamps, region-edge clip, per-region dilation and circuit state (only the agent region is recorded).
