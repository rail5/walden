/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/memory/vmm_unmap.h>

#include <src/memory/paging.h>
#include <src/memory/vm_object.h>

namespace Rocinante::Memory::VmmUnmap {

bool UnmapVma4KiB(
	PhysicalMemoryManager* pmm,
	const Paging::PageTableRoot& root,
	const VirtualMemoryArea& vma
) {
	if (!pmm) return false;
	if (!vma.IsValid()) return false;

	const std::uintptr_t virtual_base = vma.virtual_base;
	const std::uintptr_t virtual_limit = vma.virtual_limit;

	const std::uintptr_t size_bytes = virtual_limit - virtual_base;
	if ((size_bytes % Paging::kPageSizeBytes) != 0) return false;

	bool ok = true;
	std::uintptr_t unmapped_bytes = 0;
	while (unmapped_bytes < size_bytes) {
		const std::uintptr_t virtual_page = virtual_base + unmapped_bytes;
		const std::size_t page_offset = static_cast<std::size_t>(unmapped_bytes / Paging::kPageSizeBytes);

		const auto translated = Paging::Translate(root, virtual_page);
		if (translated.has_value()) {
			if (!Paging::UnmapPage4KiB(pmm, root, virtual_page)) {
				ok = false;
			}
		}

		if (vma.owns_frames) {
			if (vma.backing_type != VirtualMemoryArea::BackingType::Anonymous) {
				ok = false;
			} else if (!vma.anonymous_object) {
				ok = false;
			} else {
				// Policy note:
				// Drop object ownership only after unmapping. If we release first, the
				// PMM may observe ref_count==0 while map_count>0, and the subsequent
				// unmap will not revisit the ref_count to reclaim the frame.
				if (!vma.anonymous_object->ReleaseFrameForPageOffset(pmm, page_offset)) {
					ok = false;
				}
			}
		}

		unmapped_bytes += Paging::kPageSizeBytes;
	}

	return ok;
}

} // namespace Rocinante::Memory::VmmUnmap
