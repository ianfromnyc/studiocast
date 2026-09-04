# PipeWire backends

StudioCast can speak to the audio and video graph with native PipeWire nodes.
The native backends are optional. The PulseAudio and v4l2loopback paths stay in
the product and stay supported.

## Goals

- Remove the `pactl` and `libpulse` layer when the system runs PipeWire.
- Show the canonical StudioCast devices as first-class PipeWire nodes.
- Detect consumers from graph links, not from `/proc/<pid>/fd` scans.
- Keep one build that works on PulseAudio-only systems.

## Build option

The CMake option `STUDIOCAST_ENABLE_PIPEWIRE` controls the native code. Its
default is ON when `pkg-config` finds `libpipewire-0.3`, and OFF when it does
not. The option sets the compile definition `STUDIOCAST_HAVE_PIPEWIRE` to `1`
or `0`.

    cmake -S . -B build -DSTUDIOCAST_ENABLE_PIPEWIRE=OFF

With `STUDIOCAST_HAVE_PIPEWIRE=0` the program behaves as before. No PipeWire
symbol is referenced and no PipeWire library is linked.

Install the development package first:

- Fedora: `pipewire-devel`
- Ubuntu/Debian: `libpipewire-0.3-dev`

## The three paths

| Path | Default backend | Native PipeWire backend |
| --- | --- | --- |
| Virtual microphone and speakers | `pactl` null sink plus `libpulse` streams | `Audio/Source` and `Audio/Sink` nodes plus `pw_stream` capture and playback |
| Virtual camera output | v4l2loopback `write()` | `Video/Source` node |
| Physical camera capture | V4L2 (`/dev/videoN`) | not done, see *Future work* |

Physical camera capture stays on V4L2.

### Device names

The names do not change with the backend:

| Node name | Description | Media class |
| --- | --- | --- |
| `studiocast_mic` | `StudioCast Microphone` | `Audio/Source` |
| `studiocast_speakers` | `StudioCast Speakers` | `Audio/Sink` |
| `studiocast_camera` | `StudioCast Camera` | `Video/Source` |

Each native node also sets `node.virtual = true`.

## Backend selection

### Audio

Config key `audio.backend`, or the command-line flag `--audio-backend`:

| Value | Result |
| --- | --- |
| `pulse` (default) | always PulseAudio |
| `auto` | native PipeWire when the option is compiled in and a PipeWire server is reachable, else PulseAudio |
| `pipewire` | native PipeWire, with a fall back to PulseAudio and a note when it is not available |

PulseAudio is the default because it works on a PulseAudio server and on a
PipeWire server through `pipewire-pulse`. An upgrade therefore changes nothing
on a machine that already works. Set `auto` or `pipewire` to move to the native
nodes.

A server is "reachable" when a PipeWire socket exists. StudioCast looks for
`$PIPEWIRE_RUNTIME_DIR`, then `$XDG_RUNTIME_DIR`, then `$USERPROFILE`, and
tests for the socket named by `$PIPEWIRE_REMOTE`, or `pipewire-0` when that
variable is not set.

If the native backend fails to start, the daemon falls back to PulseAudio and
puts the reason in the status note. To pin the old behaviour, set
`audio.backend = pulse`.

### Video output

Config key `video.output.backend`, or the flag `--video-output-backend`:

| Value | Result |
| --- | --- |
| `auto` (default) | v4l2loopback only |
| `v4l2loopback` | v4l2loopback only |
| `pipewire` | the `Video/Source` node, beside v4l2loopback |
| `both` | v4l2loopback and the node together |

v4l2loopback stays the source of truth for the frame format, because the
pipeline negotiates the output size, rate and pixel format with the loopback
device. `pipewire` therefore still opens the loopback device today. A
node-only mode is future work.

`auto` does not select PipeWire because most video conference applications read
V4L2 devices only. Mirroring the frames to both outputs is cheap: the node
writes the same buffer that goes to v4l2loopback.

The node is a graph driver. A frame arriving from the pipeline triggers a
cycle, so the consumer receives frames at the pipeline rate. Without that the
graph has no clock for the link and a consumer receives nothing.

| Consumer | v4l2loopback | PipeWire node |
| --- | --- | --- |
| Zoom, Teams, most Chromium builds | yes | no |
| OBS "Video Capture Device (V4L2)" | yes | no |
| GStreamer `pipewiresrc target-object=studiocast_camera` | no | yes |
| `pw-cat --record` and other node-addressing tools | no | yes |
| Camera portal clients (Firefox, GNOME, KDE) | yes | no |

