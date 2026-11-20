#include "file.hpp"
#include "io-device.hpp"
#include "orbis/utils/Logs.hpp"
#include "sys/sysproto.hpp"
#include "thread/Process.hpp"
#include "thread/Thread.hpp"
#include "uio.hpp"
#include <array>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <vector>

orbis::SysResult orbis::sys_read(Thread *thread, sint fd, ptr<void> buf,
                                 size_t nbyte) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto read = file->ops->read;
  if (read == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);
  IoVec vec{.base = (void *)buf, .len = nbyte};

  Uio io{
      .offset = file->nextOff,
      .iov = &vec,
      .iovcnt = 1,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Read,
      .td = thread,
  };

  auto error = read(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    if (error == ErrorCode::BUSY) {
      return SysResult::notAnError(error);
    }

    return error;
  }

  auto cnt = io.offset - file->nextOff;
  file->nextOff = io.offset;

  // ORBIS_LOG_ERROR(__FUNCTION__, fd, buf, nbyte, cnt);
  thread->retval[0] = cnt;
  return {};
}
orbis::SysResult orbis::sys_pread(Thread *thread, sint fd, ptr<void> buf,
                                  size_t nbyte, off_t offset) {
  // ORBIS_LOG_ERROR(__FUNCTION__, fd, buf, nbyte, offset);
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto read = file->ops->read;
  if (read == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);
  IoVec vec{.base = (void *)buf, .len = nbyte};

  Uio io{
      .offset = static_cast<std::uint64_t>(offset),
      .iov = &vec,
      .iovcnt = 1,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Read,
      .td = thread,
  };

  auto error = read(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  thread->retval[0] = io.offset - offset;
  return {};
}
orbis::SysResult orbis::sys_freebsd6_pread(Thread *thread, sint fd,
                                           ptr<void> buf, size_t nbyte, sint,
                                           off_t offset) {
  return sys_pread(thread, fd, buf, nbyte, offset);
}
orbis::SysResult orbis::sys_readv(Thread *thread, sint fd, ptr<IoVec> iovp,
                                  uint iovcnt) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto read = file->ops->read;
  if (read == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);

  Uio io{
      .offset = file->nextOff,
      .iov = iovp,
      .iovcnt = iovcnt,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Read,
      .td = thread,
  };

  auto error = read(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  auto cnt = io.offset - file->nextOff;
  file->nextOff = io.offset;
  thread->retval[0] = cnt;
  return {};
}
orbis::SysResult orbis::sys_preadv(Thread *thread, sint fd, ptr<IoVec> iovp,
                                   uint iovcnt, off_t offset) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto read = file->ops->read;
  if (read == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);

  Uio io{
      .offset = static_cast<std::uint64_t>(offset),
      .iov = iovp,
      .iovcnt = iovcnt,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Read,
      .td = thread,
  };

  auto error = read(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  thread->retval[0] = io.offset - offset;
  return {};
}
orbis::SysResult orbis::sys_write(Thread *thread, sint fd, ptr<const void> buf,
                                  size_t nbyte) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto write = file->ops->write;
  if (write == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);
  IoVec vec{.base = (void *)buf, .len = nbyte};

  Uio io{
      .offset = file->nextOff,
      .iov = &vec,
      .iovcnt = 1,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Write,
      .td = thread,
  };

  auto error = write(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  auto cnt = io.offset - file->nextOff;
  file->nextOff = io.offset;

  thread->retval[0] = cnt;
  return {};
}
orbis::SysResult orbis::sys_pwrite(Thread *thread, sint fd, ptr<const void> buf,
                                   size_t nbyte, off_t offset) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto write = file->ops->write;
  if (write == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);
  IoVec vec{.base = (void *)buf, .len = nbyte};

  Uio io{
      .offset = static_cast<std::uint64_t>(offset),
      .iov = &vec,
      .iovcnt = 1,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Write,
      .td = thread,
  };

  auto error = write(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  thread->retval[0] = io.offset - offset;
  return {};
}
orbis::SysResult orbis::sys_freebsd6_pwrite(Thread *thread, sint fd,
                                            ptr<const void> buf, size_t nbyte,
                                            sint, off_t offset) {
  return sys_pwrite(thread, fd, buf, nbyte, offset);
}
orbis::SysResult orbis::sys_writev(Thread *thread, sint fd, ptr<IoVec> iovp,
                                   uint iovcnt) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto write = file->ops->write;
  if (write == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);

  Uio io{
      .offset = file->nextOff,
      .iov = iovp,
      .iovcnt = iovcnt,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Write,
      .td = thread,
  };

  auto error = write(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  auto cnt = io.offset - file->nextOff;
  file->nextOff = io.offset;

  thread->retval[0] = cnt;
  return {};
}
orbis::SysResult orbis::sys_pwritev(Thread *thread, sint fd, ptr<IoVec> iovp,
                                    uint iovcnt, off_t offset) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto write = file->ops->write;
  if (write == nullptr) {
    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);

  Uio io{
      .offset = static_cast<std::uint64_t>(offset),
      .iov = iovp,
      .iovcnt = iovcnt,
      .segflg = UioSeg::UserSpace,
      .rw = UioRw::Write,
      .td = thread,
  };
  auto error = write(file.get(), &io, thread);
  if (error != ErrorCode{} && error != ErrorCode::AGAIN) {
    return error;
  }

  thread->retval[0] = io.offset - offset;
  return {};
}
orbis::SysResult orbis::sys_ftruncate(Thread *thread, sint fd, off_t length) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto truncate = file->ops->truncate;
  if (truncate == nullptr) {
    return ErrorCode::NOTSUP;
  }

  ORBIS_LOG_WARNING(__FUNCTION__, fd, length);
  std::lock_guard lock(file->mtx);
  return truncate(file.get(), length, thread);
}
orbis::SysResult orbis::sys_freebsd6_ftruncate(Thread *thread, sint fd, sint,
                                               off_t length) {
  return sys_ftruncate(thread, fd, length);
}
orbis::SysResult orbis::sys_ioctl(Thread *thread, sint fd, ulong com,
                                  caddr_t data) {
  rx::Ref<File> file = thread->tproc->fileDescriptors.get(fd);
  if (file == nullptr) {
    return ErrorCode::BADF;
  }

  auto ioctl = file->ops->ioctl;
  if (ioctl == nullptr) {
    if (auto device = file->device.get()) {
      return device->ioctl(com, data, thread);
    }

    return ErrorCode::NOTSUP;
  }

  std::lock_guard lock(file->mtx);
  return ioctl(file.get(), com, data, thread);
}
orbis::SysResult orbis::sys_pselect(Thread *thread, sint nd, ptr<fd_set> in,
                                    ptr<fd_set> ou, ptr<fd_set> ex,
                                    ptr<const timespec> ts,
                                    ptr<const sigset_t> sm) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_select(Thread *thread, sint nd,
                                   ptr<struct fd_set_t> in,
                                   ptr<struct fd_set_t> out,
                                   ptr<struct fd_set_t> ex,
                                   ptr<struct timeval> tv) {
  // fd_set_t is forward-declared (incomplete type). On PS4/FreeBSD x86-64,
  // fd_set is typically a bitmap covering FD_SETSIZE (1024) fds = 128 bytes.
  // We read/write it as an opaque byte blob.
  constexpr std::size_t kFdSetSize = 128;

  std::vector<::pollfd> pollFds;
  std::vector<int> pollFdToIndex; // maps poll slot -> original fd

  auto addPollFd = [&](int fd, short events) {
    for (auto &pf : pollFds) {
      if (pf.fd == fd) {
        pf.events |= events;
        return;
      }
    }
    pollFds.push_back(::pollfd{fd, events, 0});
    pollFdToIndex.push_back(fd);
  };

  // Read the fd_set bitmaps from guest memory and register fds for polling.
  auto addSetFds = [&](ptr<struct fd_set_t> set, short events) {
    if (set == nullptr) {
      return;
    }
    std::array<std::byte, kFdSetSize> bits{};
    if (auto e = ureadRaw(bits.data(), set, kFdSetSize); e != ErrorCode{}) {
      return; // best effort
    }
    for (sint fd = 0; fd < nd; ++fd) {
      std::size_t byteIdx = fd / 8;
      unsigned bit = fd % 8;
      if (byteIdx < bits.size() &&
          (static_cast<int>(bits[byteIdx]) & (1 << bit)) != 0) {
        addPollFd(fd, events);
      }
    }
  };

  addSetFds(in, POLLIN);
  addSetFds(out, POLLOUT);
  addSetFds(ex, POLLERR);

  // Compute timeout in milliseconds.
  int timeoutMs = -1; // block indefinitely
  if (tv != nullptr) {
    timeoutMs = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    if (timeoutMs < 0) {
      timeoutMs = 0;
    }
  }

  int readyCount = 0;
  if (!pollFds.empty()) {
    readyCount = ::poll(pollFds.data(), pollFds.size(), timeoutMs);
    if (readyCount < 0) {
      return convertErrno();
    }
  }

  // Build result fd_sets from poll results.
  auto writeResultSet = [&](ptr<struct fd_set_t> set, short reventsMask) {
    if (set == nullptr) {
      return;
    }
    std::array<std::byte, kFdSetSize> bits{};
    // Start by clearing everything (we only write back ready fds)
    std::memset(bits.data(), 0, bits.size());

    for (std::size_t i = 0; i < pollFds.size(); ++i) {
      if (pollFds[i].revents & reventsMask) {
        int fd = pollFds[i].fd;
        if (fd >= 0 && fd < nd) {
          std::size_t byteIdx = fd / 8;
          unsigned bit = fd % 8;
          if (byteIdx < bits.size()) {
            bits[byteIdx] = static_cast<std::byte>(
                static_cast<int>(bits[byteIdx]) | (1 << bit));
          }
        }
      }
    }

    uwriteRaw(set, bits.data(), kFdSetSize);
  };

  // For select, we only write back fds that are ready for the requested event.
  // POLLIN -> readable, POLLOUT -> writable, POLLERR -> exception.
  writeResultSet(in, POLLIN);
  writeResultSet(out, POLLOUT);
  writeResultSet(ex, POLLERR);

  thread->retval[0] = readyCount;
  return {};
}
orbis::SysResult orbis::sys_poll(Thread *thread, ptr<struct pollfd> fds,
                                 uint nfds, sint timeout) {
  // orbis::pollfd is forward-declared; use host ::pollfd (same layout on x86-64)
  std::vector<::pollfd> hostFds;
  if (nfds > 0 && fds != nullptr) {
    hostFds.resize(nfds);
    if (auto e = ureadRaw(hostFds.data(), fds,
                          nfds * sizeof(::pollfd));
        e != ErrorCode{}) {
      return e;
    }
  }

  int readyCount = ::poll(hostFds.data(), nfds, timeout);
  if (readyCount < 0) {
    return convertErrno();
  }

  // Write back revents to guest memory.
  if (nfds > 0 && fds != nullptr) {
    if (auto e = uwriteRaw(fds, hostFds.data(),
                           nfds * sizeof(::pollfd));
        e != ErrorCode{}) {
      return e;
    }
  }

  thread->retval[0] = readyCount;
  return {};
}
orbis::SysResult orbis::sys_openbsd_poll(Thread *thread, ptr<struct pollfd> fds,
                                         uint nfds, sint timeout) {
  return ErrorCode::NOSYS;
}

