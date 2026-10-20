#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "PPCRecompilerThreadPool.h"
#include "Cafe/OS/RPL/rpl.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <mach/task_info.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/mach_init.h>
#if __has_include(<os/proc.h>)
#include <os/proc.h>
#endif
#endif
#endif

#include "IML/IML.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <map>
#include <mutex>
#include <limits>
#include <string>
#include <vector>
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Latte/Core/Latte.h"

// Compilation runs on the worker thread pool. Set MELOCAFE_JIT_SYNC=1 to compile on the emulation threads instead (debugging aid).
#ifndef PPCREC_FORCE_SYNCHRONOUS_COMPILATION
#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION    0
#endif
#define PPCREC_LOG_RECOMPILATION_RESULTS        0

static bool s_dualMapJITEnabled = false;
static PPCRecompilerThreadPool s_threadPool;

bool PPCRecompiler_isDualMapJITEnabled()
{
    return s_dualMapJITEnabled;
}

#if defined(__APPLE__)
extern "C" int csops(pid_t pid, int ops, void* useraddr, size_t usersize);

constexpr uint32_t CS_DEBUGGED = 0x10000000;

bool checkDebugged() {
    int flags = 0;

    int result = csops(getpid(), 0, &flags, sizeof(flags));
    return (result == 0) && ((flags & CS_DEBUGGED) != 0);
}
#else
bool checkDebugged() {
    return true;
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)

#include <fcntl.h>
#include <mach/mach.h>

extern "C" {

__attribute__((noinline, optnone, naked))
void* BreakGetJITMapping(void *addr, size_t len) {
    asm("mov x16, #1 \n"
        "brk #0xf00d \n"
        "ret");
}

__attribute__((noinline,optnone,naked))
void JIT26Detach(void) {
    asm("mov x16, #0 \n"
        "brk #0xf00d \n"
        "ret");
}

__attribute__((noinline,optnone,naked))
void* BreakMarkJITMapping(size_t bytes) {
    asm("brk #0x69 \n"
        "ret");
}

}

static int PPCRecompiler_readBoolEnvVar(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return -1;
    }

    if (strcmp(value, "1") == 0)
    {
        return 1;
    }

    if (strcmp(value, "0") == 0)
    {
        return 0;
    }

    return -1;
}

static int PPCRecompiler_getAppleOsMajorVersion()
{
#if BOOST_OS_IOS
    char versionBuffer[32] = {};
    size_t versionLength = sizeof(versionBuffer);

    if (sysctlbyname("kern.osproductversion", versionBuffer, &versionLength, nullptr, 0) != 0)
    {
        return 0;
    }

    versionBuffer[sizeof(versionBuffer) - 1] = '\0';
    return atoi(versionBuffer);
#else
    return 0;
#endif
}

static bool PPCRecompiler_shouldUseDualMapByDefault()
{
#if BOOST_OS_IOS
    return PPCRecompiler_getAppleOsMajorVersion() >= 26;
#else
    return false;
#endif
}

bool PPCRecompiler_readTXMEnvVar()
{
    if (!s_dualMapJITEnabled) { return false; }

    return PPCRecompiler_readBoolEnvVar("HAS_TXM") == 1;
}

static void PPCRecompiler_finishJitMappingSession()
{
    if (PPCRecompiler_readTXMEnvVar())
    {
        cemuLog_log(LogType::Force, "Recompiler: detaching TXM debugger after JIT mapping");
        JIT26Detach();
    }
}

DualMapRegion PPCRecompiler_allocateDualMap(size_t size)
{
    DualMapRegion r;
    r.size = size;

    void* rxPtr = nullptr;

    if (PPCRecompiler_readTXMEnvVar())
        rxPtr = BreakGetJITMapping(nullptr, size);

    if (!rxPtr)
        rxPtr = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (!rxPtr || rxPtr == MAP_FAILED)
        return r;

    vm_address_t rwAddr = 0;
    vm_prot_t curProt = VM_PROT_NONE, maxProt = VM_PROT_NONE;

    kern_return_t kr = vm_remap(
        mach_task_self(), &rwAddr, (vm_size_t)size, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), (vm_address_t)rxPtr, FALSE,
        &curProt, &maxProt, VM_INHERIT_NONE);

    cemuLog_log(LogType::Force, "[DualMap] vm_remap: kr={} rwAddr={:p} curProt={} maxProt={}",
        kr, (void*)rwAddr, curProt, maxProt);
    if (kr != KERN_SUCCESS)
    {
        munmap(rxPtr, size);
        return r;
    }

    kr = vm_protect(mach_task_self(), rwAddr, (vm_size_t)size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    cemuLog_log(LogType::Force, "[DualMap] vm_protect(RW): kr={}", kr);
    if (kr != KERN_SUCCESS)
    {
        vm_deallocate(mach_task_self(), rwAddr, (vm_size_t)size);
        munmap(rxPtr, size);
        return r;
    }

    r.rxAlias = rxPtr;
    r.rwAlias = (void*)rwAddr;

   // int result2 = func();
    // cemuLog_log(LogType::Force, "Executed JIT-ed function, result: {}\n", result2);

    return r;
}

#endif // __APPLE__ && __aarch64__

#if !defined(__APPLE__) || !defined(__aarch64__)

static int PPCRecompiler_readBoolEnvVar(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return -1;
    }

    if (strcmp(value, "1") == 0)
    {
        return 1;
    }

    if (strcmp(value, "0") == 0)
    {
        return 0;
    }

    return -1;
}

static bool PPCRecompiler_shouldUseDualMapByDefault()
{
    return false;
}

DualMapRegion PPCRecompiler_allocateDualMap(size_t size)
{
    (void)size;
    return {};
}

#endif

