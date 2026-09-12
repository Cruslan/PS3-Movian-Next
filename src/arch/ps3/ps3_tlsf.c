/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <string.h>
#include <assert.h>

/**
 * PPU LV2 Kernel Services Header (PSL1GHT v2):
 * Exposes Lv2Syscall6 and memory virtualization primitives for virtual memory allocation
 * (sys_mmapper_allocate_address / sys_mmapper_allocate_shared_memory).
 */
#include <ppu-lv2.h>
#include <malloc.h>
#include <limits.h>
#include <errno.h>

#include "arch/arch.h"
#include "arch/threads.h"
#include "main.h"
#include "ext/tlsf/tlsf.h"
#include "networking/http_server.h"
#include "arch/halloc.h"

#include <sys/memory.h>

#define MB(x) ((x) * 1024 * 1024)

static int total_avail;

static int memstats(http_connection_t *hc, const char *remain, void *opaque,
		    http_cmd_t method);

static hts_lwmutex_t mutex __attribute__((aligned(8)));
static tlsf_pool gpool;
uint32_t heap_base;

/**
 * @brief Initializes the TLSF dynamic memory allocator on PlayStation 3.
 *
 * Allocates contiguous user memory pages from the GameOS kernel via sysMemoryAllocate
 * (Syscall 348) using 1MB page granularity.
 *
 * Architectural Memory Budget Rationale:
 * Total PS3 XDR RAM is 256MB. The GameOS kernel, hypervisor, and system reservations
 * reserve ~43MB to ~48MB, leaving an effective user-space physical memory ceiling of ~208MB to ~213MB.
 * The hardware video decoder subsystem (libvdec / cellVdec / libavcdec.sprx) allocates
 * its SPU work buffers and frame picture surfaces directly from GameOS user RAM via
 * Lv2Syscall3(348, allocsize, 0x400, &taddr). For H.264 High Profile Level 4.2 at 1080p,
 * dec_attr.mem_size requires 57,299,581 bytes (~58MB aligned to 1MB pages). For MPEG-2 MP@HL,
 * it requires ~24MB. Additionally, the RSX shared host I/O command buffer window consumes 8MB.
 *
 * If TLSF requests 192MB or 160MB at boot, the committed address space leaves fewer than 20MB
 * of physical memory, causing sys_memory_allocate to inevitably fail with CELL_ENOMEM (0x80010004)
 * when opening AVC or MPEG-2 videos.
 *
 * By sizing the primary TLSF heap pool to 96MB:
 *   96MB (TLSF Heap) + 8MB (RSX Host I/O) + 58MB (cellVdec AVC L4.2) + ~8MB (PPU Thread Stacks) = 170MB,
 * which comfortably resides well within the ~210MB ceiling, leaving ~40MB of uncommitted safety headroom.
 *
 * @complexity Time: O(1) page allocation and pool initialization. Space: O(size) physical RAM mapped.
 */
static void mallocsetup(void)
{
  if(gpool != NULL)
    return;

  hts_lwmutex_init(&mutex);

  sys_mem_addr_t addr = 0;
  int size = MB(96);

  /* Attempt primary 96MB allocation, leaving 100MB+ for hardware video decoding */
  s32 r = sysMemoryAllocate(size, SYS_MEMORY_PAGE_SIZE_1M, &addr);
  if(r != 0) {
    size = MB(80);
    r = sysMemoryAllocate(size, SYS_MEMORY_PAGE_SIZE_1M, &addr);
  }
  if(r != 0) {
    size = MB(64);
    r = sysMemoryAllocate(size, SYS_MEMORY_PAGE_SIZE_1M, &addr);
  }
  if(r != 0) {
    size = MB(48);
    r = sysMemoryAllocate(size, SYS_MEMORY_PAGE_SIZE_1M, &addr);
  }
  if(r != 0) {
    panic("sysMemoryAllocate failed: error 0x%x", r);
  }

  heap_base = (uint32_t)addr;
  total_avail = size;
  gpool = tlsf_create((void *)(intptr_t)heap_base, size);
  if(gpool == NULL) {
    panic("tlsf_create failed on heap_base 0x%x with size %d", heap_base, size);
  }
}

/**
 * High-priority constructor (priority 101) ensures mallocsetup runs
 * before other constructors that may perform early dynamic allocations.
 */
static void __attribute__((constructor(101))) mallocsetup_ctor(void)
{
  mallocsetup();
}


/**
 *
 */
static void
tlsf_stats_init(void)
{
  http_path_add("/api/memstats", NULL, memstats, 1);
}

INITME(INIT_GROUP_API, tlsf_stats_init, NULL, 0);


typedef struct memstats {
  int used;
  int free;
  int used_segs;
  int free_segs;

  uint16_t hist_used[33];
  uint16_t hist_free[33];

} memstats_t;


