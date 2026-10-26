#include "shared_frame.h"

#include <cstring>

namespace wcs {

// Readers map the channel read-only, so this must not be an interlocked
// (writing) operation; ReadAcquire64 is an atomic load on x86 and x64.
static LONG64 load(const volatile LONG64 *value)
{
    return ReadAcquire64(value);
}

static LONG64 qpcTime100ns()
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);

        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    return LONG64(now.QuadPart / frequency.QuadPart * 10000000
                  + now.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart);
}

// --- FrameWriter ---------------------------------------------------------

static bool writerIsAlive(DWORD pid, uint64_t heartbeatMs)
{
    if (!pid || pid == GetCurrentProcessId()
        || !heartbeatMs || GetTickCount64() - heartbeatMs >= kWriterTimeoutMs)
        return false;

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);

    if (!process)
        return false;

    bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);

    return running;
}

FrameWriter::~FrameWriter()
{
    close();
}

bool FrameWriter::open()
{
    if (m_header)
        return true;

    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, kMappingSize, kMappingName);

    if (!m_mapping)
        return false;

    void *view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize);
    m_events[0] = CreateEventW(nullptr, TRUE, FALSE, kEventName0);
    m_events[1] = CreateEventW(nullptr, TRUE, FALSE, kEventName1);

    if (!view || !m_events[0] || !m_events[1]) {
        if (view)
            UnmapViewOfFile(view);

        close();

        return false;
    }

    auto header = static_cast<SharedHeader *>(view);

    // Two writers would interleave frames from independent sequences and
    // clients would flicker between them: refuse while another one is live.
    if (header->magic == kMagic && header->version == kVersion
        && writerIsAlive(DWORD(header->writerPid), uint64_t(load(&header->heartbeatMs)))) {
        UnmapViewOfFile(view);
        close();
        SetLastError(ERROR_BUSY);

        return false;
    }

    m_header = header;
    m_data = static_cast<uint8_t *>(view) + kDataOffset;

    // Readers may keep an old mapping alive across writer restarts, so
    // continue the sequence instead of starting over.
    if (m_header->magic == kMagic && m_header->version == kVersion) {
        m_next = load(&m_header->latest) + 1;
    } else {
        m_header->maxWidth = kMaxWidth;
        m_header->maxHeight = kMaxHeight;
        m_header->slotCount = kSlots;
        m_header->slotStride = kSlotStride;
        m_header->dataOffset = kDataOffset;
        m_header->version = kVersion;
        InterlockedExchange64(&m_header->latest, 0);
        MemoryBarrier();
        m_header->magic = kMagic;
        m_next = 1;
    }

    InterlockedExchange(&m_header->writerPid, LONG(GetCurrentProcessId()));

    return true;
}

void FrameWriter::close()
{
    if (m_header) {
        InterlockedExchange64(&m_header->heartbeatMs, 0);
        InterlockedExchange(&m_header->writerPid, 0);
        UnmapViewOfFile(m_header);
        m_header = nullptr;
        m_data = nullptr;
    }

    for (auto &event: m_events)
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }

    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}

uint8_t *FrameWriter::beginFrame(uint32_t width, uint32_t height)
{
    if (!m_header || width > kMaxWidth || height > kMaxHeight || (width | height) & 1)
        return nullptr;

    auto &slot = m_header->slots[m_next % kSlots];
    InterlockedExchange64(&slot.seq, 0);
    m_width = width;
    m_height = height;

    return m_data + size_t(m_next % kSlots) * kSlotStride;
}

void FrameWriter::publish(uint32_t kind)
{
    if (!m_header)
        return;

    auto &slot = m_header->slots[m_next % kSlots];
    slot.timestamp = qpcTime100ns();
    slot.kind = kind;
    slot.width = m_width;
    slot.height = m_height;
    InterlockedExchange64(&slot.seq, m_next);
    InterlockedExchange64(&m_header->latest, m_next);
    InterlockedExchange64(&m_header->heartbeatMs, LONG64(GetTickCount64()));
    SetEvent(m_events[m_next % 2]);
    ResetEvent(m_events[(m_next + 1) % 2]);
    m_next++;
}

// --- FrameReader ---------------------------------------------------------

FrameReader::~FrameReader()
{
    close();
}

bool FrameReader::tryOpen()
{
    if (m_header)
        return true;

    // Opening fails while the app is not running; don't hammer the kernel.
    auto now = GetTickCount64();

    if (m_lastOpenAttempt && now - m_lastOpenAttempt < 1000)
        return false;

    m_lastOpenAttempt = now;
    m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);

    if (!m_mapping)
        return false;

    void *view = MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, kMappingSize);
    m_events[0] = OpenEventW(SYNCHRONIZE, FALSE, kEventName0);
    m_events[1] = OpenEventW(SYNCHRONIZE, FALSE, kEventName1);
    auto header = static_cast<const SharedHeader *>(view);

    if (!view || !m_events[0] || !m_events[1]
        || header->magic != kMagic || header->version != kVersion
        || header->maxWidth != kMaxWidth || header->maxHeight != kMaxHeight) {
        if (view)
            UnmapViewOfFile(view);

        close();

        return false;
    }

    m_header = header;
    m_data = static_cast<const uint8_t *>(view) + kDataOffset;

    return true;
}

void FrameReader::close()
{
    if (m_header) {
        UnmapViewOfFile(m_header);
        m_header = nullptr;
        m_data = nullptr;
    }

    for (auto &event: m_events)
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }

    if (m_mapping) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}

bool FrameReader::writerAlive() const
{
    if (!m_header)
        return false;

    auto heartbeat = uint64_t(load(&m_header->heartbeatMs));

    return heartbeat && GetTickCount64() - heartbeat < kWriterTimeoutMs;
}

bool FrameReader::waitNewer(LONG64 lastSeq, DWORD timeoutMs, HANDLE cancel)
{
    if (!m_header || !writerAlive())
        return WaitForSingleObject(cancel, timeoutMs) != WAIT_OBJECT_0;

    if (load(&m_header->latest) > lastSeq)
        return WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0;

    HANDLE handles[] = {cancel, m_events[(lastSeq + 1) % 2]};

    return WaitForMultipleObjects(2, handles, FALSE, timeoutMs) != WAIT_OBJECT_0;
}

bool FrameReader::readLatest(uint8_t *dst, FrameInfo *info)
{
    if (!writerAlive())
        return false;

    auto n = load(&m_header->latest);

    if (n <= 0 || n == info->seq)
        return n > 0 && n == info->seq;

    auto &slot = m_header->slots[n % kSlots];

    if (load(&slot.seq) != n)
        return false;

    // Read the size before the pixels; both are validated by the re-check.
    uint32_t width = slot.width;
    uint32_t height = slot.height;
    uint32_t kind = slot.kind;

    if (!width || !height || width > kMaxWidth || height > kMaxHeight)
        return false;

    memcpy(dst, m_data + size_t(n % kSlots) * kSlotStride, nv12Size(width, height));
    MemoryBarrier();

    // The writer lapped us while copying: drop this copy.
    if (load(&slot.seq) != n)
        return false;

    *info = {n, kind, width, height};

    return true;
}

}  // namespace wcs