void PPCRecompiler_flushInstructionCache(void* codePtr, size_t codeSize)
{
#if defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), codePtr, codeSize);
#elif defined(__APPLE__) && defined(__aarch64__)
    sys_icache_invalidate(codePtr, codeSize);
#elif !defined(ARCH_X86_64)
    __builtin___clear_cache((char*)codePtr, (char*)codePtr + codeSize);
#else
    (void)codePtr;
    (void)codeSize;
#endif
}

// Set while the JIT arena has no free range left. Compilation requests are skipped (the interpreter runs the code) instead of generating
// code that would be thrown away. Cleared again as soon as functions are released and space is available.
static std::atomic<bool> s_jitArenaFull{false};

// ---- statistics, logged periodically by the stats thread (see PPCRecompiler_startStatsThread) ----
static std::atomic<uint64> s_statCompiled{0};
static std::atomic<uint64> s_statCompileFailed{0};
static std::atomic<uint64> s_statCompileNs{0};
static std::atomic<uint64> s_statCompileMaxNs{0};
static std::atomic<uint64> s_statInvalidations{0};
static std::atomic<uint64> s_statInvalidatedBytes{0};
static std::atomic<uint64> s_statDeleted{0};
static std::atomic<uint64> s_statActivated{0};
static std::atomic<uint64> s_statDiscarded{0};
static bool s_syncCompile = false; // MELOCAFE_JIT_SYNC=1
static void* s_ifaceEnter = nullptr;
static void* s_ifaceLeaveUnvisited = nullptr;
static void* s_ifaceLeaveVisited = nullptr;

struct DualMapArena
{
    DualMapRegion region;
    std::mutex mutex;
    std::map<size_t, size_t> freeRanges;

    bool init(size_t size)
    {
        region = PPCRecompiler_allocateDualMap(size);
        if (!region.rwAlias || !region.rxAlias)
        {
            cemuLog_log(LogType::Force, "JIT arena: allocation failed (rw={:p} rx={:p} size={}MB)",
                region.rwAlias, region.rxAlias, size / 1024 / 1024);
            return false;
        }
        reset();
        cemuLog_log(LogType::Force, "JIT arena: rw={:p} rx={:p} size={}MB",
            region.rwAlias, region.rxAlias, size / 1024 / 1024);
        return true;
    }

    // Tries `preferred` first and halves the size until the mapping succeeds or `minimum` is reached.
    // The arena needs two aliases (rx + rw) of the same size, so it costs twice its size in address space.
    bool initWithFallback(size_t preferred, size_t minimum)
    {
        for (size_t size = preferred; size >= minimum; size /= 2)
        {
            if (init(size))
            {
                if (size != preferred)
                    cemuLog_log(LogType::Force, "JIT arena: using reduced size of {}MB (wanted {}MB), address space is limited", size / 1024 / 1024, preferred / 1024 / 1024);
                return true;
            }
        }
        return false;
    }

    size_t freeBytes()
    {
        std::lock_guard lock(mutex);
        size_t total = 0;
        for (auto& it : freeRanges)
            total += it.second;
        return total;
    }

    DualMapRegion alloc(size_t size)
    {
        if (!size || size > std::numeric_limits<size_t>::max() - 15)
            return {};
        size = (size + 15) & ~size_t(15);
        std::lock_guard lock(mutex);
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it)
        {
            if (it->second < size)
                continue;
            const size_t pos = it->first;
            // Reuse the map node so splitting a range cannot lose it on bad_alloc.
            auto range = freeRanges.extract(it);
            if (range.mapped() > size)
            {
                range.key() += size;
                range.mapped() -= size;
                freeRanges.insert(std::move(range));
            }
            return {(uint8*)region.rwAlias + pos, (uint8*)region.rxAlias + pos, size};
        }
        if (!s_jitArenaFull.exchange(true))
            cemuLog_log(LogType::Force, "JIT arena: full (no free range for {} bytes). Code that is not compiled yet runs in the interpreter until space is freed", size);
        return {};
    }

    void release(const DualMapRegion& allocation)
    {
        if (!allocation.rwAlias || !allocation.size)
            return;
        const size_t pos = (uint8*)allocation.rwAlias - (uint8*)region.rwAlias;
        cemu_assert(pos <= region.size && allocation.size <= region.size - pos);
        s_jitArenaFull = false; // space is available again
        std::lock_guard lock(mutex);
        auto next = freeRanges.lower_bound(pos);
        cemu_assert(next == freeRanges.end() || pos + allocation.size <= next->first);
        if (next != freeRanges.begin())
        {
            auto previous = std::prev(next);
            cemu_assert(previous->first + previous->second <= pos);
            if (previous->first + previous->second == pos)
            {
                previous->second += allocation.size;
                if (next != freeRanges.end() && previous->first + previous->second == next->first)
                {
                    previous->second += next->second;
                    freeRanges.erase(next);
                }
                return;
            }
        }
        if (next != freeRanges.end() && pos + allocation.size == next->first)
        {
            auto range = freeRanges.extract(next);
            range.key() = pos;
            range.mapped() += allocation.size;
            freeRanges.insert(std::move(range));
        }
        else
        {
            freeRanges.emplace(pos, allocation.size);
        }
    }

    void reset()
    {
        std::lock_guard lock(mutex);
        freeRanges.clear();
        if (region.size)
            freeRanges.emplace(0, region.size);
    }
};

static DualMapArena s_jitArena;

DualMapRegion PPCRecompiler_allocateJitArena(size_t size)
{
    return s_jitArena.alloc(size);
}

void PPCRecompiler_releaseJitArena(const DualMapRegion& region)
{
    s_jitArena.release(region);
}

void PPCRecompiler_freeDualMap(const DualMapRegion& region)
{
#if defined(__APPLE__) && defined(__aarch64__)
    if (region.rwAlias)
        vm_deallocate(mach_task_self(), (vm_address_t)region.rwAlias, region.size);
    if (region.rxAlias)
        munmap(region.rxAlias, region.size);
#else
    (void)region;
#endif
}

