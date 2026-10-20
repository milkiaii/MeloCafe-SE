#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "util/MemMapper/MemMapper.h"
#include "config/ActiveSettings.h"
#include <algorithm>
#include <vector>
#if BOOST_OS_IOS
#include "util/MemMapper/SparseReservation.h"
#endif

uint8* memory_base = NULL; // origin of the guest address space (gaps may be unreserved)
uint8* memory_elfCodeArena = NULL;

#if BOOST_OS_IOS
static MemMapper::SparseReservation s_sparseReservation;
static bool s_usingSparseReservation = false;
static uintptr_t s_failedSparseAddress = 0;
static size_t s_failedSparseSize = 0;

static void memory_releaseSparseRange(uintptr_t address, size_t size)
{
	MemMapper::FreeReservation(reinterpret_cast<void*>(address), size);
}

static bool memory_reserveSparseRange(uintptr_t base, uint64_t offset, uint64_t size)
{
	return s_sparseReservation.Ensure(base, offset, size, MemMapper::GetPageSize(),
		[](uintptr_t address, size_t length) {
			if (MemMapper::ReserveMemory(reinterpret_cast<void*>(address), length,
				MemMapper::PAGE_PERMISSION::P_NONE) == reinterpret_cast<void*>(address))
				return true;
			s_failedSparseAddress = address;
			s_failedSparseSize = length;
			return false;
		}, memory_releaseSparseRange);
}
#endif

// ---------------------------------------------------------------------------------------------
// Page table mode
//
// Fallback for hosts that cannot hand out the contiguous 4GiB guest reservation (for example iPhones with 3GB of RAM).
// Only the guest ranges that exist are reserved, packed back to back into one smaller arena. Guest accesses go through
// g_memoryPageTable (see MMUTranslate.h) instead of memory_base + address.
// ---------------------------------------------------------------------------------------------
extern std::vector<struct MMURange*> g_mmuRanges; // defined further down

uint8** g_memoryPageTable = nullptr;
MMUTranslate::Arena g_memoryArenas[MMUTranslate::MAX_ARENAS];
uint32 g_memoryArenaCount = 0;
uint8* g_memoryArenaBase = nullptr;

static std::vector<std::vector<uint32>> s_pageTableBackingReverse; // owns the reverse tables (one per arena)
static std::vector<uint8*> s_pageTableBackingForward; // owns the forward table storage

// MELOCAFE_PAGETABLE=1 forces page table mode, =0 disables the automatic fallback, unset = automatic fallback only
static int memory_getPageTableOverride()
{
	const char* value = getenv("MELOCAFE_PAGETABLE");
	if (!value || !*value)
		return -1;
	return atoi(value) != 0 ? 1 : 0;
}