static void
mywalker(void *ptr, size_t size, int used, void *user)
{
  memstats_t *ms = user;
  const int clz = __builtin_clz(size);

  if(used) {
    ms->used += size;
    ms->used_segs++;
    ms->hist_used[clz]++;
  } else {
    ms->free += size;
    ms->free_segs++;
    ms->hist_free[clz]++;
  }
}


struct mallinfo mallinfo(void)
{
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();
  struct mallinfo mi;
  mi.arena =  total_avail;
  hts_lwmutex_lock(&mutex);
  mi.uordblks = tlsf_used(gpool);
  hts_lwmutex_unlock(&mutex);
  mi.fordblks = mi.arena - mi.uordblks;
  return mi;
}


static void
memtrace(void)
{
  memstats_t ms = {0};
  hts_lwmutex_lock(&mutex);
  tlsf_walk_heap(gpool, mywalker, &ms);
  hts_lwmutex_unlock(&mutex);

  tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
        "Memory allocator status -- Used: %d (%d segs) Free: %d (%d segs)",
        ms.used, ms.used_segs, ms.free, ms.free_segs);

  for(int i = 0; i < 33; i++) {
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
          "%2d: %8d %8d",
          i, ms.hist_used[i], ms.hist_free[i]);
  }
}

void *malloc(size_t bytes)
{
  void *r;
  if(bytes == 0)
    return NULL;

  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  r = tlsf_malloc(gpool, bytes);
  hts_lwmutex_unlock(&mutex);
  if(r == NULL) {
    memtrace();
    panic("OOM: malloc(%d)", (int)bytes);
  }
  return r;
}

#define ROUND_UP(p, round) ((p + (round) - 1) & ~((round) - 1))


void free(void *ptr)
{
  if(ptr == NULL)
    return;

  if(__builtin_expect(gpool == NULL, 0))
    return;

  hts_lwmutex_lock(&mutex);
  tlsf_free(gpool, ptr);
  hts_lwmutex_unlock(&mutex);
}


void *realloc(void *ptr, size_t bytes)
{
  void *r;

  if(bytes == 0) {
    free(ptr);
    return NULL;
  }

  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  r = tlsf_realloc(gpool, ptr, bytes);
  hts_lwmutex_unlock(&mutex);
  if(r == NULL) {
    memtrace();
    panic("OOM: realloc(%p, %d)", ptr, (int)bytes);
  }
  return r;
}


void *memalign(size_t align, size_t bytes)
{
  void *r;
  if(bytes == 0)
    return NULL;

  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  r = tlsf_memalign(gpool, align, bytes);
  hts_lwmutex_unlock(&mutex);
  if(r == NULL) {
    memtrace();
    panic("OOM: memalign(%d, %d)", (int)align, (int)bytes);
  }
  return r;
}


/**
 * @brief Allocates zero-initialized contiguous memory from the TLSF heap.
 *
 * Implements the standard POSIX calloc interface. To prevent GCC's loop distribution
 * and builtin optimizer from recursively replacing `malloc(...) + memset(...)` with an
 * infinite self-call to `calloc()`, this implementation directly acquires the TLSF
 * pool allocation and applies a compiler memory barrier after memset.
 *
 * @param nmemb Number of elements to allocate.
 * @param bytes Size of each element in bytes.
 * @return Pointer to zeroed allocated memory block, or panics on OOM.
 * @complexity Time: O(1) TLSF allocation + O(N) zeroing where N = nmemb * bytes. Space: O(N).
 */
__attribute__((optimize("no-tree-loop-distribute-patterns"), noinline))
void *calloc(size_t nmemb, size_t bytes)
{
  size_t total = bytes * nmemb;
  if(total == 0)
    return NULL;

  /* Defensive check ensuring the global TLSF heap pool is active */
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  void *r = tlsf_malloc(gpool, total);
  hts_lwmutex_unlock(&mutex);

  if(r == NULL) {
    memtrace();
    panic("OOM: calloc(%d, %d)", (int)nmemb, (int)bytes);
  }

  /* Explicitly clear the allocated buffer */
  memset(r, 0, total);

  /* Memory clobber barrier preventing GCC optimizer from pattern-matching into calloc */
  __asm__ __volatile__("" : : "r"(r) : "memory");

  return r;
}





void _free_r(struct _reent *r, void *ptr);
void _free_r(struct _reent *r, void *ptr)
{
	free(ptr);
}

void *_malloc_r(struct _reent *r, size_t size);
void *_malloc_r(struct _reent *r, size_t size)
{
	return malloc(size);
}

void *_calloc_r(struct _reent *r, size_t nmemb, size_t size);
void *_calloc_r(struct _reent *r, size_t nmemb, size_t size)
{
	return calloc(nmemb, size);
}

void *_realloc_r(struct _reent *r, void *ptr, size_t size);
void *_realloc_r(struct _reent *r, void *ptr, size_t size)
{
	return realloc(ptr, size);
}



typedef struct {
  int size;
  int used;
} seginfo_t;

