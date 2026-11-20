#include "io-device.hpp"
#include "orbis/KernelAllocator.hpp"
#include "orbis/SocketAddress.hpp"
#include "orbis/file.hpp"
#include "orbis/stat.hpp"
#include "orbis/thread/Process.hpp"
#include "orbis/thread/Thread.hpp"
#include "orbis/uio.hpp"
#include "orbis/utils/Logs.hpp"
#include "vfs.hpp"
#include "vm.hpp"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <optional>
#include <rx/align.hpp>
#include <rx/mem.hpp>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct HostFile : orbis::File {
  bool closeOnExit = true;
  bool alignTruncate = false;

  ~HostFile() {
    if (hostFd > 0 && closeOnExit) {
      ::close(hostFd);
    }
  }
};

struct SocketFile : orbis::File {
  orbis::kstring name;
  int dom = -1;
  int type = -1;
  int prot = -1;

  orbis::kmap<int, orbis::kvector<std::byte>> options;

  ~SocketFile() {
    if (hostFd > 0) {
      ::close(hostFd);
    }
  }
};

// Convert orbis SocketAddress to host sockaddr.
// Returns the sockaddr pointer and its size.
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
    // SocketAddress data: [2 bytes port][4 bytes addr] = 6 bytes used
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
    // SocketAddress data: [2 bytes port][4 bytes flowinfo][16 bytes addr]
    // but we only have 14 bytes of data, so we do a best-effort copy
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
    out->len = 6; // 2 bytes port + 4 bytes addr
    std::memcpy(out->data, &in->sin_port, 2);
    std::memcpy(out->data + 2, &in->sin_addr, 4);
    return true;
  }

  if (addr->sa_family == AF_INET6) {
    const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    out->family = 10;
    out->len = 18; // 2 bytes port + 4 bytes flowinfo + 16 bytes addr (truncated)
    std::memcpy(out->data, &in6->sin6_port, 2);
    std::memcpy(out->data + 2, &in6->sin6_addr,
                std::min<std::size_t>(14, sizeof(in6->sin6_addr)));
    return true;
  }

  return false;
}