void* g_jitArenaRxBase = nullptr;
void* g_jitArenaRxEnd = nullptr;
void* g_jitArenaRwBase = nullptr;

struct PPCInvalidationRange
{
    MPTR startAddress;
    uint32 size;
    PPCInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {}
};

struct
{
    FSpinlock             recompilerSpinlock;
    std::queue<MPTR>      targetQueue;
    std::vector<PPCInvalidationRange> invalidationRanges;
} PPCRecompilerState;


RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> rangeStore_ppcRanges;

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

static std::mutex s_singleRecompilationMutex;

bool ppcRecompilerEnabled = false;
bool ppcRecompilerInited = false;
static std::atomic_int_fast32_t s_recompilerEnableCount{0};

void PPCRecompiler_recompileAtAddress(uint32 address);

void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
    if (s_jitArenaFull.load(std::memory_order_relaxed))
        return; // no room for more code, keep interpreting
    if (s_syncCompile)
    {
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
        return;
    PPCRecompilerState.recompilerSpinlock.lock();
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
    {
        PPCRecompilerState.recompilerSpinlock.unlock();
        return;
    }
    ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
    PPCRecompilerState.recompilerSpinlock.unlock();
    s_singleRecompilationMutex.lock();
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
        PPCRecompiler_recompileAtAddress(enterAddress);
    s_singleRecompilationMutex.unlock();
    return;
    }
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
        return;
    if (!PPCRecompilerState.recompilerSpinlock.try_lock())
        return;
    auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
    if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
    {
        PPCRecompilerState.recompilerSpinlock.unlock();
        return;
    }
    PPCRecompilerState.targetQueue.emplace(enterAddress);
    ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
    PPCRecompilerState.recompilerSpinlock.unlock();
    s_threadPool.Notify();
}

void PPCRecompiler_notifyWorkers()
{
    s_threadPool.Notify();
}

bool PPCRecompiler_drainQueue()
{
    PPCRecompilerState.recompilerSpinlock.lock();
    if (PPCRecompilerState.targetQueue.empty())
    {
        PPCRecompilerState.recompilerSpinlock.unlock();
        return false;
    }
    uint32 enterAddress = PPCRecompilerState.targetQueue.front();
    PPCRecompilerState.targetQueue.pop();
    auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
    
    if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
    {
        PPCRecompilerState.recompilerSpinlock.unlock();
        return true;
    }
    
    PPCRecompilerState.recompilerSpinlock.unlock();
    // Only one function is compiled at a time. The IML/backend passes were only ever run from a single thread (the synchronous path
    // also serializes with this mutex), so parallel compilation on the worker pool is not safe. Compiling still happens off the emulation threads.
    std::lock_guard<std::mutex> compileLock(s_singleRecompilationMutex);
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
        PPCRecompiler_recompileAtAddress(enterAddress);
    return true;
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
    if (!ppcRecompilerEnabled)
        return;
    PPCRecompiler_visitAddressNoBlock(enterAddress);
}

static std::atomic<int> s_enterLogCount{0};

static bool isInArena(void* ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t rxBase = (uintptr_t)s_jitArena.region.rxAlias;
    uintptr_t rxEnd  = rxBase + s_jitArena.region.size;
    return p >= rxBase && p < rxEnd;
}

static bool isInterfaceFunc(PPCREC_JUMP_ENTRY funcPtr)
{
    return funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited ||
           funcPtr == PPCRecompiler_leaveRecompilerCode_visited;
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevState = _controlfp(0, 0);
	_controlfp(_RC_NEAR, _MCW_RC);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	_controlfp(prevState, _MCW_RC);
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
    cemu_assert_debug(hCPU->instructionPointer == enterAddress);
    if (!ppcRecompilerEnabled)
        return;
    auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
    if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
    {
        cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
        PPCRecompiler_enter(hCPU, funcPtr);
    }
}

static std::atomic<int> s_attemptLogCount{0};

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
    cemu_assert_debug(hCPU->instructionPointer == enterAddress);
    if (!ppcRecompilerEnabled)
        return;
    if (hCPU->remainingCycles <= 0)
        return;
    auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
    if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
    {
        PPCRecompiler_visitAddressNoBlock(enterAddress);
    }
    else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
    {
        cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
        PPCRecompiler_enter(hCPU, funcPtr);
    }
}

bool PPCRecompiler_enabled()
{
    return ppcRecompilerEnabled;
}

void PPCRecompiler_Enable()
{
    if (!ppcRecompilerInited)
        return;

    s_recompilerEnableCount++;
    ppcRecompilerEnabled = true;
    PPCCore_InitializePointer(true);
}

void PPCRecompiler_Disable()
{
    if (!ppcRecompilerInited)
        return;

    const auto currentCount = s_recompilerEnableCount.load();
    if (currentCount > 0)
        s_recompilerEnableCount--;
    ppcRecompilerEnabled = s_recompilerEnableCount.load() > 0;
    PPCCore_InitializePointer(ppcRecompilerEnabled);
}

void PPCRecompiler_reprotectAsExecutable(void* codePtr, size_t codeSize)
{
    if (codePtr == nullptr || codeSize == 0)
        return;

    if (s_dualMapJITEnabled)
    {
        PPCRecompiler_flushInstructionCache(codePtr, codeSize);
        return;
    }

    static const uintptr_t pageSize = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)codePtr & ~(pageSize - 1);
    size_t alignedSize = (codeSize + ((uintptr_t)codePtr - base) + pageSize - 1) & ~(pageSize - 1);
    if (mprotect((void*)base, alignedSize, PROT_READ | PROT_EXEC) != 0)
        cemuLog_log(LogType::Force, "Recompiler: mprotect RX failed for {:p} ({} bytes)", codePtr, codeSize);

    PPCRecompiler_flushInstructionCache(codePtr, codeSize);
}

