# Native DSD and bit-perfect PCM on Linux

Stop playback before changing output options, then start a track again to
create a pipeline with the new settings.
In **Settings → Backend**, select **ALSA** and a direct **hw:** device.
For a Topping DX3 Pro+, a stable device name is usually `hw:CARD=Pro,DEV=0`
(equivalent to `hw:Pro,0`). Check `/proc/asound/cards` for your card ID.
Use the hardware device rather than `plughw:`, `default`, PulseAudio or PipeWire
when you want Strawberry to send samples directly to the DAC.

Enable **Bit-perfect output (ALSA hardware)**. This bypasses Strawberry's
software volume, balance, equalizer, ReplayGain, EBU R 128 normalization,
crossfeed, channel mixing and fades, including previously enabled effects.
Control listening volume on the DAC. The source sample rate and channel count
are preserved. Integer PCM may be repacked or widened without changing sample
values (for example, 16-bit FLAC to the DX3 Pro+'s 32-bit PCM interface).
Dithering, noise shaping and resampling are disabled. An unsupported rate,
channel count or integer precision produces a playback error rather than
silently reducing quality. Floating point decoder output can be converted to
an integer DAC format; bit-perfect guarantees apply to integer PCM sources.

**Automatic native DSD for DSF files (ALSA hardware)** is enabled by default.
With direct ALSA output, `.dsf` files take this path:

```text
DSF demuxer → DSD queue → dsdconvert → ALSA native DSD
```

`dsdconvert` changes byte packing, bit order and channel interleaving to match
the DAC; it does not convert DSD to PCM. Native DSD always bypasses audio
processing and software volume, independently of the bit-perfect PCM option.
Ordinary tracks remain PCM. Both playbin and playbin3 are supported.

**DAC settling delay** in **Settings → Backend → Buffer** controls the wait
between configuring the audio device and starting the track. The default is
500 ms; try 300–500 ms for the DX3 Pro+. Playback prerolls with the selected
sample rate and PCM/DSD format, then holds the track paused until this delay
elapses. The opening notes are retained and audio samples are unchanged.

With direct/exclusive hardware output and a nonzero delay, each new file uses
a fresh pipeline, waits for the previous pipeline to release the device, and
applies the settling delay. This also covers manual skips, automatic advances
and PCM/DSD switches. Consecutive CUE sections of the same file stay continuous.
Set the delay to 0 to allow gapless playback between tracks on the same path.
PCM/DSD boundaries always need separate pipelines. Spectrum visualizers do not
receive native DSD buffers.

Native routing currently recognizes `.dsf` URL paths (case-insensitively).
DSDIFF/DFF and extensionless DSD URLs retain the existing decoding behavior.
Disable automatic native DSD to decode DSF to PCM, or use another output
backend. A failed native DSD negotiation reports an error; it does not silently
fall back to PCM.

## Dependencies

GStreamer 1.24 or newer, built with native DSD support, is required, including:

- `avdemux_dsf` from gst-libav
- `dsdconvert` from gst-plugins-base
- `alsasink` from the ALSA plugin, advertising `audio/x-dsd`

The kernel and DAC must expose an ALSA DSD format. Strawberry detects missing
DSD support in the sink at runtime; a newer GStreamer is not required to build
Strawberry or to use ordinary PCM playback.

Check the installed plugins:

```bash
gst-inspect-1.0 avdemux_dsf
gst-inspect-1.0 dsdconvert
gst-inspect-1.0 alsasink
```

Upstream documents [DSD packing and bit order](https://gstreamer.freedesktop.org/documentation/audio/gstdsd.html)
and the [ALSA sink's native DSD caps](https://gstreamer.freedesktop.org/documentation/alsa/alsasink.html).

## Verify the actual DAC output

While a DSF is playing, inspect the card's active ALSA parameters. Replace `4`
with your card number from `/proc/asound/cards`:

```bash
cat /proc/asound/card4/pcm0p/sub0/hw_params
cat /proc/asound/card4/stream0
```

The DX3 Pro+ should show `format: DSD_U32_BE` and USB alternate setting 3 for
native DSD. DSD64 uses an ALSA word rate of 88200 Hz with this 32-bit format;
that is 2,822,400 DSD bits per second per channel, not 88.2 kHz PCM.
On switching to a PCM track, the format should become `S32_LE`, with the track's
original sample rate. Other programs using the DAC must release the device
before Strawberry can open a direct hardware stream.

## Build and regression tests

Use the normal CMake build. On this checkout, the verified build command is:

```bash
cmake -S . -B build-native-dsd -DCMAKE_BUILD_TYPE=Release -DBUILD_WERROR=ON \
  -DENABLE_LAYERSHELLQT=OFF -DENABLE_TRANSLATIONS=OFF -DENABLE_STREAMTAGREADER=OFF
cmake --build build-native-dsd --target strawberry gstenginepipeline_test --parallel 8
./build-native-dsd/tests/gstenginepipeline_test
```

The three disabled optional features have missing development dependencies on
this machine; none is required for audio output.

Regression tests capture output without opening a DAC. They compare all native
DSD bits against synthetic DSF files with both bit orders and both playbins,
verify exact 16/24-bit PCM widening at 96 kHz with effects enabled, and check
that sample-rate conversion, channel mixing and precision reduction fail in
bit-perfect mode.