static orbis::ErrorCode convertErrc(std::errc errc) {
  if (errc == std::errc{}) {
    return {};
  }

  switch (errc) {
  case std::errc::address_family_not_supported:
    return orbis::ErrorCode::AFNOSUPPORT;
  case std::errc::address_in_use:
    return orbis::ErrorCode::ADDRINUSE;
  case std::errc::address_not_available:
    return orbis::ErrorCode::ADDRNOTAVAIL;
  case std::errc::already_connected:
    return orbis::ErrorCode::ISCONN;
  case std::errc::argument_out_of_domain:
    return orbis::ErrorCode::DOM;
  case std::errc::bad_address:
    return orbis::ErrorCode::FAULT;
  case std::errc::bad_file_descriptor:
    return orbis::ErrorCode::BADF;
  case std::errc::bad_message:
    return orbis::ErrorCode::BADMSG;
  case std::errc::broken_pipe:
    return orbis::ErrorCode::PIPE;
  case std::errc::connection_aborted:
    return orbis::ErrorCode::CONNABORTED;
  case std::errc::connection_already_in_progress:
    return orbis::ErrorCode::ALREADY;
  case std::errc::connection_refused:
    return orbis::ErrorCode::CONNREFUSED;
  case std::errc::connection_reset:
    return orbis::ErrorCode::CONNRESET;
  case std::errc::cross_device_link:
    return orbis::ErrorCode::XDEV;
  case std::errc::destination_address_required:
    return orbis::ErrorCode::DESTADDRREQ;
  case std::errc::device_or_resource_busy:
    return orbis::ErrorCode::BUSY;
  case std::errc::directory_not_empty:
    return orbis::ErrorCode::NOTEMPTY;
  case std::errc::executable_format_error:
    return orbis::ErrorCode::NOEXEC;
  case std::errc::file_exists:
    return orbis::ErrorCode::EXIST;
  case std::errc::file_too_large:
    return orbis::ErrorCode::FBIG;
  case std::errc::filename_too_long:
    return orbis::ErrorCode::NAMETOOLONG;
  case std::errc::function_not_supported:
    return orbis::ErrorCode::NOSYS;
  case std::errc::host_unreachable:
    return orbis::ErrorCode::HOSTUNREACH;
  case std::errc::identifier_removed:
    return orbis::ErrorCode::IDRM;
  case std::errc::illegal_byte_sequence:
    return orbis::ErrorCode::ILSEQ;
  case std::errc::inappropriate_io_control_operation:
    return orbis::ErrorCode::NOTTY;
  case std::errc::interrupted:
    return orbis::ErrorCode::INTR;
  case std::errc::invalid_argument:
    return orbis::ErrorCode::INVAL;
  case std::errc::invalid_seek:
    return orbis::ErrorCode::SPIPE;
  case std::errc::io_error:
    return orbis::ErrorCode::IO;
  case std::errc::is_a_directory:
    return orbis::ErrorCode::ISDIR;
  case std::errc::message_size:
    return orbis::ErrorCode::MSGSIZE;
  case std::errc::network_down:
    return orbis::ErrorCode::NETDOWN;
  case std::errc::network_reset:
    return orbis::ErrorCode::NETRESET;
  case std::errc::network_unreachable:
    return orbis::ErrorCode::NETUNREACH;
  case std::errc::no_buffer_space:
    return orbis::ErrorCode::NOBUFS;
  case std::errc::no_child_process:
    return orbis::ErrorCode::CHILD;
  case std::errc::no_link:
    return orbis::ErrorCode::NOLINK;
  case std::errc::no_lock_available:
    return orbis::ErrorCode::NOLCK;
  case std::errc::no_message:
    return orbis::ErrorCode::NOMSG;
  case std::errc::no_protocol_option:
    return orbis::ErrorCode::NOPROTOOPT;
  case std::errc::no_space_on_device:
    return orbis::ErrorCode::NOSPC;
  case std::errc::no_such_device_or_address:
    return orbis::ErrorCode::NXIO;
  case std::errc::no_such_device:
    return orbis::ErrorCode::NODEV;
  case std::errc::no_such_file_or_directory:
    return orbis::ErrorCode::NOENT;
  case std::errc::no_such_process:
    return orbis::ErrorCode::SRCH;
  case std::errc::not_a_directory:
    return orbis::ErrorCode::NOTDIR;
  case std::errc::not_a_socket:
    return orbis::ErrorCode::NOTSOCK;
  case std::errc::not_connected:
    return orbis::ErrorCode::NOTCONN;
  case std::errc::not_enough_memory:
    return orbis::ErrorCode::NOMEM;
  case std::errc::not_supported:
    return orbis::ErrorCode::NOTSUP;
  case std::errc::operation_canceled:
    return orbis::ErrorCode::CANCELED;
  case std::errc::operation_in_progress:
    return orbis::ErrorCode::INPROGRESS;
  case std::errc::operation_not_permitted:
    return orbis::ErrorCode::PERM;
  case std::errc::operation_would_block:
    return orbis::ErrorCode::WOULDBLOCK;
  case std::errc::permission_denied:
    return orbis::ErrorCode::ACCES;
  case std::errc::protocol_error:
    return orbis::ErrorCode::PROTO;
  case std::errc::protocol_not_supported:
    return orbis::ErrorCode::PROTONOSUPPORT;
  case std::errc::read_only_file_system:
    return orbis::ErrorCode::ROFS;
  case std::errc::resource_deadlock_would_occur:
    return orbis::ErrorCode::DEADLK;
  case std::errc::result_out_of_range:
    return orbis::ErrorCode::RANGE;
  case std::errc::text_file_busy:
    return orbis::ErrorCode::TXTBSY;
  case std::errc::timed_out:
    return orbis::ErrorCode::TIMEDOUT;
  case std::errc::too_many_files_open_in_system:
    return orbis::ErrorCode::NFILE;
  case std::errc::too_many_files_open:
    return orbis::ErrorCode::MFILE;
  case std::errc::too_many_links:
    return orbis::ErrorCode::MLINK;
  case std::errc::too_many_symbolic_link_levels:
    return orbis::ErrorCode::LOOP;
  case std::errc::value_too_large:
    return orbis::ErrorCode::OVERFLOW;
  case std::errc::wrong_protocol_type:
    return orbis::ErrorCode::PROTOTYPE;
  default:
    return orbis::ErrorCode::FAULT;
  }
}

orbis::ErrorCode convertErrorCode(const std::error_code &code) {
  if (!code) {
    return {};
  }
  return convertErrc(static_cast<std::errc>(code.value()));
}

