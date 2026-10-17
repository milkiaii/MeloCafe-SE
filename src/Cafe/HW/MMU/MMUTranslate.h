#pragma once

// Guest <-> host address translation for the emulated Wii U address space.
//
// Linear mode (default):
//     host = memory_base + guest
//   Requires one contiguous 4GiB host reservation.
//
// Page table mode (fallback for devices that cannot provide the 4GiB reservation, e.g. 3GB iPhones):
//     host = g_memoryPageTable[guest >> PAGE_SHIFT] + (guest & PAGE_MASK)
//   The guest ranges that actually exist (~1.9GiB) are packed into one smaller host arena. The
//   arena keeps guest order, and guest ranges that touch each other stay contiguous on the host, so
//   a buffer inside a guest range (or across two adjacent ones) is still contiguous host memory.
//   A guard granule follows every packed segment so a one-past-the-end host pointer still maps back
//   to the guest address one past the end.

extern uint8* memory_base; // linear mode: origin of the guest address space. page table mode: start of the packed arena

namespace MMUTranslate
{
	constexpr uint32 PAGE_SHIFT = 14; // 16KiB, matches the iOS arm64 VM page size. Must stay >= the host page size.
	constexpr uint32 PAGE_SIZE = 1u << PAGE_SHIFT;
	constexpr uint32 PAGE_MASK = PAGE_SIZE - 1;
	constexpr uint32 NUM_PAGES = 1u << (32 - PAGE_SHIFT);
	constexpr uint32 REVERSE_UNMAPPED = 0xFFFFFFFFu;
}

// Forward table: one host page pointer per guest page. nullptr while in linear mode.
// Guest pages that are not backed point at the arena's PROT_NONE guard page so a bad access faults like it does in linear mode.
extern uint8** g_memoryPageTable;
// Reverse table: one guest page index per host arena page (REVERSE_UNMAPPED for padding). nullptr while in linear mode.
extern uint32* g_memoryReverseTable;
extern uint8* g_memoryArenaBase;
extern size_t g_memoryArenaSize;

inline bool memory_isPageTableMode()
{
	return g_memoryPageTable != nullptr;
}

// guest address -> host pointer
inline uint8* memory_translate(uint32 guestAddress)
{
	if (g_memoryPageTable != nullptr) [[unlikely]]
		return g_memoryPageTable[guestAddress >> MMUTranslate::PAGE_SHIFT] + (guestAddress & MMUTranslate::PAGE_MASK);
	return memory_base + guestAddress;
}

// host pointer -> guest address. Returns 0 for pointers outside of guest memory in page table mode.
inline uint32 memory_reverseTranslate(const void* hostPtr)
{
	if (g_memoryReverseTable != nullptr) [[unlikely]]
	{
		const uint8* p = (const uint8*)hostPtr;
		// unsigned compare also rejects p < arena base. size includes the trailing guard granule, so one-past-the-end is accepted
		const size_t offset = (size_t)(p - g_memoryArenaBase);
		if (offset >= g_memoryArenaSize)
			return 0;
		const uint32 guestPage = g_memoryReverseTable[offset >> MMUTranslate::PAGE_SHIFT];
		if (guestPage == MMUTranslate::REVERSE_UNMAPPED)
			return 0;
		return (guestPage << MMUTranslate::PAGE_SHIFT) | (uint32)(offset & MMUTranslate::PAGE_MASK);
	}
	return (uint32)((uintptr_t)hostPtr - (uintptr_t)memory_base);
}

// true if the host pointer lies inside the host memory that backs the guest address space
inline bool memory_isHostPtrInGuestSpace(const void* hostPtr)
{
	const uint8* p = (const uint8*)hostPtr;
	if (g_memoryPageTable != nullptr)
		return (size_t)(p - g_memoryArenaBase) < g_memoryArenaSize;
	return p >= memory_base && p < (memory_base + 0x100000000ULL);
}