The camera portal shows only the devices that the PipeWire camera manager owns
(libcamera and V4L2). It does not show an application node. Keep v4l2loopback
on for portal clients.

## Consumer detection

The PulseAudio path counts `pactl` source outputs and sink inputs. The V4L2
path scans `/proc/<pid>/fd` for open handles on the loopback device.

The native path listens to the PipeWire registry:

- Bind the registry and keep a map of `Link` objects.
- A link carries `link.output.node` and `link.input.node`.
- For `studiocast_mic` (an `Audio/Source`), count the links whose
  `link.output.node` is the node id.
- For `studiocast_speakers` (an `Audio/Sink`), count the links whose
  `link.input.node` is the node id.
- For `studiocast_camera` (a `Video/Source`), count the links whose
  `link.output.node` is the node id.

Link events arrive on the PipeWire thread loop. The count is published through
an atomic, so the supervisor thread reads it without a lock. There is no poll
and no process scan.

## Latency

The native audio node asks for a 10 ms quantum with the property
`node.latency = 480/48000`. The server can give a smaller quantum. A
single-producer single-consumer ring decouples the real-time callback from the
10 ms pipeline frame, so the pipeline format does not change.

Expected added latency:

- Graph quantum: 5 ms to 20 ms, set by the server.
- StudioCast ring: one pipeline frame, 10 ms.
- Effects: see `docs/LATENCY_POLICY.md`.

This is the same order as the PulseAudio path, which uses 10 ms fragments.

## Safety

The rules in `src/core/audio/audio_device_safety.cpp` still apply. The native
backend refuses to capture from a StudioCast virtual source and refuses to
play into a StudioCast virtual sink. This stops a feedback loop.

## Status fields

`studiocastctl status` reports the active backend:

- `audio.transport_backend`: `pulse` or `pipewire`.
- `audio.transport_backend_note`: the reason for a fallback, if any.
- `video.output_backends`: an array with `v4l2loopback`, `pipewire`, or both.

The GUI shows the same values on the Audio page and the Video page.

## Tests

`tests/pipewire_backend_tests.cpp` holds both kinds of test.

Tests that need no server: the two selection rules, the socket probe with
injected environment hooks, the node property arithmetic, the canonical names,
and the refusal to open the pipeline I/O before the virtual microphone exists.

Live tests: they run only when the build has PipeWire, a server socket
answers, and `pw-dump` is on the path. One of them also needs
`gst-launch-1.0`. Without those the test prints a `[SKIP]` line and passes, so
continuous integration and the RPM `%check` stay green.

- The virtual source node reaches the graph and `pw-dump` lists it.
- The virtual source accepts writes with no consumer attached.
- The native virtual microphone comes up under its canonical name.
- The camera node reaches the graph as a `Video/Source`.
- A real `gst-launch-1.0 pipewiresrc` consumer links to the camera node and
  receives frames.

Verified on Fedora 44, PipeWire 1.6.8:

    $ pw-dump | ... # application.name == "StudioCast"
    {"id": 130, "node.name": "studiocast_mic",
     "node.description": "StudioCast Microphone", "media.class": "Audio/Source",
     "node.virtual": true, "node.latency": "480/48000", "node.rate": "1/48000"}
    {"id": 93,  "node.name": "studiocast_speakers",
     "node.description": "StudioCast Speakers", "media.class": "Audio/Sink",
     "node.virtual": true, "node.latency": "480/48000", "node.rate": "1/48000"}
    {"id": 93,  "node.name": "studiocast_camera",
     "node.description": "StudioCast Camera", "media.class": "Video/Source",
     "node.virtual": true, "media.role": "Camera"}

    $ pw-cat --record --target 130 cap.wav    # 757804 bytes in 4 s
    $ gst-launch-1.0 pipewiresrc target-object=studiocast_camera \
        num-buffers=20 ! videoconvert ! jpegenc ! multifilesink ...
    # 20 frames written

## Future work

PipeWire camera capture, as an alternative to V4L2 capture, is not done. The
seam is `studiocast::video::CameraPipeline::Start`, which opens a
`V4l2Capture`. A native capture backend must:

- Connect to the camera manager with `PW_KEY_MEDIA_ROLE = "Camera"`, or take a
  node id from the camera portal.
- Give `CameraPipeline` the same `CaptureFormat` and frame buffer that
  `V4l2Capture` gives it today.
- Keep the format negotiation rules in `docs/PIPELINE_POLICY.md`.

The portal path also needs a D-Bus dependency, which the daemon does not have
today.
