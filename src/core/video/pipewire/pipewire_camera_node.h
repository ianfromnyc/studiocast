#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/pipewire/pipewire_support.h"
#include "core/pipewire/triple_frame_buffer.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video::pw_backend {

struct CameraNodeConfig {
  // node.name and node.description. Empty values take the canonical
  // "studiocast_camera" and "StudioCast Camera".
  std::string node_name;
  std::string node_description;

  int width = 1280;
  int height = 720;
  int fps = 30;

  // rgb24 maps to SPA_VIDEO_FORMAT_RGB, yuyv maps to SPA_VIDEO_FORMAT_YUY2.
  PixelFormat format = PixelFormat::rgb24;

  // Bytes between two rows of the frames WriteFrame receives. Zero means the
  // rows are packed.
  //
  // The camera pipeline hands over the buffer it writes to the loopback
  // device, and the driver can ask for padding after every row. The node hands
  // its consumers packed rows, so it drops that padding on the way in. A node
  // that assumed a packed source would read every row after the first from the
  // wrong offset, which shears the picture.
  std::size_t stride_bytes = 0;
};

// A PipeWire Video/Source node fed with the processed camera frames.
//
// The pipeline stages a frame and the real-time callback hands the newest one
// to the server. A slow consumer therefore drops frames instead of holding the
// pipeline back, which is the latest-frame-wins rule the rest of the video
// path follows.
//
// In a build with STUDIOCAST_HAVE_PIPEWIRE=0, Start always fails and says why.
class PipeWireCameraNode final {
public:
  // Holds the PipeWire objects. The definition stays in the source file, so no
  // PipeWire header leaks into the rest of StudioCast. It is public only so
  // the C callbacks can name it.
  struct Impl;

  PipeWireCameraNode();
  ~PipeWireCameraNode();

  PipeWireCameraNode(const PipeWireCameraNode &) = delete;
  PipeWireCameraNode &operator=(const PipeWireCameraNode &) = delete;

  bool Start(const CameraNodeConfig &cfg, std::string *error);
  void Stop();

  bool IsRunning() const;

  // Stages one frame. It never blocks.
  bool WriteFrame(const std::uint8_t *data, std::size_t bytes,
                  std::string *error);

  // PipeWire global id of the node, or 0 before the node reaches the graph.
  std::uint32_t NodeId() const;

  // Number of graph links that consumers hold on this node.
  int ConsumerCount() const;

  // Frames the server took, and frames that a newer one replaced.
  std::uint64_t FramesSent() const;
  std::uint64_t FramesDropped() const;

  std::string LastError() const;

private:
  std::unique_ptr<Impl> impl_;
};

namespace internal {

// Compares the video format the server negotiated with the one the node
// offered. Returns an empty string when they match, and a message that names
// both otherwise.
//
// The node offers exactly one format, so a negotiated format that differs is
// a node that will never hand out a frame: the buffer answer is built from the
// configured size, and the callback refuses to give one for anything else. The
// caller must therefore take such a node down, not only record the reason.
//
// The formats are the plain SPA video format numbers, so this rule needs no
// PipeWire header.
std::string CameraFormatMismatch(std::uint32_t offered_format, int width,
                                 int height, std::uint32_t negotiated_format,
                                 std::uint32_t negotiated_width,
                                 std::uint32_t negotiated_height);

// What WriteFrame answers for one publish.
struct FrameWriteAnswer {
  // What WriteFrame returns.
  bool ok = true;
  // True for a frame that replaced one the callback never took.
  bool dropped = false;
  // Empty unless the publish was refused.
  std::string error;
};

// Turns the answer of the frame hand-off into the answer of a write.
//
// A refused publish is a layout error, not a dropped frame: the node copied
// nothing, and no consumer will ever see that frame. It must reach the
// pipeline as a failure with a reason, or a node whose row arithmetic went
// wrong would drop every frame while the status said "running" and the drop
// count stayed at zero.
FrameWriteAnswer FrameWriteAnswerOf(studiocast::pw::PublishOutcome outcome);

} // namespace internal

// Bytes one packed row of this format needs, which is the row size the node
// hands its consumers. It is the same rule the loopback writer follows, so an
// odd YUYV width counts the last pixel pair whole.
std::size_t CameraStrideBytes(int width, PixelFormat format);

// Bytes one frame of this format needs, with the rows packed.
std::size_t CameraFrameBytes(int width, int height, PixelFormat format);

} // namespace studiocast::video::pw_backend
