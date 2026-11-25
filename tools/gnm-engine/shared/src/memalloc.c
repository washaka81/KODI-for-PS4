#include "memalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

/* Diagnostic section canary: every used section reserves eight bytes past the
 * requested size and checks them on free/destroy.  A texture or vertex buffer
 * that writes past its request corrupts the canary instead of the next
 * section's data.  Keep it inside the section so the allocator API is
 * unchanged; the returned pointer and alignment are the same. */
#define MEM_SECTION_CANARY 0x544345534D345350ULL /* PS4MSECT */
#define MEM_SECTION_CANARY_BYTES 8u

/* Direct-memory blocks and section offsets are kept on this boundary.  Using
 * one common granularity makes a free section reusable by any request whose
 * alignment is <= 64 KiB; otherwise a section reserved for a 4-byte shader
 * could never satisfy a 64 KiB texture request and each request would grow a
 * new block. */
/* PS4 translators keep texel sizes and pitches in 2-byte units (NV12/RG8),
 * but CPU-visible allocations are consumed by SSE/AVX code that assumes the
 * 16-byte alignment malloc provides.  Using 2/4-byte section starts made
 * movaps fault with SIGBUS (FreeBSD signal 10) on the console.  Keep the
 * translator contract (even sizes, 2 B/texel) and align every section to 16. */
#define MEM_MIN_ALIGN 16u

#define MEM_BLOCK_ALIGN (64u * 1024u)

static size_t align_up(size_t value, size_t alignment) {
	if (alignment == 0 || (alignment & (alignment - 1)) != 0)
		return 0;
	if (value > SIZE_MAX - (alignment - 1))
		return 0;
	return (value + alignment - 1) / alignment * alignment;
}

/* sceKernelAllocateDirectMemory wants a power-of-two length on this
 * firmware.  Keep the normal 64 MiB block unchanged and only round up for a
 * single request bigger than the allocator's default block. */
static size_t next_pow2(size_t value) {
	size_t result = 1;
	while (result < value) {
		if (result > SIZE_MAX / 2)
			return 0;
		result <<= 1;
	}
	return result;
}

static void write_section_canary(MemMappedBlock* mb, MemSection* s, size_t requested) {
	s->requested = requested;
	if (requested + MEM_SECTION_CANARY_BYTES > s->len) {
		s->requested = 0;
		return;
	}
	const uint64_t magic = MEM_SECTION_CANARY;
	memcpy((uint8_t*)mb->mappedmem + s->offset + requested, &magic, sizeof(magic));
}

static int check_section_canary(const MemMappedBlock* mb, const MemSection* s,
				const char* where) {
	if (s->requested == 0 || s->requested + MEM_SECTION_CANARY_BYTES > s->len)
		return 1;
	uint64_t magic = 0;
	memcpy(&magic, (const uint8_t*)mb->mappedmem + s->offset + s->requested,
	       sizeof(magic));
	if (magic != MEM_SECTION_CANARY) {
		printf("mem: section canary corrupt where=%s offset=%zu size=%zu got=0x%llx\n",
		       where, s->offset, s->requested, (unsigned long long)magic);
		return 0;
	}
	return 1;
}

MemoryAllocator memalloc_init(size_t blocksize, int prot, int memtype) {
	MemoryAllocator res = {
		.mblocks = uvalloc(sizeof(MemMappedBlock), 0),
		.blocksize = blocksize,
		.protection = prot,
		.memtype = memtype,
	};
	return res;
}

void memalloc_destroy(MemoryAllocator* ma) {
	assert(ma);

	for (size_t i = 0; i < uvlen(&ma->mblocks); i += 1) {
		MemMappedBlock* mb = uvdata(&ma->mblocks, i);
		/* Release the physical range *and* the virtual mapping.  Releasing
		 * only the direct memory leaves the mapping alive and a later
		 * glShimInit() in the same process can run out of address space. */
		for (size_t y = 0; y < uvlen(&mb->sections); y += 1) {
			MemSection* s = uvdata(&mb->sections, y);
			if (s->used)
				check_section_canary(mb, s, "destroy");
		}
		if (mb->mappedmem) {
			const int munmapres = sceKernelMunmap(mb->mappedmem, mb->len);
			if (munmapres != 0)
				printf("mem: sceKernelMunmap failed with %i\n", munmapres);
		}
		const int relres = sceKernelReleaseDirectMemory(mb->dmem, mb->len);
		if (relres != 0)
			printf("mem: sceKernelReleaseDirectMemory failed with %i\n", relres);
		uvfree(&mb->sections);
	}
	uvfree(&ma->mblocks);
}

