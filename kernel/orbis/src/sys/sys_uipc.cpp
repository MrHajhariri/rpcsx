#include "file.hpp"
#include "io-device.hpp"
#include "orbis/SocketAddress.hpp"
#include "sys/sysproto.hpp"
#include "thread/Process.hpp"
#include "thread/ProcessOps.hpp"
#include "thread/Thread.hpp"
#include "uio.hpp"
#include "utils/Logs.hpp"
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

// Convert orbis SocketAddress to host sockaddr.
static bool socketAddressToSockaddr(const orbis::SocketAddress *addr,
                                    sockaddr_storage &storage,
                                    socklen_t &storageLen) {
  storageLen = 0;
  if (addr->family == 1) { // AF_UNIX
    sockaddr_un un{};
    un.sun_family = AF_UNIX;
    auto pathLen = std::min<std::size_t>(
        addr->len - 2, sizeof(un.sun_path) - 1);
    std::memcpy(un.sun_path, addr->data, pathLen);
    un.sun_path[pathLen] = '\0';
    storageLen = sizeof(un);
    std::memcpy(&storage, &un, storageLen);
    return true;
  }

  if (addr->family == 2) { // AF_INET
    sockaddr_in in{};
    in.sin_family = AF_INET;
    std::uint16_t port = 0;
    std::memcpy(&port, addr->data, 2);
    in.sin_port = port;
    std::memcpy(&in.sin_addr, addr->data + 2, 4);
    storageLen = sizeof(in);
    std::memcpy(&storage, &in, storageLen);
    return true;
  }

  if (addr->family == 10) { // AF_INET6
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    std::uint16_t port = 0;
    std::memcpy(&port, addr->data, 2);
    in6.sin6_port = port;
    std::memcpy(&in6.sin6_addr, addr->data + 2,
                std::min<std::size_t>(14, sizeof(in6.sin6_addr)));
    storageLen = sizeof(in6);
    std::memcpy(&storage, &in6, storageLen);
    return true;
  }

  return false;
}

// Convert host sockaddr to orbis SocketAddress.
static bool sockaddrToSocketAddress(const sockaddr *addr, socklen_t addrLen,
                                    orbis::SocketAddress *out) {
  out->len = 0;
  out->family = 0;
  std::memset(out->data, 0, sizeof(out->data));

  if (addr->sa_family == AF_UNIX) {
    const auto *un = reinterpret_cast<const sockaddr_un *>(addr);
    out->family = 1;
    auto pathLen = std::strlen(un->sun_path);
    out->len = 2 + static_cast<unsigned char>(pathLen);
    std::memcpy(out->data, un->sun_path,
                std::min<std::size_t>(pathLen, sizeof(out->data)));
    return true;
  }

  if (addr->sa_family == AF_INET) {
    const auto *in = reinterpret_cast<const sockaddr_in *>(addr);
    out->family = 2;
    out->len = 6;
    std::memcpy(out->data, &in->sin_port, 2);
    std::memcpy(out->data + 2, &in->sin_addr, 4);
    return true;
  }

  if (addr->sa_family == AF_INET6) {
    const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    out->family = 10;
    out->len = 18;
    std::memcpy(out->data, &in6->sin6_port, 2);
    std::memcpy(out->data + 2, &in6->sin6_addr,
                std::min<std::size_t>(14, sizeof(in6->sin6_addr)));
    return true;
  }

  return false;
}

orbis::SysResult orbis::sys_socket(Thread *thread, sint domain, sint type,
                                   sint protocol) {
  ORBIS_LOG_TODO(__FUNCTION__, domain, type, protocol);
  if (auto socket = thread->tproc->ops->socket) {
    rx::Ref<File> file;
    auto result = socket(thread, nullptr, domain, type, protocol, &file);

    if (result.isError()) {
      return result;
    }

    auto fd = thread->tproc->fileDescriptors.insert(file);
    ORBIS_LOG_WARNING("Socket opened", fd);
    thread->retval[0] = fd;
    return {};
  }
  return ErrorCode::NOSYS;
}

orbis::SysResult orbis::sys_bind(Thread *thread, sint s, caddr_t name,
                                 sint namelen) {
  // ORBIS_LOG_ERROR(__FUNCTION__, s, name, namelen);

  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto bind = file->ops->bind) {
    return bind(file.get(), ptr<SocketAddress>(name), namelen, thread);
  }

  return ErrorCode::NOTSUP;
}

orbis::SysResult orbis::sys_listen(Thread *thread, sint s, sint backlog) {
  ORBIS_LOG_ERROR(__FUNCTION__, s, backlog);
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto listen = file->ops->listen) {
    return listen(file.get(), backlog, thread);
  }

  return ErrorCode::NOTSUP;
}