void PPCRecompiler_reprotectAsWriteable(void* codePtr, size_t codeSize)
{
    if (codePtr == nullptr || codeSize == 0)
        return;

    if (s_dualMapJITEnabled)
        return;

    static const uintptr_t pageSize = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)codePtr & ~(pageSize - 1);
    size_t alignedSize = (codeSize + ((uintptr_t)codePtr - base) + pageSize - 1) & ~(pageSize - 1);
    if (mprotect((void*)base, alignedSize, PROT_READ | PROT_WRITE) != 0)
        cemuLog_log(LogType::Force, "Recompiler: mprotect RW failed for {:p} ({} bytes)", codePtr, codeSize);
}

void PPCRecompiler_commitDualMapCode(PPCRecFunction_t* func)
{
    if (!s_dualMapJITEnabled)
        return;
    PPCRecompiler_reprotectAsExecutable(func->x86Code, func->x86Size);
}

static bool PPCRecompiler_rangesOverlap(MPTR startA, MPTR endA, MPTR startB, MPTR endB)
{
    return startA < endB && endA > startB;
}

static void PPCRecompiler_resetVisitedTableRange(uint32 startAddress, uint32 endAddress)
{
    cemu_assert_debug(PPCRecompilerState.recompilerSpinlock.is_locked());
    if (ppcRecompilerInstanceData == nullptr)
        return;

    uint64 start = (uint64)startAddress & ~3ULL;
    uint64 end = ((uint64)endAddress + 3ULL) & ~3ULL;
    for (uint64 currentAddr = start; currentAddr < end; currentAddr += 4)
    {
        auto& jumpTableEntry = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[currentAddr / 4];
        if (jumpTableEntry == PPCRecompiler_leaveRecompilerCode_visited)
            jumpTableEntry = PPCRecompiler_leaveRecompilerCode_unvisited;
    }
}

static void PPCRecompiler_resetVisitedMarkersForFunction(PPCRecFunction_t* func)
{
    cemu_assert_debug(PPCRecompilerState.recompilerSpinlock.is_locked());
    for (auto& recFuncRange : func->list_ranges)
        PPCRecompiler_resetVisitedTableRange(recFuncRange.ppcAddress, recFuncRange.ppcAddress + recFuncRange.ppcSize);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
    if (range.startAddress >= PPC_REC_CODE_AREA_END)
    {
        cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
        return nullptr;
    }
    
    uint32 codeGenRangeStart, codeGenRangeSize = 0;
    coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
    if (codeGenRangeSize != 0)
    {
        if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
        {
            if (coreinit::codeGenShouldAvoid())
                return nullptr;
        }
    }
    
    PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
    ppcRecFunc->ppcAddress = range.startAddress;
    ppcRecFunc->ppcSize = range.length;
    
#if PPCREC_LOG_RECOMPILATION_RESULTS
    BenchmarkTimer bt;
    bt.Start();
#endif

    ppcImlGenContext_t ppcImlGenContext = { 0 };
    ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
    if (!PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker))
    {
        delete ppcRecFunc;
        return nullptr;
    }

    uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
    uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();
    if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
    {
        if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
        {
            delete ppcRecFunc;
            return nullptr;
        }
    }

    if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
    {
        delete ppcRecFunc;
        return nullptr;
    }

#if defined(ARCH_X86_64)
    if (!PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext))
    {
        delete ppcRecFunc;
        return nullptr;
    }

    if (s_dualMapJITEnabled && ppcRecFunc->x86Code && ppcRecFunc->x86Size > 0)
    {
        DualMapRegion region = s_jitArena.alloc(ppcRecFunc->x86Size);
        if (region.rwAlias && region.rxAlias)
        {
            memcpy(region.rwAlias, ppcRecFunc->x86Code, ppcRecFunc->x86Size);
#if defined(_WIN32)
            VirtualFree(ppcRecFunc->x86Code, 0, MEM_RELEASE);
#else
            munmap(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
#endif
            ppcRecFunc->x86Code         = region.rxAlias;
            ppcRecFunc->x86CodeWritable = region.rwAlias;
            ppcRecFunc->dualMapRegion   = region;
            PPCRecompiler_flushInstructionCache(region.rxAlias, region.size);
        }
    }

    if (!s_dualMapJITEnabled)
        PPCRecompiler_reprotectAsExecutable(ppcRecFunc->x86Code, ppcRecFunc->x86Size);

#elif defined(__aarch64__)
    if (!PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext))
    {
        delete ppcRecFunc;
        return nullptr;
    }
#endif

    if (ActiveSettings::DumpRecompilerFunctionsEnabled())
    {
        const void* dumpPtr = (s_dualMapJITEnabled && ppcRecFunc->x86CodeWritable)
            ? ppcRecFunc->x86CodeWritable
            : ppcRecFunc->x86Code;
        FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(
            fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
        if (fs)
        {
            fs->writeData(dumpPtr, ppcRecFunc->x86Size);
            delete fs;
        }
    }

    entryPointsOut.clear();
    for (IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
    {
        if (!imlSegment->isEnterable)
            continue;
        entryPointsOut.emplace_back(imlSegment->enterPPCAddress, imlSegment->x64Offset);
    }

#if PPCREC_LOG_RECOMPILATION_RESULTS
    bt.Stop();
    const void* hashPtr = (s_dualMapJITEnabled && ppcRecFunc->x86CodeWritable)
        ? ppcRecFunc->x86CodeWritable
        : ppcRecFunc->x86Code;
    uint32 codeHash = 0;
    for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
    {
        codeHash = _rotr(codeHash, 3);
        codeHash += ((uint8*)hashPtr)[i];
    }
    cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> native: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}",
        (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code,
        bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

    return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
    IMLRegisterAllocatorParameters raParam;
    
    for (auto& it : ppcImlGenContext.mappedRegs)
        raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);
    
#if defined(ARCH_X86_64)
    auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
    gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);
    
    auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
    for (int i = 0; i < 15; i++)
        fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + i);
    