// mergeAdjacentRanges: keep guest ranges that touch each other contiguous on the host (safest). Without it only overlapping ranges are merged,
// which allows reservations as small as the biggest single range (MEM2, 1GiB) but a guest access that straddles two ranges would fault.
static bool memory_buildPageTable(bool includeOptionalRanges, bool mergeAdjacentRanges = true)
{
	using namespace MMUTranslate;
	struct Interval
	{
		uint64 begin;
		uint64 end;
	};
	std::vector<Interval> intervals;
	auto addInterval = [&](uint64 begin, uint64 end)
	{
		begin &= ~(uint64)PAGE_MASK;
		end = (end + PAGE_MASK) & ~(uint64)PAGE_MASK;
		if (end > begin)
			intervals.push_back({begin, end});
	};
	for (auto* range : g_mmuRanges)
	{
		if (range->isOptional() && !includeOptionalRanges)
			continue;
		addInterval(range->getBase(), (uint64)range->getBase() + range->getInitSize());
	}
	addInterval(0x00800000, 0x01000000); // HBL ELF code arena, see memory_enableHBLELFCodeArea()

	std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.begin < b.begin; });
	std::vector<Interval> segments;
	for (const auto& itr : intervals)
	{
		if (!segments.empty() && (mergeAdjacentRanges ? itr.begin <= segments.back().end : itr.begin < segments.back().end))
			segments.back().end = std::max(segments.back().end, itr.end);
		else
			segments.push_back(itr);
	}

	// Every segment stays contiguous on the host (a guest range is always one block of host memory). Segments are packed into one arena
	// if the address space allows it, otherwise they are distributed over several smaller reservations. A guard granule follows every
	// segment so a one-past-the-end host pointer still maps back to the guest address one past the end.
	// Arena layout: [fault granule (first arena only)][segment][guard][segment][guard]...
	std::vector<size_t> order(segments.size()); // segment indices, biggest first
	for (size_t i = 0; i < order.size(); i++)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return (segments[a].end - segments[a].begin) > (segments[b].end - segments[b].begin); });

	auto segSize = [&](size_t i) { return (size_t)(segments[i].end - segments[i].begin); };
	std::vector<std::vector<std::vector<size_t>>> strategies; // strategy -> arena -> segment indices
	strategies.push_back({order}); // everything in a single arena
	if (order.size() > 1)
	{
		std::vector<size_t> rest(order.begin() + 1, order.end());
		strategies.push_back({rest, {order[0]}}); // biggest segment (MEM2) in its own arena
	}
	{
		std::vector<std::vector<size_t>> perSegment;
		for (size_t i : order)
			perSegment.push_back({i});
		if (perSegment.size() <= MAX_ARENAS)
			strategies.push_back(perSegment);
	}

	for (size_t strategyIndex = 0; strategyIndex < strategies.size(); strategyIndex++)
	{
		auto groups = strategies[strategyIndex];
		auto groupSize = [&](const std::vector<size_t>& g) { size_t total = 0; for (size_t i : g) total += segSize(i) + PAGE_SIZE; return total; };
		std::sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) { return groupSize(a) > groupSize(b); });
		if (groups.size() > MAX_ARENAS)
			continue;

		std::vector<uint8*> bases;
		std::vector<size_t> sizes;
		bool ok = true;
		for (size_t g = 0; g < groups.size(); g++)
		{
			const size_t arenaSize = groupSize(groups[g]) + (g == 0 ? PAGE_SIZE : 0);
			uint8* base = (uint8*)MemMapper::ReserveMemory(nullptr, arenaSize, MemMapper::PAGE_PERMISSION::P_NONE);
			if (!base)
			{
				cemuLog_log(LogType::Force, "Page table mode: unable to reserve a {} MiB arena ({} arena layout)", arenaSize >> 20, groups.size());
				ok = false;
				break;
			}
			bases.push_back(base);
			sizes.push_back(arenaSize);
		}
		if (!ok)
		{
			for (size_t g = 0; g < bases.size(); g++)
				MemMapper::FreeReservation(bases[g], sizes[g]);
			continue;
		}

		uint8* faultGranule = bases[0];
		s_pageTableBackingForward.assign(NUM_PAGES, faultGranule); // everything unbacked points at the PROT_NONE fault granule
		s_pageTableBackingReverse.assign(groups.size(), {});
		for (size_t g = 0; g < groups.size(); g++)
		{
			s_pageTableBackingReverse[g].assign(sizes[g] >> PAGE_SHIFT, REVERSE_UNMAPPED);
			size_t hostOffset = (g == 0) ? PAGE_SIZE : 0;
			// keep guest order inside an arena so that neighbouring segments stay in guest order on the host
			std::vector<size_t> members = groups[g];
			std::sort(members.begin(), members.end());
			for (size_t segIndex : members)
			{
				const Interval& seg = segments[segIndex];
				const uint32 firstPage = (uint32)(seg.begin >> PAGE_SHIFT);
				const uint32 endPage = (uint32)(seg.end >> PAGE_SHIFT); // wraps to 0 for a segment ending at 4GiB
				const uint32 pageCount = (uint32)((seg.end - seg.begin) >> PAGE_SHIFT);
				const size_t hostPage = hostOffset >> PAGE_SHIFT;
				for (uint32 p = 0; p < pageCount; p++)
				{
					s_pageTableBackingForward[firstPage + p] = bases[g] + hostOffset + ((size_t)p << PAGE_SHIFT);
					s_pageTableBackingReverse[g][hostPage + p] = firstPage + p;
				}
				s_pageTableBackingReverse[g][hostPage + pageCount] = endPage; // guard granule
				hostOffset += (size_t)(seg.end - seg.begin) + PAGE_SIZE;
			}
			g_memoryArenas[g] = {bases[g], sizes[g], s_pageTableBackingReverse[g].data()};
		}
		g_memoryArenaBase = faultGranule;
		g_memoryArenaCount = (uint32)groups.size();
		g_memoryPageTable = s_pageTableBackingForward.data(); // enables page table mode, keep last
		memory_base = faultGranule; // only a non-null marker in this mode. Guest addresses must never be added to it
		size_t totalSize = 0;
		for (size_t sz : sizes)
			totalSize += sz;
		cemuLog_log(LogType::Force, "Page table mode: {} guest segments packed into {} arena(s), {} MiB total, first arena at {:#x} (4GiB reservation not used)", segments.size(), groups.size(), totalSize >> 20, (uintptr_t)faultGranule);
		return true;
	}
	return false;
}

