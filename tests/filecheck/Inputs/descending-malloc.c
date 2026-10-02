/*
 * This file is distributed under the MIT License. See LICENSE.md for details.
 */

/* A malloc that hands out addresses in decreasing order and never reuses
   memory. Preloaded into a program, every container ordered by address
   iterates in the reverse of allocation order, which makes any output that
   depends on such an order differ from a run under the normal allocator. */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_SIZE ((size_t) 1 << 37) /* 128 GiB of address space, reserved lazily */
#define HEADER 16

static char *Base;
static char *Top;

static void init(void) {
  Base = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  Top = Base + ARENA_SIZE;
}

static void *allocate(size_t Size, size_t Alignment) {
  if (Base == NULL)
    init();
  if (Alignment < HEADER)
    Alignment = HEADER;
  uintptr_t P = ((uintptr_t) Top - Size) & ~(uintptr_t) (Alignment - 1);
  if (P - HEADER < (uintptr_t) Base) {
    errno = ENOMEM;
    return NULL;
  }
  *(size_t *) (P - HEADER) = Size;
  Top = (char *) (P - HEADER);
  return (void *) P;
}

static int owned(void *P) {
  return Base != NULL && (char *) P >= Base && (char *) P < Base + ARENA_SIZE;
}

void *malloc(size_t Size) { return allocate(Size, 16); }
void free(void *P) { (void) P; }
void *calloc(size_t N, size_t Size) {
  void *P = allocate(N * Size, 16);
  if (P)
    memset(P, 0, N * Size);
  return P;
}
void *realloc(void *Old, size_t Size) {
  void *P = allocate(Size, 16);
  if (P && Old && owned(Old)) {
    size_t OldSize = *(size_t *) ((char *) Old - HEADER);
    memcpy(P, Old, OldSize < Size ? OldSize : Size);
  }
  return P;
}
void *memalign(size_t A, size_t Size) { return allocate(Size, A); }
void *aligned_alloc(size_t A, size_t Size) { return allocate(Size, A); }
int posix_memalign(void **Out, size_t A, size_t Size) {
  void *P = allocate(Size, A);
  if (!P)
    return ENOMEM;
  *Out = P;
  return 0;
}
void *valloc(size_t Size) { return allocate(Size, 4096); }
void *pvalloc(size_t Size) { return allocate((Size + 4095) & ~(size_t) 4095, 4096); }
size_t malloc_usable_size(void *P) { return owned(P) ? *(size_t *) ((char *) P - HEADER) : 0; }
