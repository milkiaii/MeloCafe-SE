#include "util/MemMapper/MemMapper.h"

#include <algorithm>

namespace MemMapper
{
	FreeAddressSpaceInfo ProbeFreeAddressSpace(size_t capBytes)
	{
		// Greedily reserve (PROT_NONE, costs no RAM) the biggest blocks the process can still get, then give everything back.
		// This shows the real address space budget which is much smaller than the 64bit range on constrained devices (e.g. 3GB iPhones).
		FreeAddressSpaceInfo info;
		struct Block
		{
			void* ptr;
			size_t size;
		};
		std::vector<Block> held;
		constexpr size_t MiB = 1024 * 1024;
		for (size_t size = 4096 * MiB; size >= 32 * MiB && info.totalBytes < capBytes; size /= 2)
		{
			while (info.totalBytes < capBytes)
			{
				void* p = ReserveMemory(nullptr, size, PAGE_PERMISSION::P_NONE);
				if (!p)
					break;
				held.push_back({p, size});
				info.totalBytes += size;
				info.largestBlockBytes = std::max(info.largestBlockBytes, size);
				if (!info.blocks.empty() && info.blocks.back().first == size)
					info.blocks.back().second++;
				else
					info.blocks.emplace_back(size, 1);
			}
		}
		for (const auto& block : held)
			FreeReservation(block.ptr, block.size);
		return info;
	}
}