bool memory_isGuestRangeReserved(uint32 address, uint32 size)
{
	if (!g_memoryPageTable)
		return true;
	using namespace MMUTranslate;
	if (size == 0)
		return true;
	const uint64 beginPage = address >> PAGE_SHIFT;
	const uint64 endPage = (((uint64)address + size - 1) >> PAGE_SHIFT) + 1;
	if (endPage > NUM_PAGES)
		return false;
	uint8* previous = nullptr;
	for (uint64 p = beginPage; p < endPage; p++)
	{
		uint8* host = g_memoryPageTable[p];
		if (host == g_memoryArenaBase)
			return false; // not backed
		if (previous && host != previous + PAGE_SIZE)
			return false; // would not be contiguous on the host
		previous = host;
	}
	return true;
}

static void* memory_commitRange(uint32 offset, uint32 size)
{
	if (g_memoryPageTable)
	{
		using namespace MMUTranslate;
		const uint64 begin = offset & ~(uint64)PAGE_MASK;
		const uint64 end = ((uint64)offset + size + PAGE_MASK) & ~(uint64)PAGE_MASK;
		if (!memory_isGuestRangeReserved((uint32)begin, (uint32)(end - begin)))
			return nullptr;
		if (!MemMapper::AllocateMemory(memory_translate((uint32)begin), (size_t)(end - begin), MemMapper::PAGE_PERMISSION::P_RW, true))
			return nullptr;
		return memory_translate(offset);
	}
#if BOOST_OS_IOS
	if (s_usingSparseReservation && !memory_reserveSparseRange(reinterpret_cast<uintptr_t>(memory_base), offset, size))
	{
		cemuLog_log(LogType::Force, "Sparse reservation failed: host {:#x}, size {:#x}", s_failedSparseAddress, s_failedSparseSize);
		return nullptr;
	}
#endif
	return MemMapper::AllocateMemory(memory_base + offset, size, MemMapper::PAGE_PERMISSION::P_RW, true);
}

void checkMemAlloc(void* result)
{
	if (result == nullptr)
		assert_dbg();
}