typedef struct {
  int count;
  seginfo_t *ptr;
} allsegs_t;


static int seginfo_cmp(const void *A, const void *B)
{
  const seginfo_t *a = A;
  const seginfo_t *b = B;
  if(a->used == b->used)
    return a->size - b->size;
  return a->used - b->used;
}


static void
list_all_segs_walk(void *ptr, size_t size, int used, void *user)
{
  allsegs_t *as = user;

  if(as->ptr != NULL) {
    as->ptr[as->count].size = size;
    as->ptr[as->count].used = used;
  }
  as->count++;
}


static int
memstats(http_connection_t *hc, const char *remain, void *opaque,
	 http_cmd_t method)
{
  htsbuf_queue_t out;
  allsegs_t as = {};

  hts_lwmutex_lock(&mutex);
  tlsf_walk_heap(gpool, list_all_segs_walk, &as);
  int size = as.count * sizeof(seginfo_t);
  as.ptr = halloc(size);
  as.count = 0;
  tlsf_walk_heap(gpool, list_all_segs_walk, &as);
  hts_lwmutex_unlock(&mutex);

  qsort(as.ptr, as.count, sizeof(seginfo_t), seginfo_cmp);

  htsbuf_queue_init(&out, 0);

  htsbuf_qprintf(&out, "%d segments ptr=%p\n\n", as.count, as.ptr);
  int lastsize = -1;
  int dup = 0;
  for(int i = 0; i < as.count; i++) {
    if(as.ptr[i].size == lastsize && i != as.count - 1) {
      dup++;
    } else {
      htsbuf_qprintf(&out, "%s %10d * %d\n",
		     as.ptr[i].used ? "Used" : "Free", as.ptr[i].size,
		     dup + 1);
      dup = 0;
    }
    lastsize = as.ptr[i].size;
  }

  hfree(as.ptr, size);
  

  return http_send_reply(hc, 0, "text/plain", NULL, NULL, 0, &out);
}

void verify_heap(void);


void 
verify_heap(void)
{
  hts_lwmutex_lock(&mutex);
  int r = tlsf_check_heap(gpool);
  hts_lwmutex_unlock(&mutex);

  if(r)
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "HEAPCHECK", "Heap check verify failed");
  else
    tracelog(TRACE_NO_PROP, TRACE_DEBUG, "HEAPCHECK", "Heap OK");
}


void *
mymalloc(size_t bytes)
{
  if(bytes == 0)
    return NULL;

  /* Ensure TLSF dynamic heap pool is initialized before first allocation */
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  void *r = tlsf_malloc(gpool, bytes);
  hts_lwmutex_unlock(&mutex);

  if(r == NULL) {
    memtrace();
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
          "malloc(%d) failed", (int)bytes);
    errno = ENOMEM;
  }
  return r;
}

void *
myrealloc(void *ptr, size_t bytes)
{
  /* Ensure TLSF dynamic heap pool is initialized before reallocating */
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  void *r = tlsf_realloc(gpool, ptr, bytes);
  hts_lwmutex_unlock(&mutex);

  if(r == NULL) {
    memtrace();
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
          "realloc(%d) failed", (int)bytes);
    errno = ENOMEM;
  }
  return r;
}

void *
mycalloc(size_t nmemb, size_t bytes)
{
  /* Ensure TLSF dynamic heap pool is initialized before calloc allocation */
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  void *r = mymalloc(bytes * nmemb);
  if(r != NULL) {
    memset(r, 0, bytes * nmemb);
  } else {
    memtrace();
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
          "calloc(%d,%d) failed", (int)nmemb, (int)bytes);
    errno = ENOMEM;
  }
  return r;
}


void *
mymemalign(size_t align, size_t bytes)
{
  if(bytes == 0)
    return NULL;

  /* Ensure TLSF dynamic heap pool is initialized before aligned allocation */
  if(__builtin_expect(gpool == NULL, 0))
    mallocsetup();

  hts_lwmutex_lock(&mutex);
  void *r = tlsf_memalign(gpool, align, bytes);
  hts_lwmutex_unlock(&mutex);
  if(r == NULL) {
    memtrace();
    tracelog(TRACE_NO_PROP, TRACE_ERROR, "MEMORY",
          "memalign(%d,%d) failed", (int)align, (int)bytes);
    errno = ENOMEM;
  }
  return r;
}


void myfree(void *ptr);

void
myfree(void *ptr)
{
  if(ptr == NULL)
    return;

  /* If heap is uninitialized or torn down, ignore dangling deallocations */
  if(__builtin_expect(gpool == NULL, 0))
    return;

  hts_lwmutex_lock(&mutex);
  tlsf_free(gpool, ptr);
  hts_lwmutex_unlock(&mutex);
}


size_t
arch_malloc_size(void *ptr)
{
  if(ptr == NULL || __builtin_expect(gpool == NULL, 0))
    return 0;
  return tlsf_block_size(ptr);
}


