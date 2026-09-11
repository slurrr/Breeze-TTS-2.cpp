# Breeze provider checkpoint

## Current Goal
Keep Breeze reliable as the active production TTS provider. The WebSocket terminal-event race is fixed and deployed, and the separate absolute 180-second total speech cap has now been removed locally so multi-minute responses can play completely; continue the Vulkan Q4_K quality trial.

## Live State
- Selected provider: Breeze; `voice-agentd` effective mode `native`, healthy.
- Live Breeze: Vulkan backend explicitly pinned to physical device 1 (RTX 4090), Q4_K, chunk ramp 4→16, split threshold 600, default voice `all-day-candidate-0`.
- HTTP/UI `127.0.0.1:8137`; WebSocket `127.0.0.1:8138`; 24 kHz mono s16le.
- Persistent config: `~/.config/breeze-tts/runtime.env` (dotfiles source: `/home/poop/code/dev/dotfiles/.config/breeze-tts/runtime.env`).
- No silent CPU fallback.
- CUDA Q8 baseline remains built and available for immediate rollback.
- Live server includes the `apps/server/ws_api.cpp` fix for idle `end` with an empty tail. Direct old-server reproduction failed to receive `done`; four post-fix protocol cases all completed in 0–3 seconds. The fix and coherent native stream buffering are committed in `219713e`; Vulkan build fingerprint is current.
- Latest abrupt cutoff root cause was a distinct client cap: `_run_incremental_session` used `thread.join(timeout=180)`. A real 5,710-character response produced 386.6 seconds of audio, but playback-rate backpressure kept the worker alive beyond three minutes and `voice-agentd` closed it abruptly. The absolute cap is removed; the 30-second **post-finish inactivity** watchdog remains, and explicit cancel/shutdown still close the socket immediately.
- `voice-agentd` restarted at 2026-09-10 14:25 with active session `sess_dc1273781013`; status is Breeze/native/healthy and error-free.

## Footprint Study
Full report, raw JSON, transcripts, source text, and deterministic WAVs:
`~/runs/breeze-tts/footprint-study-20260908/`

All tests used the same saved default voice, seed 42, 749-character text, and native WebSocket streaming.

Key results after warmup:
- CUDA Q8_0 4→40: 4076 MiB idle, 4814 MiB peak, 2.779× realtime, 124 ms TTFA.
- Vulkan Q8_0 4→40: 3649 MiB idle, 4403 MiB peak, 3.836× realtime, 121 ms TTFA.
- Vulkan Q6_K 4→40: 3204 MiB idle, 3963 MiB peak, 3.967× realtime, 126 ms TTFA.
- Vulkan Q4_K 4→40: 2698 MiB idle, 3415 MiB peak, 4.356× realtime, 107 ms TTFA.
- **Recommended/live Vulkan Q4_K 4→16:** 2698 MiB idle, 3287 MiB peak, 3.944× realtime, 116 ms TTFA.
- Absolute measured floor, experimental Vulkan Q4_K-DD2 4→4: 2602 MiB idle, 3145 MiB peak, 2.587× realtime, 107 ms TTFA.

Recommended profile saves 1378 MiB warm-idle and 1527 MiB peak versus CUDA Q8 while running ~42% faster in this matrix. Whisper normalized WER was 0% for Vulkan Q4_K, Q6_K, and chunk sweep outputs. WER only confirms intelligibility; the user must judge timbre/naturalness.

DD2 is not recommended: it saves only 75–142 MiB beyond normal Q4_K and has documented cumulative depth-code quality degradation on long uninterrupted generations. Split threshold 600 mitigates but does not justify the tiny saving.

## Hybrid CPU/GPU Finding
There is no hidden partial-offload option. `BreezeModel` owns one shared `Backend`; `GGUFModel::load` allocates every tensor in that backend, and all model graphs/KV caches use it. Runtime choices are all-GPU or all-CPU. Useful hybrid placement requires a source-level multi-backend redesign plus explicit activation transfers. Historical all-CPU performance (~0.3× realtime for Q8) cannot support streaming. Quantization + Vulkan achieved the desired savings without transfer latency or invasive code.

