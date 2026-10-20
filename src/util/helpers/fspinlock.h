#pragma once

// minimal but efficient non-recursive spinlock implementation

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

static inline void cemuSpinlockPause()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
	_mm_pause();
#elif defined(__aarch64__) || defined(__arm64__)
	asm volatile("yield");
#else
	std::this_thread::yield();
#endif
}

class FSpinlock
{
public:
	bool is_locked() const
	{
		return m_lockBool.load(std::memory_order_relaxed);
	}

	// implement BasicLockable and Lockable
	void lock() const
	{
		while (true)
		{
			if (!m_lockBool.exchange(true, std::memory_order_acquire))
				break;
			uint32_t spins = 0;
			while (m_lockBool.load(std::memory_order_relaxed))
			{
				cemuSpinlockPause();
#if defined(__APPLE__)
				// iOS gives no priority donation to the thread that holds a spinlock. If the holder is descheduled, a pure spin wait can
				// starve it for a very long time, so hand the CPU back after a short spin.
				if (++spins >= 256)
				{
					std::this_thread::yield();
					spins = 0;
				}
#endif
			}
		}
	}

	bool try_lock() const
	{
		return !m_lockBool.exchange(true, std::memory_order_acquire);
	}

	void unlock() const
	{
		m_lockBool.store(false, std::memory_order_release);
	}

private:
	
	mutable std::atomic<bool> m_lockBool = false;
};