orbis::ErrorCode convertErrno() {
  switch (auto error = errno) {
  case EPERM:
    return orbis::ErrorCode::PERM;
  case ENOENT:
    return orbis::ErrorCode::NOENT;
  case EBADF:
    return orbis::ErrorCode::BADF;
  case EIO:
    return orbis::ErrorCode::IO;
  case EACCES:
    return orbis::ErrorCode::ACCES;
  case EEXIST:
    return orbis::ErrorCode::EXIST;
  case EBUSY:
    return orbis::ErrorCode::BUSY;
  case ENOTDIR:
    return orbis::ErrorCode::NOTDIR;
  case EISDIR:
    return orbis::ErrorCode::ISDIR;
  case EFBIG:
    return orbis::ErrorCode::FBIG;
  case ENOSPC:
    return orbis::ErrorCode::NOSPC;
  case ESPIPE:
    return orbis::ErrorCode::SPIPE;
  case EPIPE:
    return orbis::ErrorCode::PIPE;
  case EINVAL:
    return orbis::ErrorCode::INVAL;

  default:
    ORBIS_LOG_ERROR("Unconverted errno", error);
  }

  return orbis::ErrorCode::IO;
}

static orbis::ErrorCode host_fd_read(int hostFd, orbis::Uio *uio) {
  std::vector<iovec> vec;
  for (auto entry : std::span(uio->iov, uio->iovcnt)) {
    vec.push_back({.iov_base = entry.base, .iov_len = entry.len});
  }

  auto cnt = ::preadv(hostFd, vec.data(), vec.size(), uio->offset);
  if (cnt < 0) {
    cnt = 0;
    for (auto io : vec) {
      auto result = ::read(hostFd, io.iov_base, io.iov_len);
      if (result < 0) {
        if (cnt == 0) {
          return convertErrno();
        }
        break;
      }

      cnt += result;

      if (cnt != io.iov_len) {
        break;
      }
    }
  }

  uio->resid -= cnt;
  uio->offset += cnt;
  return {};
}

static orbis::ErrorCode host_fd_write(int hostFd, orbis::Uio *uio) {
  std::vector<iovec> vec;
  for (auto entry : std::span(uio->iov, uio->iovcnt)) {
    vec.push_back({.iov_base = entry.base, .iov_len = entry.len});
  }

  ssize_t cnt = ::pwritev(hostFd, vec.data(), vec.size(), uio->offset);

  if (cnt < 0) {
    cnt = 0;

    for (auto io : vec) {
      auto result = ::write(hostFd, io.iov_base, io.iov_len);
      if (result < 0) {
        if (cnt == 0) {
          return convertErrno();
        }

        break;
      }

      cnt += result;

      if (cnt != io.iov_len) {
        break;
      }
    }
  }

  uio->resid -= cnt;
  uio->offset += cnt;
  return {};
}

static orbis::ErrorCode host_read(orbis::File *file, orbis::Uio *uio,
                                  orbis::Thread *) {
  auto hostFile = static_cast<HostFile *>(file);
  if (!hostFile->dirEntries.empty())
    return orbis::ErrorCode::ISDIR;

  return host_fd_read(hostFile->hostFd, uio);
}

static orbis::ErrorCode host_write(orbis::File *file, orbis::Uio *uio,
                                   orbis::Thread *) {
  auto hostFile = static_cast<HostFile *>(file);
  if (!hostFile->dirEntries.empty())
    return orbis::ErrorCode::ISDIR;

  return host_fd_write(hostFile->hostFd, uio);
}