void memory_initPhysicalLayout()
{
	assert_dbg();
	// todo - rewrite this using new MemMapper and MMU tables
	//memory_base = (uint8*)VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE, PAGE_READWRITE);
	//VirtualFree(memory_base, 0, MEM_RELEASE);

	//// todo - figure out all the ranges and allocate them properly

	//// allocate memory for the kernel
	////checkMemAlloc(VirtualAlloc(memory_base + 0x08000000, 1024*1024*2, MEM_COMMIT, PAGE_READWRITE));
	//// allocate memory for bootrom
	//checkMemAlloc(VirtualAlloc(memory_base + 0x00000000, 1024*16, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));


	//// allocate memory at 0x016FFFFC (is this some sort of register interface or maybe just temporary storage?)
	//checkMemAlloc(VirtualAlloc(memory_base + 0x016FF000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

	//// temporary storage for bootrom copy
	//checkMemAlloc(VirtualAlloc(memory_base + 0x016c0000, 0x4000 + 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	//// 0x016c0000

	//// L2
	//checkMemAlloc(VirtualAlloc(memory_base + 0xE0000000, 1024 * 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

	//// kernel memory
	//// currently it is unknown if this is it's own physical memory region or if this is mapped somehow
	//// considering the ancast is never copied here and no memory mapping is setup it seems like a hardwired mirror to 0x08000000?
	////checkMemAlloc(VirtualAlloc(memory_base + 0xFFE00000, 0x180000, MEM_COMMIT, PAGE_READWRITE));
	//HANDLE hKernelMem = CreateFileMappingA(
	//	INVALID_HANDLE_VALUE,    // use paging file
	//	NULL,                    // default security
	//	PAGE_READWRITE,          // read/write access
	//	0,                       // maximum object size (high-order DWORD)
	//	1024 * 1024 * 2,         // maximum object size (low-order DWORD)
	//	"kernelMem08000000");    // name of mapping object
	//
	//checkMemAlloc(MapViewOfFileEx(hKernelMem, FILE_MAP_ALL_ACCESS, 0, 0, 1024 * 1024 * 2, memory_base + 0x08000000));
	//checkMemAlloc(MapViewOfFileEx(hKernelMem, FILE_MAP_ALL_ACCESS, 0, 0, 1024 * 1024 * 2, memory_base + 0xFFE00000));

	//// IOSU->PPC bootParamBlock
	//checkMemAlloc(VirtualAlloc(memory_base + 0x01FFF000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

	//// used as dynamic kernel memory?
	//checkMemAlloc(VirtualAlloc(memory_base + 0x1C000000, 0x01000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

	//// mapped by kernel to FF200000 (loader.elf?)
	//checkMemAlloc(VirtualAlloc(memory_base + 0x1B800000, 0x00800000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

std::vector<struct MMURange*> g_mmuRanges;

std::vector<MMURange*> memory_getMMURanges()
{
	return g_mmuRanges;
}

MMURange* memory_getMMURangeByAddress(MPTR address)
{
	for (auto& itr : g_mmuRanges)
	{
		if (address >= itr->getBase() && address < itr->getEnd())
			return itr;
	}
	return nullptr;
}

MMURange::MMURange(const uint32 baseAddress, const uint32 size, MMU_MEM_AREA_ID areaId, const std::string_view name, MFLAG flags) : baseAddress(baseAddress), size(size), initSize(size), areaId(areaId), name(name), flags(flags)
{
	g_mmuRanges.emplace_back(this);
}

void MMURange::mapMem()
{
	cemu_assert_debug(!m_isMapped);
	if (memory_commitRange(baseAddress, size) == nullptr)
	{
		std::string errorMsg = _tr("Unable to allocate {} memory", name);
		WindowSystem::ShowErrorDialog(errorMsg, _tr("Error"));
		#if BOOST_OS_WINDOWS
		ExitProcess(-1);
		#else
		exit(-1);
		#endif
	}
	m_isMapped = true;
}

void MMURange::unmapMem()
{
	if (g_memoryPageTable)
	{
		using namespace MMUTranslate;
		const uint64 begin = baseAddress & ~(uint64)PAGE_MASK;
		const uint64 end = ((uint64)baseAddress + size + PAGE_MASK) & ~(uint64)PAGE_MASK;
		MemMapper::FreeMemory(memory_translate((uint32)begin), (size_t)(end - begin), true);
	}
	else
		MemMapper::FreeMemory(memory_base + baseAddress, size, true);
	m_isMapped = false;
}

MMURange mmuRange_LOW0					{ 0x00010000, 0x000F0000, MMU_MEM_AREA_ID::CODE_LOW0, "CODE_LOW0" }; // code cave (Cemuhook)
MMURange mmuRange_TRAMPOLINE_AREA		{ 0x00E00000, 0x00200000, MMU_MEM_AREA_ID::CODE_TRAMPOLINE, "TRAMPOLINE_AREA" }; // code area for trampolines and imports
MMURange mmuRange_CODECAVE				{ 0x01800000, 0x00400000, MMU_MEM_AREA_ID::CODE_CAVE, "CODECAVE" }; // code cave area (4MiB)
MMURange mmuRange_TEXT_AREA				{ 0x02000000, 0x0C000000, MMU_MEM_AREA_ID::CODE_MAIN, "TEXT_AREA" }; // module text sections go here (0x02000000 to 0x10000000, 224MiB)
MMURange mmuRange_CEMU_AREA				{ 0x0E000000, 0x02000000, MMU_MEM_AREA_ID::CEMU_PRIVATE, "CEMU_AREA", MMURange::MFLAG::FLAG_MAP_EARLY }; // Cemu-only, 32MiB. Should be allocated early for SysAllocator
MMURange mmuRange_MEM2					{ 0x10000000, 0x40000000, MMU_MEM_AREA_ID::MEM2_DATA, "MEM2" }; // main memory area (1GB)
MMURange mmuRange_OVERLAY_AREA			{ 0xA0000000, 0x1C000000, MMU_MEM_AREA_ID::OVERLAY, "OVERLAY_AREA", MMURange::MFLAG::FLAG_OPTIONAL }; // has to be requested, 448MiB
MMURange mmuRange_FGBUCKET				{ 0xE0000000, 0x04000000, MMU_MEM_AREA_ID::FGBUCKET, "FGBUCKET" }; // foreground bucket (64MiB)
MMURange mmuRange_TILINGAPERTURE		{ 0xE8000000, 0x02000000, MMU_MEM_AREA_ID::TILING_APERATURE, "TILINGAPERTURE" }; // tiling aperture
MMURange mmuRange_MEM1					{ 0xF4000000, 0x02000000, MMU_MEM_AREA_ID::MEM1, "MEM1" }; // 32MiB
MMURange mmuRange_RPLLOADER				{ 0xF6000000, 0x02000000, MMU_MEM_AREA_ID::RPLLOADER, "RPLLOADER_AREA" }; // shared with RPLLoader
MMURange mmuRange_SHARED_AREA			{ 0xF8000000, 0x02000000, MMU_MEM_AREA_ID::SHAREDDATA, "SHARED_AREA", MMURange::MFLAG::FLAG_MAP_EARLY }; // 32MiB, Cemuhook accesses this memory region at boot
MMURange mmuRange_CORE0_LC				{ 0xFFC00000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC0, "CORE0_LC" }; // locked L2 cache of core 0
MMURange mmuRange_CORE1_LC				{ 0xFFC40000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC1, "CORE1_LC" }; // locked L2 cache of core 1
MMURange mmuRange_CORE2_LC				{ 0xFFC80000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC2, "CORE2_LC" }; // locked L2 cache of core 2
MMURange mmuRange_HIGHMEM				{ 0xFFFFF000, 0x00001000, MMU_MEM_AREA_ID::CPU_PER_CORE, "PER-CORE" }; // per-core memory? Used by coreinit and PPC kernel to store core context specific data (like current thread ptr). We dont use it but Project Zero has a bug where it writes a byte at 0xfffffffe thus this memory range needs to be writable

// Logs how much address space this process can still reserve. Helps to tune reservations on devices with a small address space budget.
void memory_logFreeAddressSpace(const char* label)
{
#if BOOST_OS_IOS
	const MemMapper::FreeAddressSpaceInfo info = MemMapper::ProbeFreeAddressSpace();
	std::string blocks;
	for (const auto& block : info.blocks)
		blocks += fmt::format("{}x{}MiB ", block.second, block.first >> 20);
	cemuLog_log(LogType::Force, "Address space ({}): largest free block {} MiB, total reservable {} MiB (cap 16 GiB) [{}]", label, info.largestBlockBytes >> 20, info.totalBytes >> 20, blocks);
#endif
}

void memory_init()
{
	memory_logFreeAddressSpace("before guest memory reservation");
	const int pageTableOverride = memory_getPageTableOverride();
	if (!memory_base && pageTableOverride != 1)
		memory_base = (uint8*)MemMapper::ReserveMemory(nullptr, size_t{0x100000000ULL}, MemMapper::PAGE_PERMISSION::P_RW);
#if BOOST_OS_IOS
	if (!memory_base && pageTableOverride != 1)
	{
		// this kinda doesn't work, but it produces practically the same result as without it except giving me more info, so like i'll keep it -stossy11
		cemuLog_log(LogType::Force, "Contiguous guest reservation failed; trying sparse reservations (host page size: {})", MemMapper::GetPageSize());
        
		for (uintptr_t base = 0x10000000ULL; base < 0x1000000000ULL; base += 0x10000000ULL)
		{
			bool success = true;
			for (auto* range : g_mmuRanges)
			{
				if (!memory_reserveSparseRange(base, range->getBase(), range->getSize()))
				{
					success = false;
					break;
				}
			}
			
			if (success)
				success = memory_reserveSparseRange(base, 0x00800000, 0x00800000);
			if (success)
			{
				memory_base = reinterpret_cast<uint8*>(base);
				s_usingSparseReservation = true;
				cemuLog_log(LogType::Force, "Using sparse iOS guest memory reservations");
				break;
			}
			s_sparseReservation.ReleaseAll(base, memory_releaseSparseRange);
		}
		if (!memory_base)
			cemuLog_log(LogType::Force, "Sparse guest reservation failed: last host address {:#x}, size {:#x}", s_failedSparseAddress, s_failedSparseSize);
	}
#endif
	if (!memory_base && pageTableOverride != 0)
	{
		// 4GiB reservation (contiguous or sparse) is not available. Fall back to the packed arena + page table.
		cemuLog_log(LogType::Force, "Guest memory: {}, using page table mode", pageTableOverride == 1 ? "page table mode forced via MELOCAFE_PAGETABLE" : "4GiB reservation failed");
		// The optional overlay arena (448MiB) is not reserved in page table mode: address space is the scarce resource on these devices
		// (the JIT arena, the recompiler jump table and Metal all need it) and only a few graphic packs ask for the overlay.
		if (!memory_buildPageTable(false))
		{
			// last resort: allow the big code/data ranges to live in separate reservations
			cemuLog_log(LogType::Force, "Page table mode: retrying with separate reservations for adjacent ranges");
			memory_buildPageTable(false, false);
		}
	}
	if( !memory_base )
	{
		debug_printf("memory_init(): Unable to reserve guest memory regions\n");
		debugBreakpoint();
		WindowSystem::ShowErrorDialog(_tr("Unable to reserve guest memory regions"), _tr("Error"));
		exit(-1);
	}
	memory_logFreeAddressSpace("after guest memory reservation");
	for (auto& itr : g_mmuRanges)
	{
		if (itr->isMappedEarly())
			itr->mapMem();
	}
}

void memory_mapForCurrentTitle()
{
	for (auto& itr : g_mmuRanges)
		if(!itr->isMapped())
			itr->resetConfig();
	// expand ranges
	auto gfxPackMappings = GraphicPack2::GetActiveRAMMappings();
	for (auto& mapping : gfxPackMappings)
	{
		MMURange* mmuRange = nullptr;
		for (auto& itr : g_mmuRanges)
		{
			if (itr->getBase() == mapping.first)
			{
				mmuRange = itr;
				break;
			}
		}
		if (!mmuRange)
		{
			cemuLog_log(LogType::Force, fmt::format("Graphic pack error: Unable to apply modified RAM mapping {:08x}-{:08x}. Start address must match one of the existing MMU ranges:", mapping.first, mapping.second));
			for (auto& itr : g_mmuRanges)
			{
				if(itr->isMapped())
					continue;
				cemuLog_log(LogType::Force, fmt::format("{:08x}-{:08x} ({:})", itr->getBase(), itr->getEnd(), itr->getName()));
			}
			continue;
		}
		// make sure the new range isn't overlapping with anything
		bool isOverlapping = false;
		for (auto& itr : g_mmuRanges)
		{
			if(itr == mmuRange)
				continue;
			if (mapping.first < itr->getEnd() && mapping.second > itr->getBase())
			{
				cemuLog_log(LogType::Force, fmt::format("Graphic pack error: Unable to apply modified memory range {:08x}-{:08x} since it is overlapping with {:08x}-{:08x} ({:})", mapping.first, mapping.second, itr->getBase(), itr->getEnd(), itr->getName()));
				isOverlapping = true;
			}
		}
		if(isOverlapping)
			continue;
		if (g_memoryPageTable && !memory_isGuestRangeReserved(mapping.first, mapping.second - mapping.first))
		{
			cemuLog_log(LogType::Force, fmt::format("Graphic pack: Unable to apply modified RAM mapping {:08x}-{:08x} in page table mode (range exceeds the packed guest memory arena)", mapping.first, mapping.second));
			continue;
		}
		mmuRange->setEnd(mapping.second);
	}

	for (auto& itr : g_mmuRanges)
	{
		if (!itr->isOptional() && !itr->isMappedEarly())
			itr->mapMem();
	}
}

void memory_unmapForCurrentTitle()
{
    for (auto& itr : g_mmuRanges)
    {
        if (itr->isMapped() && !itr->isMappedEarly())
            itr->unmapMem();
    }
}

void memory_logModifiedMemoryRanges()
{
	auto gfxPackMappings = GraphicPack2::GetActiveRAMMappings();
	for (auto& mapping : gfxPackMappings)
	{
		MMURange* mmuRange = nullptr;
		for (auto& itr : g_mmuRanges)
		{
			if (itr->getBase() == mapping.first)
			{
				mmuRange = itr;
				break;
			}
		}
		if (!mmuRange)
			continue;
		sint32 extraMem = (sint32)mapping.second - (sint32)(mmuRange->getBase() + mmuRange->getInitSize());
		extraMem = (extraMem + 1023) / 1024;
		std::string memAmountStr;
		if (extraMem >= 8 * 1024 * 1024)
			memAmountStr = fmt::format("{:+}MiB", (extraMem + 1023) / 1024);
		else
			memAmountStr = fmt::format("{:+}KiB", extraMem);
		cemuLog_log(LogType::Force, fmt::format("Graphic pack: Using modified RAM mapping {:08x}-{:08x} ({})", mapping.first, mapping.second, memAmountStr));
	}
}

void memory_enableOverlayArena()
{
	if (mmuRange_OVERLAY_AREA.isMapped())
		return;
	if (!memory_isGuestRangeReserved(mmuRange_OVERLAY_AREA.getBase(), mmuRange_OVERLAY_AREA.getSize()))
	{
		cemuLog_log(LogType::Force, "Overlay arena is not available in page table mode on this device");
		return;
	}
	mmuRange_OVERLAY_AREA.mapMem();
}

void memory_enableHBLELFCodeArea()
{
	if (memory_elfCodeArena != NULL)
		return;
	memory_elfCodeArena = (uint8*)memory_commitRange(0x00800000, 0x00800000);
	if (memory_elfCodeArena == NULL)
	{
		debug_printf("memory_enableHBLELFCodeArea(): Unable to allocate memory for ELF arena\n");
		debugBreakpoint();
	}
}

bool memory_isAddressRangeAccessible(MPTR virtualAddress, uint32 size)
{
	for (auto& itr : g_mmuRanges)
	{
		if(!itr->isMapped())
			continue;
		if (virtualAddress >= itr->getBase() && virtualAddress < itr->getEnd())
		{
			uint32 remainingSize = itr->getEnd() - virtualAddress;
			return size <= remainingSize && itr->isMapped();
		}
	}
	return false;
}

uint32 memory_virtualToPhysical(uint32 virtualOffset)
{
	// currently we map virtual to physical space 1:1
	return virtualOffset;
}

uint32 memory_physicalToVirtual(uint32 physicalOffset)
{
	// currently we map virtual to physical space 1:1
	return physicalOffset;
}

uint8* memory_getPointerFromPhysicalOffset(uint32 physicalOffset)
{
	return memory_translate(physicalOffset);
}

uint32 memory_getVirtualOffsetFromPointer(void* ptr)
{
	if( !ptr )
		return MPTR_NULL;
	return memory_reverseTranslate(ptr);
}

// write access
void memory_writeDouble(uint32 address, double vf)
{
	uint64 v = *(uint64*)&vf;
	uint32 v1 = v&0xFFFFFFFF;
	uint32 v2 = v>>32;
	uint8* ptr = memory_getPointerFromVirtualOffset(address);
	*(uint32*)(ptr+4) = CPU_swapEndianU32(v1);
	*(uint32*)(ptr+0) = CPU_swapEndianU32(v2);
}

void memory_writeFloat(uint32 address, float vf)
{
	uint32 v = *(uint32*)&vf;
	*(uint32*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU32(v);
}

void memory_writeU32(uint32 address, uint32 v)
{
	*(uint32*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU32(v);
}

void memory_writeU64(uint32 address, uint64 v)
{
	*(uint64*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU64(v);
}

void memory_writeU16(uint32 address, uint16 v)
{
	*(uint16*)(memory_getPointerFromVirtualOffset(address)) = CPU_swapEndianU16(v);
}

void memory_writeU8(uint32 address, uint8 v)
{
	*(uint8*)(memory_getPointerFromVirtualOffset(address)) = v;
}

// read access

double memory_readDouble(uint32 address)
{
	uint32 v[2];
	v[1] = *(uint32*)(memory_getPointerFromVirtualOffset(address));
	v[0] = *(uint32*)(memory_getPointerFromVirtualOffset(address)+4);
	v[0] = CPU_swapEndianU32(v[0]);
	v[1] = CPU_swapEndianU32(v[1]);
	return *(double*)v;
}

float memory_readFloat(uint32 address)
{
	uint32 v = *(uint32*)(memory_getPointerFromVirtualOffset(address));
	v = CPU_swapEndianU32(v);
	return *(float*)&v;
}

uint64 memory_readU64(uint32 address)
{
	uint64 v = *(uint64*)(memory_getPointerFromVirtualOffset(address));
	return CPU_swapEndianU64(v);
}

uint32 memory_readU32(uint32 address)
{
	uint32 v = *(uint32*)(memory_getPointerFromVirtualOffset(address));
	return CPU_swapEndianU32(v);
}

uint16 memory_readU16(uint32 address)
{
	uint16 v = *(uint16*)(memory_getPointerFromVirtualOffset(address));
	return CPU_swapEndianU16(v);
}

uint8 memory_readU8(uint32 address)
{
	return *(uint8*)(memory_getPointerFromVirtualOffset(address));
}

uint32 memory_getContiguousReadableBytes(uint32 virtualAddress)
{
    uint32 maxBytes = 0;
	
    for (auto& r : g_mmuRanges)
    {
        if (!r->isMapped())
            continue;
		
        if (virtualAddress >= r->getBase() && virtualAddress < r->getEnd())
        {
            uint32 remaining = r->getEnd() - virtualAddress;
            return remaining;
        }
    }
	
    return 0;
}

extern "C" DLLEXPORT void* memory_getBase()
{
	return memory_base;
}

void memory_writeDumpFile(uint32 startAddr, uint32 size, const fs::path& path)
{
	fs::path filePath = path;
	filePath /= fmt::format("{:08x}.bin", startAddr);
	FileStream* fs = FileStream::createFile2(filePath);
	if (fs)
	{
		fs->writeData(memory_translate(startAddr), size);
		delete fs;
	}
}

void memory_createDump()
{
	const uint32 pageSize = MemMapper::GetPageSize();
	fs::path path = ActiveSettings::GetUserDataPath("dump/ramDump{:}", (uint32)time(nullptr));
	fs::create_directories(path);

	for (auto& itr : g_mmuRanges)
	{
		if(!itr->isMapped())
			continue;
		memory_writeDumpFile(itr->getBase(), itr->getSize(), path);
	}
}

namespace MMU
{
	// MMIO access handler
	// located in address region 0x0C000000 - 0x0E000000
	// there seem to be multiple subregions + special meanings for some address bits maybe?
	// Try to figure this out. We know these regions (in Wii U mode):
	// 0x0C000000 (the old GC register interface?)
	// 0x0D000000 (new Wii U stuff?)

	std::unordered_map<PAddr, MMIOFuncWrite32>* g_mmioHandlerW32{};
	std::unordered_map<PAddr, MMIOFuncWrite16>* g_mmioHandlerW16{};
	std::unordered_map<PAddr, MMIOFuncRead32>* g_mmioHandlerR32{};
	std::unordered_map<PAddr, MMIOFuncRead16>* g_mmioHandlerR16{};

	void _initHandlers()
	{
		if (g_mmioHandlerW32)
			return;
		g_mmioHandlerW32 = new std::unordered_map<PAddr, MMIOFuncWrite32>();
		g_mmioHandlerW16 = new std::unordered_map<PAddr, MMIOFuncWrite16>();
		g_mmioHandlerR32 = new std::unordered_map<PAddr, MMIOFuncRead32>();
		g_mmioHandlerR16 = new std::unordered_map<PAddr, MMIOFuncRead16>();
	}

	PAddr _MakeMMIOAddress(MMIOInterface interfaceLocation, uint32 relativeAddress)
	{
		PAddr addr = 0;
		if (interfaceLocation == MMIOInterface::INTERFACE_0C000000)
			addr = 0x0C000000;
		else if (interfaceLocation == MMIOInterface::INTERFACE_0D000000)
			addr = 0x0D000000;
		else
			assert_dbg();
		return addr + relativeAddress;
	}

	void RegisterMMIO_W32(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncWrite32 ptr)
	{
		_initHandlers();
		g_mmioHandlerW32->emplace(_MakeMMIOAddress(interfaceLocation, relativeAddress), ptr);
	}

	void RegisterMMIO_W16(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncWrite16 ptr)
	{
		_initHandlers();
		g_mmioHandlerW16->emplace(_MakeMMIOAddress(interfaceLocation, relativeAddress), ptr);
	}

	void RegisterMMIO_R32(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncRead32 ptr)
	{
		_initHandlers();
		PAddr addr = _MakeMMIOAddress(interfaceLocation, relativeAddress);
		g_mmioHandlerR32->emplace(addr, ptr);
	}

	void RegisterMMIO_R16(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncRead16 ptr)
	{
		_initHandlers();
		g_mmioHandlerR16->emplace(_MakeMMIOAddress(interfaceLocation, relativeAddress), ptr);
	}

	void WriteMMIO_32(PAddr address, uint32 value)
	{
		cemu_assert_debug((address & 0x3) == 0);
		auto itr = g_mmioHandlerW32->find(address);
		if (itr == g_mmioHandlerW32->end())
		{
			//cemuLog_logDebug(LogType::Force, "[MMU] MMIO write u32 0x{:08x} from unhandled address 0x{:08x}", value, address);
			return;
		}
		return itr->second(address, value);
	}

	void WriteMMIO_16(PAddr address, uint16 value)
	{
		cemu_assert_debug((address & 0x1) == 0);
		auto itr = g_mmioHandlerW16->find(address);
		if (itr == g_mmioHandlerW16->end())
		{
			//cemuLog_logDebug(LogType::Force, "[MMU] MMIO write u16 0x{:04x} from unhandled address 0x{:08x}", (uint32)value, address);
			return;
		}
		return itr->second(address, value);
	}


	// todo - instead of passing the physical address to Read/WriteMMIO we should pass an interface id and a relative address? This would allow remapping the hardware address (tho we can just unregister + register at different addresses)

	uint32 ReadMMIO_32(PAddr address)
	{
		cemu_assert_debug((address & 0x3) == 0);
		auto itr = g_mmioHandlerR32->find(address);
		if(itr == g_mmioHandlerR32->end())
		{
			//cemuLog_logDebug(LogType::Force, "[MMU] MMIO read u32 from unhandled address 0x{:08x}", address);
			return 0;
		}
		return itr->second(address);
	}

	uint16 ReadMMIO_16(PAddr address)
	{
		cemu_assert_debug((address & 0x1) == 0);
		auto itr = g_mmioHandlerR16->find(address);
		if (itr == g_mmioHandlerR16->end())
		{
			//cemuLog_logDebug(LogType::Force, "[MMU] MMIO read u16 from unhandled address 0x{:08x}", address);
			return 0;
		}
		return itr->second(address);
	}


}
