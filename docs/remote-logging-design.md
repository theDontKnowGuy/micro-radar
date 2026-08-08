# Remote logging and crash reporting

Design for getting logs and crash dumps off radars that live in other people's
houses.

**Status:** step 1 below is implemented — ESP Insights, wired up in
`src/Diagnostics.h`, off until a key is entered on the configuration page. See
the *Remote diagnostics* section of the README for how to turn it on. Everything
else here is still a design.

## The problem, stated honestly

`scripts/pull-coredump.sh` works because the board is on the end of a USB cable.
Once a unit is on a friend's shelf, every assumption that script makes is gone:
there is no port, no host running esptool, and no one who will read a serial
console. The unit is also behind a NAT, so nothing can reach *in* to it. Whatever
we build has to be something the radar does on its own initiative, outbound only.

Three different problems get called "logging", and they want different machinery:

| | What it is | When it is sent | Size |
|---|---|---|---|
| **Rolling log** | The `Serial.print` stream — what the radar was doing | Batched, every minute or so | ~100 KB/day/device |
| **Crash report** | Coredump + the log lines immediately before the panic | On the *next* boot, after the reset | Up to 64 KB, rare |
| **Heartbeat** | Version, uptime, free heap, RSSI, reset reason | Every few minutes | Tens of bytes |

The crash report is the hard one and the valuable one. The rolling log is easy
and mostly useless on its own. Do not build the easy one and call it done.

## What we already have

More than it looks like:

- **Coredumps are already being written.** `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`
  and `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y` are set in the Arduino core's
  sdkconfig, so a panic already lands a full ELF-format dump in the `coredump`
  partition at `0x7F0000`. We are not adding crash capture; we are adding
  *collection*.
- **The API to read it from the running firmware exists.**
  `esp_core_dump_image_get(&addr, &size)` and `esp_core_dump_image_erase()` are
  both present in core 2.0.17 (`include/espcoredump/include/esp_core_dump.h`).
  So the firmware can find its own dump on the next boot and post it.
- **A working outbound TLS fetch with pinned roots** — `FirmwareUpdater` already
  does a background task with a large stack, follows redirects, and refuses
  anything that leaves the pinned hosts.
- **A settings store and a setup UI** — `Preferences` (NVS) plus
  `ConfigurationWebServer`, which is where the per-device logging credential has
  to be entered (see below).
- **A decode script with the right instincts** — `pull-coredump.sh` already
  refuses to decode against a mismatched ELF. That discipline carries over
  unchanged; only the *source* of the dump changes from a serial port to a file.
- **Room.** Flash is 16 MB and `partitions_ota.csv` only describes the first
  8 MB. PSRAM is 8 MB with 129 KB used for the backbuffer.

## The constraint that shapes everything

**Firmware binaries are published on public GitHub releases.** Any ingest key
compiled into the image is a public ingest key. `strings micro-radar-*.bin` will
find it.

This rules out the obvious design — bake a write token in, point it at a log
service — for any service where a leaked write token lets a stranger run up a
bill or poison the log stream.

The fix is cheap and has a useful side effect: **the log endpoint URL and its key
are NVS settings, entered on the configuration page during setup, defaulting to
empty.** Empty means logging is off. That gives us, in one decision:

- no secret in the public binary;
- per-device credentials, so one leaked key can be revoked alone;
- an explicit opt-in for the friend whose house it is, which we need anyway (see
  *Consent*).

## Options for the cloud side

### 1. ESP Insights — Espressif's own remote diagnostics

Worth taking seriously because **it is already compiled into the core we use.**
`libesp_insights.a`, `libesp_diagnostics.a` and `librtc_store.a` all ship in the
esp32s3 SDK, HTTPS transport is the configured default (`client.insights.espressif.com`,
posting every 60–240 s — no persistent MQTT socket), and the Arduino wrapper is a
one-liner:

```cpp
Insights.begin(auth_key, node_id, log_type, /*alloc_ext_ram=*/true);
```

`alloc_ext_ram` puts its buffers in our spare PSRAM. It even solves the
"what was it printing when it died" problem properly, via a 4 KB critical / 6 KB
normal RTC-memory store that survives the reset.

Two things it does **not** do, and they matter:

- **Coredump summary only.** It sends the program counter, exception cause,
  registers and backtrace — not the full dump. Our own `pull-coredump.sh` header
  argues, correctly, that the value of the full dump is "every task's real stack,
  the task that actually died, what the other five were doing, and how much stack
  each had left". Insights gives us strictly less than that.
- **It captures `ESP_LOG*`, not `Serial.print`.** All 67 of our logging call
  sites are invisible to it until they are migrated.

Also note it decodes log strings by *address* against a firmware ELF you upload
to Espressif's cloud, so adopting it means uploading our ELF to them.

**Verdict:** the fastest possible path to "I get told when a radar crashes, with
a backtrace" — genuinely a weekend's work. Not sufficient on its own for the
crashes that need task stacks.