static orbis::ErrorCode host_mmap(orbis::File *file, void **address,
                                  std::uint64_t size, std::int32_t prot,
                                  std::int32_t flags, std::int64_t offset,
                                  orbis::Thread *thread) {
  auto hostFile = static_cast<HostFile *>(file);
  if (!hostFile->dirEntries.empty())
    return orbis::ErrorCode::ISDIR;

  auto result = vm::map(*address, size, prot, flags,
                        vm::kMapInternalReserveOnly, hostFile->device.get(), offset);

  if (result == (void *)-1) {
    return orbis::ErrorCode::NOMEM;
  }

  size = rx::alignUp(size, vm::kPageSize);

  result =
      ::mmap(result, size, prot & vm::kMapProtCpuAll,
             ((flags & vm::kMapFlagPrivate) != 0 ? MAP_PRIVATE : MAP_SHARED) |
                 MAP_FIXED,
             hostFile->hostFd, offset);
  if (result == (void *)-1) {
    auto errc = convertErrno();
    std::printf("Failed to map file at %p-%p\n", *address,
                (char *)*address + size);
    return errc;
  }

  std::printf("file mapped at %p-%p:%lx\n", result, (char *)result + size,
              offset);

  struct stat stat;
  fstat(hostFile->hostFd, &stat);
  if (stat.st_size < offset + size) {
    std::size_t rest = std::min(offset + size - stat.st_size, vm::kPageSize);

    if (rest > rx::mem::pageSize) {
      auto fillSize = rx::alignUp(rest, rx::mem::pageSize) - rx::mem::pageSize;

      std::printf("adding dummy mapping %p-%p, file ends at %p\n",
                  (char *)result + size - fillSize, (char *)result + size,
                  (char *)result + (stat.st_size - offset));

      auto ptr = ::mmap((char *)result + size - fillSize, fillSize,
                        prot & vm::kMapProtCpuAll,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

      if (ptr == (void *)-1) {
        std::printf("failed to add dummy mapping %p-%p\n", result,
                    (char *)result + size);
      }
    }
  }

  *address = result;
  return {};
}

static orbis::ErrorCode host_stat(orbis::File *file, orbis::Stat *sb,
                                  orbis::Thread *thread) {
  auto hostFile = static_cast<HostFile *>(file);
  struct stat hostStat;
  ::fstat(hostFile->hostFd, &hostStat);
  sb->dev = hostStat.st_dev; // TODO
  sb->ino = hostStat.st_ino;
  sb->mode = hostStat.st_mode;
  sb->nlink = hostStat.st_nlink;
  sb->uid = hostStat.st_uid;
  sb->gid = hostStat.st_gid;
  sb->rdev = hostStat.st_rdev;
  sb->atim = {
      .sec = static_cast<uint64_t>(hostStat.st_atim.tv_sec),
      .nsec = static_cast<uint64_t>(hostStat.st_atim.tv_nsec),
  };
  sb->mtim = {
      .sec = static_cast<uint64_t>(hostStat.st_mtim.tv_sec),
      .nsec = static_cast<uint64_t>(hostStat.st_mtim.tv_nsec),
  };
  sb->ctim = {
      .sec = static_cast<uint64_t>(hostStat.st_mtim.tv_sec),
      .nsec = static_cast<uint64_t>(hostStat.st_mtim.tv_nsec),
  };
  sb->size = hostStat.st_size;
  sb->blocks = hostStat.st_blocks;
  sb->blksize = hostStat.st_blksize;
  // TODO
  sb->flags = 0;
  sb->gen = 0;
  sb->lspare = 0;
  sb->birthtim = {
      .sec = static_cast<uint64_t>(hostStat.st_mtim.tv_sec),
      .nsec = static_cast<uint64_t>(hostStat.st_mtim.tv_nsec),
  };

  return {};
}

static orbis::ErrorCode host_truncate(orbis::File *file, std::uint64_t len,
                                      orbis::Thread *thread) {
  auto hostFile = static_cast<HostFile *>(file);
  if (!hostFile->dirEntries.empty()) {
    return orbis::ErrorCode::ISDIR;
  }

  if (hostFile->alignTruncate) {
    len = rx::alignUp(len, vm::kPageSize);
  }

  if (::ftruncate(hostFile->hostFd, len)) {
    return convertErrno();
  }

  return {};
}

static orbis::ErrorCode socket_ioctl(orbis::File *file, std::uint64_t request,
                                     void *argp, orbis::Thread *thread) {
  auto soc = static_cast<SocketFile *>(file);

  // FIONBIO: set/clear non-blocking mode
  // On Linux, FIONBIO is 0x5421 (from <sys/ioctl.h>)
  if (request == 0x5421) { // FIONBIO
    if (argp) {
      int nonblock = 0;
      std::memcpy(&nonblock, argp, sizeof(nonblock));
      int flags = ::fcntl(soc->hostFd, F_GETFL, 0);
      if (flags < 0) {
        return convertErrno();
      }
      if (nonblock) {
        flags |= O_NONBLOCK;
      } else {
        flags &= ~O_NONBLOCK;
      }
      if (::fcntl(soc->hostFd, F_SETFL, flags) < 0) {
        return convertErrno();
      }
    }
    return {};
  }

  // FIONREAD: get number of bytes available to read
  if (request == 0x541B) { // FIONREAD
    if (argp) {
      int avail = 0;
      if (::ioctl(soc->hostFd, FIONREAD, &avail) < 0) {
        return convertErrno();
      }
      std::memcpy(argp, &avail, sizeof(avail));
    }
    return {};
  }

  // SIOCGIFADDR / SIOCGIFCONF etc. - network interface queries
  // These are used by network configuration tools
  if (request >= 0x8900 && request <= 0x892F) {
    // Socket device ioctls - pass through to host
    if (::ioctl(soc->hostFd, static_cast<unsigned long>(request), argp) < 0) {
      return convertErrno();
    }
    return {};
  }

  ORBIS_LOG_ERROR("Unhandled socket ioctl", request, soc->name);
  return {};
}

static orbis::ErrorCode socket_read(orbis::File *file, orbis::Uio *uio,
                                    orbis::Thread *) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    while (true) {
      std::this_thread::sleep_for(std::chrono::days(1));
    }
    return orbis::ErrorCode::INVAL;
  }

  if (uio->iov->len) {
    ORBIS_LOG_FATAL(__FUNCTION__, file, uio->iov->len);
  }
  return host_fd_read(socket->hostFd, uio);
}

