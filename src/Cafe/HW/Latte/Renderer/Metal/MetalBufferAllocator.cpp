#include "Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h"
#include <chrono>
#include <thread>

MetalBufferChunkedHeap::~MetalBufferChunkedHeap()
{
	for (auto& chunk : m_chunkBuffers)
		chunk->release();
}

uint32 MetalBufferChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	size_t allocationSize = std::max<size_t>(m_minimumBufferAllocationSize, minimumAllocationSize);
	MTL::Buffer* buffer = m_mtlr->GetDevice()->newBuffer(allocationSize, m_options);
	cemu_assert_debug(buffer);
	cemu_assert_debug(m_chunkBuffers.size() == chunkIndex);
	m_chunkBuffers.emplace_back(buffer);

	return allocationSize;
}

void MetalSynchronizedRingAllocator::addUploadBufferSyncPoint(AllocatorBuffer_t& buffer, uint32 offset)
{
	auto commandBuffer = m_mtlr->GetCurrentCommandBuffer();
	if (!buffer.queue_syncPoints.empty() && buffer.queue_syncPoints.back().commandBuffer == commandBuffer)
		return;
	buffer.queue_syncPoints.emplace(commandBuffer, offset);
}

bool MetalSynchronizedRingAllocator::allocateAdditionalUploadBuffer(uint32 sizeRequiredForAlloc)
{
	// calculate buffer size, should be a multiple of bufferAllocSize that is at least as large as sizeRequiredForAlloc
	uint32 bufferAllocSize = m_minimumBufferAllocSize;
	while (bufferAllocSize < sizeRequiredForAlloc)
		bufferAllocSize += m_minimumBufferAllocSize;

	MTL::Buffer* mtlBuffer = m_mtlr->GetDevice()->newBuffer(bufferAllocSize, m_options);
	if (!mtlBuffer || !mtlBuffer->contents())
	{
		// Out of memory or address space (common on devices with 3GB of RAM). Try progressively smaller buffers, and wait for the GPU
		// to finish its queued work so memory can be reclaimed, before giving up.
		cemuLog_log(LogType::Force, "Metal: failed to allocate a {} KiB upload buffer (device allocated {} MiB, recommended working set {} MiB, {} upload buffers in use). Retrying", bufferAllocSize >> 10, m_mtlr->GetDevice()->currentAllocatedSize() >> 20, m_mtlr->GetDevice()->recommendedMaxWorkingSetSize() >> 20, m_buffers.size());
		if (mtlBuffer)
			mtlBuffer->release();
		mtlBuffer = nullptr;
		const uint32 smallSize = (sizeRequiredForAlloc + 0xFFFFF) & ~0xFFFFFu; // exact size rounded up to 1MiB
		for (int attempt = 0; attempt < 3 && !mtlBuffer; attempt++)
		{
			if (attempt >= 1)
			{
				const_cast<MetalRenderer*>(m_mtlr)->Flush(true);
				std::this_thread::sleep_for(std::chrono::milliseconds(20 * attempt));
			}
			bufferAllocSize = (attempt == 0) ? std::max(smallSize, 1u << 20) : smallSize;
			mtlBuffer = m_mtlr->GetDevice()->newBuffer(bufferAllocSize, m_options);
			if (mtlBuffer && !mtlBuffer->contents())
			{
				mtlBuffer->release();
				mtlBuffer = nullptr;
			}
		}
		if (!mtlBuffer)
		{
			cemuLog_log(LogType::Force, "Metal: unable to allocate an upload buffer of {} KiB, skipping the upload", smallSize >> 10);
			return false;
		}
	}

	AllocatorBuffer_t newBuffer{};
	newBuffer.writeIndex = 0;
	newBuffer.mtlBuffer = mtlBuffer;
	newBuffer.basePtr = (uint8*)newBuffer.mtlBuffer->contents();
	newBuffer.size = bufferAllocSize;
	newBuffer.index = (uint32)m_buffers.size();
	m_buffers.push_back(newBuffer);
	return true;
}

MetalSynchronizedRingAllocator::AllocatorReservation_t MetalSynchronizedRingAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	if (alignment < 128)
		alignment = 128;
	size = (size + 127) & ~127;

	for (auto& itr : m_buffers)
	{
		// align pointer
		uint32 alignmentPadding = (alignment - (itr.writeIndex % alignment)) % alignment;
		uint32 distanceToSyncPoint;
		if (!itr.queue_syncPoints.empty())
		{
			if (itr.queue_syncPoints.front().offset < itr.writeIndex)
				distanceToSyncPoint = 0xFFFFFFFF;
			else
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
		}
		else
			distanceToSyncPoint = 0xFFFFFFFF;
		uint32 spaceNeeded = alignmentPadding + size;
		if (spaceNeeded > distanceToSyncPoint)
			continue; // not enough space in current buffer
		if ((itr.writeIndex + spaceNeeded) > itr.size)
		{
			// wrap-around
			spaceNeeded = size;
			alignmentPadding = 0;
			// check if there is enough space in current buffer after wrap-around
			if (!itr.queue_syncPoints.empty())
			{
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - 0;
				if (spaceNeeded > distanceToSyncPoint)
					continue;
			}
			else if (spaceNeeded > itr.size)
				continue;
			itr.writeIndex = 0;
		}
		addUploadBufferSyncPoint(itr, itr.writeIndex);
		itr.writeIndex += alignmentPadding;
		uint32 offset = itr.writeIndex;
		itr.writeIndex += size;
		itr.cleanupCounter = 0;
		MetalSynchronizedRingAllocator::AllocatorReservation_t res;
		res.mtlBuffer = itr.mtlBuffer;
		res.memPtr = itr.basePtr + offset;
		res.bufferOffset = offset;
		res.size = size;
		res.bufferIndex = itr.index;

		return res;
	}

	// allocate new buffer
	if (!allocateAdditionalUploadBuffer(size))
		return AllocatorReservation_t{}; // memPtr == nullptr: callers must skip the upload

	return AllocateBufferMemory(size, alignment);
}