static MemSection* takevmem(MemMappedBlock* mb, size_t size, size_t alignment) {
	/* Keep every section start at least 16-byte aligned: Kodi/FFmpeg/OpenSSL
	 * use aligned SSE loads on buffers that pass through the shim, and an
	 * unaligned movaps is reported as SIGBUS on the PS4 kernel.  Texel
	 * formats still use 2-byte units; this is only the base alignment. */
	if (alignment < MEM_MIN_ALIGN)
		alignment = MEM_MIN_ALIGN;
	/* A common 64 KiB alignment made every small vertex/shader section
	 * consume a full 64 KiB: under the 395-logo load the shim reached
	 * gmem_used=389 MB / 7 blocks and flex=24 MB.  Use the requested
	 * alignment for small allocations and 64 KiB only for large
	 * allocations (or requests that already ask for >= 64 KiB). */
	const size_t sec_align =
		(size >= MEM_BLOCK_ALIGN || alignment >= MEM_BLOCK_ALIGN)
			? (alignment > MEM_BLOCK_ALIGN ? alignment : MEM_BLOCK_ALIGN)
			: alignment;
	if (sec_align == 0 || size > SIZE_MAX - (sec_align - 1) ||
	    size > SIZE_MAX - MEM_SECTION_CANARY_BYTES)
		return NULL;

	const size_t alignedsize =
		(size + MEM_SECTION_CANARY_BYTES + sec_align - 1) / sec_align * sec_align;

	// try to reuse an empty reserved section.  Large sections are 64 KiB
	// aligned; small sections keep their requested alignment.
	for (size_t i = 0; i < uvlen(&mb->sections); i += 1) {
		MemSection* s = uvdata(&mb->sections, i);
		if (!s->used && s->len >= alignedsize &&
		    (s->offset % alignment) == 0) {
			s->used = true;
			write_section_canary(mb, s, size);
			return s;
		}
	}

	// try to reserve a new section
	if (mb->freelen >= alignedsize) {
		MemSection* lastsec = NULL;
		size_t startoff = 0;
		const size_t numsecs = uvlen(&mb->sections);
		if (numsecs > 0) {
			lastsec = uvdata(&mb->sections, numsecs - 1);
			startoff = lastsec->offset + lastsec->len;
		}

		if (startoff > SIZE_MAX - (sec_align - 1))
			return NULL;
		const size_t alignedoff =
			(startoff + sec_align - 1) / sec_align * sec_align;
		if (alignedoff > SIZE_MAX - alignedsize)
			return NULL;
		const size_t offdiff = alignedoff - startoff;
		if (mb->len >= alignedoff + alignedsize) {
			mb->freelen -= offdiff + alignedsize;

			// add misaligned bytes to the previous section if possible
			if (lastsec) {
				lastsec->len += offdiff;
			}

			MemSection newsec = {
				.offset = alignedoff,
				.len = alignedsize,
				.requested = size,
				.used = true,
			};
			uvappend(&mb->sections, &newsec);

			MemSection* added = uvdata(&mb->sections, numsecs);
			write_section_canary(mb, added, size);
			return added;
		}
	}

	// mapped memory does not have enough size
	return NULL;
}

void* memalloc_alloc(MemoryAllocator* ma, size_t size, size_t alignment) {
	if (!ma || size == 0 || alignment == 0)
		return NULL;
	if ((alignment & (alignment - 1)) != 0)
		return NULL;
	if (alignment < MEM_MIN_ALIGN)
		alignment = MEM_MIN_ALIGN;
	if (size > SIZE_MAX - (alignment - 1) ||
	    size > SIZE_MAX - MEM_SECTION_CANARY_BYTES)
		return NULL;

	// probe all memory blocks for available memory
	for (size_t i = 0; i < uvlen(&ma->mblocks); i += 1) {
		MemMappedBlock* mb = uvdata(&ma->mblocks, i);
		MemSection* s = takevmem(mb, size, alignment);
		if (s) {
			void* ptr = (void*)((uintptr_t)mb->mappedmem + s->offset);
			if (((uintptr_t)ptr & (MEM_MIN_ALIGN - 1)) != 0) {
				printf("mem: misaligned section offset=%zu ptr=%p align=%zu\n",
				       s->offset, ptr, (size_t)MEM_MIN_ALIGN);
				s->used = false;
				return NULL;
			}
			return ptr;
		}
	}

	// no available memory was found,
	// try to reserve memory and map it

	// direct memory's size must be a multiple of 16kB and power of 2
	// the same applies to the alignment
	const size_t alignedsize =
		align_up(size + MEM_SECTION_CANARY_BYTES, alignment);
	if (alignedsize == 0)
		return NULL;
	size_t blockneed = alignedsize > ma->blocksize ? alignedsize : ma->blocksize;
	if (blockneed > SIZE_MAX - (MEM_BLOCK_ALIGN - 1))
		return NULL;
	const size_t blocklen = next_pow2((blockneed + MEM_BLOCK_ALIGN - 1) /
					 MEM_BLOCK_ALIGN * MEM_BLOCK_ALIGN);
	if (blocklen < MEM_BLOCK_ALIGN)
		return NULL;
	MemMappedBlock newmb = {
		.len = blocklen,
		.freelen = blocklen,
		.sections = uvalloc(sizeof(MemSection), 0),
	};

	int res = sceKernelAllocateDirectMemory(
		0, sceKernelGetDirectMemorySize(), blocklen, MEM_BLOCK_ALIGN,
		ma->memtype, &newmb.dmem
	);
	if (res != 0) {
		printf("mem: sceKernelAllocateDirectMemory failed with %i\n", res);
		uvfree(&newmb.sections);
		return NULL;
	}

	res = sceKernelMapDirectMemory(
		&newmb.mappedmem, newmb.len, ma->protection, 0, newmb.dmem,
		MEM_BLOCK_ALIGN
	);
	if (res != 0) {
		printf("mem: sceKernelMapDirectMemory failed with %i\n", res);
		sceKernelReleaseDirectMemory(newmb.dmem, newmb.len);
		uvfree(&newmb.sections);
		return NULL;
	}

	// Take the first section before publishing the block.  If the request
	// cannot fit (for example a size larger than blocksize), release the
	// whole mapping instead of leaking a 64 MiB block per failed call.
	MemSection* s = takevmem(&newmb, size, alignment);
	if (!s) {
		puts("mem: failed to take memory from new allocated memory");
		sceKernelMunmap(newmb.mappedmem, newmb.len);
		sceKernelReleaseDirectMemory(newmb.dmem, newmb.len);
		uvfree(&newmb.sections);
		return NULL;
	}

	uvappend(&ma->mblocks, &newmb);
	return (void*)((uintptr_t)newmb.mappedmem + s->offset);
}

