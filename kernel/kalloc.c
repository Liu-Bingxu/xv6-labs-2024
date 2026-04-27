// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct page_ref_struct{
    union {
        uint8 data;
        struct{
            uint8 ref  : 7;
            uint8 save : 1;
        }warp;
    };
};

static struct page_ref_struct *page_ref = 0;
struct spinlock                page_ref_lock;

void page_acquire_lock(void){
    // acquire(&page_ref_lock);
}
void page_release_lock(void){
    // release(&page_ref_lock);
}

uint8 page_get_save(uint64 pa){
    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
        panic("page_get_save");
    }
    acquire(&page_ref_lock);
    uint32 index = (((uint64)pa - KERNBASE) / PGSIZE);
    uint8 ret = page_ref[index].warp.save;
    release(&page_ref_lock);
    return ret;
}

uint8 page_get_ref(uint64 pa){
    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
        panic("page_get_ref");
    }
    acquire(&page_ref_lock);
    uint32 index = (((uint64)pa - KERNBASE) / PGSIZE);
    uint8 ret = page_ref[index].warp.ref;
    release(&page_ref_lock);
    return ret;
}

void page_save(uint64 pa, uint perm){
    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
        panic("page_save");
    }
    acquire(&page_ref_lock);
    uint32 index = (((uint64)pa - KERNBASE) / PGSIZE);
    page_ref[index].warp.save = (perm != 0);
    release(&page_ref_lock);
}
void page_get(uint64 pa){
    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
        panic("page_get");
    }
    acquire(&page_ref_lock);
    uint32 index = (((uint64)pa - KERNBASE) / PGSIZE);
    page_ref[index].warp.ref++;
    release(&page_ref_lock);
}

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  kmem.freelist = 0;
  initlock(&page_ref_lock, "page_ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
    page_ref = (struct page_ref_struct *)p;
    p += (PGSIZE * 8);
    for(struct page_ref_struct *_page_ref = page_ref; (uint64)_page_ref < (uint64)p; _page_ref++){
        _page_ref->data = 0;
    }
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
        panic("kfree");
    }

    acquire(&page_ref_lock);
    uint32 index = (((uint64)pa - KERNBASE) / PGSIZE);
    if(page_ref[index].warp.ref > 1){
        page_ref[index].warp.ref--;
        release(&page_ref_lock);
        return;
    }
    release(&page_ref_lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

    acquire(&page_ref_lock);
    if(r){
        uint32 index = (((uint64)r - KERNBASE) / PGSIZE);
        page_ref[index].warp.ref  = 1;
        page_ref[index].warp.save = 0;
    }
    release(&page_ref_lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