#elif defined(__aarch64__)
    auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
    for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
    {
        if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
            continue;
        gprPhysPool.SetAvailable(i);
    }
    
    auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
    for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
        fprPhysPool.SetAvailable(i);
#endif
    
    IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);

	return true;
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
    // update jump table
    PPCRecompilerState.recompilerSpinlock.lock();
    
    // check if the initial entrypoint is still flagged for recompilation
    // its possible that the range has been invalidated during the time it took to translate the function
    if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
    {
        PPCRecompilerState.recompilerSpinlock.unlock();
        delete ppcRecFunc;
        return false;
    }
    
    // check if the current range got invalidated during the time it took to recompile it
    bool isInvalidated = false;
    for (auto& invRange : PPCRecompilerState.invalidationRanges)
    {
        MPTR rStartAddr = invRange.startAddress;
        MPTR rEndAddr = rStartAddr + invRange.size;
        for (auto& recFuncRange : ppcRecFunc->list_ranges)
        {
            if (PPCRecompiler_rangesOverlap(recFuncRange.ppcAddress, recFuncRange.ppcAddress + recFuncRange.ppcSize, rStartAddr, rEndAddr))
            {
                isInvalidated = true;
                break;
            }
        }
    }
    PPCRecompilerState.invalidationRanges.clear();
    if (isInvalidated)
    {
        PPCRecompiler_resetVisitedMarkersForFunction(ppcRecFunc);
        PPCRecompilerState.recompilerSpinlock.unlock();
        delete ppcRecFunc;
        return false;
    }
    
    
    // update jump table
    for (auto& itr : entryPoints)
    {
        ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
    }
    
    
    // due to inlining, some entrypoints can get optimized away
    // therefore we reset all addresses that are still marked as visited (but not recompiled)
    // we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
    // if they are reachable, the interpreter will queue them again
    for (uint32 v = range.startAddress; v < (range.startAddress + range.length); v += 4)
    {
        auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
        if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
            ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
    }
    
    // register ranges
    for (auto& r : ppcRecFunc->list_ranges)
    {
        r.storedRange = rangeStore_ppcRanges.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
    }
    PPCRecompilerState.recompilerSpinlock.unlock();
    return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	PPCRecompilerState.recompilerSpinlock.lock();

	std::set<uint32> entryAddresses;

	entryAddresses.emplace(address);

	PPCRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	const auto statStart = std::chrono::steady_clock::now();
	auto func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);
	{
		const uint64 ns = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - statStart).count();
		s_statCompileNs += ns;
		uint64 prevMax = s_statCompileMaxNs.load();
		while (ns > prevMax && !s_statCompileMaxNs.compare_exchange_weak(prevMax, ns)) {}
		if (func)
			s_statCompiled++;
		else
			s_statCompileFailed++;
	}

	if (!func)
	{
		return; // recompilation failed
	}
	bool r = PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
	if (r)
		s_statActivated++;
	else
		s_statDiscarded++; // invalidated while it was being compiled
}


/*
void PPCRecompiler_thread()
{
    SetThreadName("PPCRecompiler");
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
    return;
#endif
    while (true)
    {
        if (s_recompilerThreadStopSignal)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        while (true)
        {
            PPCRecompilerState.recompilerSpinlock.lock();
            if (PPCRecompilerState.targetQueue.empty())
            {
                PPCRecompilerState.recompilerSpinlock.unlock();
                break;
            }
            auto enterAddress = PPCRecompilerState.targetQueue.front();
            PPCRecompilerState.targetQueue.pop();

            auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
            if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
            {
                PPCRecompilerState.recompilerSpinlock.unlock();
                continue;
            }
            PPCRecompilerState.recompilerSpinlock.unlock();

            PPCRecompiler_recompileAtAddress(enterAddress);
            if (s_recompilerThreadStopSignal)
                return;
        }
    }
}
 */

#define PPC_REC_ALLOC_BLOCK_SIZE    (4*1024*1024)

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p3 = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p3 )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

struct ppcRecompilerFuncRange_t
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange_t* rangesOut, size_t* countInOut)
{
	PPCRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	rangeStore_ppcRanges.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	PPCRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t * PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_invalidateTableRange(uint32 offset, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	for (uint32 i = 0; i < size / 4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset / 4 + i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
    cemu_assert_debug(PPCRecompilerState.recompilerSpinlock.is_locked());
    s_statDeleted++;
    for (auto& r : func->list_ranges)
    {
        PPCRecompiler_invalidateTableRange(r.ppcAddress, r.ppcSize);
        if (r.storedRange)
            rangeStore_ppcRanges.deleteRange(r.storedRange);
        r.storedRange = nullptr;
    }

    if (func->dualMapRegion.rwAlias)
    {
        PPCRecompiler_releaseJitArena(func->dualMapRegion);
        func->dualMapRegion   = DualMapRegion{};
        func->x86Code         = nullptr;
        func->x86CodeWritable = nullptr;
    }
}

namespace
{
void PPCRecompiler_invalidateRangeInternal(uint32 startAddr, uint32 endAddr)
{
	if (ppcRecompilerEnabled == false)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);
	s_statInvalidations++;
	s_statInvalidatedBytes += (uint64)(endAddr - startAddr);

	PPCRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;

	// mark range as unvisited
	for (uint64 currentAddr = (uint64)startAddr&~3; currentAddr < (uint64)(endAddr&~3); currentAddr += 4)
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[currentAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;

	// add entry to invalidation queue
	PPCRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);


	while (rangeStore_ppcRanges.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc) )
	{
		PPCRecompiler_deleteFunction(rFunc);
		delete rFunc;
	}

	PPCRecompilerState.recompilerSpinlock.unlock();
}
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	PPCRecompiler_invalidateRangeInternal(startAddr, endAddr);
}

