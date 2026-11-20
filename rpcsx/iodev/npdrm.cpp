#include "orbis/IoDevice.hpp"
#include "orbis/KernelAllocator.hpp"
#include "orbis/KernelContext.hpp"
#include "orbis/file.hpp"
#include "orbis/utils/Logs.hpp"
#include <cstring>

struct NpdrmFile : orbis::File {};

static orbis::ErrorCode npdrm_ioctl(orbis::File *file, std::uint64_t request,
                                    void *argp, orbis::Thread *thread) {
  // NPDRM license check ioctls. In development mode, bypass license checks
  // by returning success so homebrew and unsigned content can run.
  if (orbis::g_context->devMode != 0) {
    ORBIS_LOG_ERROR("NPDRM ioctl bypassed (dev mode)", request);
    if (argp != nullptr) {
      std::memset(argp, 0, 8);
    }
    return {};
  }

  ORBIS_LOG_FATAL("Unhandled NPDRM ioctl", request);
  return {};
}

static const orbis::FileOps fileOps = {
    .ioctl = npdrm_ioctl,
};

struct NpdrmDevice : orbis::IoDevice {
  orbis::ErrorCode open(rx::Ref<orbis::File> *file, const char *path,
                        std::uint32_t flags, std::uint32_t mode,
                        orbis::Thread *thread) override {
    auto newFile = orbis::knew<NpdrmFile>();
    newFile->ops = &fileOps;
    newFile->device = this;

    *file = newFile;
    return {};
  }
};

orbis::IoDevice *createNpdrmCharacterDevice() {
  return orbis::knew<NpdrmDevice>();
}