static orbis::ErrorCode socket_write(orbis::File *file, orbis::Uio *uio,
                                     orbis::Thread *) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    for (auto io : std::span(uio->iov, uio->iovcnt)) {
      uio->offset += io.len;
    }
    return {};
  }

  ORBIS_LOG_FATAL(__FUNCTION__, file, uio->iov->len);
  return host_fd_write(socket->hostFd, uio);
}

static orbis::ErrorCode socket_bind(orbis::File *file,
                                    orbis::SocketAddress *address,
                                    std::size_t addressLen,
                                    orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return {};
  }

  sockaddr_storage storage{};
  socklen_t storageLen = 0;
  if (!socketAddressToSockaddr(address, storage, storageLen)) {
    return orbis::ErrorCode::AFNOSUPPORT;
  }

  if (::bind(socket->hostFd, reinterpret_cast<sockaddr *>(&storage),
             storageLen) < 0) {
    return convertErrno();
  }

  return {};
}

static orbis::ErrorCode socket_listen(orbis::File *file, int backlog,
                                      orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return {};
  }

  if (::listen(socket->hostFd, backlog) < 0) {
    return convertErrno();
  }

  return {};
}

static orbis::ErrorCode socket_accept(orbis::File *file,
                                      orbis::SocketAddress *address,
                                      std::uint32_t *addressLen,
                                      orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return orbis::ErrorCode::BADF;
  }

  sockaddr_storage storage{};
  socklen_t len = sizeof(storage);
  int result =
      ::accept(socket->hostFd, reinterpret_cast<sockaddr *>(&storage), &len);

  if (result < 0) {
    return convertErrno();
  }

  if (address && addressLen) {
    sockaddrToSocketAddress(reinterpret_cast<sockaddr *>(&storage), len,
                            address);
    *addressLen = address->len;
  }

  // Determine orbis family from host family
  int orbisFamily = socket->dom; // default to the listening socket's family
  if (storage.ss_family == AF_INET) {
    orbisFamily = 2;
  } else if (storage.ss_family == AF_INET6) {
    orbisFamily = 10;
  }

  auto guestSocket = wrapSocket(result, "", orbisFamily, socket->type,
                                socket->prot);
  auto guestFd = thread->tproc->fileDescriptors.insert(guestSocket);
  thread->retval[0] = guestFd;
  ORBIS_LOG_ERROR(__FUNCTION__, socket->name, guestFd);
  return {};
}

static orbis::ErrorCode socket_connect(orbis::File *file,
                                       orbis::SocketAddress *address,
                                       std::uint32_t addressLen,
                                       orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return orbis::ErrorCode::CONNREFUSED;
  }

  sockaddr_storage storage{};
  socklen_t storageLen = 0;
  if (!socketAddressToSockaddr(address, storage, storageLen)) {
    return orbis::ErrorCode::AFNOSUPPORT;
  }

  if (::connect(socket->hostFd, reinterpret_cast<sockaddr *>(&storage),
                storageLen) < 0) {
    return convertErrno();
  }

  return {};
}

orbis::ErrorCode socket_setsockopt(orbis::File *file, orbis::sint level,
                                   orbis::sint name, const void *val,
                                   orbis::sint valsize, orbis::Thread *thread) {
  ORBIS_LOG_ERROR(__FUNCTION__, level, name);
  auto socket = static_cast<SocketFile *>(file);
  orbis::kvector<std::byte> option(valsize);
  std::memcpy(option.data(), val, valsize);
  socket->options[name] = std::move(option);
  return {};
}

