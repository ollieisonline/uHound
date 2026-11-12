#include "http_stream_io.h"

#include <cstdio>

HttpStreamIO::HttpStreamIO(std::string host, int port, std::string path)
    : reader_(std::move(host), port, std::move(path)) {}

HttpStreamIO::~HttpStreamIO() {
    if (avio_ctx_) {
        // avio_ctx_->buffer may have been reallocated internally by
        // FFmpeg since we handed it avio_internal_buffer_ -- always free
        // via the context's own pointer, not our original one.
        av_free(avio_ctx_->buffer);
        avio_context_free(&avio_ctx_);
    }
}

bool HttpStreamIO::open() {
    if (!reader_.open()) {
        last_error_ = "HttpStreamReader::open failed: " + reader_.lastError();
        return false;
    }

    const int avioBufferSize = 32 * 1024; // FFmpeg's own conventional default
    avio_internal_buffer_ = (uint8_t*)av_malloc(avioBufferSize);

    avio_ctx_ = avio_alloc_context(
        avio_internal_buffer_, avioBufferSize,
        0, // write_flag: 0 = read-only
        this,
        &HttpStreamIO::readPacketTrampoline,
        nullptr, // no write callback, we're read-only
        &HttpStreamIO::seekTrampoline
    );

    if (!avio_ctx_) {
        last_error_ = "avio_alloc_context failed";
        return false;
    }

    // See the header comment: seekability follows the server. A live
    // chunked transcode stays non-seekable; a whole-file endpoint with
    // "Accept-Ranges: bytes" gets a real seekable input.
    avio_ctx_->seekable = reader_.seekable() ? AVIO_SEEKABLE_NORMAL : 0;

    return true;
}

int HttpStreamIO::readPacket(uint8_t* buf, int bufSize) {
    int n = reader_.read(buf, bufSize);
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    return n;
}

int64_t HttpStreamIO::seek(int64_t offset, int whence) {
    // Non-seekable stream (Jellyfin's chunked live transcode): returning
    // -1 tells FFmpeg "can't do this," including for AVSEEK_SIZE (there's
    // no Content-Length to size against either).
    if (!reader_.seekable()) return -1;

    if (whence == AVSEEK_SIZE) return reader_.contentLength();

    int64_t target;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = reader_.position() + offset;
    } else if (whence == SEEK_END) {
        target = reader_.contentLength() + offset;
    } else {
        return -1;
    }
    if (target < 0) target = 0;

    // Offsets are in the file's own terms: the first open() always starts
    // at byte 0, and every reopen re-anchors the view there, so an
    // absolute target maps straight onto a Range request.
    if (target == reader_.position()) return target;
    if (!reader_.reopenAt(target)) {
        last_error_ = "seek to byte " + std::to_string(target) + " failed: " + reader_.lastError();
        return -1;
    }
    return target;
}

int HttpStreamIO::readPacketTrampoline(void* opaque, uint8_t* buf, int bufSize) {
    return static_cast<HttpStreamIO*>(opaque)->readPacket(buf, bufSize);
}

int64_t HttpStreamIO::seekTrampoline(void* opaque, int64_t offset, int whence) {
    return static_cast<HttpStreamIO*>(opaque)->seek(offset, whence);
}
