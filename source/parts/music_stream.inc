/* Bounded streaming WAV reader. HTTP fragments need not align with headers or PCM frames. */
#define MUSIC_BUFFER_FRAMES 4096
#define MUSIC_BUFFER_COUNT 8
typedef struct {
    u8 *data;
    size_t capacity, read, used;
    bool riff, format, ready, failed, limited;
    u32 skip, header_bytes, rate;
    u16 channels, frame_bytes;
    u64 remaining;
} MusicPcmStream;

static u16 music_u16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 music_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static bool music_ring_write(MusicPcmStream *stream, const u8 *data, size_t size)
{
    if (!stream->data || !stream->capacity || size > stream->capacity - stream->used) return false;
    size_t at = stream->read + stream->used;
    if (at >= stream->capacity) at -= stream->capacity;
    size_t first = size < stream->capacity - at ? size : stream->capacity - at;
    memcpy(stream->data + at, data, first);
    memcpy(stream->data, data + first, size - first);
    stream->used += size;
    return true;
}

static void music_ring_peek(const MusicPcmStream *stream, u8 *out, size_t size)
{
    size_t first = size < stream->capacity - stream->read ? size : stream->capacity - stream->read;
    memcpy(out, stream->data + stream->read, first);
    memcpy(out + first, stream->data, size - first);
}

static void music_ring_drop(MusicPcmStream *stream, size_t size)
{
    stream->read += size;
    if (stream->read >= stream->capacity) stream->read -= stream->capacity;
    stream->used -= size;
}

static bool music_wav_header(MusicPcmStream *stream)
{
    u8 header[64];
    if (stream->failed || stream->ready) return stream->ready;
    if (!stream->riff) {
        if (stream->used < 12) return false;
        music_ring_peek(stream, header, 12);
        if ((memcmp(header, "RIFF", 4) && memcmp(header, "RF64", 4)) || memcmp(header + 8, "WAVE", 4)) {
            stream->failed = true; return false;
        }
        stream->riff = true;
        music_ring_drop(stream, 12);
    }
    for (int chunks = 0; chunks < 32; ++chunks) {
        if (stream->skip) {
            size_t take = stream->used < stream->skip ? stream->used : stream->skip;
            music_ring_drop(stream, take);
            stream->skip -= take;
            if (stream->skip) return false;
        }
        if (stream->used < 8) return false;
        music_ring_peek(stream, header, 8);
        u32 size = music_u32(header + 4);
        if (!memcmp(header, "data", 4)) {
            if (!stream->format || (size != UINT32_MAX && size % stream->frame_bytes)) {
                stream->failed = true; return false;
            }
            music_ring_drop(stream, 8);
            stream->ready = true;
            stream->limited = size != UINT32_MAX;
            stream->remaining = size;
            return true;
        }
        if (size > 1024 * 1024 || stream->header_bytes > 1024 * 1024 - size) {
            stream->failed = true; return false;
        }
        if (!memcmp(header, "fmt ", 4)) {
            if (size < 16 || size > 4096) { stream->failed = true; return false; }
            size_t need = size < 40 ? size : 40;
            if (stream->used < 8 + need) return false;
            music_ring_peek(stream, header, 8 + need);
            u16 format = music_u16(header + 8), channels = music_u16(header + 10);
            u32 rate = music_u32(header + 12);
            if (format == 0xFFFE && size >= 40) {
                static const u8 pcm_guid[16] = {1,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113};
                if (music_u16(header + 24) >= 22 && music_u16(header + 26) == 16 && !memcmp(header + 32, pcm_guid, 16)) format = 1;
            }
            if (format != 1 || (channels != 1 && channels != 2) || rate < 8000 || rate > 48000 ||
                music_u16(header + 22) != 16 || music_u16(header + 20) != channels * 2) {
                stream->failed = true; return false;
            }
            stream->channels = channels; stream->rate = rate; stream->frame_bytes = channels * 2;
            stream->format = true;
        }
        stream->header_bytes += size;
        music_ring_drop(stream, 8);
        stream->skip = size + (size & 1);
    }
    return false;
}

static size_t music_pcm_available(const MusicPcmStream *stream)
{
    if (!stream->ready || !stream->frame_bytes) return 0;
    size_t bytes = stream->used;
    if (stream->limited && stream->remaining < bytes) bytes = (size_t)stream->remaining;
    return bytes - bytes % stream->frame_bytes;
}

static size_t music_pcm_read(MusicPcmStream *stream, u8 *out, size_t capacity)
{
    size_t bytes = music_pcm_available(stream);
    if (bytes > capacity) bytes = capacity - capacity % stream->frame_bytes;
    music_ring_peek(stream, out, bytes);
    music_ring_drop(stream, bytes);
    if (stream->limited) stream->remaining -= bytes;
    return bytes;
}