void MetalSynchronizedRingAllocator::FlushReservation(AllocatorReservation_t& uploadReservation)
{
    if (RequiresFlush())
    {
        uploadReservation.mtlBuffer->didModifyRange(NS::Range(uploadReservation.bufferOffset, uploadReservation.size));
    }
}

void MetalSynchronizedRingAllocator::CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer)
{
	for (auto& itr : m_buffers)
	{
		while (!itr.queue_syncPoints.empty() && latestFinishedCommandBuffer == itr.queue_syncPoints.front().commandBuffer)
		{
			itr.queue_syncPoints.pop();
		}
		if (itr.queue_syncPoints.empty())
			itr.cleanupCounter++;
	}

	// check if last buffer is available for deletion
	if (m_buffers.size() >= 2)
	{
		auto& lastBuffer = m_buffers.back();
		if (lastBuffer.cleanupCounter >= 1000)
		{
			// release buffer
			lastBuffer.mtlBuffer->release();
			m_buffers.pop_back();
		}
	}
}

MTL::Buffer* MetalSynchronizedRingAllocator::GetBufferByIndex(uint32 index) const
{
	return m_buffers[index].mtlBuffer;
}

void MetalSynchronizedRingAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	numBuffers = (uint32)m_buffers.size();
	totalBufferSize = 0;
	freeBufferSize = 0;
	for (auto& itr : m_buffers)
	{
		totalBufferSize += itr.size;
		// calculate free space in buffer
		uint32 distanceToSyncPoint;
		if (!itr.queue_syncPoints.empty())
		{
			if (itr.queue_syncPoints.front().offset < itr.writeIndex)
				distanceToSyncPoint = (itr.size - itr.writeIndex) + itr.queue_syncPoints.front().offset; // size with wrap-around
			else
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
		}
		else
			distanceToSyncPoint = itr.size;
		freeBufferSize += distanceToSyncPoint;
	}
}

/* MetalSynchronizedHeapAllocator */

MetalSynchronizedHeapAllocator::AllocatorReservation* MetalSynchronizedHeapAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	CHAddr addr = m_chunkedHeap.alloc(size, alignment);
	AllocatorReservation* res = m_poolAllocatorReservation.allocObj();
	res->bufferIndex = addr.chunkIndex;
	res->bufferOffset = addr.offset;
	res->size = size;
	res->mtlBuffer = m_chunkedHeap.GetBufferByIndex(addr.chunkIndex);
	res->memPtr = m_chunkedHeap.GetChunkPtr(addr.chunkIndex) + addr.offset;
	res->heapAllocation = addr;

	return res;
}

void MetalSynchronizedHeapAllocator::FreeReservation(AllocatorReservation* uploadReservation)
{
	// put the allocation on a delayed release queue for the current command buffer
	MTL::CommandBuffer* currentCommandBuffer = m_mtlr->GetCurrentCommandBuffer();
	cemu_assert_debug(uploadReservation->heapAllocation.isValid());
	m_releaseQueue[currentCommandBuffer].emplace_back(uploadReservation->heapAllocation);
	m_poolAllocatorReservation.freeObj(uploadReservation);
}

void MetalSynchronizedHeapAllocator::FlushReservation(AllocatorReservation* uploadReservation)
{
	if (m_chunkedHeap.RequiresFlush())
	{
	    uploadReservation->mtlBuffer->didModifyRange(NS::Range(uploadReservation->bufferOffset, uploadReservation->size));
	}
}

void MetalSynchronizedHeapAllocator::CleanupBuffer(MTL::CommandBuffer* latestFinishedCommandBuffer)
{
    auto it = m_releaseQueue.find(latestFinishedCommandBuffer);
    if (it == m_releaseQueue.end())
        return;

    // release allocations
	for (auto& addr : it->second)
		m_chunkedHeap.free(addr);
	m_releaseQueue.erase(it);
}

void MetalSynchronizedHeapAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	m_chunkedHeap.GetStats(numBuffers, totalBufferSize, freeBufferSize);
}