void PPCRecompiler_invalidateRangeFromICBI(uint32 startAddr, uint32 endAddr)
{
	PPCRecompiler_invalidateRangeInternal(startAddr, endAddr);
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
    ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
    ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
    ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
    ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
    ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
    ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
    ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
    ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
    ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
    ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
    ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
    ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
    ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
    ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
    ppcRecompilerInstanceData->_x64XMM_singleWordMask[0] = 0xFFFFFFFFULL;
    ppcRecompilerInstanceData->_x64XMM_singleWordMask[1] = 0ULL;
    ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
    ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
    ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
    ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
    ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
    ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
    ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
    ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
    *(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
    *(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
    ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;
    ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn  = 0x1F80 | 0x8000;
    ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
}
#else
void PPCRecompiler_initPlatform() {}
#endif

static bool PPCRecompiler_readDualMapEnvVar()
{
    int envValue = PPCRecompiler_readBoolEnvVar("DUAL_MAPPED_JIT");
    if (envValue != -1)
    {
        return envValue == 1;
    }

    return PPCRecompiler_shouldUseDualMapByDefault();
}

bool PPCRecompilerInitialized()
{
    return ppcRecompilerInited;
}

bool PPCRecompiler_Init26() {
    if (!checkDebugged() && ActiveSettings::GetCPUMode() == CPUMode::Auto) {
        cemuLog_log(LogType::Force, "Debugger not attached, JIT cannot continue.");
        ppcRecompilerEnabled = false;
        PPCCore_InitializePointer(ppcRecompilerEnabled);
        return false;
    }
    
    s_dualMapJITEnabled = PPCRecompiler_readDualMapEnvVar();
    
    if (!s_dualMapJITEnabled)
        return true;
    
    if (s_jitArena.region.rxAlias)
        return true;
    
    try
    {
        // The jump table is one contiguous ~512MiB reservation. On devices with little address space left it has to be reserved
        // BEFORE the JIT arena (rx+rw alias), which would otherwise take the large free blocks and make this reservation fail.
        if (ppcRecompilerInstanceData)
        {
            MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
            ppcRecompilerInstanceData = nullptr;
        }
        ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
        if (!ppcRecompilerInstanceData)
        {
            cemuLog_log(LogType::Force, "Recompiler: unable to reserve {} MiB for the jump table", sizeof(PPCRecompilerInstanceData_t) >> 20);
            memory_logFreeAddressSpace("jump table reservation failed");
            return false;
        }
        if (!MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true))
        {
            cemuLog_log(LogType::Force, "Recompiler: unable to commit the recompiler instance data");
            return false;
        }
        memory_logFreeAddressSpace("after jump table reservation");

        if (!s_jitArena.initWithFallback(1024 * 1024 * 1024, 64 * 1024 * 1024))
        {
            cemuLog_log(LogType::Force, "JIT arena allocation failed, disabling JIT");
            ppcRecompilerEnabled = false;
            ppcRecompilerInited = false;
        }
        else
        {
            g_jitArenaRxBase = s_jitArena.region.rxAlias;
            g_jitArenaRxEnd  = (uint8*)s_jitArena.region.rxAlias + s_jitArena.region.size;
            g_jitArenaRwBase = s_jitArena.region.rwAlias;
            ppcRecompilerInited = true;
            memory_logFreeAddressSpace("after JIT arena allocation");
            PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
            s_ifaceEnter = (void*)PPCRecompiler_enterRecompilerCode;
            s_ifaceLeaveUnvisited = (void*)PPCRecompiler_leaveRecompilerCode_unvisited;
            s_ifaceLeaveVisited = (void*)PPCRecompiler_leaveRecompilerCode_visited;
            cemuLog_log(LogType::Force, "JIT interface functions: enter={:p} leave={:p} leaveVisited={:p}", s_ifaceEnter, s_ifaceLeaveUnvisited, s_ifaceLeaveVisited);
        }
    }
    catch (const std::exception& e)
    {
        if (ActiveSettings::GetCPUMode() == CPUMode::Auto) {
            PPCCore_InitializePointer(false);
            ppcRecompilerEnabled = false;
        } else { throw e; }
    }
    
    PPCRecompiler_finishJitMappingSession();
    
    return ppcRecompilerInited;
}


// ---- periodic statistics ----
#if defined(__APPLE__)
// Samples the program counter and a short frame-pointer backtrace of every thread. Used when the emulator stops making progress.
// Nothing is allocated or logged while a thread is suspended (the suspended thread could hold the malloc or log lock).
struct PPCRecompilerThreadSample
{
    char name[40];
    uint64 frames[10];
    uint32 numFrames;
};

static void PPCRecompiler_dumpAllThreads(const char* reason)
{
    static PPCRecompilerThreadSample samples[96];
    uint32 numSamples = 0;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return;
    const mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count && numSamples < 96; i++)
    {
        thread_t t = threads[i];
        if (t == self)
            continue;
        PPCRecompilerThreadSample& sample = samples[numSamples];
        sample.name[0] = '\0';
        sample.numFrames = 0;
        uintptr_t stackLow = 0, stackHigh = 0;
        pthread_t pt = pthread_from_mach_thread_np(t);
        if (pt)
        {
            pthread_getname_np(pt, sample.name, sizeof(sample.name));
            stackHigh = (uintptr_t)pthread_get_stackaddr_np(pt);
            stackLow = stackHigh - pthread_get_stacksize_np(pt);
        }
        if (thread_suspend(t) != KERN_SUCCESS)
            continue;
        arm_thread_state64_t state;
        mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount) == KERN_SUCCESS)
        {
            sample.frames[sample.numFrames++] = (uint64)arm_thread_state64_get_pc(state);
            sample.frames[sample.numFrames++] = (uint64)arm_thread_state64_get_lr(state);
            uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);
            while (sample.numFrames < 10 && pt && fp >= stackLow && fp + 16 <= stackHigh && (fp & 15) == 0)
            {
                const uintptr_t* frame = (const uintptr_t*)fp;
                sample.frames[sample.numFrames++] = (uint64)frame[1];
                const uintptr_t next = frame[0];
                if (next <= fp)
                    break;
                fp = next;
            }
        }
        thread_resume(t);
        numSamples++;
    }
    for (mach_msg_type_number_t i = 0; i < count; i++)
        mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), self);

    cemuLog_log(LogType::Force, "[Thread dump] {} ({} threads)", reason, numSamples);
    for (uint32 i = 0; i < numSamples; i++)
    {
        std::string line;
        for (uint32 f = 0; f < samples[i].numFrames; f++)
        {
            const uint64 addr = samples[i].frames[f];
            Dl_info info;
            if (addr >= (uint64)g_jitArenaRxBase && addr < (uint64)g_jitArenaRxEnd)
                line += fmt::format(" [JIT {:#x}]", addr - (uint64)g_jitArenaRxBase);
            else if (dladdr((void*)addr, &info) && info.dli_sname)
                line += fmt::format(" {}+{}", info.dli_sname, addr - (uint64)info.dli_saddr);
            else
                line += fmt::format(" {:#x}", addr);
        }
        cemuLog_log(LogType::Force, "  [{}]{}", samples[i].name[0] ? samples[i].name : "?", line);
    }
}
#endif

