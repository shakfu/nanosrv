#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: iobuf ----

static size_t roundup(size_t size, size_t align)
{
    return align == 0 ? size : (size + align - 1) / align * align;
}

bool iobuf_resize(struct IOBuffer* io, size_t new_size)
{
    bool ok = true;
    new_size = roundup(new_size, io->align);
    if (new_size == 0) {
        bzero_(io->buf, io->size);
        mem_free(io->buf);
        io->buf = NULL;
        io->len = io->size = 0;
    } else if (new_size != io->size) {
        // NOTE(lsm): do not use realloc here. Use mem_calloc/mem_free only
        void* p = mem_calloc(1, new_size);
        if (p != NULL) {
            size_t len = new_size < io->len ? new_size : io->len;
            if (len > 0 && io->buf != NULL)
                memmove(p, io->buf, len);
            bzero_(io->buf, io->size);
            mem_free(io->buf);
            io->buf = static_cast<unsigned char*>(p);
            io->size = new_size;
            io->len = len;
        } else {
            ok = false;
            MG_ERROR(("%lld->%lld", static_cast<uint64_t>(io->size), static_cast<uint64_t>(new_size)));
        }
    }
    return ok;
}

bool iobuf_init(struct IOBuffer* io, size_t size, size_t align)
{
    io->buf = NULL;
    io->align = align;
    io->size = io->len = 0;
    return iobuf_resize(io, size);
}

size_t iobuf_add(struct IOBuffer* io, size_t ofs, const void* buf,
                    size_t len)
{
    size_t new_size = roundup(io->len + len, io->align);
    (void)iobuf_resize(io, new_size); // Attempt to resize
    if (new_size != io->size)
        len = 0; // Resize failure, append nothing
    if (ofs < io->len)
        memmove(io->buf + ofs + len, io->buf + ofs, io->len - ofs);
    if (buf != NULL)
        memmove(io->buf + ofs, buf, len);
    if (ofs > io->len)
        io->len += ofs - io->len;
    io->len += len;
    return len;
}

size_t iobuf_del(struct IOBuffer* io, size_t ofs, size_t len)
{
    if (ofs > io->len)
        ofs = io->len;
    if (ofs + len > io->len)
        len = io->len - ofs;
    if (io->buf)
        memmove(io->buf + ofs, io->buf + ofs + len, io->len - ofs - len);
    if (io->buf)
        bzero_(io->buf + io->len - len, len);
    io->len -= len;
    return len;
}

void iobuf_free(struct IOBuffer* io) { (void)iobuf_resize(io, 0); }

IOBuffer::~IOBuffer()
{
    if (buf != nullptr) {
        iobuf_free(this);
    }
}

IOBuffer::IOBuffer(IOBuffer&& other) noexcept
    : buf(other.buf), size(other.size), len(other.len), align(other.align)
{
    other.buf = nullptr;
    other.size = other.len = 0;
}

IOBuffer& IOBuffer::operator=(IOBuffer&& other) noexcept
{
    if (this != &other) {
        if (buf != nullptr)
            iobuf_free(this);
        buf = other.buf;
        size = other.size;
        len = other.len;
        align = other.align;
        other.buf = nullptr;
        other.size = other.len = 0;
    }
    return *this;
}

} // namespace nanosrv
