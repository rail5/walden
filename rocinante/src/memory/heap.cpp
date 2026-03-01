/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "heap.h"

namespace Rocinante::Memory::Heap {

// -----------------------------
// Block format and invariants
// -----------------------------
//
// The heap is a single contiguous region split into "blocks".
//
// Each block has:
//   [Header][Payload ...][Footer]
//
// Header and footer both store the block size, with low bits reserved for flags.
// We use 16-byte alignment, so the lower 4 bits of any valid block size are 0.
// That gives us room for a small flag bitfield.
//
// When a block is FREE, we store a doubly-linked list node at the beginning of
// the payload area so we can maintain an explicit free list.
//
// This is a classic "boundary tag" allocator:
// - Coalescing with the next block is easy (look at next header).
// - Coalescing with the previous block is easy (look at previous footer).

namespace {

constexpr std::size_t kHeapAlign = 16;
constexpr std::size_t kFlagMask = kHeapAlign - 1; // 0xF
constexpr std::size_t kUsedFlag = 1;

static_assert((kHeapAlign & (kHeapAlign - 1)) == 0, "kHeapAlign must be power of two");

// Header is aligned so the payload begins on a 16-byte boundary.
struct alignas(kHeapAlign) BlockHeader {
	std::size_t SizeAndFlags;
};

// Footer is just a copy of SizeAndFlags.
using BlockFooter = std::size_t;

// Stored inside the payload of a free block.
struct FreeNode {
	FreeNode* Next;
	FreeNode* Prev;
};

constexpr std::size_t HeaderSize = sizeof(BlockHeader);
constexpr std::size_t FooterSize = sizeof(BlockFooter);
constexpr std::size_t FreeNodeSize = sizeof(FreeNode);

// Smallest block we can keep as a free block.
// It must be large enough to hold header + free-list node + footer.
constexpr std::size_t MinFreeBlockSize =
	((HeaderSize + FreeNodeSize + FooterSize + (kHeapAlign - 1)) & ~kFlagMask);

static std::uint8_t* g_heap_begin = nullptr;
static std::uint8_t* g_heap_end = nullptr;
static FreeNode* g_free_list_head = nullptr;
static bool g_initialized = false;

static constexpr std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
	return (value + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

static constexpr std::size_t RoundUp(std::size_t value, std::size_t alignment) {
	return (value + (alignment - 1)) & ~(alignment - 1);
}

static bool IsPowerOfTwo(std::size_t x) {
	return x && ((x & (x - 1)) == 0);
}

static std::size_t BlockSize(const BlockHeader* header) {
	return header->SizeAndFlags & ~kFlagMask;
}

static bool IsUsed(const BlockHeader* header) {
	return (header->SizeAndFlags & kUsedFlag) != 0;
}

static void SetHeaderAndFooter(BlockHeader* header, std::size_t size_bytes, bool used) {
	// size_bytes must be aligned to kHeapAlign.
	const std::size_t flags = used ? kUsedFlag : 0;
	header->SizeAndFlags = size_bytes | flags;

	auto* footer = reinterpret_cast<BlockFooter*>(reinterpret_cast<std::uint8_t*>(header) + size_bytes - FooterSize);
	*footer = header->SizeAndFlags;
}

static BlockHeader* NextBlock(BlockHeader* header) {
	return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(header) + BlockSize(header));
}

static BlockHeader* PrevBlock(BlockHeader* header) {
	// Previous block's footer is immediately before this block's header.
	auto* prev_footer = reinterpret_cast<BlockFooter*>(reinterpret_cast<std::uint8_t*>(header) - FooterSize);
	const std::size_t prev_size = (*prev_footer) & ~kFlagMask;
	return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(header) - prev_size);
}

static FreeNode* NodeFor(BlockHeader* header) {
	return reinterpret_cast<FreeNode*>(reinterpret_cast<std::uint8_t*>(header) + HeaderSize);
}

static BlockHeader* HeaderFor(FreeNode* node) {
	return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(node) - HeaderSize);
}

static void FreeListRemove(FreeNode* node) {
	if (node->Prev) {
		node->Prev->Next = node->Next;
	} else {
		g_free_list_head = node->Next;
	}
	if (node->Next) {
		node->Next->Prev = node->Prev;
	}
	node->Next = nullptr;
	node->Prev = nullptr;
}

static void FreeListInsertFront(FreeNode* node) {
	node->Prev = nullptr;
	node->Next = g_free_list_head;
	if (g_free_list_head) g_free_list_head->Prev = node;
	g_free_list_head = node;
}

} // namespace

bool IsInitialized() {
	return g_initialized;
}

void Init(void* heap_start, std::size_t heap_size_bytes) {
	// Align the heap start up, and shrink the size accordingly.
	std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(heap_start);
	std::uintptr_t aligned_begin = AlignUp(begin, kHeapAlign);

	if (aligned_begin > begin) {
		const std::size_t delta = static_cast<std::size_t>(aligned_begin - begin);
		if (heap_size_bytes <= delta) {
			// Not enough space.
			g_initialized = false;
			g_heap_begin = g_heap_end = nullptr;
			g_free_list_head = nullptr;
			return;
		}
		heap_size_bytes -= delta;
	}

	heap_size_bytes &= ~kFlagMask; // round down

	g_heap_begin = reinterpret_cast<std::uint8_t*>(aligned_begin);
	g_heap_end = g_heap_begin + heap_size_bytes;
	g_free_list_head = nullptr;

	if (heap_size_bytes < MinFreeBlockSize) {
		g_initialized = false;
		g_heap_begin = g_heap_end = nullptr;
		return;
	}

	// Create a single large free block spanning the entire heap.
	auto* first = reinterpret_cast<BlockHeader*>(g_heap_begin);
	SetHeaderAndFooter(first, heap_size_bytes, /*used=*/false);

	FreeNode* node = NodeFor(first);
	node->Next = nullptr;
	node->Prev = nullptr;
	g_free_list_head = node;

	g_initialized = true;
}

