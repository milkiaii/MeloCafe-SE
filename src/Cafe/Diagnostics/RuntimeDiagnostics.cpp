#include "Cafe/Diagnostics/RuntimeDiagnostics.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Common/precompiled.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#if __has_include(<os/proc.h>)
#include <os/proc.h>
#endif

namespace
{
std::thread s_thread;
std::atomic<bool> s_stop{false};
dispatch_source_t s_pressureSource = nullptr;

struct MemInfo
{
	uint64 footprintMB = 0;
	uint64 availableMB = 0;
};

MemInfo GetMemInfo()
{
	MemInfo m;
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
		m.footprintMB = (uint64)info.phys_footprint >> 20;
#if BOOST_OS_IOS
	m.availableMB = (uint64)os_proc_available_memory() >> 20;
#endif
	return m;
}

constexpr int MAX_THREADS = 64;
constexpr int MAX_FRAMES = 10;

struct ThreadSnapshot
{
	bool valid = false;
	thread_t port = 0;
	int runState = 0;
	int cpuUsage = 0;
	int numFrames = 0;
	uint64 frames[MAX_FRAMES];
};

// reads up to MAX_FRAMES return addresses by following the frame pointer chain. Memory is read with mach_vm_read_overwrite so a bad pointer cannot crash us
void CaptureThread(thread_t t, ThreadSnapshot& snap)
{
	snap.port = t;
	thread_basic_info_data_t bi;
	mach_msg_type_number_t biCount = THREAD_BASIC_INFO_COUNT;
	if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &biCount) == KERN_SUCCESS)
	{
		snap.runState = bi.run_state;
		snap.cpuUsage = bi.cpu_usage;
	}
#if defined(__aarch64__)
	// no allocation or logging between suspend and resume
	if (thread_suspend(t) != KERN_SUCCESS)
		return;
	arm_thread_state64_t st;
	mach_msg_type_number_t stCount = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &stCount) == KERN_SUCCESS)
	{
		snap.valid = true;
		uint64 pc = arm_thread_state64_get_pc(st);
		uint64 fp = arm_thread_state64_get_fp(st);
		uint64 lr = arm_thread_state64_get_lr(st);
		snap.frames[snap.numFrames++] = pc;
		if (lr)
			snap.frames[snap.numFrames++] = lr;
		uint64 prevFp = 0;
		while (snap.numFrames < MAX_FRAMES && fp && (fp & 0xF) == 0 && fp > prevFp)
		{
			uint64 pair[2] = {0, 0};
			mach_vm_size_t outSize = 0;
			if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)fp, sizeof(pair), (mach_vm_address_t)pair, &outSize) != KERN_SUCCESS || outSize != sizeof(pair))
				break;
			if (!pair[1])
				break;
			snap.frames[snap.numFrames++] = pair[1];
			prevFp = fp;
			fp = pair[0];
		}
	}
	thread_resume(t);
#endif
}

const char* RunStateName(int s)
{
	switch (s)
	{
	case TH_STATE_RUNNING: return "running";
	case TH_STATE_STOPPED: return "stopped";
	case TH_STATE_WAITING: return "waiting";
	case TH_STATE_UNINTERRUPTIBLE: return "uninterruptible";
	case TH_STATE_HALTED: return "halted";
	}
	return "?";
}

std::string Symbolize(uint64 addr)
{
	Dl_info di;
	if (dladdr((void*)addr, &di) && di.dli_fname)
	{
		const char* image = strrchr(di.dli_fname, '/');
		image = image ? image + 1 : di.dli_fname;
		if (di.dli_sname)
			return fmt::format("{}!{}+{:#x}", image, di.dli_sname, addr - (uint64)di.dli_saddr);
		return fmt::format("{}+{:#x}", image, addr - (uint64)di.dli_fbase);
	}
	return fmt::format("{:#x}", addr); // unknown, for example JIT code
}

