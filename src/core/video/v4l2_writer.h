#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace studiocast::video {

    enum class PixelFormat {
        yuyv,
        rgb24,
      };

    std::string PixelFormatName(PixelFormat fmt);
    std::optional<PixelFormat> ParsePixelFormat(const std::string& s);

    struct ActualFormat {
        int width = 0;
        int height = 0;
        int fps = 0;
        int fps_num = 0;
        int fps_den = 0;
        PixelFormat format = PixelFormat::yuyv;

        // V4L2 negotiated pixel format, as FourCC.
        // Example: V4L2_PIX_FMT_YUYV -> "YUYV".
        std::uint32_t pixfmt_fourcc = 0;
        std::string pixfmt;

        std::size_t bytes_per_line = 0;
        std::size_t size_image = 0;
    };

    class V4l2Writer final {
    public:
        V4l2Writer() = default;
        ~V4l2Writer();

        V4l2Writer(const V4l2Writer&) = delete;
        V4l2Writer& operator=(const V4l2Writer&) = delete;

        bool Open(const std::string& device,
                  int width,
                  int height,
                  int fps,
                  PixelFormat fmt,
                  std::string* error);

        void Close();

        // Refresh cached negotiated format from the kernel.
        //
        // This is useful for v4l2loopback: some consumers may renegotiate
        // global caps via VIDIOC_S_FMT. When that happens, the writer must
        // update its cached size_image/bytes_per_line to avoid write() failures.
        bool RefreshActual(std::string* error);

        bool WriteFrame(const std::uint8_t* data, std::size_t bytes, std::string* error);

        bool IsOpen() const { return fd_ >= 0; }
        const ActualFormat& Actual() const { return actual_; }

    private:
        int fd_ = -1;
        ActualFormat actual_{};
    };

}  // namespace studiocast::video