static std::thread s_statsThread;
static std::atomic<bool> s_statsStop{false};

static void PPCRecompiler_getMemoryStats(uint64& footprintMB, uint64& availableMB)
{
    footprintMB = 0;
    availableMB = 0;
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        footprintMB = (uint64)info.phys_footprint >> 20;
#if BOOST_OS_IOS
    availableMB = (uint64)os_proc_available_memory() >> 20;
#endif
#endif
}

static void PPCRecompiler_statsThreadFunc()
{
    uint64 lastCompiled = 0, lastInval = 0, lastNs = 0;
    uint32 lastFrames = LatteGPUState.frameCounter;
    uint32 stalledIntervals = 0, threadDumps = 0;
    auto lastTime = std::chrono::steady_clock::now();
    while (!s_statsStop.load())
    {
        for (int i = 0; i < 50 && !s_statsStop.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (s_statsStop.load())
            break;
        size_t queued;
        PPCRecompilerState.recompilerSpinlock.lock();
        queued = PPCRecompilerState.targetQueue.size();
        PPCRecompilerState.recompilerSpinlock.unlock();
        const uint64 compiled = s_statCompiled.load(), inval = s_statInvalidations.load(), ns = s_statCompileNs.load();
        uint64 footprintMB, availableMB;
        PPCRecompiler_getMemoryStats(footprintMB, availableMB);
        const size_t arenaTotal = s_jitArena.region.size, arenaFree = s_jitArena.freeBytes();
        const uint32 frames = LatteGPUState.frameCounter;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - lastTime).count();
        const double fps = seconds > 0.0 ? (double)(frames - lastFrames) / seconds : 0.0;
        lastFrames = frames;
        lastTime = now;
        if (s_ifaceEnter && ((void*)PPCRecompiler_enterRecompilerCode != s_ifaceEnter || (void*)PPCRecompiler_leaveRecompilerCode_unvisited != s_ifaceLeaveUnvisited || (void*)PPCRecompiler_leaveRecompilerCode_visited != s_ifaceLeaveVisited))
            cemuLog_log(LogType::Force, "[JIT] ERROR: interface function pointers changed! enter={:p} (was {:p}) leave={:p} (was {:p})", (void*)PPCRecompiler_enterRecompilerCode, s_ifaceEnter, (void*)PPCRecompiler_leaveRecompilerCode_unvisited, s_ifaceLeaveUnvisited);
        stalledIntervals = (fps < 0.5) ? stalledIntervals + 1 : 0;
        cemuLog_log(LogType::Force, "[JIT stats] fps {:.1f} | compiled {} (+{} in 5s, {:.1f}ms avg, {:.0f}ms max) failed {} discarded {} deleted {} | invalidations {} (+{}, {} KiB) | queue {} | arena {}/{} MiB used{} | mem footprint {} MiB, available {} MiB",
            fps, compiled, compiled - lastCompiled, compiled ? (double)ns / compiled / 1e6 : 0.0, (double)s_statCompileMaxNs.load() / 1e6,
            s_statCompileFailed.load(), s_statDiscarded.load(), s_statDeleted.load(),
            inval, inval - lastInval, s_statInvalidatedBytes.load() >> 10, queued,
            (arenaTotal - arenaFree) >> 20, arenaTotal >> 20, s_jitArenaFull.load() ? " FULL" : "", footprintMB, availableMB);
        lastCompiled = compiled; lastInval = inval; lastNs = ns;
#if defined(__APPLE__)
        // No frame was presented for 10+ seconds: record what every thread is doing so freezes can be diagnosed from the log
        if (stalledIntervals >= 2 && threadDumps < 4 && (stalledIntervals - 2) % 6 == 0)
        {
            threadDumps++;
            PPCRecompiler_dumpAllThreads("no frames presented for 10s+");
        }
#endif
    }
}

static void PPCRecompiler_startStatsThread()
{
    if (s_statsThread.joinable())
        return;
    s_statsStop = false;
    s_statsThread = std::thread(PPCRecompiler_statsThreadFunc);
}

