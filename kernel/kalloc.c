// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "list.h"
#include "page.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct page *pages;
struct list  file_page;
struct spinlock page_lock;

#define page_list_entry(page, node) \
    page = container_of(node, struct page, page_list)

uint64 find_and_get_file_page(uint off, struct inode *ip){
    void *pa = kalloc();
    acquire(&page_lock);
    struct list *pos;
    struct page *page;
    list_for_each(pos, &file_page){
        page_list_entry(page, pos);
        if(page->ref < 0)
            panic("find_and_get_file_page: ref error");
        if((page->off == off) && (page->ip == ip)){
            page->ref++;
            release(&page_lock);
            if(pa)
                kfree(pa);
            return (KERNBASE + ((uint64)(page - pages) * PGSIZE));
        }
    }
    page = &pages[((uint64)pa - KERNBASE) / PGSIZE];
    page->off     = off;
    page->ip      = ip;
    page->ref     = 1;
    list_add_head(&file_page, &page->page_list);
    begin_op();
    ilock(page->ip);
    uint n = 0;
    if(page->ip->size < page->off)
        panic("put_file_page off error");
    if((page->ip->size - page->off) < PGSIZE)
        n = (page->ip->size - page->off);
    else 
        n = PGSIZE;
    if (readi(page->ip, 0, (uint64)pa, page->off, n) != n)
        panic("put_file_page: writei");
    if(n != PGSIZE)
        memset((void *)(pa + n), 0, (PGSIZE -n));
    iunlock(page->ip);
    end_op();
    release(&page_lock);
    return (uint64)pa;
}

uint put_file_page(uint64 pa, uint8 dirty){
    struct page *page;
    page = &pages[((uint64)pa - KERNBASE) / PGSIZE];
    acquire(&page_lock);
    page->ref--;
    uint ref = page->ref;
    if(ref == 0){
        list_del(&page->page_list);
    }
    release(&page_lock);
    if(ref < 0)
        panic("put_file_page ref error");
    if(ref == 0){
        if(dirty){
            begin_op();
            ilock(page->ip);
            uint n = 0;
            if(page->ip->size < page->off)
                panic("put_file_page off error");
            if((page->ip->size - page->off) < PGSIZE)
                n = (page->ip->size - page->off);
            else 
                n = PGSIZE;
            if (writei(page->ip, 0, pa, page->off, n) != n)
                panic("put_file_page: writei");
            iunlock(page->ip);
            end_op();
        }
    }
    return ref;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&page_lock, "page");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  pages = (struct page *)p;
  uint64 npages = ((PHYSTOP - KERNBASE) / PGSIZE);
  for(uint64 i = 0; i < npages; i++){
    pages[i].off     = 0;
    pages[i].ip      = 0;
    pages[i].ref     = 0;
    init_list(&pages[i].page_list);
  }
  init_list(&file_page);
  p += ((PHYSTOP - KERNBASE) / PGSIZE * sizeof(struct page));
  p = (char*)PGROUNDUP((uint64)p);
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

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

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

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
