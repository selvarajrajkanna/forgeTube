<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="branding/omegatube-mark-dark.png">
    <img src="branding/omegatube-mark-light.png" width="128" alt="omegatube-modern logo">
  </picture>
</p>

<h1 align="center">omegatube-modern</h1>

<p align="center">
  A fast, distraction-free native YouTube client for <b>Samsung Smart TVs (Tizen 10)</b><br>
  with built-in SponsorBlock, <b>AV1 &amp; 10-bit HDR</b> playback, and a custom hardware-accelerated video player.<br>
  <sub>Started as a fork of <a href="https://github.com/IIyn/omegatube">IIyn/omegatube</a>, rebuilt and optimized exclusively for Tizen 10. Independent project, not affiliated with Google, YouTube, or Samsung.</sub>
</p>

---

## Contents

- [About](#about)
- [Key Features](#key-features)
- [Why It Feels So Fast](#why-it-feels-so-fast)
- [Technical Architecture & Optimizations](#technical-architecture--optimizations)
- [Install on Your TV](#install-on-your-tv)
- [Install with Apps2Samsung](#install-with-apps2samsung)
- [Troubleshooting](#troubleshooting)
- [Reporting a Bug](#reporting-a-bug)
- [Contributing](#contributing)
- [Credits and License](#credits-and-license)

---

## About

**omegatube-modern** started as a fork of [IIyn/omegatube](https://github.com/IIyn/omegatube) with one clear goal: strip away legacy compatibility layers and squeeze maximum speed, picture quality, and efficiency out of modern **Samsung Tizen 10 TVs**.

Unlike browser-based TV apps that wrap YouTube's heavy web interface, **omegatube-modern** talks directly to YouTube and feeds video and audio straight into your TV's dedicated hardware decoders. The result is instant startup, crisp 4K HDR playback, and zero browser stutter.

---

## Key Features

### Picture & Sound Built for Modern TVs
- **AV1-First & 10-bit HDR Support:** Defaults to **AV1 first** (`av01, vp9, avc1`) with **VP9 first** and **H.264 first** options in Settings, plus full **HDR10** and **HLG** metadata passthrough for vibrant 4K 10-bit playback.
- **Hardware-Aware Quality Picker:** Automatically filters available resolutions, frame rates (up to 60fps), and HDR badges to match what your TV panel supports.
- **Multi-Language Audio & Subtitles:** Switch audio tracks on multi-language videos, adjust subtitle size and background, or use auto-translated captions.

### Minimal, Distraction-Free Video Player
- **Clean, Compact Controls:** A modern Material-inspired overlay that stays out of your way so the video stays front and center.
- **Per-Video Autoplay Toggle:** Quickly turn autoplay on or off right next to the subtitle button for the current video without changing your global settings.
- **Side-by-Side Comments & Description:** Read video descriptions and browse comments in a sleek 30% side panel without covering or stopping the video.
- **Smart Playback Controls:** Loop mode, variable playback speed, reverse-logarithmic seek acceleration, and a "Play Next" countdown for playlists and mixes.

### Ad-Free & Sponsor-Free Watching
- **No Unwanted Interruptions:** Direct stream playback means smooth, uninterrupted watching from start to finish.
- **Built-In SponsorBlock:** Automatically skips sponsors, intros, outros, and self-promotions—or offers a clean one-click skip button per category.

### Made for Your Remote & Account
- **QR-Code Google Sign-In:** Sign in with your phone to access your Home feed, Subscriptions, History, Likes, and synced watch progress across devices.
- **TV Keyboard & Voice Search:** Uses your TV's native on-screen keyboard and voice dictation for fast searching.
- **Customizable Side Menu:** Reorder or hide tabs to match how you watch, with support for Live streams, Playlists, and Channels.
- **Stats for Nerds:** Live overlay showing video/audio codecs, bitrate, buffer health, dropped frames, CPU/memory usage, Tizen OS version, and TV firmware.

---

## Why It Feels So Fast

Most third-party YouTube apps on Samsung TVs run inside the TV's built-in web browser. That browser layer uses extra memory, slows down scrolling, and often causes audio thumps or frame drops because the app has no control over buffering.

**omegatube-modern** skips the web browser completely:
- **Direct to Hardware:** It downloads audio and video streams itself and hands them straight to your TV's native media chip.
- **Instant Launch:** The splash screen appears immediately while local watch history and network security certificates warm up in the background.
- **Fewer Delays Between Videos:** The app remembers recent server addresses, reuses secure connections, and loads video and audio tracks at the same time instead of one after another.
- **Lightweight Interface:** Every card, icon, and animation is drawn using Tizen 10's native hardware-accelerated UI components—no heavy image masks or wasted memory.

---

## Technical Architecture & Optimizations

| Layer | Technology | Role |
| --- | --- | --- |
| **UI Shell** (`app/OmegaTube`) | C# / .NET 8 (`net8.0-tizen10.0`, API 10.0) | Native `Tizen.NUI` screens, GPU-accelerated animations, remote focus handling, TV IME/voice input, settings, and `.tpk` packaging. |
| **Media & Network Core** (`core`) | C++20 (`libomegacore.so`) | HTTPS stack, InnerTube API client, MP4/WebM demuxers with HDR10/HLG metadata, SponsorBlock, and Samsung ESPlayer integration (`libesplusplayer.so`). |

### Native UI & Memory Optimizations
- **Exclusive Tizen 10 Target:** Legacy Tizen 5.5–9.0 workarounds, software fallbacks, and unused assets were removed, keeping the UI and runtime lean.
- **ARMv8 Optimized C++20 Core:** Cross-compiled with `-O3` and ARMv8 NEON/crypto acceleration (`-march=armv8-a -mfpu=crypto-neon-fp-armv8`) with statically linked `libstdc++`/`libgcc` for predictable performance.
- **Native NUI Components & Card Recycling:**
  - Uses GPU `CornerRadius` shaders and `DesiredWidth`/`DesiredHeight` decode-size capping instead of bitmap alpha masks (`AlphaMaskURL`), cutting texture memory and GPU composition passes.
  - Native vector-drawn icons and indicators inside player controls instead of extra PNG textures.
  - Reuses focus `Shadow` and `Animation` instances across card navigation, recycles `Card` instances (and their loaded thumbnails) by video ID across `FeedGrid` and `FeedRow` refreshes, and disposes the 1080p loading backdrop once the first video frame renders.
  - Pure-C# QR encoder (`Util.Qr.Encode`) used for both sign-in and crash screens—eliminating the duplicate C++ `qrcodegen` dependency and P/Invoke JSON round-trip.
  - Shared `OnScreenKeyboard` widget and unified `Widgets.WrapLines` text measurement keep UI code compact and allocations low, while frame-budgeted `UiWork` and `SetShown` culling keep D-pad scrolling smooth.

### Network, Demuxing & Startup Optimizations
- **Non-Blocking Startup & Fast Splash Handoff:** `AppConfig`, `libomegacore.so` loading/P/Invoke binding, `omega_init`, `LocalStore`, and root CA (`cacert.pem`) parsing all run off the UI thread while the splash screen renders immediately (`250 ms` minimum hold so cached Home rows appear right away).
- **Zero Startup Network Contention:** `remote-config.json` hot-fix refresh waits until **after** the initial Home feed finishes loading, and redundant startup GitHub release polling was removed.
- **DNS & TLS Caching:** 60-second in-memory DNS cache and Mbed TLS session resumption per `host:port` eliminate repeated lookups and full TLS handshakes.
- **HTTP Keep-Alive Pool & Gzip Compression:** Persistent connection pooling for `net::fetch()` combined with `Accept-Encoding: gzip` (`zlib` inflate) and pre-allocated response buffers shrinks API payloads and speeds up feed/metadata loads.
- **Parallel Network & Stream Pipelines:**
  - `PlayerScreen` fires stream setup, video details, suggestions/playlist, and SponsorBlock lookups concurrently.
  - `Client::details()` fetches WEB metadata and signed-in TV like/subscription state concurrently on separate threads.
  - Video and audio tracks open, handshake, and parse container headers concurrently at playback start.
- **Zero-Allocation Demux Replay:** Container probing replays the initial header bytes from a fixed stack-sized buffer with a zero-overhead fast path for all subsequent reads.
- **Dynamic Remote Config:** YouTube client parameters live in `remote-config.json` and update automatically in the background so API changes can be resolved without reinstalling.

---

## Install on Your TV

> **Supported TVs:** Samsung Smart TVs running **Tizen 10.0 or newer**. For older Tizen 6.0–9.0 TVs, see [IIyn/omegatube](https://github.com/IIyn/omegatube).

You need a computer (Windows 10/11, macOS, or Linux) on the **same Wi-Fi/network** as your TV and a free **Samsung account** (used to sign the package for your TV).

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

1. The installer downloads the required Samsung TV tools (first time only).
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

*(Stick to one installer method for updates so the signing certificate stays the same.)*

---

## Troubleshooting

| Problem | Solution |
| --- | --- |
| *"The TV did not answer"* | Confirm Developer Mode is **On** with your computer's IP, the TV was **restarted**, both devices are on the same network, and no VPN is blocking the connection. |
| Windows firewall prompt for *sdb* or *Java* | Allow access on private networks so the installer can connect to your TV and receive the Samsung sign-in callback. |
| Signature or certificate error on install | Uninstall any previous version signed with a different certificate on the TV (**Apps → omegatube-modern → long-press OK → Delete**) and run the installer again. |
| Video won't play after a YouTube change | [Report the issue](#reporting-a-bug) with the video link. Many client fixes roll out via `remote-config.json` without needing an app update. |

---

## Reporting a Bug

1. On your TV, open **Settings → About → Report a problem** (or scan the QR code shown automatically if an error occurs).
2. Scan the QR code with your phone to open a pre-filled [GitHub issue](https://github.com/selvarajrajkanna/omegatube-modern/issues/new?template=bug.yml) containing your app version, TV model, Tizen version, and error details.
3. Add a brief note on what happened and submit.

You can optionally attach the session log from the same screen; sensitive items (account tokens, emails, IP addresses, stream URLs, and titles) are stripped automatically before upload.

---

## Contributing

Pull requests and feature ideas are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting code, and open a [feature request](https://github.com/selvarajrajkanna/omegatube-modern/issues/new?template=feature.yml) for new ideas.

For local development, the C++20 core builds natively on Linux (`core/tools` for PC testing) and cross-compiles via Docker (`toolchain/`), while the C# app builds with .NET SDK 8/10 + Tizen workload (`scripts/build-app.sh`).

---

## Credits and License

**omegatube-modern** is free software licensed under the [GNU GPL v3](LICENSE).

- Originally forked from [IIyn/omegatube](https://github.com/IIyn/omegatube).
- Built on the work of [SmartTube](https://github.com/yuliskov/SmartTube), [ytps5-sponsorblock](https://github.com/animatian112/ytps5-sponsorblock), [SponsorBlock](https://sponsor.ajay.app), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls), [zlib](https://zlib.net/), [QuickJS-ng](https://github.com/quickjs-ng/quickjs), and [yt-dlp EJS](https://github.com/yt-dlp/ejs). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list.

YouTube is a trademark of Google LLC. **omegatube-modern** is an independent project and is not affiliated with Google, YouTube, or Samsung.