void* Alloc(std::size_t size, std::size_t alignment) {
	if (!g_initialized) return nullptr;

	// Normalize alignment. We guarantee at least 16-byte alignment.
	if (alignment < kHeapAlign) alignment = kHeapAlign;
	if (!IsPowerOfTwo(alignment)) return nullptr;

	// Payload size is rounded up to the base heap alignment.
	const std::size_t payload_size = RoundUp(size, kHeapAlign);
	const std::size_t block_needed = RoundUp(HeaderSize + payload_size + FooterSize, kHeapAlign);

	for (FreeNode* node = g_free_list_head; node; node = node->Next) {
		BlockHeader* free_block = HeaderFor(node);
		const std::size_t free_size = BlockSize(free_block);

		// Try to place an allocated block inside this free block so that the
		// returned payload pointer meets `alignment`.
		std::uint8_t* block_begin = reinterpret_cast<std::uint8_t*>(free_block);
		std::uint8_t* block_end = block_begin + free_size;

		// We want:
		//   payload = alloc_header + HeaderSize
		// to be aligned.
		std::uintptr_t payload_begin = reinterpret_cast<std::uintptr_t>(block_begin + HeaderSize);
		std::uintptr_t payload_aligned = AlignUp(payload_begin, alignment);
		std::uint8_t* alloc_header_begin = reinterpret_cast<std::uint8_t*>(payload_aligned - HeaderSize);

		// If the prefix is too small to be its own free block, bump by alignment
		// until it is (or until it no longer fits).
		while (alloc_header_begin > block_begin &&
			static_cast<std::size_t>(alloc_header_begin - block_begin) < MinFreeBlockSize) {
			payload_aligned += alignment;
			alloc_header_begin = reinterpret_cast<std::uint8_t*>(payload_aligned - HeaderSize);
			if (alloc_header_begin + block_needed > block_end) break;
		}

		const std::size_t prefix = static_cast<std::size_t>(alloc_header_begin - block_begin);
		if (alloc_header_begin + block_needed > block_end) continue;

		// Success: remove this free block from the free list and split it into:
		// [optional prefix free][allocated][optional suffix free]
		FreeListRemove(node);

		// Prefix free block
		if (prefix >= MinFreeBlockSize) {
			auto* prefix_header = reinterpret_cast<BlockHeader*>(block_begin);
			SetHeaderAndFooter(prefix_header, prefix, /*used=*/false);
			FreeListInsertFront(NodeFor(prefix_header));
		}

		// Allocated block
		auto* alloc_header = reinterpret_cast<BlockHeader*>(alloc_header_begin);
		SetHeaderAndFooter(alloc_header, block_needed, /*used=*/true);

		// Suffix free block
		std::uint8_t* suffix_begin = alloc_header_begin + block_needed;
		const std::size_t suffix = static_cast<std::size_t>(block_end - suffix_begin);
		if (suffix >= MinFreeBlockSize) {
			auto* suffix_header = reinterpret_cast<BlockHeader*>(suffix_begin);
			SetHeaderAndFooter(suffix_header, suffix, /*used=*/false);
			FreeListInsertFront(NodeFor(suffix_header));
		} else if (suffix != 0) {
			// Not enough space to form a valid free block; just give it to the
			// allocation (prevents creating an unusable fragment).
			SetHeaderAndFooter(alloc_header, block_needed + suffix, /*used=*/true);
		}

		return reinterpret_cast<void*>(alloc_header_begin + HeaderSize);
	}

	return nullptr;
}

void Free(void* ptr) {
	if (!g_initialized || !ptr) return;

	auto* header = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(ptr) - HeaderSize);

	// Mark the block free.
	SetHeaderAndFooter(header, BlockSize(header), /*used=*/false);

	// Coalesce with next block if it exists and is free.
	BlockHeader* next = NextBlock(header);
	if (reinterpret_cast<std::uint8_t*>(next) < g_heap_end && !IsUsed(next)) {
		FreeListRemove(NodeFor(next));
		const std::size_t merged = BlockSize(header) + BlockSize(next);
		SetHeaderAndFooter(header, merged, /*used=*/false);
	}

	// Coalesce with previous block if it exists and is free.
	if (reinterpret_cast<std::uint8_t*>(header) > g_heap_begin) {
		BlockHeader* prev = PrevBlock(header);
		if (reinterpret_cast<std::uint8_t*>(prev) >= g_heap_begin && !IsUsed(prev)) {
			FreeListRemove(NodeFor(prev));
			const std::size_t merged = BlockSize(prev) + BlockSize(header);
			SetHeaderAndFooter(prev, merged, /*used=*/false);
			header = prev;
		}
	}

	// Insert the (possibly merged) free block back into the free list.
	FreeListInsertFront(NodeFor(header));
}

std::size_t TotalBytes() {
	if (!g_initialized) return 0;
	return static_cast<std::size_t>(g_heap_end - g_heap_begin);
}

std::size_t FreeBytes() {
	if (!g_initialized) return 0;

	std::size_t total = 0;
	for (FreeNode* node = g_free_list_head; node; node = node->Next) {
		total += BlockSize(HeaderFor(node));
	}
	return total;
}

} // namespace Rocinante::Memory::Heap