## Changes/Commits
- Breeze source default voice/runtime metadata: `55fb022`.
- Dotfiles initial operational provider setup: `ad3f134`.
- Voice-client long stream ping-timeout fix: `98ea7a6`.
- Current `voice-clients` worktree already contains unrelated/in-progress voice-selection changes. The total-duration-cap fix and its focused regression are deliberately uncommitted to avoid capturing or disrupting that work.
- Dotfiles Vulkan/Q4 footprint profile, explicit Vulkan device selection, and metadata: `c2fba77`.
- Breeze provider-side coherent native buffering and complete `end` remainder handling: `219713e`.
- Durable 500-character native stream target and launcher metadata: `bc03e5b`.
- Existing unrelated dirty Audio8 launcher work remains unstaged and preserved.
- `notes/` remains untracked in Breeze.

## Validation
- Isolated `build-vulkan` compiles successfully with current source fingerprint.
- Vulkan was forced to physical device 1; runtime metadata records `vulkan_devices: "1"`.
- Every measured candidate remained >2.5× realtime; recommended profile is 3.944×.
- All tested WAVs completed through native WebSocket and have matching audition artifacts.
- Native grouping protocol test proved that eight complete sentence deltas totaling 503 characters remain buffered and become one `speaking` model piece; `end` also speaks trailing unpunctuated text. Comparison WAVs are under `~/runs/breeze-tts/stream-grouping-audition-20260910/`: sentence mode used 5 independent pieces, while live grouped-500 mode used 2.
- Live `/health` reports `stream_chars: 500`; server arguments and `server.json` agree. `voice-agentd` was restarted and is Breeze/native/healthy with no error.
- `voice-agentd` was restarted after benchmark server cycling to clear expected disconnect history.
- After removing the 180-second cap: full contract smoke passes, Python compilation passes, focused Ruff fatal checks pass, and `git diff --check` passes. A regression verifies the runner joins without a wall timeout; the existing test verifies a missing post-finish `done` still fails via progress timeout.

## Resume
1. Confirm `curl http://127.0.0.1:8137/health` shows `Vulkan0`, `q4_k`, and the intended default voice.
2. Have the user exercise normal long direct-mode agent-tui responses and listen for timbre, prosody, clone identity, and seams.
3. Compare WAVs with:
   `ffplay -nodisp -autoexit ~/runs/breeze-tts/footprint-study-20260908/vulkan-q4_k-chunk16.wav`
   and equivalent `vulkan-q6_k.wav`, `vulkan-q8_0.wav`, `cuda-q8_0.wav`.
4. If Q4 quality is accepted, leave current durable config. If not, next choice is Vulkan Q6_K, then Vulkan Q8_0.
5. Temporary Q8 rollback:
   `breeze-tts-stop && BREEZE_BACKEND=cuda BREEZE_MODEL_VARIANT=q8_0 BREEZE_CHUNK_MAX=40 breeze-tts-serve --bg`
6. Do not pursue hybrid backend work unless the remaining ~3.3 GiB peak is materially problematic; it is a significant architecture change.

---

# Voice clip capture (Breeze voice experiments)

## Current Goal
Build a browse-and-pick catalog of ~15 s clean single-speaker clips (v2: categories as subfolders, clips named `<title-slug>_<start>s`) captured via the system-output monitor; user tests the current 10-clip batch, then scales to ~100 clips and picks voices to clone.