orbis::ErrorCode socket_getsockopt(orbis::File *file, orbis::sint level,
                                   orbis::sint name, void *val,
                                   orbis::sint *avalsize,
                                   orbis::Thread *thread) {
  ORBIS_LOG_ERROR(__FUNCTION__, level, name);
  auto socket = static_cast<SocketFile *>(file);
  auto &option = socket->options[name];
  if (option.size() > *avalsize) {
    return orbis::ErrorCode::INVAL;
  }

  *avalsize = option.size();
  std::memcpy(val, option.data(), option.size());
  return {};
}

orbis::ErrorCode socket_recvfrom(orbis::File *file, void *buf,
                                 orbis::size_t len, orbis::sint flags,
                                 orbis::SocketAddress *from,
                                 orbis::uint32_t *fromlenaddr,
                                 orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return orbis::ErrorCode::BADF;
  }

  sockaddr_storage fromStorage{};
  socklen_t fromLen = sizeof(fromStorage);
  auto count = ::recvfrom(socket->hostFd, buf, len, flags,
                          reinterpret_cast<sockaddr *>(&fromStorage), &fromLen);
  if (count < 0) {
    return convertErrno();
  }
  thread->retval[0] = count;

  if (from && fromlenaddr) {
    sockaddrToSocketAddress(reinterpret_cast<sockaddr *>(&fromStorage),
                            fromLen, from);
    *fromlenaddr = from->len;
  }

  return {};
}

static orbis::ErrorCode socket_shutdown(orbis::File *file, orbis::sint how,
                                        orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return orbis::ErrorCode::BADF;
  }

  // Map orbis shutdown how values to host SHUT_RD/SHUT_WR/SHUT_RDWR
  int hostHow = SHUT_RDWR;
  switch (how) {
  case 0: // SHUT_RD
    hostHow = SHUT_RD;
    break;
  case 1: // SHUT_WR
    hostHow = SHUT_WR;
    break;
  case 2: // SHUT_RDWR
    hostHow = SHUT_RDWR;
    break;
  default:
    return orbis::ErrorCode::INVAL;
  }

  if (::shutdown(socket->hostFd, hostHow) < 0) {
    return convertErrno();
  }

  return {};
}

static orbis::ErrorCode socket_sendto(orbis::File *file, const void *buf,
                                      orbis::size_t len, orbis::sint flags,
                                      orbis::caddr_t to, orbis::sint tolen,
                                      orbis::Thread *thread) {
  auto socket = static_cast<SocketFile *>(file);

  if (socket->hostFd < 0) {
    return orbis::ErrorCode::BADF;
  }

  if (to != nullptr && tolen > 0) {
    sockaddr_storage toStorage{};
    socklen_t toLen = 0;
    if (!socketAddressToSockaddr(reinterpret_cast<orbis::SocketAddress *>(to),
                                 toStorage, toLen)) {
      return orbis::ErrorCode::AFNOSUPPORT;
    }

    auto count = ::sendto(socket->hostFd, buf, len, flags,
                          reinterpret_cast<sockaddr *>(&toStorage), toLen);
    if (count < 0) {
      return convertErrno();
    }
    thread->retval[0] = count;
    return {};
  }

  auto count = ::send(socket->hostFd, buf, len, flags);
  if (count < 0) {
    return convertErrno();
  }
  thread->retval[0] = count;
  return {};
}

static const orbis::FileOps hostOps = {
    .read = host_read,
    .write = host_write,
    .truncate = host_truncate,
    .stat = host_stat,
    .mmap = host_mmap,
};

static const orbis::FileOps socketOps = {
    .ioctl = socket_ioctl,
    .read = socket_read,
    .write = socket_write,
    .bind = socket_bind,
    .listen = socket_listen,
    .accept = socket_accept,
    .connect = socket_connect,
    .sendto = socket_sendto,
    .recvfrom = socket_recvfrom,
    .shutdown = socket_shutdown,
    .setsockopt = socket_setsockopt,
    .getsockopt = socket_getsockopt,
};

orbis::IoDevice *createHostIoDevice(orbis::kstring hostPath,
                                    orbis::kstring virtualPath) {
  while (hostPath.size() > 0 && hostPath.ends_with("/")) {
    hostPath.resize(hostPath.size() - 1);
  }

  return orbis::knew<HostFsDevice>(std::move(hostPath), std::move(virtualPath));
}

rx::Ref<orbis::File> wrapSocket(int hostFd, orbis::kstring name, int dom,
                                int type, int prot) {
  auto s = orbis::knew<SocketFile>();
  s->name = std::move(name);
  s->dom = dom;
  s->type = type;
  s->prot = prot;
  s->hostFd = hostFd;
  s->ops = &socketOps;
  return s;
}

