#include "Buffer.h"

#include <sys/uio.h>

#include <cassert>
#include <cerrno>

#include "SocketsOps.h"
#include "muduo/base/Types.h"

namespace muduo{
     namespace net{
         const char Buffer::kCRLF[] = "\r\n";

         ssize_t Buffer::readFd(int fd,int *savedErrno){
             char extrabuf[65536];

             struct iovec vec[2];
             const size_t writable = writableBytes();

             vec[0].iov_base = begin() + writerIndex_;
             vec[0].iov_len = writable;

             vec[1].iov_base = extrabuf;
             vec[1].iov_len = sizeof(extrabuf);
             const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
             const ssize_t n = sockets::readv(fd, vec, iovcnt);
             if (n < 0)
             {
                 *savedErrno = errno;
             }else if(implicit_cast<size_t>(n)<=writable){
                 writerIndex_ += static_cast<size_t>(n);
             }else{
                 writerIndex_ = buffer_.size();
                 append(extrabuf, static_cast<size_t>(n) - writable);
             }
             return n;
         }
         // 只写、不消费：写出去多少由调用方自己 retrieve。
         // 和上游 muduo 的语义保持一致。
         // （如果这里顺手 retrieve，TcpConnection::handleWrite 里再
         //   retrieve 一次就会重复消费 → 丢数据）
         ssize_t Buffer::writeFd(int fd, int *savedErrno)
         {
             ssize_t n = sockets::write(fd, peek(), readableBytes());
             if (n < 0)
             {
                 *savedErrno = errno;
             }
             return n;
         }
         void Buffer::makeSpace(size_t len){
            if(writableBytes()+prependableBytes()<len+kCheapPrepend){
                buffer_.resize(writerIndex_ + len);
            }else{
                assert(kCheapPrepend < readerIndex_);
                size_t readable = readableBytes();
                std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
                readerIndex_ = kCheapPrepend;
                writerIndex_ = readerIndex_ + readable;
                assert(readable == readableBytes());
            }
         }
     }
}