static void PPCRecompiler_stopStatsThread()
{
    s_statsStop = true;
    if (s_statsThread.joinable())
        s_statsThread.join();
}

void PPCRecompiler_init()
{
    s_recompilerEnableCount = 0;
    // PPCRecompiler_notifyWorkers();
    if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter || ActiveSettings::GetCPUMode() == CPUMode::MulticoreInterpreter)
    {
        ppcRecompilerEnabled = false;
        PPCCore_InitializePointer(ppcRecompilerEnabled);
        return;
    }
    if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
    {
        cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
        ppcRecompilerEnabled = false;
        PPCCore_InitializePointer(false);
        return;
    }

#if !defined(__aarch64__)
    // the x64 backend addresses guest memory as memory_base + address and has no page table support
    if (memory_isPageTableMode())
    {
        cemuLog_log(LogType::Force, "Recompiler disabled. Guest memory runs in page table mode which this recompiler backend does not support");
        ppcRecompilerEnabled = false;
        PPCCore_InitializePointer(false);
        return;
    }
#endif
    
#if defined(__APPLE__) && BOOST_OS_IOS
    if (!checkDebugged() && ActiveSettings::GetCPUMode() == CPUMode::Auto) {
        cemuLog_log(LogType::Force, "Debugger not attached, JIT cannot continue.");
        ppcRecompilerEnabled = false;
        PPCCore_InitializePointer(ppcRecompilerEnabled);
        return;
    }
#endif
    
    
    s_dualMapJITEnabled = PPCRecompiler_readDualMapEnvVar();
    s_syncCompile = PPCRecompiler_readBoolEnvVar("MELOCAFE_JIT_SYNC") == 1 || PPCREC_FORCE_SYNCHRONOUS_COMPILATION;
    cemuLog_log(LogType::Force, "Recompiler: compiling on {}", s_syncCompile ? "the emulation threads (synchronous)" : "worker threads");
    
#if defined(__APPLE__) && defined(__aarch64__)
    const int appleOsMajorVersion = PPCRecompiler_getAppleOsMajorVersion();
    if (s_dualMapJITEnabled)
    {
        if (appleOsMajorVersion >= 26)
        {
            cemuLog_log(LogType::Force, PPCRecompiler_readTXMEnvVar()
                        ? "Recompiler: using iOS 26+ dual-mapped JIT via TXM/vm_remap"
                        : "Recompiler: using iOS 26+ dual-mapped JIT via vm_remap");
        }
        else
        {
            cemuLog_log(LogType::Force, "Recompiler: dual-mapped JIT enabled");
        }
    }
    else if (appleOsMajorVersion > 0 && appleOsMajorVersion <= 18)
    {
        cemuLog_log(LogType::Force, "Recompiler: using W^X for iOS {}", appleOsMajorVersion);
    }
    else
#endif
    {
        cemuLog_log(LogType::Force, s_dualMapJITEnabled
                    ? "Recompiler: dual-mapped JIT enabled"
                    : "Recompiler: using W^X");
    }
    
    bool init26 = false;
    if (s_dualMapJITEnabled && !ppcRecompilerInited)
    {
        init26 = PPCRecompiler_Init26();
        if (!init26)
        {
            // Without the JIT arena no function can be placed, so the recompiler would only burn CPU generating code it cannot use. Stay on the interpreter.
            cemuLog_log(LogType::Force, "Recompiler unavailable (JIT arena could not be set up), using the interpreter");
            memory_logFreeAddressSpace("after JIT arena failure");
            ppcRecompilerEnabled = false;
            PPCCore_InitializePointer(false);
            return;
        }
    }
    
    if (!init26) {
        if (ppcRecompilerInstanceData)
        {
            MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
            ppcRecompilerInstanceData = nullptr;
        }
        
        debug_printf("Allocating %dMB for recompiler instance data...\n", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
        ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
        MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
        PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
        PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    }
    
    
    PPCRecompiler_allocateRange(0, 0x1000);
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());
    
    PPCRecompiler_initPlatform();
    
    cemuLog_log(LogType::Force, "Recompiler initialized");
    ppcRecompilerEnabled = true;
    s_recompilerEnableCount = 1;
    PPCCore_InitializePointer(ppcRecompilerEnabled);
    ppcRecompilerInited = true;
    
    if (!s_syncCompile)
        s_threadPool.Start();
    PPCRecompiler_startStatsThread();
    
    PPCRecompiler_finishJitMappingSession();

}

void PPCRecompiler_Shutdown()
{
    PPCRecompiler_stopStatsThread();
    s_threadPool.Stop();

    while (!PPCRecompilerState.targetQueue.empty())
        PPCRecompilerState.targetQueue.pop();
    PPCRecompilerState.invalidationRanges.clear();
    {
        MPTR rStart, rEnd;
        PPCRecFunction_t* rFunc;
        PPCRecompilerState.recompilerSpinlock.lock();
        while (rangeStore_ppcRanges.findFirstRange(PPC_REC_CODE_AREA_START, PPC_REC_CODE_AREA_END, rStart, rEnd, rFunc))
        {
            PPCRecompiler_deleteFunction(rFunc);
            delete rFunc;
        }
        PPCRecompilerState.recompilerSpinlock.unlock();
    }
    rangeStore_ppcRanges.clear();

    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for (uint32 i = 0; i < numBlocks; i++)
    {
        if (!ppcRecompiler_reservedBlockMask[i])
            continue;
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        ppcRecompiler_reservedBlockMask[i] = false;
    }

    s_jitArena.reset();
    ppcRecompilerEnabled = false;
    ppcRecompilerInited = false;
    s_recompilerEnableCount = 0;
    s_dualMapJITEnabled = false;
}