orbis::ErrorCode createSocket(rx::Ref<orbis::File> *file, orbis::kstring name,
                              int dom, int type, int prot) {
  int fd = -1;

  int domain = dom;
  int sockType = type;
  int protocol = prot;

  // Map orbis socket domains to host domains.
  // orbis family 1 = AF_UNIX, 2 = AF_INET, 10 = AF_INET6
  if (domain == 1) {
    domain = AF_UNIX;
  } else if (domain == 2) {
    domain = AF_INET;
  } else if (domain == 10) {
    domain = AF_INET6;
  }

  // Map orbis socket types to host types.
  // SOCK_STREAM=1, SOCK_DGRAM=2, SOCK_RAW=3
  if (sockType == 1) {
    sockType = SOCK_STREAM;
  } else if (sockType == 2) {
    sockType = SOCK_DGRAM;
  } else if (sockType == 3) {
    sockType = SOCK_RAW;
  }

  fd = ::socket(domain, sockType, protocol);

  if (fd < 0) {
    return convertErrno();
  }

  *file = wrapSocket(fd, std::move(name), dom, type, prot);
  return {};
}

static std::optional<std::string>
findFileInDir(const std::filesystem::path &dir, const char *name) {
  for (auto entry : std::filesystem::directory_iterator(dir)) {
    auto entryName = entry.path().filename();
    if (strcasecmp(entryName.c_str(), name) == 0) {
      return entryName;
    }
  }
  return {};
}

static std::optional<std::filesystem::path>
toRealPath(const std::filesystem::path &inp) {
  if (inp.empty()) {
    return {};
  }

  std::filesystem::path result;
  for (auto elem : inp) {
    if (result.empty() || std::filesystem::exists(result / elem)) {
      result /= elem;
      continue;
    }

    auto icaseElem = findFileInDir(result, elem.c_str());
    if (!icaseElem) {
      return {};
    }

    result /= *icaseElem;
  }

  return result;
}

orbis::ErrorCode HostFsDevice::open(rx::Ref<orbis::File> *file,
                                    const char *path, std::uint32_t flags,
                                    std::uint32_t mode, orbis::Thread *thread) {
  auto realPath = hostPath + "/" + path;

  int realFlags = flags & O_ACCMODE;
  flags &= ~O_ACCMODE;

  if ((flags & orbis::kOpenFlagAppend) != 0) {
    realFlags |= O_APPEND;
    flags &= ~orbis::kOpenFlagAppend;
  }

  if ((flags & orbis::kOpenFlagNonBlock) != 0) {
    realFlags |= O_NONBLOCK;
    flags &= ~orbis::kOpenFlagNonBlock;
  }

  if ((flags & orbis::kOpenFlagFsync) != 0) {
    realFlags |= O_FSYNC;
    flags &= ~orbis::kOpenFlagFsync;
  }

  if ((flags & orbis::kOpenFlagAsync) != 0) {
    realFlags |= O_ASYNC;
    flags &= ~orbis::kOpenFlagAsync;
  }

  if ((flags & orbis::kOpenFlagTrunc) != 0) {
    realFlags |= O_TRUNC;
    flags &= ~orbis::kOpenFlagTrunc;
  }

  if ((flags & orbis::kOpenFlagCreat) != 0) {
    realFlags |= O_CREAT;
    flags &= ~orbis::kOpenFlagCreat;
  }

  if ((flags & orbis::kOpenFlagExcl) != 0) {
    realFlags |= O_EXCL;
    flags &= ~orbis::kOpenFlagExcl;
  }

  if ((flags & orbis::kOpenFlagDirectory) != 0) {
    realFlags |= O_DIRECTORY;
    flags &= ~orbis::kOpenFlagDirectory;
  }

  if (flags != 0) {
    ORBIS_LOG_ERROR("host_open: ***ERROR*** Unhandled open flags", flags);
  }

  int hostFd = ::open(realPath.c_str(), realFlags, 0777);

  orbis::ErrorCode error{};
  if (hostFd < 0) {
    error = convertErrno();

    if (auto icaseRealPath = toRealPath(realPath)) {
      ORBIS_LOG_WARNING(__FUNCTION__, path, realPath.c_str(),
                        icaseRealPath->c_str());
      hostFd = ::open(icaseRealPath->c_str(), realFlags, 0777);

      if (hostFd < 0) {
        ORBIS_LOG_ERROR("host_open failed", path, realPath.c_str(),
                        icaseRealPath->c_str(), error);
        return convertErrno();
      }
    }
  }
  if (hostFd < 0) {
    ORBIS_LOG_ERROR("host_open failed", path, realPath.c_str(), error);
    return error;
  }

  // Assume the file is a directory and try to read direntries
  orbis::kvector<orbis::Dirent> dirEntries;
  char hostEntryBuffer[sizeof(dirent64) * 4];
  while (true) {
    auto r = getdents64(hostFd, hostEntryBuffer, sizeof(hostEntryBuffer));
    if (r <= 0)
      break;

    std::size_t offset = 0;

    while (offset < r) {
      ::dirent64 *entryPtr =
          reinterpret_cast<dirent64 *>(hostEntryBuffer + offset);
      offset += entryPtr->d_reclen;

      if (entryPtr->d_name == std::string_view("..") ||
          entryPtr->d_name == std::string_view(".") ||
          entryPtr->d_name == std::string_view(".rpcsx")) {
        continue;
      }

      // auto entryPath = (std::filesystem::path(virtualPath) / path /
      // entryPtr->d_name).lexically_normal().string();
      std::string entryPath = entryPtr->d_name;

      auto &entry = dirEntries.emplace_back();
      entry.fileno = dirEntries.size(); // TODO
      entry.reclen = sizeof(entry);
      entry.namlen = entryPath.length();
      std::strncpy(entry.name, entryPath.c_str(), sizeof(entry.name));
      if (entryPtr->d_type == DT_REG)
        entry.type = orbis::kDtReg;
      else if (entryPtr->d_type == DT_DIR || entryPtr->d_type == DT_LNK)
        entry.type = orbis::kDtDir; // Assume symlinks to be dirs
      else {
        ORBIS_LOG_ERROR("host_open: unknown directory entry d_type",
                        realPath.c_str(), entryPtr->d_name, entryPtr->d_type);
        dirEntries.pop_back();
      }
    }
  }

  auto newFile = orbis::knew<HostFile>();
  newFile->hostFd = hostFd;
  newFile->dirEntries = std::move(dirEntries);
  newFile->ops = &hostOps;
  newFile->device = this;
  *file = newFile;
  return {};
}

