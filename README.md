<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="branding/omegatube-mark-dark.png">
    <img src="branding/omegatube-mark-light.png" width="128" alt="omegatube-modern logo">
  </picture>
</p>

<h1 align="center">omegatube-modern</h1>

<p align="center">
  <b>A rebuilt, zero-bloat native YouTube client for Samsung Smart TVs (Tizen 10+)</b><br>
  Engineered for reference video clarity, studio-clean 48 kHz audio, instant responsiveness, and an uninterrupted couch experience.<br>
  <sub>Forked from <a href="https://github.com/IIyn/omegatube">IIyn/omegatube</a> — kept the spirit, replaced almost everything else. Independent project, not affiliated with Google, YouTube, or Samsung.</sub>
</p>

---

## Contents

- [Why omegatube-modern Exists](#why-omegatube-modern-exists)
- [omegatube-modern vs Original OmegaTube](#omegatube-modern-vs-original-omegatube)
- [Built for Watching Videos](#built-for-watching-videos)
  - [Picture and Hardware Decoding](#picture-and-hardware-decoding)
  - [Audiophile C++ Float Audio Pipeline and Bluetooth Sync](#audiophile-c-float-audio-pipeline-and-bluetooth-sync)
  - [Player UX Made for the Big Screen](#player-ux-made-for-the-big-screen)
  - [Ad-Free, Sponsor-Free, and Personal](#ad-free-sponsor-free-and-personal)
- [Speed and Architecture Under the Hood](#speed-and-architecture-under-the-hood)
- [Install on Your TV](#install-on-your-tv)
- [Install with Apps2Samsung](#install-with-apps2samsung)
- [Troubleshooting](#troubleshooting)
- [Reporting a Bug](#reporting-a-bug)
- [Contributing](#contributing)
- [Credits and License](#credits-and-license)

---

## Why omegatube-modern Exists

**omegatube-modern** started as a fork of [IIyn/omegatube](https://github.com/IIyn/omegatube) with a simple rule: treat your TV like modern 2026 hardware, not a Tizen 5.5 museum exhibit.

Instead of wrapping YouTube's sluggish web player in a TV browser—or carrying legacy workarounds for decade-old firmware—**omegatube-modern** pairs a native GPU-accelerated **Tizen 10 NUI** interface with a **C++20 / ARMv8 NEON** media core that decodes, cleans, and feeds streams directly into Samsung's hardware pipelines.

You press play, the video starts cleanly, the picture looks crisp, the sound stays uncolored and dead-on synced, and the interface gets out of your way.

---

## omegatube-modern vs Original OmegaTube

Much love to [IIyn/omegatube](https://github.com/IIyn/omegatube) for proving a native YouTube app on Samsung TVs could exist without a browser. That said, living on the original fork felt like driving a sports car with the handbrake on:

| Experience | Original `IIyn/omegatube` | `omegatube-modern` |
| --- | --- | --- |
| **Target Platform** | Stretched across Tizen 5.5–9.0 with runtime ELF-loader hacks, glibc shims, and legacy API 6 fallbacks | Laser-focused on **Tizen 10+ (`.NET 8`, ARMv8 NEON)** — zero legacy shims, zero dead fallback baggage |
| **Home and Chrome** | Heavy carousel/preview scaffolding, bitmap `round_mask.png` alpha masks, disk feed caches, and blocking startup checks | Stripped **~2,100+ lines** of heavy preview/theme bloat (`Hero`, `PreviewRing`, `Backdrop`); native **GPU `CornerRadius`** + crisp **skeleton loading placeholders** |
| **Audio** | Hands compressed YouTube audio straight to the TV and hopes for the best; Bluetooth headphones drift out of sync | Full **C++ floating-point DSP pipeline** (Opus 1.5.2 + FAAD2, 160-tap sinc resampling, `loudnessDb` normalization, signal-gated HF restoration, `S32LE`/`S24LE`/`S16LE` PCM) + **automatic BlueZ A2DP lip-sync** |
| **Video and HDR** | Missed progressive-scan flags, sized 1080p decoders at 1080p tiers, skipped VP9-in-MP4, and suffered `ESMatroskaColor` / SDR false-HDR bugs | **AV1-first**, **HDR10 / HLG** colorimetry fixes, **VP9-in-MP4**, **4K UHD decoder tier for 1080p**, **`VIDEO_SCAN_PROGRESSIVE`** unlocking Samsung's AI upscaler (`DVDE0`), and dimmed OSD during HDR |
| **Comments and Browse While Watching** | Pushed comments inline *below* the player so reading replies meant scrolling off the video | **100% full-screen video** with a **70 / 30 side-by-side** scrollable comments and description overlay |
| **Seeking and Controls** | Fixed step jumps, jumpy seekbar playhead snap-back, and extra skip buttons cluttering the bar | **Reverse-logarithmic acceleration** (up to `10x` over 10s), **non-snapping seekbar**, atomic seek spinner on frame, and compact Material-inspired controls |
| **Startup and Requests** | Repeated `youtube.com` `visitorData` fetches per video, sequential metadata/track requests, and startup GitHub release polling | **Shared `visitorData` cache**, **60s DNS + TLS resumption + HTTP Keep-Alive + gzip**, off-UI splash init, and **parallel** track/details/SponsorBlock fetching |

In short: the original got YouTube on the TV; **omegatube-modern** makes you actually want to stay on it for four hours straight.

---

## Built for Watching Videos

### Picture and Hardware Decoding

- **AV1-First with Smart Fallbacks:** Defaults to **AV1** (`av01, vp9, avc1`), prioritizing **60fps** and **1080p Premium** high-bitrate streams, with codec preference configurable in Settings.
- **Unlocks Your Panel's Upscaler & Tier:** Marks streams with `VIDEO_SCAN_PROGRESSIVE` (`esplusplayer_set_video_scan_type`) to engage Samsung's progressive **DVDE0 AI decoder**, and provisions at least a **4K UHD (`3840x2160`)** decoder/scaler tier even for 1080p video so upscaled HD content doesn't get downgraded to a lower scaler path.
- **True HDR10 & HLG + Fixed SDR Colorimetry:** Fixes `ESMatroskaColor` ABI alignment and false SDR-as-HDR detection so standard content keeps accurate **BT.709/BT.601** tones while HDR videos deliver authentic **HDR PQ / HLG** color primaries and mastering metadata (**MDCV / CLLI**).
- **Container & Format Flexibility:** Supports **VP9-in-MP4** alongside WebM VP9/AV1 and H.264 MP4, with a hardware-filtered quality selector showing resolution, fps, codec, and HDR badges.
- **HDR-Considerate OSD:** Automatically dims NUI overlay brightness and opacity during HDR playback so pausing in a dark room never blasts your eyes with peak-white menus.

### Audiophile C++ Float Audio Pipeline and Bluetooth Sync

Instead of letting a compressed stream hit unknown TV post-processing or dynamic volume pumping (`DRC` off, zero TV DSP suites, zero EQ coloration), `omegatube-modern` decodes and refines audio in a zero-UI **64-bit/32-bit C++ DSP pipeline** inside `libomegacore.so`, falling back automatically to TV hardware passthrough if a model ever rejects raw PCM:

- **Pure Software Decode (`Opus 1.5.2` + `FAAD2 2.11.4`):** Native **48 kHz Opus** and **AAC** floating-point decode with RFC 7845 `OpusHead` pre-skip/gain handling and FAAD2 DRC disabled so dynamics remain untouched.
- **Zero-Coloration Linear Normalization:** Uses YouTube's embedded `audioConfig.loudnessDb` metadata for linear attenuation with at least **1.0 dB inter-sample peak headroom** and a soft `> 0.995` peak guard—no multiband compression, no loudness pumping, no wasted CPU re-measuring ITU-R BS.1770-4.
- **Mastering-Grade Sample Rate Conversion:** `44.1 -> 48 kHz` runs through a **160-tap double-precision Kaiser polyphase sinc resampler** (`beta = 12.6`, exact `160:147` stepping), while native `48 kHz` streams run a clean `1:1` bypass.
- **Signal-Gated HF Spectral Band Extension (SBR/DSEE Style):**
  - Strictly additive (`dry + HF residual`) in a **512-pt 75%-overlap Hann WOLA** engine—everything below cutoff stays **100% dry and bit-faithful**.
  - Completely **inactive** on native **HE-AAC SBR** (`SBR_UPSAMPLED` / `SBR_DOWNSAMPLED`) and full-band **48 kHz Opus**—verified by slope analysis before touching a single sample.
  - Activates only on genuine codec brickwalls (`16 kHz` AAC / AAC-transcoded Opus) or `12 kHz` Opus Superwideband (`OPUS_BANDWIDTH_SUPERWIDEBAND`), restoring crisp air tucked **-9 dB** below the extrapolated natural slope.
  - Built-in **Speech Sibilance Guard** (`6–10 kHz`) fast-ducks harsh `"S"` and `"T"` fricatives so podcasts, commentary, and vocals never turn spitty or metallic.
- **Precision PCM Output & Seamless Fallback:** Probes `S32LE -> S24LE` (TPDF dithered) `-> S16LE` (TPDF + 1st-order E-weighted noise shaping) in fixed `20 ms` (`960`-frame) ESPlayer packets, caches the winning format per session, and falls back smoothly to TV HW compressed passthrough on refusal.
- **Automatic Bluetooth Lip-Sync Compensation:** Declares `SOUND_STREAM_TYPE_MEDIA` for proper TV audio routing and polls live **BlueZ `MediaTransport1.Delay`** (falling back cleanly to `180 ms` when needed) whenever Bluetooth headphones or speakers connect/disconnect—no manual audio delay sliders required.

### Player UX Made for the Big Screen

- **Video First, Always:** Compact bottom controls fade out automatically, leaving zero chrome during playback.
- **70 / 30 Split Comments & Description:** Open descriptions, comments, and replies in a crisp **30% right-hand panel** while the video stays **100% visible** on the left.
- **Smart Seeking That Feels Instant:**
  - **Reverse-logarithmic seek acceleration** ramps smoothly up to **`10x`** as you hold Left/Right over 10 seconds, keeping micro-skips precise and long scrubs fast.
  - Seekbar playhead appears cleanly when focused and **never snaps back** to the old timestamp mid-seek.
  - Shows a crisp loading spinner directly over the video frame while priming a seek (`120 ms` pre-seek DSP & MDCT warm-up for click-free resumes).
- **Thoughtful Quick Controls:** Per-video **Autoplay toggle** right beside captions (without overwriting your global setting), **Audio Track** selector right beside Quality for multi-language videos, loop mode, variable playback speed, and playlist **"Play Next"** countdown.
- **TV Keyboard & Voice Dictation:** Native Tizen `TextField` integration gives you full TV on-screen keyboard (`IME Done`) and remote mic voice search out of the box.
- **Live Stats for Nerds:** Inspect real-time video/audio codecs, encoded stream bitrates, windowed download throughput, buffer lead, dropped/underrun frames, ESPlayer scaler ID, decoder tier (`3840x2160` etc.), colorimetry tags (`BT.709`, `HDR PQ BT.2020 10b`), active Bluetooth sync offset, CPU, and RAM.

### Ad-Free, Sponsor-Free, and Personal

- **Uninterrupted Streams:** Direct DASH/MP4/WebM streaming with automatic expiring URL refresh (`403`/`410`) means zero pre-roll, mid-roll, or overlay ad breaks.
- **Built-In SponsorBlock:** Auto-skip or one-click skip sponsors, self-promotions, intros, outros, previews, fillers, and interaction reminders—with colored segments right on the seekbar.
- **Google Account Sync + Multi-Account Switching:** Sign in via QR code to access Home, **personalized TV Watch Next** shelves & organic autoplay suggestions, Subscriptions, History, Likes, synced watch progress, Playlists, and Channels—with one-click switching between profiles.

---

## Speed and Architecture Under the Hood

| Layer | Technology | What It Owns |
| --- | --- | --- |
| **UI Shell** (`app/OmegaTube`) | C# / .NET 8 (`net8.0-tizen10.0`, API 10.0) | Native `Tizen.NUI` screens, GPU shaders, remote focus, BlueZ / IME bindings, P/Invoke marshal, settings, and `.tpk` packaging. |
| **Media & DSP Core** (`core`) | C++20 (`libomegacore.so`, ARMv8 NEON `-O3`) | InnerTube client, Mbed TLS + QuickJS-ng EJS solver, zero-alloc MP4/WebM demuxers, Opus/FAAD2 DSP pipeline, SponsorBlock, and `libesplusplayer.so` integration. |

Key engineering optimizations baked across both layers:

- **Instant Splash Handoff:** `AppConfig`, `libomegacore.so` loading, `LocalStore` history, and Mozilla CA (`cacert.pem`) parsing all warm up off the UI thread while the splash screen displays cleanly (`250 ms` min hold). Zero GitHub release polling on launch, and `remote-config.json` refreshes strictly *after* Home finishes loading.
- **Shared `visitorData` + Connection Reuse:** Caches YouTube's `visitorData` across `Client` instances instead of re-fetching `youtube.com` before every video, backed by a **60s in-memory DNS cache**, **Mbed TLS session resumption**, **persistent HTTP Keep-Alive pooling**, and **`gzip`** decompression (`zlib`).
- **True Parallelism Everywhere:** Video and audio tracks open, TLS-handshake, and parse `moov`/EBML headers concurrently; `Client::details()` fetches WEB metadata and signed-in TV interaction state in parallel; `PlayerScreen` launches stream prepare, video details, suggestions, and SponsorBlock concurrently.
- **Condition-Variable Pacing & Zero-Alloc Demux Replay:** Track demux and ESPlayer feed threads communicate via CV wakeups factoring in live ESPlayer buffer lead (no polling sleeps), while format probing replays header bytes from a fixed stack buffer.
- **Lean NUI Rendering:** Reuses focus `Shadow`/`Animation` objects, recycles `Card` nodes and decoded thumbnails across refreshes (`DesiredWidth`/`DesiredHeight` decode caps), draws player icons with native primitives instead of extra PNG sheets, runs caption timers strictly on demand, disposes 1080p loading backdrops after frame 1, and shows native skeleton placeholders instead of stale feed disk caches.
- **Compact Shared UI:** Pure-C# QR generator (`Util.Qr.Encode`) shared across sign-in and bug reports (dropping the C++ `qrcodegen` dependency), shared `OnScreenKeyboard`, and frame-budgeted UI scrolling (`UiWork` + `SetShown` culling).

---

## Install on Your TV

> **Supported TVs:** Samsung Smart TVs running **Tizen 10.0 or newer**. For older Tizen 6.0–9.0 TVs, see [IIyn/omegatube](https://github.com/IIyn/omegatube).

You need a computer (Windows 10/11, macOS, or Linux) on the **same local network** as your TV and a free **Samsung account** (used to sign the `.tpk` for your TV).

### 1. Run the Installer on Your Computer

- **Windows (PowerShell):**
  ```powershell
  irm https://raw.githubusercontent.com/selvarajrajkanna/omegatube-modern/main/scripts/install-tv.ps1 | iex
  ```
- **macOS / Linux (Terminal):**
  ```sh
  curl -fsSL https://github.com/selvarajrajkanna/omegatube-modern/releases/latest/download/install.sh | bash
  ```

### 2. Follow the On-Screen Prompts

1. The installer downloads the required Samsung TV SDK tools (first time only).
2. Enable **Developer Mode** on your TV:
   - Open the **Apps** screen on your TV.
   - Press **1 2 3 4 5** on your remote.
   - Turn **Developer Mode** **On**, enter the computer IP shown by the installer, and confirm.
   - **Restart your TV** by holding the power button until the Samsung logo appears.
3. Enter your TV's IP address (**Settings → General → Network → Network Status → IP Settings**).
4. Sign in to your Samsung account in the browser window that opens to generate your TV's signing certificate (first time only).
5. The installer downloads, signs, and launches **omegatube-modern** on your TV.

To **update** later, run the same command again—your certificate, settings, and YouTube account are preserved.

---

## Install with Apps2Samsung

Prefer a graphical installer? **omegatube-modern** can also be installed using [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung) (available for Windows, macOS, and Linux):

1. Download and open **Apps2Samsung** from its [releases page](https://github.com/Apps2Samsung/Apps2Samsung/releases).
2. Enable **Developer Mode** on your TV using your computer's IP address and restart the TV.
3. Select your TV in Apps2Samsung, pick **omegatube-modern**, and install.

*(Stick to one installer method for updates so your signing certificate stays the same.)*

---

## Troubleshooting

| Problem | Solution |
| --- | --- |
| *"The TV did not answer"* | Confirm Developer Mode is **On** with your computer's IP, the TV was **restarted**, both devices are on the same network, and no VPN is blocking the connection. |
| Windows firewall prompt for *sdb* or *Java* | Allow access on private networks so the installer can connect to your TV and receive the Samsung sign-in callback. |
| Signature or certificate error on install | Uninstall any previous version signed with a different certificate on the TV (**Apps → omegatube-modern → long-press OK → Delete**) and run the installer again. |
| Video won't play after a YouTube change | [Report the issue](#reporting-a-bug) with the video link. Many client fixes roll out automatically via `remote-config.json` without needing an app update. |

---

## Reporting a Bug

1. On your TV, open **Settings → About → Report a problem** (or scan the QR code shown automatically if an error occurs).
2. Scan the QR code with your phone to open a pre-filled [GitHub issue](https://github.com/selvarajrajkanna/omegatube-modern/issues/new?template=bug.yml) containing your app version, TV model, Tizen version, and error details.
3. Add a brief note on what happened and submit.

You can optionally attach the session log from the same screen; sensitive items (account tokens, emails, IP addresses, stream URLs, and titles) are stripped automatically before upload.

---

## Contributing

Pull requests and feature ideas are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting code, and open a [feature request](https://github.com/selvarajrajkanna/omegatube-modern/issues/new?template=feature.yml) for new ideas.

For local development, run `scripts/fetch-deps.sh` once, then build the C++20 core natively on Linux (`core/tools` for PC diagnostics via `-DOMEGA_HOST_TOOLS=ON`) or cross-compile + package the `.tpk` via Docker + .NET SDK (`scripts/build-app.sh`).

---

## Credits and License

**omegatube-modern** is free software licensed under the [GNU GPL v3](LICENSE).

- Originally forked from [IIyn/omegatube](https://github.com/IIyn/omegatube).
- Built on the work of [SmartTube](https://github.com/yuliskov/SmartTube), [ytps5-sponsorblock](https://github.com/animatian112/ytps5-sponsorblock), [SponsorBlock](https://sponsor.ajay.app), [Opus](https://opus-codec.org/), [FAAD2](https://github.com/knik0/faad2), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls), [zlib](https://zlib.net/), [QuickJS-ng](https://github.com/quickjs-ng/quickjs), and [yt-dlp EJS](https://github.com/yt-dlp/ejs). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for full notices and licenses.

YouTube is a trademark of Google LLC. **omegatube-modern** is an independent project and is not affiliated with Google, YouTube, or Samsung.
