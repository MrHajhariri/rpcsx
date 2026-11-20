#include "sys/sysproto.hpp"
#include "thread/Process.hpp"
#include "thread/Thread.hpp"
#include "orbis-config.hpp"
#include "ucontext.h"

// ptrace request numbers (FreeBSD/PS4 x86-64)
// PT_TRACE_ME = 0, PT_READ_I = 1, PT_READ_D = 2, PT_READ_I (user) = 3,
// PT_WRITE_I = 4, PT_WRITE_D = 5, PT_WRITE_I (user) = 6,
// PT_CONTINUE = 7, PT_KILL = 8, ... PT_ATTACH = 10, PT_DETACH = 11,
// PT_GETREGS = 12, PT_SETREGS = 13, ...

namespace {

// x86-64 ptrace general-purpose register set (subset available on host)
struct PtraceRegs {
  std::uint64_t r15;
  std::uint64_t r14;
  std::uint64_t r13;
  std::uint64_t r12;
  std::uint64_t r11;
  std::uint64_t r10;
  std::uint64_t r9;
  std::uint64_t r8;
  std::uint64_t rsi;
  std::uint64_t rdi;
  std::uint64_t rbp;
  std::uint64_t rbx;
  std::uint64_t rdx;
  std::uint64_t rcx;
  std::uint64_t rax;
  std::uint64_t rip;
  std::uint64_t rflags;
  std::uint64_t rsp;
};

} // namespace

orbis::SysResult orbis::sys_ptrace(Thread *thread, sint req, pid_t pid,
                                   caddr_t addr, sint data) {
  // Find target process
  orbis::Process *targetProc = nullptr;
  if (pid == 0 || pid == thread->tproc->pid || pid == thread->tproc->hostPid) {
    targetProc = thread->tproc;
  } else {
    targetProc = orbis::findProcessById(pid);
    if (targetProc == nullptr) {
      return ErrorCode::SRCH;
    }
  }

  // Find a target thread in the process (use the first one)
  orbis::Thread *targetThread = nullptr;
  for (auto [tid, t] : targetProc->threadsMap) {
    targetThread = t;
    break;
  }

  switch (req) {
  case 0: // PT_TRACE_ME - trace this process
    return {};

  case 1: // PT_READ_I - read word from instruction space
  case 2: { // PT_READ_D - read word from data space
    if (addr == nullptr) {
      return ErrorCode::INVAL;
    }
    std::uint64_t val = 0;
    if (auto e = uread(val, reinterpret_cast<ptr<std::uint64_t>>(addr));
        e != ErrorCode{}) {
      return e;
    }
    thread->retval[0] = val;
    return {};
  }

  case 4: // PT_WRITE_I - write word to instruction space
  case 5: { // PT_WRITE_D - write word to data space
    if (addr == nullptr) {
      return ErrorCode::INVAL;
    }
    if (auto e = uwrite(reinterpret_cast<ptr<std::uint64_t>>(addr),
                        static_cast<std::uint64_t>(data));
        e != ErrorCode{}) {
      return e;
    }
    return {};
  }

  case 7: // PT_CONTINUE - continue execution with optional signal
    // Threads run natively; just acknowledge
    return {};

  case 8: // PT_KILL - kill the target process/thread
    if (targetThread != nullptr) {
      targetThread->sendSignal(9); // SIGKILL
    }
    return {};

  case 10: // PT_ATTACH - attach to a running process
    return {};

  case 11: // PT_DETACH - detach from a process
    return {};

  case 12: { // PT_GETREGS - get general-purpose registers
    if (addr == nullptr || targetThread == nullptr ||
        targetThread->context == nullptr) {
      return ErrorCode::INVAL;
    }

    auto *ctx = reinterpret_cast<ucontext_t *>(targetThread->context);
    PtraceRegs regs{};
    regs.r15 = ctx->uc_mcontext.gregs[REG_R15];
    regs.r14 = ctx->uc_mcontext.gregs[REG_R14];
    regs.r13 = ctx->uc_mcontext.gregs[REG_R13];
    regs.r12 = ctx->uc_mcontext.gregs[REG_R12];
    regs.r11 = ctx->uc_mcontext.gregs[REG_R11];
    regs.r10 = ctx->uc_mcontext.gregs[REG_R10];
    regs.r9 = ctx->uc_mcontext.gregs[REG_R9];
    regs.r8 = ctx->uc_mcontext.gregs[REG_R8];
    regs.rsi = ctx->uc_mcontext.gregs[REG_RSI];
    regs.rdi = ctx->uc_mcontext.gregs[REG_RDI];
    regs.rbp = ctx->uc_mcontext.gregs[REG_RBP];
    regs.rbx = ctx->uc_mcontext.gregs[REG_RBX];
    regs.rdx = ctx->uc_mcontext.gregs[REG_RDX];
    regs.rcx = ctx->uc_mcontext.gregs[REG_RCX];
    regs.rax = ctx->uc_mcontext.gregs[REG_RAX];
    regs.rip = ctx->uc_mcontext.gregs[REG_RIP];
    regs.rflags = ctx->uc_mcontext.gregs[REG_EFL];
    regs.rsp = ctx->uc_mcontext.gregs[REG_RSP];

    if (auto e = uwrite(reinterpret_cast<ptr<PtraceRegs>>(addr), regs);
        e != ErrorCode{}) {
      return e;
    }
    return {};
  }

  case 13: { // PT_SETREGS - set general-purpose registers
    if (addr == nullptr || targetThread == nullptr ||
        targetThread->context == nullptr) {
      return ErrorCode::INVAL;
    }

    PtraceRegs regs{};
    if (auto e = uread(regs, reinterpret_cast<ptr<const PtraceRegs>>(addr));
        e != ErrorCode{}) {
      return e;
    }

    auto *ctx = reinterpret_cast<ucontext_t *>(targetThread->context);
    ctx->uc_mcontext.gregs[REG_R15] = regs.r15;
    ctx->uc_mcontext.gregs[REG_R14] = regs.r14;
    ctx->uc_mcontext.gregs[REG_R13] = regs.r13;
    ctx->uc_mcontext.gregs[REG_R12] = regs.r12;
    ctx->uc_mcontext.gregs[REG_R11] = regs.r11;
    ctx->uc_mcontext.gregs[REG_R10] = regs.r10;
    ctx->uc_mcontext.gregs[REG_R9] = regs.r9;
    ctx->uc_mcontext.gregs[REG_R8] = regs.r8;
    ctx->uc_mcontext.gregs[REG_RSI] = regs.rsi;
    ctx->uc_mcontext.gregs[REG_RDI] = regs.rdi;
    ctx->uc_mcontext.gregs[REG_RBP] = regs.rbp;
    ctx->uc_mcontext.gregs[REG_RBX] = regs.rbx;
    ctx->uc_mcontext.gregs[REG_RDX] = regs.rdx;
    ctx->uc_mcontext.gregs[REG_RCX] = regs.rcx;
    ctx->uc_mcontext.gregs[REG_RAX] = regs.rax;
    ctx->uc_mcontext.gregs[REG_RIP] = regs.rip;
    ctx->uc_mcontext.gregs[REG_EFL] = regs.rflags;
    ctx->uc_mcontext.gregs[REG_RSP] = regs.rsp;

    return {};
  }

  default:
    return ErrorCode::IO;
  }
}