orbis::ErrorCode HostFsDevice::unlink(const char *path, bool recursive,
                                      orbis::Thread *thread) {
  std::error_code ec;

  if (recursive) {
    std::filesystem::remove_all(hostPath + "/" + path, ec);
  } else {
    std::filesystem::remove(hostPath + "/" + path, ec);
  }

  return convertErrorCode(ec);
}

orbis::ErrorCode HostFsDevice::createSymlink(const char *linkPath,
                                             const char *target,
                                             orbis::Thread *thread) {
  std::error_code ec;
  std::filesystem::create_symlink(
      std::filesystem::absolute(hostPath + "/" + linkPath),
      hostPath + "/" + target, ec);
  return convertErrorCode(ec);
}

orbis::ErrorCode HostFsDevice::mkdir(const char *path, int mode,
                                     orbis::Thread *thread) {
  std::error_code ec;
  std::filesystem::create_directories(hostPath + "/" + path, ec);
  return convertErrorCode(ec);
}
orbis::ErrorCode HostFsDevice::rmdir(const char *path, orbis::Thread *thread) {
  std::error_code ec;
  std::filesystem::remove_all(hostPath + "/" + path, ec);
  return convertErrorCode(ec);
}
orbis::ErrorCode HostFsDevice::rename(const char *from, const char *to,
                                      orbis::Thread *thread) {
  std::error_code ec;
  std::filesystem::rename(hostPath + "/" + from, hostPath + "/" + to, ec);
  return convertErrorCode(ec);
}

orbis::File *createHostFile(int hostFd, rx::Ref<orbis::IoDevice> device,
                            bool alignTruncate) {
  auto newFile = orbis::knew<HostFile>();
  newFile->hostFd = hostFd;
  newFile->ops = &hostOps;
  newFile->device = device;
  newFile->alignTruncate = alignTruncate;
  return newFile;
}

struct FdWrapDevice : public orbis::IoDevice {
  int fd;

  orbis::ErrorCode open(rx::Ref<orbis::File> *file, const char *path,
                        std::uint32_t flags, std::uint32_t mode,
                        orbis::Thread *thread) override {
    *file = createHostFile(fd, this);
    static_cast<HostFile *>(file->get())->closeOnExit = false;
    return {};
  }
};

orbis::IoDevice *createFdWrapDevice(int fd) {
  auto result = orbis::knew<FdWrapDevice>();
  result->fd = fd;
  return result;
}