static void coalesce_sections(MemMappedBlock* mb, size_t index) {
	size_t i = index;
	if (i + 1 < uvlen(&mb->sections)) {
		MemSection* s = uvdata(&mb->sections, i);
		MemSection* next = uvdata(&mb->sections, i + 1);
		if (!next->used && s->offset + s->len == next->offset) {
			s->len += next->len;
			uvremove(&mb->sections, i + 1);
		}
	}
	if (i > 0) {
		MemSection* prev = uvdata(&mb->sections, i - 1);
		MemSection* s = uvdata(&mb->sections, i);
		if (!prev->used && !s->used &&
		    prev->offset + prev->len == s->offset) {
			prev->len += s->len;
			uvremove(&mb->sections, i);
		}
	}
}

void memalloc_free(MemoryAllocator* ma, void* ptr) {
	assert(ma);
	assert(ptr);

	for (size_t i = 0; i < uvlen(&ma->mblocks); i += 1) {
		MemMappedBlock* mb = uvdata(&ma->mblocks, i);

		// check if ptr is within range.  The original comparison was
		// inverted (ptr <= base), so memalloc_free() never matched and
		// every texture/buffer free leaked its direct-memory section.
		if ((uintptr_t)ptr < (uintptr_t)mb->mappedmem ||
		    (uintptr_t)ptr >= ((uintptr_t)mb->mappedmem + mb->len)) {
			continue;
		}

		const uintptr_t targetoff = (uintptr_t)ptr - (uintptr_t)mb->mappedmem;

		// get its corresponding section
		for (size_t y = 0; y < uvlen(&mb->sections); y += 1) {
			MemSection* s = uvdata(&mb->sections, y);
			if (s->offset == targetoff) {
				check_section_canary(mb, s, "free");
				// mark section as empty and merge with free neighbours
				s->requested = 0;
				s->used = false;
				coalesce_sections(mb, y);
				return;
			}
		}
	}
}

static size_t section_bytes(const MemoryAllocator* ma, bool used) {
	size_t total = 0;
	if (!ma)
		return 0;
	for (size_t i = 0; i < uvlen(&ma->mblocks); i += 1) {
		const MemMappedBlock* mb = uvdatac(&ma->mblocks, i);
		if (!used)
			total += mb->freelen;
		for (size_t y = 0; y < uvlen(&mb->sections); y += 1) {
			const MemSection* s = uvdatac(&mb->sections, y);
			if (s->used == used)
				total += s->len;
		}
	}
	return total;
}

size_t memalloc_used_bytes(const MemoryAllocator* ma) {
	return section_bytes(ma, true);
}

size_t memalloc_free_bytes(const MemoryAllocator* ma) {
	return section_bytes(ma, false);
}

size_t memalloc_total_bytes(const MemoryAllocator* ma) {
	size_t total = 0;
	if (!ma)
		return 0;
	for (size_t i = 0; i < uvlen(&ma->mblocks); i += 1) {
		const MemMappedBlock* mb = uvdatac(&ma->mblocks, i);
		total += mb->len;
	}
	return total;
}