void DumpThreads(const char* reason)
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t threadCount = 0;
	if (task_threads(mach_task_self(), &threads, &threadCount) != KERN_SUCCESS)
		return;
	static ThreadSnapshot snaps[MAX_THREADS];
	const thread_t self = mach_thread_self();
	const mach_msg_type_number_t n = std::min<mach_msg_type_number_t>(threadCount, MAX_THREADS);
	for (mach_msg_type_number_t i = 0; i < n; i++)
	{
		snaps[i] = ThreadSnapshot();
		if (threads[i] == self)
			continue;
		CaptureThread(threads[i], snaps[i]);
	}
	cemuLog_log(LogType::Force, "[diag] ---- thread snapshot ({}), {} threads ----", reason, threadCount);
	for (mach_msg_type_number_t i = 0; i < n; i++)
	{
		if (threads[i] == self)
			continue;
		char name[64] = {};
		pthread_t pt = pthread_from_mach_thread_np(threads[i]);
		if (pt)
			pthread_getname_np(pt, name, sizeof(name));
		std::string line = fmt::format("[diag] thread {} '{}' {} cpu {}%:", threads[i], name, RunStateName(snaps[i].runState), snaps[i].cpuUsage / 10);
		for (int f = 0; f < snaps[i].numFrames; f++)
			line += fmt::format("\n[diag]     {}", Symbolize(snaps[i].frames[f]));
		cemuLog_log(LogType::Force, "{}", line);
	}
	for (mach_msg_type_number_t i = 0; i < threadCount; i++)
		mach_port_deallocate(mach_task_self(), threads[i]);
	mach_port_deallocate(mach_task_self(), self);
	vm_deallocate(mach_task_self(), (vm_address_t)threads, threadCount * sizeof(thread_t));
}

void ThreadFunc()
{
	pthread_setname_np("RuntimeDiagnostics");
	uint64 lastFrames = g_latteFramesEnded.load();
	uint32 secondsWithoutFrame = 0;
	bool stallDumped = false;
	uint32 fpsHistory[5] = {};
	uint32 tick = 0;
	uint64 lastFootprintMB = GetMemInfo().footprintMB;
	uint32 lowMemLogs = 0;
	while (!s_stop.load())
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (s_stop.load())
			break;
		const uint64 frames = g_latteFramesEnded.load();
		const uint32 fps = (uint32)(frames - lastFrames);
		lastFrames = frames;
		fpsHistory[tick % 5] = fps;
		tick++;

		const MemInfo mem = GetMemInfo();
		const bool lowMemory = mem.availableMB != 0 && mem.availableMB < 450;
		const bool bigJump = mem.footprintMB > lastFootprintMB + 120;
		if ((lowMemory && lowMemLogs < 30) || bigJump)
		{
			cemuLog_log(LogType::Force, "[diag] memory {}: footprint {} MiB (was {}), available {} MiB, fps {}",
				lowMemory ? "LOW" : "jump", mem.footprintMB, lastFootprintMB, mem.availableMB, fps);
			if (lowMemory)
				lowMemLogs++;
		}
		lastFootprintMB = mem.footprintMB;

		if (tick % 5 == 0)
		{
			cemuLog_log(LogType::Force, "[diag] fps (last 5s) {} {} {} {} {} | footprint {} MiB, available {} MiB",
				fpsHistory[0], fpsHistory[1], fpsHistory[2], fpsHistory[3], fpsHistory[4], mem.footprintMB, mem.availableMB);
		}

		if (fps == 0 && frames != 0)
		{
			secondsWithoutFrame++;
			if (secondsWithoutFrame == 3 || (secondsWithoutFrame > 3 && secondsWithoutFrame % 15 == 0))
			{
				cemuLog_log(LogType::Force, "[diag] no frame presented for {}s (footprint {} MiB, available {} MiB)", secondsWithoutFrame, mem.footprintMB, mem.availableMB);
				DumpThreads(stallDumped ? "still stalled" : "stall");
				stallDumped = true;
			}
		}
		else
		{
			if (stallDumped)
				cemuLog_log(LogType::Force, "[diag] frames are being presented again after {}s", secondsWithoutFrame);
			secondsWithoutFrame = 0;
			stallDumped = false;
		}
	}
}
} // namespace

void RuntimeDiagnostics_Start()
{
	if (s_thread.joinable())
		return;
	s_stop = false;
	if (!s_pressureSource)
	{
		s_pressureSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
			DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_NORMAL,
			dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		if (s_pressureSource)
		{
			dispatch_source_set_event_handler(s_pressureSource, ^{
				const auto flags = dispatch_source_get_data(s_pressureSource);
				const MemInfo m = GetMemInfo();
				cemuLog_log(LogType::Force, "[diag] MEMORY PRESSURE event: {}{}{} footprint {} MiB, available {} MiB",
					(flags & DISPATCH_MEMORYPRESSURE_CRITICAL) ? "CRITICAL " : "", (flags & DISPATCH_MEMORYPRESSURE_WARN) ? "WARN " : "",
					(flags & DISPATCH_MEMORYPRESSURE_NORMAL) ? "normal " : "", m.footprintMB, m.availableMB);
			});
			dispatch_resume(s_pressureSource);
		}
	}
	s_thread = std::thread(ThreadFunc);
}

void RuntimeDiagnostics_Stop()
{
	s_stop = true;
	if (s_thread.joinable())
		s_thread.join();
}

#else
void RuntimeDiagnostics_Start() {}
void RuntimeDiagnostics_Stop() {}
#endif
