#pragma once

#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "muduo/base/Types.h"

namespace muduo{
    namespace net{
        class Buffer{
        public:
            static constexpr size_t kCheapPrepend = 8;
            static constexpr size_t kInitialSize = 1024;

            explicit Buffer(size_t initialSize = kInitialSize)
                : buffer_(kCheapPrepend + initialSize),
                  readerIndex_(kCheapPrepend),
                  writerIndex_(kCheapPrepend) {}
            void swap(Buffer &rhs)
            {
                buffer_.swap(rhs.buffer_);
                std::swap(readerIndex_, rhs.readerIndex_);
                std::swap(writerIndex_, rhs.writerIndex_);
            }

            size_t readableBytes() const { return writerIndex_ - readerIndex_; }
            size_t writableBytes() const { return buffer_.size() - writerIndex_; }
            size_t prependableBytes() const { return readerIndex_; }

            const char *peek() const { return begin() + readerIndex_; }

            const char *findCRLF() const
            {
                const char *crlf =
                    std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
                return crlf == beginWrite() ? nullptr : crlf;
            }
            const char *findCRLF(const char *start) const
            {
                const char *crlf = std::search(start, beginWrite(), kCRLF, kCRLF + 2);
                return crlf == beginWrite() ? nullptr : crlf;
            }
            const char *findEOL() const
            {
                const void *eol = std::memchr(peek(), '\n', readableBytes());
                return static_cast<const char *>(eol);
            }

            void retrieve(size_t len)
            {
                if (len < readableBytes())
                {
                    readerIndex_ += len;
                }
                else
                {
                    retrieveAll();
                }
            }
            void retrieveUntil(const char *end)
            {
                retrieve(static_cast<size_t>(end - peek()));
            }

            void retrieveAll()
            {
                readerIndex_ = kCheapPrepend;
                writerIndex_ = kCheapPrepend;
            }

            std::string retrieveAllAsString()
            {
                return retrieveAsString(readableBytes());
            }

            std::string retrieveAsString(size_t len)
            {
                std::string result(peek(), len);
                retrieve(len);
                return result;
            }

            void append(const std::string &str)
            {
                append(str.data(), str.size());
            }

            void append(const char *data, size_t len)
            {
                ensureWritableBytes(len);
                std::copy(data, data + len, beginWrite());
                hasWritten(len);
            }

            void append(const void *data, size_t len)
            {
                append(static_cast<const char *>(data), len);
            }

            void ensureWritableBytes(size_t len)
            {
                if (writableBytes() < len)
                {
                    makeSpace(len);
                }
            }

            char *beginWrite() { return begin() + writerIndex_; }
            const char *beginWrite() const { return begin() + writerIndex_; }

            void hasWritten(size_t len) { writerIndex_ += len; }

            void unwrite(size_t len) { writerIndex_ -= len; }
            void prepend(const void *data, size_t len)
            {
                readerIndex_ -= len;
                const char *d = static_cast<const char *>(data);
                std::copy(d, d + len, begin() + readerIndex_);
            }

            void shrink(size_t reserve)
            {
                Buffer other;
                other.ensureWritableBytes(readableBytes() + reserve);
                other.append(peek(), readableBytes());
                swap(other);
            }
            ssize_t readFd(int fd, int *savedErrno);
            ssize_t writeFd(int fd, int *savedErrno);

        private:
            
            char *begin() { return buffer_.data(); }
            const char *begin() const { return buffer_.data(); }

            void makeSpace(size_t len);

            std::vector<char> buffer_;
            size_t readerIndex_;
            size_t writerIndex_;

            static const char kCRLF[];
        };
    }
}