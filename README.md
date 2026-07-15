# mbp-soundloop

A small macOS utility that routes audio from any input device to any output device, designed to run quietly in the background as a launchd agent.

Built for the specific case of routing a Windows PC's audio (via a 3.5mm AUX cable into a USB dock's headphone jack) to a pair of Bluetooth headphones connected to a MacBook.

> **Hardware requirement:** This utility only works if you have physically connected the audio output of the source device (e.g. a PC's headphone jack) to the audio **input** of a dock or adapter that is connected to your Mac. The Mac must see that dock as an audio input device. Without this physical cable connection, there is no audio signal to capture and route.

**Features:**
- Self-healing — reconnects automatically when either device disconnects or the source goes silent
- Sleep-aware — registers with IOKit so macOS can sleep normally; pauses on sleep, resumes on wake
- Idle detection — closes the output stream after configurable silence, releasing CoreAudio's sleep assertion so the Mac's idle timer runs normally
- Gain control — configurable output gain with optional noise gate to suppress USB dock electrical hiss
- Minimal footprint — near-zero CPU when idle; lightweight real-time audio callback when active
- Pure C — no runtime, no garbage collector, no package manager beyond `brew install portaudio`

---

## Requirements

- macOS 12 or later (Darwin/arm64 or Darwin/amd64)
- Xcode Command Line Tools: `xcode-select --install` (provides `clang` and `make`)
- [Homebrew](https://brew.sh)

> **Note:** This utility uses IOKit and CoreAudio, which are macOS-only. `power.c` would need stub replacements to compile on Linux or Windows.

---

## Installation

**1. Install PortAudio**

```sh
brew install portaudio
```

**2. Clone and build**

```sh
git clone https://github.com/akagitsunee/mbp-soundloop
cd mbp-soundloop
make build   # compiles main.c audio.c power.c with clang
```

**3. Install as a background service**

```sh
make install
```

This copies the binary to `/usr/local/bin/mbp-soundloop`, installs the launchd plist to `~/Library/LaunchAgents/`, and starts the agent immediately. It will relaunch automatically on login.

**4. Grant microphone access**

On first run, macOS will prompt for microphone access. You can also grant it manually in **System Settings → Privacy & Security → Microphone**.

---

## Finding your device names

Before configuring, list all available audio devices to find the exact names:

```sh
mbp-soundloop -list
```

Example output:

```
Available audio devices:
  [in:2  out:0 ]  USB Audio
  [in:0  out:2 ]  WH-1000XM4
  [in:0  out:2 ]  MacBook Pro Speakers
  [in:1  out:0 ]  MacBook Pro Microphone
  ...
```

The `-input` and `-output` flags match by substring, so `USB` matches `USB Audio` and `WH-1000XM4` matches exactly.

---

## Usage

```
mbp-soundloop [flags]
```

| Flag | Default | Description |
|---|---|---|
| `-input <name>` | `USB` | Substring to match the input device name |
| `-output <name>` | `WH-1000XM4` | Substring to match the output device name |
| `-list` | — | List all audio devices and exit |
| `-rms` | — | Print RMS, peak, and clip indicator to stdout once per second |
| `-rate <hz>` | `48000` | Sample rate in Hz |
| `-buf <frames>` | `256` | Frames per buffer — see latency table below |
| `-gain <0.0–1.0>` | `0.07` | Output gain multiplier applied to every sample |
| `-gate <rms>` | `0` (off) | Noise gate: mute output when input RMS is below this level |
| `-silence <duration>` | `30s` | How long the input must stay below `-threshold` before the stream closes |
| `-threshold <rms>` | `0.002` | RMS level below which audio is considered silent |
| `-retry <duration>` | `2s` | Delay between reconnect attempts when a device is unavailable |

Duration values accept `s` (seconds), `ms` (milliseconds), or `m` (minutes). A bare number is treated as seconds.

### Buffer size and latency

All figures at 48000 Hz. Bluetooth A2DP (AAC) adds a fixed ~120 ms on top regardless of buffer size.

| `-buf` | PortAudio latency | Total with Bluetooth | Notes |
|--------|-------------------|----------------------|-------|
| `128` | 2.7 ms | ~123 ms | Low; may crackle under CPU load |
| `256` | 5.3 ms | ~126 ms | Default; good balance |
| `512` | 10.7 ms | ~131 ms | Stable on all machines |
| `1024` | 21.3 ms | ~141 ms | |
| `2048` | 42.7 ms | ~163 ms | |

> **Note:** Bluetooth A2DP is unsuitable for latency-critical use (competitive gaming, rhythm games). The ~120 ms floor is inherent to the wireless protocol.

### Gain staging

USB dock analog inputs have an electrical noise floor even with nothing playing. Getting the gain right requires balancing two limits:

- **Too much source volume:** the dock's analog-to-digital converter saturates, introducing harmonic distortion before the signal even reaches this utility. Symptoms: harsh, unclear highs and lows regardless of gain setting.
- **Too much `-gain`:** the dock's noise floor is amplified to a continuous background hiss.

**Finding the right combination using `-rms`:**

```sh
mbp-soundloop -rms
```

Sample output:
```
[rms] 1.23e-02  peak=8.45e-02  thr=2.00e-03
[rms] 9.87e-01  peak=1.00e+00  thr=2.00e-03  *** CLIP ***
```

`*** CLIP ***` means the source volume is overdriving the dock's input. Lower the source volume until it disappears, then adjust `-gain` to reach a comfortable listening level.

**Suppressing background hiss with `-gate`:**

If the dock's noise floor is audible when the source is quiet, set `-gate` just above the noise RMS level you see in `-rms` output when nothing is playing:

```sh
mbp-soundloop -rms   # note the rms value when PC audio is silent
mbp-soundloop -gate 5e-3
```

The gate opens fast (~20 ms) and closes slowly (~600 ms) to avoid clicks at signal transitions.

### Silence detection: threshold vs timeout

These two flags work together. `-threshold` decides what counts as silence; `-silence` decides how long it must last before the stream closes.

**`-threshold`** is the RMS level below which audio is considered silent. USB dock inputs always have an electrical noise floor even with nothing playing. Set this just above that floor so the utility can distinguish "source is off" from "something is playing." Too low and dock noise keeps the stream open forever (Activity Monitor shows "Prevent Sleep: Yes"); too high and quiet audio gets mistaken for silence and cuts out.

**`-silence`** is a timer. Real audio sources have natural quiet gaps — cutscenes, menus, loading screens, pauses between songs. This prevents the stream from closing during those gaps. For gaming or music, `30s` or more is recommended.

**Finding the right threshold:**

```sh
# Watch whether the stream closes when the source is silent
mbp-soundloop -rms -silence 5s
# If the stream never closes, threshold is below the dock's noise floor
# Increase until it closes within a few seconds of the source going quiet
mbp-soundloop -threshold 0.005 -silence 5s
```

---

## Configuring the launchd agent

All flags can be set in `com.mbp-soundloop.plist` under `ProgramArguments`. Edit before `make install`, or edit in place and restart:

```xml
<array>
    <string>/usr/local/bin/mbp-soundloop</string>
    <string>-input</string>
    <string>USB</string>
    <string>-output</string>
    <string>WH-1000XM4</string>
    <string>-buf</string>
    <string>256</string>
    <string>-rate</string>
    <string>48000</string>
    <string>-silence</string>
    <string>30s</string>
</array>
```

```sh
make restart   # reload after editing the plist
make logs      # tail the log file
make status    # check whether the agent is running
make uninstall # remove everything
```

---

## How it works

```
[Windows PC]
    │
    │  3.5mm AUX cable — PC headphone OUT → Dock headphone IN
    │  (this physical cable is required; without it there is no signal)
    ▼
[USB Dock / Adapter with audio input jack]
    │
    │  USB / USB-C / Thunderbolt
    ▼
[MacBook]  ←── mbp-soundloop reads the dock's audio input
    │
    │  Bluetooth (A2DP / AAC)
    ▼
[Headphones / Speakers]
```

The audio hot path is a PortAudio callback in `audio.c` running on a dedicated real-time thread. Each invocation multiplies the interleaved input samples by the configured gain and writes them to the output buffer. An optional noise gate follows: an envelope follower tracks input RMS and smoothly fades the output to zero when the signal falls below the gate threshold. Silence detection and the watchdog tick counter use C11 atomics, read by the main loop every 500 ms.

When the input has been silent for `-silence` seconds, the output stream is closed entirely. A lightweight polling loop then checks the input every 500 ms using a brief input-only stream (which holds no sleep assertion). When signal returns, the full stream reopens.

Sleep/wake is handled via IOKit in `power.c`: a background `pthread` runs a `CFRunLoop` for power notifications. On sleep, `IOAllowPowerChange` is called immediately so macOS does not wait, and `sleeping` is set to 1. The main thread detects stalled callback ticks (within 500 ms), closes the stream with `Pa_AbortStream`, and blocks on a `pthread_cond_t`. On wake, the notification sets `sleeping` back to 0 and broadcasts the condition. `wait_if_sleeping` uses a 200 ms timed wait so that `Ctrl-C` exits the process promptly even while sleeping.

---

## Platform support

| Platform | Audio routing | Sleep/wake handling |
|---|---|---|
| macOS (arm64, amd64) | Yes | Yes (IOKit) |
| Linux | Partial — see note | No |
| Windows | Partial — see note | No |

PortAudio supports Linux (ALSA, JACK) and Windows (WASAPI, ASIO, DirectSound), so `audio.c` is portable. `power.c` uses Apple's IOKit. Replacing it with stubs that provide no-op `init_power_notifications` and `wait_if_sleeping` functions is enough to compile on other platforms.

---

## License

[MIT](LICENSE)