orbis::SysResult orbis::sys_accept(Thread *thread, sint s,
                                   ptr<SocketAddress> from,
                                   ptr<uint32_t> fromlenaddr) {
  ORBIS_LOG_ERROR(__FUNCTION__, s, from, fromlenaddr);
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto accept = file->ops->accept) {
    return accept(file.get(), from, fromlenaddr, thread);
  }

  return ErrorCode::NOTSUP;
}

orbis::SysResult orbis::sys_connect(Thread *thread, sint s, caddr_t name,
                                    sint namelen) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto connect = file->ops->connect) {
    return connect(file.get(), ptr<SocketAddress>(name), namelen, thread);
  }

  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_socketpair(Thread *thread, sint domain, sint type,
                                       sint protocol, ptr<sint> rsv) {
  ORBIS_LOG_TODO(__FUNCTION__, domain, type, protocol, rsv);
  if (auto socketpair = thread->tproc->ops->socketpair) {
    rx::Ref<File> a;
    rx::Ref<File> b;
    auto result = socketpair(thread, domain, type, protocol, &a, &b);

    if (result.isError()) {
      return result;
    }

    auto aFd = thread->tproc->fileDescriptors.insert(a);
    auto bFd = thread->tproc->fileDescriptors.insert(b);

    if (auto errc = uwrite(rsv, aFd); errc != ErrorCode{}) {
      return errc;
    }

    return uwrite(rsv + 1, bFd);
  }

  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_sendto(Thread *thread, sint s, caddr_t buf,
                                   size_t len, sint flags, caddr_t to,
                                   sint tolen) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }
  if (auto sendto = file->ops->sendto) {
    return SysResult::notAnError(
        sendto(file.get(), buf, len, flags, to, tolen, thread));
  }
  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_sendmsg(Thread *thread, sint s,
                                    ptr<struct msghdr> msg, sint flags) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  // orbis::msghdr is forward-declared; cast to host ::msghdr (same layout on x86-64)
  auto *hostMsgPtr = reinterpret_cast<::msghdr *>(msg);

  // Read the guest msghdr into a host-side copy.
  struct ::msghdr hostMsg{};
  if (auto e = uread(hostMsg, hostMsgPtr); e != ErrorCode{}) {
    return e;
  }

  // Read the iovec array from guest memory.
  std::vector<struct ::iovec> hostIov;
  if (hostMsg.msg_iovlen > 0) {
    hostIov.resize(hostMsg.msg_iovlen);
    if (auto e = ureadRaw(hostIov.data(), hostMsg.msg_iov,
                          hostMsg.msg_iovlen * sizeof(struct ::iovec));
        e != ErrorCode{}) {
      return e;
    }
    hostMsg.msg_iov = hostIov.data();
  }

  auto count = ::sendmsg(file->hostFd, &hostMsg, flags);
  if (count < 0) {
    return convertErrno();
  }
  thread->retval[0] = count;
  return {};
}
orbis::SysResult orbis::sys_recvfrom(Thread *thread, sint s, caddr_t buf,
                                     size_t len, sint flags,
                                     ptr<SocketAddress> from,
                                     ptr<uint32_t> fromlenaddr) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto recvfrom = file->ops->recvfrom) {
    return SysResult::notAnError(
        recvfrom(file.get(), buf, len, flags, from, fromlenaddr, thread));
  }

  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_recvmsg(Thread *thread, sint s,
                                    ptr<struct msghdr> msg, sint flags) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  // orbis::msghdr is forward-declared; cast to host ::msghdr (same layout on x86-64)
  auto *hostMsgPtr = reinterpret_cast<::msghdr *>(msg);

  // Read the guest msghdr into a host-side copy.
  struct ::msghdr hostMsg{};
  if (auto e = uread(hostMsg, hostMsgPtr); e != ErrorCode{}) {
    return e;
  }

  // Read the iovec array from guest memory.
  std::vector<struct ::iovec> hostIov;
  if (hostMsg.msg_iovlen > 0) {
    hostIov.resize(hostMsg.msg_iovlen);
    if (auto e = ureadRaw(hostIov.data(), hostMsg.msg_iov,
                          hostMsg.msg_iovlen * sizeof(struct ::iovec));
        e != ErrorCode{}) {
      return e;
    }
    hostMsg.msg_iov = hostIov.data();
  }

  auto count = ::recvmsg(file->hostFd, &hostMsg, flags);
  if (count < 0) {
    return convertErrno();
  }
  thread->retval[0] = count;

  // Write back msg_flags if the kernel modified them.
  if (hostMsg.msg_flags != 0) {
    if (auto e = uwrite(&hostMsgPtr->msg_flags, hostMsg.msg_flags);
        e != ErrorCode{}) {
      return e;
    }
  }

  return {};
}
orbis::SysResult orbis::sys_shutdown(Thread *thread, sint s, sint how) {
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto shutdown = file->ops->shutdown) {
    return shutdown(file.get(), how, thread);
  }

  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_setsockopt(Thread *thread, sint s, sint level,
                                       sint name, caddr_t val, sint valsize) {
  ORBIS_LOG_TODO(__FUNCTION__, s, level, name, val, valsize);
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto setsockopt = file->ops->setsockopt) {
    return setsockopt(file.get(), level, name, val, valsize, thread);
  }

  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_getsockopt(Thread *thread, sint s, sint level,
                                       sint name, caddr_t val,
                                       ptr<sint> avalsize) {
  ORBIS_LOG_TODO(__FUNCTION__, s, level, name, val, avalsize);
  auto file = thread->tproc->fileDescriptors.get(s);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  if (auto getsockopt = file->ops->getsockopt) {
    return getsockopt(file.get(), level, name, val, avalsize, thread);
  }

  return ErrorCode::NOTSUP;
}
orbis::SysResult orbis::sys_getsockname(Thread *thread, sint fdes,
                                        ptr<SocketAddress> asa,
                                        ptr<uint32_t> alen) {
  auto file = thread->tproc->fileDescriptors.get(fdes);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  sockaddr_storage storage{};
  socklen_t len = sizeof(storage);
  if (::getsockname(file->hostFd, reinterpret_cast<sockaddr *>(&storage),
                    &len) < 0) {
    return convertErrno();
  }

  if (asa != nullptr && alen != nullptr) {
    uint32_t guestLen = 0;
    if (auto e = uread(guestLen, alen); e != ErrorCode{}) {
      return e;
    }
    sockaddrToSocketAddress(reinterpret_cast<sockaddr *>(&storage), len, asa);
    if (auto e = uwrite(alen, asa->len); e != ErrorCode{}) {
      return e;
    }
    if (guestLen < asa->len) {
      asa->len = guestLen;
    }
  }

  return {};
}

