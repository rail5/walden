/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include "boot_memory_map.h"

namespace Rocinante::Memory {

namespace {

// Flattened Device Tree (FDT) / device tree blob (DTB) parsing.
//
// Reference:
// - Devicetree Specification (Flattened Devicetree format)
//
// This parser is intentionally small and conservative:
// - We only implement what we need for early memory discovery.
// - We do not attempt to fully validate every DTB invariant.
// - We prefer explicit code and checks over cleverness.

struct FdtHeader final {
	std::uint32_t magic_be;
	std::uint32_t totalsize_be;
	std::uint32_t off_dt_struct_be;
	std::uint32_t off_dt_strings_be;
	std::uint32_t off_mem_rsvmap_be;
	std::uint32_t version_be;
	std::uint32_t last_comp_version_be;
	std::uint32_t boot_cpuid_phys_be;
	std::uint32_t size_dt_strings_be;
	std::uint32_t size_dt_struct_be;
};

static_assert(sizeof(FdtHeader) == 40);

namespace Fdt {
	// FDT header magic value (big-endian 0xd00dfeed).
	constexpr std::uint32_t kMagic = 0xd00dfeedu;

	// Structure block tokens.
	constexpr std::uint32_t kTokenBeginNode = 1;
	constexpr std::uint32_t kTokenEndNode = 2;
	constexpr std::uint32_t kTokenProp = 3;
	constexpr std::uint32_t kTokenNop = 4;
	constexpr std::uint32_t kTokenEnd = 9;
}

static constexpr std::uint32_t ReadBe32(const void* p) {
	const auto* b = static_cast<const std::uint8_t*>(p);
	return
		(static_cast<std::uint32_t>(b[0]) << 24) |
		(static_cast<std::uint32_t>(b[1]) << 16) |
		(static_cast<std::uint32_t>(b[2]) << 8) |
		(static_cast<std::uint32_t>(b[3]) << 0);
}

static constexpr std::uint64_t ReadBe64(const void* p) {
	const auto* b = static_cast<const std::uint8_t*>(p);
	return
		(static_cast<std::uint64_t>(b[0]) << 56) |
		(static_cast<std::uint64_t>(b[1]) << 48) |
		(static_cast<std::uint64_t>(b[2]) << 40) |
		(static_cast<std::uint64_t>(b[3]) << 32) |
		(static_cast<std::uint64_t>(b[4]) << 24) |
		(static_cast<std::uint64_t>(b[5]) << 16) |
		(static_cast<std::uint64_t>(b[6]) << 8) |
		(static_cast<std::uint64_t>(b[7]) << 0);
}

static bool IsNullTerminatedStringWithin(const char* str, std::size_t max_len) {
	for (std::size_t i = 0; i < max_len; i++) {
		if (str[i] == '\0') return true;
	}
	return false;
}

static bool StartsWith(const char* str, const char* prefix) {
	while (*prefix) {
		if (*str++ != *prefix++) return false;
	}
	return true;
}

struct FdtView final {
	const std::uint8_t* base;
	std::size_t total_size_bytes;
	std::size_t structure_offset_bytes;
	std::size_t structure_size_bytes;
	std::size_t strings_offset_bytes;
	std::size_t strings_size_bytes;
	std::size_t mem_rsvmap_offset_bytes;
};

static bool TryMakeFdtView(const void* device_tree_blob, FdtView* out_view) {
	if (!device_tree_blob || !out_view) return false;
	const auto* header = static_cast<const FdtHeader*>(device_tree_blob);

	const std::uint32_t magic = ReadBe32(&header->magic_be);
	if (magic != Fdt::kMagic) return false;

	const std::size_t total_size_bytes = ReadBe32(&header->totalsize_be);
	if (total_size_bytes < sizeof(FdtHeader)) return false;

	const std::size_t off_dt_struct = ReadBe32(&header->off_dt_struct_be);
	const std::size_t off_dt_strings = ReadBe32(&header->off_dt_strings_be);
	const std::size_t off_mem_rsvmap = ReadBe32(&header->off_mem_rsvmap_be);
	const std::size_t size_dt_struct = ReadBe32(&header->size_dt_struct_be);
	const std::size_t size_dt_strings = ReadBe32(&header->size_dt_strings_be);

	// Conservative bounds checks.
	if (off_dt_struct >= total_size_bytes) return false;
	if (off_dt_strings >= total_size_bytes) return false;
	if (off_mem_rsvmap >= total_size_bytes) return false;
	if ((off_dt_struct + size_dt_struct) > total_size_bytes) return false;
	if ((off_dt_strings + size_dt_strings) > total_size_bytes) return false;

	out_view->base = static_cast<const std::uint8_t*>(device_tree_blob);
	out_view->total_size_bytes = total_size_bytes;
	out_view->structure_offset_bytes = off_dt_struct;
	out_view->structure_size_bytes = size_dt_struct;
	out_view->strings_offset_bytes = off_dt_strings;
	out_view->strings_size_bytes = size_dt_strings;
	out_view->mem_rsvmap_offset_bytes = off_mem_rsvmap;
	return true;
}

static const char* TryGetStringFromStringsBlock(const FdtView& view, std::uint32_t string_offset_bytes) {
	if (string_offset_bytes >= view.strings_size_bytes) return nullptr;
	const char* s = reinterpret_cast<const char*>(view.base + view.strings_offset_bytes + string_offset_bytes);
	const std::size_t max_len = view.strings_size_bytes - string_offset_bytes;
	if (!IsNullTerminatedStringWithin(s, max_len)) return nullptr;
	return s;
}

static bool TryReadU32Property(const std::uint8_t* value, std::uint32_t value_size_bytes, std::uint32_t* out_u32) {
	if (!value || !out_u32) return false;
	if (value_size_bytes != 4) return false;
	*out_u32 = ReadBe32(value);
	return true;
}

static bool TryReadAddressSizePairs(
	const std::uint8_t* reg_value,
	std::uint32_t reg_size_bytes,
	std::uint32_t address_cells,
	std::uint32_t size_cells,
	BootMemoryMap* out_map,
	BootMemoryRegion::Type type
) {
	if (!reg_value || !out_map) return false;
	if (address_cells == 0 || size_cells == 0) return false;
	if (address_cells > 2 || size_cells > 2) {
		// For early bring-up we only support up to 64-bit addresses/sizes.
		return false;
	}

	const std::uint32_t cell_size_bytes = 4;
	const std::uint32_t tuple_cells = address_cells + size_cells;
	const std::uint32_t tuple_size_bytes = tuple_cells * cell_size_bytes;
	if (tuple_size_bytes == 0) return false;
	if ((reg_size_bytes % tuple_size_bytes) != 0) return false;

	for (std::uint32_t offset = 0; offset < reg_size_bytes; offset += tuple_size_bytes) {
		const std::uint8_t* tuple = reg_value + offset;

		std::uint64_t address = 0;
		std::uint64_t size = 0;

		// Address is encoded as N big-endian 32-bit cells.
		if (address_cells == 1) {
			address = ReadBe32(tuple);
		} else {
			address = ReadBe64(tuple);
		}

		// Size follows immediately after address.
		const std::uint8_t* size_ptr = tuple + (address_cells * cell_size_bytes);
		if (size_cells == 1) {
			size = ReadBe32(size_ptr);
		} else {
			size = ReadBe64(size_ptr);
		}

		if (size == 0) continue;
		if (!out_map->AddRegion(BootMemoryRegion{.physical_base = address, .size_bytes = size, .type = type})) {
			return false;
		}
	}

	return true;
}

struct Cursor final {
	const std::uint8_t* p;
	const std::uint8_t* end;
};

static bool CursorHasBytes(const Cursor& c, std::size_t n) {
	return (c.p + n) <= c.end;
}

static bool CursorReadBe32(Cursor* c, std::uint32_t* out_u32) {
	if (!c || !out_u32) return false;
	if (!CursorHasBytes(*c, 4)) return false;
	*out_u32 = ReadBe32(c->p);
	c->p += 4;
	return true;
}

static bool CursorSkip(Cursor* c, std::size_t n) {
	if (!c) return false;
	if (!CursorHasBytes(*c, n)) return false;
	c->p += n;
	return true;
}

static bool CursorAlignTo(Cursor* c, std::size_t alignment) {
	if (!c) return false;
	const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(c->p);
	const std::uintptr_t aligned = (addr + (alignment - 1)) & ~(alignment - 1);
	const std::size_t delta = static_cast<std::size_t>(aligned - addr);
	return CursorSkip(c, delta);
}

// Extract a C string from the structure block (node name) and advance the cursor.
// The name is NUL-terminated and padded to a 4-byte boundary.
static bool CursorReadNodeName(Cursor* c, const char** out_name) {
	if (!c || !out_name) return false;
	const char* name = reinterpret_cast<const char*>(c->p);
	const std::size_t remaining = static_cast<std::size_t>(c->end - c->p);
	if (!IsNullTerminatedStringWithin(name, remaining)) return false;

	// Skip over the string including its NUL terminator.
	std::size_t len = 0;
	while (name[len] != '\0') len++;
	len += 1;
	if (!CursorSkip(c, len)) return false;
	if (!CursorAlignTo(c, 4)) return false;
	*out_name = name;
	return true;
}

// Parses the DTB "memreserve" table and adds regions as Reserved.
static bool ParseMemReserveTable(const FdtView& view, BootMemoryMap* out_map) {
	if (!out_map) return false;

	// memreserve map is a sequence of (address, size) pairs, each 64-bit big-endian.
	// It is terminated by a pair of zeros.
	Cursor c{
		.p = view.base + view.mem_rsvmap_offset_bytes,
		.end = view.base + view.total_size_bytes,
	};

	for (;;) {
		if (!CursorHasBytes(c, 16)) return false;
		const std::uint64_t address = ReadBe64(c.p);
		const std::uint64_t size = ReadBe64(c.p + 8);
		c.p += 16;

		if (address == 0 && size == 0) break;
		if (size == 0) continue;

		if (!out_map->AddRegion(BootMemoryRegion{.physical_base = address, .size_bytes = size, .type = BootMemoryRegion::Type::Reserved})) {
			return false;
		}
	}

	return true;
}

struct NodeContext final {
	std::uint32_t address_cells;
	std::uint32_t size_cells;
};

// The structure parser above needs to know the current node name when it sees a
// property. To keep the core parse loop readable, we do a second implementation
// that tracks node names explicitly, and call that instead.
static bool ParseStructureBlockWithNodeNames(const FdtView& view, BootMemoryMap* out_map) {
	if (!out_map) return false;

	Cursor c{
		.p = view.base + view.structure_offset_bytes,
		.end = view.base + view.structure_offset_bytes + view.structure_size_bytes,
	};

	NodeContext ctx_stack[32]{};
	const char* name_stack[32]{};
	std::size_t depth = 0;

	// Defaults per the devicetree specification when not provided.
	ctx_stack[0] = NodeContext{.address_cells = 2, .size_cells = 1};
	name_stack[0] = "";

	NodeContext reserved_memory_ctx = ctx_stack[0];
	bool in_reserved_memory_node = false;

	for (;;) {
		std::uint32_t token = 0;
		if (!CursorReadBe32(&c, &token)) return false;

		switch (token) {
			case Fdt::kTokenBeginNode: {
				const char* node_name = nullptr;
				if (!CursorReadNodeName(&c, &node_name)) return false;

				if (depth + 1 >= (sizeof(ctx_stack) / sizeof(ctx_stack[0]))) return false;

				// Inherit parent context.
				ctx_stack[depth + 1] = ctx_stack[depth];
				name_stack[depth + 1] = node_name;
				depth++;

				if (depth == 1) {
					// Root node begins here. Defaults already set.
				}

				if (depth == 2 && StartsWith(node_name, "reserved-memory")) {
					in_reserved_memory_node = true;
					reserved_memory_ctx = ctx_stack[depth];
				}

				break;
			}
			case Fdt::kTokenEndNode: {
				if (depth == 0) return false;
				if (depth == 2 && in_reserved_memory_node) in_reserved_memory_node = false;
				depth--;
				break;
			}
			case Fdt::kTokenProp: {
				std::uint32_t len_bytes = 0;
				std::uint32_t nameoff = 0;
				if (!CursorReadBe32(&c, &len_bytes)) return false;
				if (!CursorReadBe32(&c, &nameoff)) return false;
				if (!CursorHasBytes(c, len_bytes)) return false;

				const char* prop_name = TryGetStringFromStringsBlock(view, nameoff);
				if (!prop_name) return false;

				const std::uint8_t* value = c.p;
				if (!CursorSkip(&c, len_bytes)) return false;
				if (!CursorAlignTo(&c, 4)) return false;

				NodeContext& ctx = ctx_stack[depth];

				// Root / node context configuration.
				if (depth == 1 && StartsWith(prop_name, "#address-cells")) {
					std::uint32_t v = 0;
					if (TryReadU32Property(value, len_bytes, &v)) ctx.address_cells = v;
				}
				if (depth == 1 && StartsWith(prop_name, "#size-cells")) {
					std::uint32_t v = 0;
					if (TryReadU32Property(value, len_bytes, &v)) ctx.size_cells = v;
				}

				if (depth == 2 && in_reserved_memory_node && StartsWith(prop_name, "#address-cells")) {
					std::uint32_t v = 0;
					if (TryReadU32Property(value, len_bytes, &v)) reserved_memory_ctx.address_cells = v;
				}
				if (depth == 2 && in_reserved_memory_node && StartsWith(prop_name, "#size-cells")) {
					std::uint32_t v = 0;
					if (TryReadU32Property(value, len_bytes, &v)) reserved_memory_ctx.size_cells = v;
				}

				// Usable RAM discovery: /memory node.
				if (depth == 2 && StartsWith(prop_name, "reg")) {
					const char* node_name = name_stack[depth];
					const bool looks_like_memory_node =
						(node_name && (StartsWith(node_name, "memory@") || (node_name[0] == 'm' && StartsWith(node_name, "memory"))));

					if (looks_like_memory_node) {
						if (!TryReadAddressSizePairs(value, len_bytes, ctx.address_cells, ctx.size_cells, out_map, BootMemoryRegion::Type::UsableRAM)) {
							return false;
						}
					}
				}

				// Reserved memory discovery: /reserved-memory children reg.
				if (in_reserved_memory_node && depth >= 3 && StartsWith(prop_name, "reg")) {
					if (!TryReadAddressSizePairs(value, len_bytes, reserved_memory_ctx.address_cells, reserved_memory_ctx.size_cells, out_map, BootMemoryRegion::Type::Reserved)) {
						return false;
					}
				}

				break;
			}
			case Fdt::kTokenNop: {
				break;
			}
			case Fdt::kTokenEnd: {
				return true;
			}
			default:
				return false;
		}
	}
}

} // namespace

bool BootMemoryMap::AddRegion(BootMemoryRegion region) {
	if (region.size_bytes == 0) return false;
	if ((region.physical_base + region.size_bytes) < region.physical_base) return false;

	// Merge simple cases to keep the map small.
	//
	// Policy: only merge if:
	// - types match
	// - ranges are exactly adjacent
	for (std::size_t i = 0; i < region_count; i++) {
		BootMemoryRegion& existing = regions[i];
		if (existing.type != region.type) continue;

		if ((existing.physical_base + existing.size_bytes) < existing.physical_base) continue;
		const std::uint64_t existing_end = existing.physical_base + existing.size_bytes;
		const std::uint64_t new_end = region.physical_base + region.size_bytes;

		if (existing_end == region.physical_base) {
			existing.size_bytes = new_end - existing.physical_base;
			return true;
		}
		if (new_end == existing.physical_base) {
			existing.physical_base = region.physical_base;
			existing.size_bytes = existing_end - existing.physical_base;
			return true;
		}
	}

	if (region_count >= kMaxRegions) return false;
	regions[region_count++] = region;
	return true;
}

bool BootMemoryMap::LooksLikeDeviceTreeBlob(const void* device_tree_blob) {
	FdtView view{};
	return TryMakeFdtView(device_tree_blob, &view);
}

std::size_t BootMemoryMap::DeviceTreeTotalSizeBytesOrZero(const void* device_tree_blob) {
	FdtView view{};
	if (!TryMakeFdtView(device_tree_blob, &view)) return 0;
	return view.total_size_bytes;
}

bool BootMemoryMap::TryParseFromDeviceTree(const void* device_tree_blob) {
	Clear();

	FdtView view{};
	if (!TryMakeFdtView(device_tree_blob, &view)) return false;

	// 1) Parse reserved ranges from the memreserve table.
	if (!ParseMemReserveTable(view, this)) return false;

	// 2) Parse the structure block for /memory and /reserved-memory.
	if (!ParseStructureBlockWithNodeNames(view, this)) return false;

	return true;
}

} // namespace Rocinante::Memory