## Current State
- **v1 delivered and preserved**: 10 clips (seed 33365), dataset `~/Documents/personal/voice/curated-clips/` (wavs + `raw/` + `transcripts/`) — untouched.
- **v2 in place**:
  - Catalog: `~/Documents/personal/voice/catalog/` — `<category>/<slug>_NNNs.wav` + `.txt`, `raw/`, `index.csv` (status: pending/kept/rejected). The 10 v1 clips migrated in with slug names (e.g. `warm/listening-to-shame_1135s.wav`), plus 2 test-captured candidates (`can-humanism-survive-the-age-of-ai_265s` in calm_narration + distinct_accent).
  - Pipeline home (git repo): `~/Documents/system/voice-capture/` — history: `0e9aa9e` (v1 baseline) → `0082552` (v2) → `b59c9f3` (README as fresh-session entry point). `scripts/`: common.py, make_candidates.py, capture.py (v2) + v1 scripts in history. `state/buckets.json` holds the video pool; `state/candidates.json` (+ .md) is the capture worklist (30 candidates at --k 3).
  - **README.md is now the agent entry point** (per user): purpose/quality bar, "capture broadly → user prunes", **fluid categories** (folder = whatever the request implies: person slug `feynman/` or style `warm/`, not locked to the 10 buckets), a step-by-step "handling a capture request" playbook with exact fetch + window-merge commands, source-diversity guidance (TED is one source, not the default — user explicitly wants non-TED voices like a specific person's clips), and taste learning via index.csv kept/rejected.
- **v2 pipeline verified end-to-end**: ranged `ffplay -vn` of master webm → parecord monitor capture → onset-trim → 24 kHz mono final + transcript + index upsert (ffprobe-clean, mean ~-26 dB on the test clip).
- **Sentence-end refine wired to the shared STT service** (commit `2e0d73d`): capture plays `dur + 8s tail`, the raw is POSTed to `fast-stt-service` (`http://127.0.0.1:8891`, `~/code/dev/fast-stt-service`, currently `small` int8 CPU) as `verbose_json`, the clip end snaps to the best segment end in the 10–22 s band (latest punctuated end, else largest-pause ≥0.3 s), final re-trimmed, `.txt` rewritten with punctuated text, `index.csv` updated. Auto-on when service up; fails soft to time-based cut. Verified: feynman batch re-cut to 14.3/20.9/21.9 s with fully punctuated transcripts. Model swap = `STT_MODEL` env on the service side, no pipeline change.
- **DRM/access verified**: normal YouTube = no blockers; age-restricted = yt-dlp still can't get formats even with chrome cookies (cookies decrypt+load, player clients refuse) → use browser + `capture.py --manual N`; true DRM is moot since monitor capture records whatever plays. yt-dlp uv tool now installed `--with secretstorage` for Chrome cookie decryption.

## Decisions
- Category = folder, clip = `<title-slug>_<start>s`; one `index.csv` replaces per-clip artifact ceremony; v1's `play/` wav layer dropped (ranged playback instead).
- Categories are **fluid and request-driven** (person or style), deliberately not constrained to the original 10 generic buckets.
- Same (video, window) may appear under two buckets; index rows keyed by (category, video_id, start).
- `ffplay` on this box wants `-vn`, not `-novideo`.
- Do **not** re-run `scripts/build_windows.py` to add videos — it regenerates `windows.json` from the hard-coded v1 `VIDS` pool; use the merge snippet in the README instead (verified to reproduce original windows exactly).

## Open Problems
- Confirm several natural pi tool-call turns remain free of dead air. voice-agentd also has a deployed 30-second post-end progress watchdog.
- The WebSocket race and provider-side stream-grouping fixes are committed. The `voice-clients` duration-cap/queued-wait fixes remain in a dirty worktree alongside unrelated voice-selection work; preserve them when handling that repo.

## Resume
A fresh agent pointed at `~/Documents/system/voice-capture/README.md` should be able to handle any request (e.g. "find me high-quality Richard Feynman clips" → category `feynman/`, diverse non-TED queries, fetch→merge→buckets→make_candidates→capture).
1. User feedback on the 10 clips → adjust buckets/queries if needed.
2. Scale-up: grow `state/buckets.json` (+ fetch subs for new vids) → `python3 scripts/make_candidates.py --all` (or bigger --k) → prune `state/candidates.json` → `python3 scripts/capture.py` (≈30 s/clip).
3. Browse: play `catalog/<category>/`, set `status=kept|rejected` in `catalog/index.csv`.
4. Cleanup when done: keep kept wavs + txts + trimmed index.csv; delete `catalog/raw/` and pipeline-home `audio/`, `subs/`, `state/` (regenerable).
