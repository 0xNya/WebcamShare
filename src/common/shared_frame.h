// Shared-memory frame channel between the WebcamShare app (single writer)
// and the virtual camera filter loaded in every client process (readers).
//
// Layout: SharedHeader, then kSlots NV12 frames at kDataOffset + i * kSlotStride,
// each up to kMaxWidth x kMaxHeight (the size is stored per slot).
// Frame n (n >= 1) goes to slot n % kSlots. A slot's seq is 0 while it is
// being written and n once frame n is complete; `latest` is the last
// complete n. Readers copy a slot and re-check its seq to detect tearing.
// Writer signals "frame n ready" by setting event n % 2 and resetting the
// other one, so a reader waiting for n + 1 waits on event (n + 1) % 2.
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

// The padding from alignas below is intended.
#pragma warning(disable: 4324)

namespace wcs {

constexpr wchar_t kMappingName[] = L"Local\\WebcamShare.Frames.v2";
constexpr wchar_t kEventName0[] = L"Local\\WebcamShare.Ready0.v2";
constexpr wchar_t kEventName1[] = L"Local\\WebcamShare.Ready1.v2";

constexpr uint32_t kMagic = 0x32534357;  // "WCS2"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1080;
constexpr uint32_t kFps = 30;
constexpr int64_t kFrameDuration = 10000000 / kFps;  // 100 ns units
constexpr uint32_t kMaxFrameSize = kMaxWidth * kMaxHeight * 3 / 2;  // NV12
constexpr uint32_t kSlots = 3;
constexpr uint32_t kDataOffset = 4096;
constexpr uint32_t kSlotStride = (kMaxFrameSize + 4095) & ~4095u;
constexpr uint32_t kMappingSize = kDataOffset + kSlots * kSlotStride;

// A writer that has not published for this long is considered gone.
constexpr uint64_t kWriterTimeoutMs = 1500;

inline uint32_t nv12Size(uint32_t width, uint32_t height)
{
    return width * height * 3 / 2;
}

enum : uint32_t {
    FrameLive = 0,        // real camera picture
    FramePlaceholder = 1  // writer has no camera, frame is its placeholder
};

struct alignas(64) SlotHeader {
    volatile LONG64 seq;
    LONG64 timestamp;  // QPC-based, 100 ns units
    uint32_t kind;     // FrameLive / FramePlaceholder
    uint32_t width;    // even, <= kMaxWidth
    uint32_t height;   // even, <= kMaxHeight
};

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t reserved;
    uint32_t slotCount;
    uint32_t slotStride;
    uint32_t dataOffset;
    volatile LONG64 latest;
    volatile LONG64 heartbeatMs;  // GetTickCount64() of the last publish
    volatile LONG writerPid;
    SlotHeader slots[kSlots];
};

static_assert(sizeof(SharedHeader) <= kDataOffset, "header too large");
// 32-bit clients read what the 64-bit app writes: the layout must not depend
// on the architecture.
static_assert(sizeof(SlotHeader) == 64, "slot header layout");
static_assert(offsetof(SharedHeader, latest) == 32, "header layout");
static_assert(offsetof(SharedHeader, writerPid) == 48, "header layout");
static_assert(offsetof(SharedHeader, slots) == 64, "header layout");

// A frame as stored in the channel.
struct FrameInfo {
    LONG64 seq = 0;
    uint32_t kind = FrameLive;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Writer side, used by the app and by the test pattern feeder.
class FrameWriter {
public:
    FrameWriter() = default;
    ~FrameWriter();
    FrameWriter(const FrameWriter &) = delete;
    FrameWriter &operator=(const FrameWriter &) = delete;

    // Fails with ERROR_BUSY while another live process is writing.
    bool open();
    void close();
    bool isOpen() const { return m_header != nullptr; }

    // Returns the buffer for the next frame (NV12, stride = width). Call
    // publish() when it is filled. The buffer stays valid until publish().
    uint8_t *beginFrame(uint32_t width, uint32_t height);
    void publish(uint32_t kind);

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_events[2] = {};
    SharedHeader *m_header = nullptr;
    uint8_t *m_data = nullptr;
    LONG64 m_next = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

// Reader side, used by the filter.
class FrameReader {
public:
    FrameReader() = default;
    ~FrameReader();
    FrameReader(const FrameReader &) = delete;
    FrameReader &operator=(const FrameReader &) = delete;

    // Opens the channel if the writer created it; cheap to call repeatedly.
    bool tryOpen();
    void close();

    // Waits up to timeoutMs for a frame newer than lastSeq, or for `cancel`.
    // Returns false if `cancel` was signaled.
    bool waitNewer(LONG64 lastSeq, DWORD timeoutMs, HANDLE cancel);

    // Makes dst (kMaxFrameSize bytes) hold the newest complete frame:
    // copies it if it is newer than info->seq, otherwise leaves dst as is.
    // Returns false if there is no live writer or the copy was torn;
    // *info is updated when a frame is copied.
    bool readLatest(uint8_t *dst, FrameInfo *info);

    bool writerAlive() const;

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_events[2] = {};
    const SharedHeader *m_header = nullptr;
    const uint8_t *m_data = nullptr;
    ULONGLONG m_lastOpenAttempt = 0;
};

}  // namespace wcs