orbis::SysResult orbis::sys_getpeername(Thread *thread, sint fdes,
                                        ptr<SocketAddress> asa,
                                        ptr<uint32_t> alen) {
  auto file = thread->tproc->fileDescriptors.get(fdes);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  sockaddr_storage storage{};
  socklen_t len = sizeof(storage);
  if (::getpeername(file->hostFd, reinterpret_cast<sockaddr *>(&storage),
                    &len) < 0) {
    return convertErrno();
  }

  if (asa != nullptr && alen != nullptr) {
    uint32_t guestLen = 0;
    if (auto e = uread(guestLen, alen); e != ErrorCode{}) {
      return e;
    }
    sockaddrToSocketAddress(reinterpret_cast<sockaddr *>(&storage), len, asa);
    if (auto e = uwrite(alen, asa->len); e != ErrorCode{}) {
      return e;
    }
    if (guestLen < asa->len) {
      asa->len = guestLen;
    }
  }

  return {};
}
orbis::SysResult orbis::sys_sendfile(Thread *thread, sint fd, sint s,
                                     off_t offset, size_t nbytes,
                                     ptr<struct sf_hdtr> hdtr,
                                     ptr<off_t> sbytes, sint flags) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_sctp_peeloff(Thread *thread, sint sd,
                                         uint32_t name) {
  return ErrorCode::NOSYS;
}
orbis::SysResult
orbis::sys_sctp_generic_sendmsg(Thread *thread, sint sd, caddr_t msg, sint mlen,
                                caddr_t to, __socklen_t tolen,
                                ptr<struct sctp_sndrcvinfo> sinfo, sint flags) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_sctp_generic_sendmsg_iov(
    Thread *thread, sint sd, ptr<IoVec> iov, sint iovlen, caddr_t to,
    __socklen_t tolen, ptr<struct sctp_sndrcvinfo> sinfo, sint flags) {
  return ErrorCode::NOSYS;
}
orbis::SysResult
orbis::sys_sctp_generic_recvmsg(Thread *thread, sint sd, ptr<IoVec> iov,
                                sint iovlen, caddr_t from, __socklen_t fromlen,
                                ptr<struct sctp_sndrcvinfo> sinfo, sint flags) {
  return ErrorCode::NOSYS;
}