### 2. A managed log service, HTTP ingest

Grafana Cloud (Loki) is the strongest free tier here — [50 GB/month ingest and
14-day retention](https://grafana.com/pricing/), which at our volumes is
effectively unlimited. Ingest is a plain `POST /loki/api/v1/push` with JSON and
Basic auth. Axiom and Better Stack are comparable in shape with smaller free
allowances.

Good for the rolling log and heartbeat. **Wrong shape for a 64 KB binary
coredump** — log pipelines are built for text lines, and base64ing a dump into
one is abuse that you will regret at decode time.

### 3. Object storage for the dumps

Coredumps are binary artifacts, not log events, and should be stored as files
keyed by device and firmware version. Cloudflare R2 (free tier, no egress fees)
or B2. Needs something in front of it to avoid handing devices S3 credentials.

### 4. Your own collector — a Cloudflare Worker in front of R2

About 60 lines of JavaScript. Free tier is 100k requests/day, which 20 radars
posting every minute will not come close to. It gives us:

- **one hostname**, so we pin **one** CA — matching the posture `UpdateRootCAs.h`
  already establishes;
- per-device keys checked at the edge, revocable without a firmware release;
- dumps written to R2 under `dumps/<device>/<version>/<timestamp>.bin`, text
  forwarded on to Loki;
- freedom to keep the ELF private.

### 5. MQTT

Rejected. A persistent TLS socket costs ~40 KB of heap continuously on a device
that has a documented history of `SSL - Memory allocation failed`. Batched HTTPS
costs the same heap for two seconds a minute instead of permanently, and we
already have the code to do it.

## Recommendation

**A Worker + R2 collector (4), with Loki (2) behind it for the text.** Reason:
the full coredump is the artifact this project has already built its debugging
practice around, and only this option delivers it. The per-device-key requirement
from the public-binary constraint needs a checkable endpoint anyway, so most of
the Worker is work we cannot avoid.

**Do ESP Insights (1) first regardless, as a two-hour spike.** It is already
linked; turning it on tells you within a day whether crash *summaries* are enough
for the failures you actually see in the field. If they are, the full-dump path
becomes a nice-to-have rather than the centrepiece, and that is worth knowing
before writing the Worker.

## On-device design

### Capture

Two sources have to converge into one buffer:

1. **Our own prints.** Introduce a `Log::Printf` facade that writes to `Serial`
   *and* the ring buffer, then mechanically replace the 67 `Serial.print*` sites.
   Tedious but low-risk, and it is the point at which log levels can be
   introduced.
2. **The IDF's internal logs** — Wi-Fi association failures, mbedTLS errors,
   heap complaints. These are the messages that explain a remote unit's misery
   and none of them go through `Serial`. Capture them with
   `esp_log_set_vprintf()`, which hands us every `ESP_LOG*` line in the system.

Note `CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL=1` (ERROR) — raising what the IDF emits
needs `-DCORE_DEBUG_LEVEL=3` in `platformio.ini`, and that is a build-size and
volume decision, not a free one.

### Buffering

A 64 KB ring buffer in PSRAM, mutex-protected. Logs arrive from at least four
tasks — the render loop, the updater's checker task, the async web server, and
the Wi-Fi stack — so the lock is not optional.

Overflow drops the **oldest** lines and increments a counter that is reported
with the next batch. The logger must never block the render loop; a stall here
would show up as a stuttering sweep, and diagnosing that as "the log buffer was
full" is a bad afternoon.

The uploader must never log its own failures into the buffer it is uploading.

### The crash path

This is the part that earns its keep.

**Before the crash.** Mirror the last ~4 KB of the ring buffer into an
`RTC_NOINIT_ATTR` buffer with a magic word and a write index. RTC slow memory
survives a panic reset, a watchdog reset and deep sleep — PSRAM and heap do not,
reliably. This is the same trick `librtc_store.a` uses, and it is the only way to
recover the printing that immediately preceded the panic. It does not survive a
power cut, which is fine and detectable: the reset reason will say `POWERON`.

**On the next boot,** before the panel comes up:

1. Read `esp_reset_reason()` — already printed in the current `setup()` diff.
   `ESP_RST_PANIC`, `ESP_RST_TASK_WDT`, `ESP_RST_INT_WDT` and `ESP_RST_BROWNOUT`
   all mean "something went wrong".
2. Check the RTC magic; if valid, that tail is the last thing the radar said.
3. Call `esp_core_dump_image_get()`. A dump present means a real panic.
4. Bring Wi-Fi up, POST the report: reset reason, firmware version, ELF SHA-256,
   uptime before the crash, RTC log tail, then the dump itself.
5. **Erase only after a 2xx.** `esp_core_dump_image_erase()` must not run before
   the server has confirmed receipt, or a crash loop in a house with flaky Wi-Fi
   destroys the evidence. Erasing on ACK also stops a boot loop re-uploading the
   same dump forever.

**Read the dump through the memory-mapped path, not `esp_partition_read()`.**
This is not paranoia: board `ad:18` has a flash chip that returns the first 32
bytes of any longer read and then repeats them. On that unit `esp_partition_read`
will produce a 64 KB file of garbage that decodes into confident nonsense.
`FirmwareUpdater::ActivateSlotVerifiedByMapping()` already contains the working
mmap read for exactly this reason — reuse it.

**Size check.** The `coredump` partition is 64 KB and
`CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=64`. If a dump does not fit it is not written
at all, and the failure is silent. Confirm the real dumps fit; there is 8 MB of
unused flash above `0x800000` to grow into if they do not.

### Transport

A dedicated low-priority task, ~12 KB stack, for the same reason
`ARDUINO_LOOP_STACK_SIZE` was raised to 16384: a TLS handshake does not fit in a
default stack.

Serialize it against the OTA checker with a shared mutex. Two concurrent TLS
handshakes is precisely the heap spike that produced
`SSL - Memory allocation failed` before, and the log uploader must never be the
reason an update fails. It must also stand down entirely while
`FirmwareUpdater::Install()` is running.

Flush on whichever comes first: 8 KB buffered, or 60 seconds. Exponential backoff
to a few minutes on failure. Crash reports jump the queue.

### Safe mode

A radar that panics before Wi-Fi comes up can never report anything, and that is
the exact unit you most need to hear from. Keep a consecutive-crash-boot counter
in NVS. After three, boot into a mode that brings up Wi-Fi and the uploader and
*nothing else* — no panel, no OpenSky. An unresponsive radar showing a static
"recovery" screen while it phones home beats a black box.

This is also the moment to confirm OTA rollback behaviour.
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in the core, which means an
image that never calls `esp_ota_mark_app_valid_cancel_rollback()` is supposed to
be reverted at the next reset. Worth verifying what our `Install()` path actually
leaves the otadata state as — a bad release that crash-loops on ten friends'
shelves is the failure this whole system exists to survive.

## The decode pipeline

**A coredump without the exact matching `firmware.elf` is worthless.**
`pull-coredump.sh` already refuses to guess, and it is right to. Today the ELF
happens to be in `.pio/build/` because you just built it. For a dump that arrives
from a friend's house six weeks and three releases later, it will not be.

So: **`scripts/release.sh` must archive `firmware.elf` for every published
version**, keyed by version and build key. GitHub release assets work, though the
ELF carries full symbols — a private R2 bucket is tidier and keeps symbols out of
public view.

Then split `pull-coredump.sh` in two, keeping all its hard-won knowledge (the
gdb-version trap, reading the partition offset from the CSV rather than
hardcoding it, the SHA-match refusal):

- `pull-coredump.sh` — unchanged, for a board on a cable.
- `decode-coredump.sh <dump-file> <version>` — fetches the archived ELF for that
  version, verifies the SHA against the one in the crash report, decodes.

Without this, remote collection produces a growing pile of undecodable binaries.

## Consent and privacy

These devices are in **other people's homes**, and the logs will contain their
Wi-Fi SSID, their local and public IP, and — since location is a configured
setting — effectively their home address.

That is not a footnote, it is a design input:

- Logging is **off** until someone enters an endpoint and key on the setup page.
- The configuration page states plainly what is collected and where it goes, and
  the same switch turns it all off.
- **Never log** the Wi-Fi password or the OpenSky client secret. Add a redaction
  pass in `Log::Printf` for the known secret settings so a future careless
  `Serial.printf` cannot leak one.
- Round the logged location to ~1 km, or omit it and send only the device ID.
- Tell your friends it is on before you hand them the box.

## Volume and cost

At INFO level in steady state, expect 50–200 lines/hour, so roughly 100–400 KB
per device per day; 20 devices is well under 10 GB/month. That fits inside a free
Loki tier with a wide margin, and R2 will hold years of coredumps for nothing.

The thing that breaks this budget is a device stuck in a fast error loop emitting
the same line thousands of times a minute. Rate-limit identical consecutive
messages in `Log::Printf` — collapse to `[last message repeated N times]`. This
is cheap to add now and annoying to retrofit after it has flooded a bucket.

## Suggested order

1. ~~Turn on ESP Insights.~~ **Done** — `src/Diagnostics.{h,cpp}`, +70 KB flash,
   +2 KB RAM. Now learn whether crash summaries are enough before building any
   of what follows.
2. `Log::Printf` facade + `esp_log_set_vprintf` + PSRAM ring buffer + repeat
   suppression. No network. Verify over serial.
3. RTC tail buffer, and the boot-time reset-reason and coredump detection. Verify
   with a deliberate `abort()`.
4. The Worker + R2 collector, with per-device keys, and the NVS settings and
   consent UI to go with it.
5. Upload path: heartbeat, then batched logs, then crash reports.
6. `release.sh` ELF archival and `decode-coredump.sh`. Do not let step 5 ship
   without this.
7. Safe mode and the crash-boot counter.