orbis::SysResult orbis::sys_nlm_syscall(Thread *thread, sint debug_level,
                                        sint grace_period, sint addr_count,
                                        ptr<ptr<char>> addrs) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_nfssvc(Thread *thread, sint flag, caddr_t argp) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_sysarch(Thread *thread, sint op, ptr<char> parms) {
  if (op == 129) {
    uint64_t fs;
    if (auto error = uread(fs, (ptr<uint64_t>)parms); error != ErrorCode{})
      return error;
    ORBIS_LOG_WARNING("sys_sysarch: set FS", (std::size_t)fs);
    thread->fsBase = fs;
    return {};
  }

  ORBIS_LOG_WARNING(__FUNCTION__, op, parms);
  return {};
}
orbis::SysResult orbis::sys_nnpfs_syscall(Thread *thread, sint operation,
                                          ptr<char> a_pathP, sint opcode,
                                          ptr<void> a_paramsP,
                                          sint a_followSymlinks) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_afs3_syscall(Thread *thread, slong syscall,
                                         slong param1, slong param2,
                                         slong param3, slong param4,
                                         slong param5, slong param6) {
  return ErrorCode::NOSYS;
}
orbis::SysResult orbis::sys_gssd_syscall(Thread *thread, ptr<char> path) {
  return ErrorCode::NOSYS;
}
