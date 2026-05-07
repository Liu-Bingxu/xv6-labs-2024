#include "param.h"
#include "types.h"
#include "list.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "vm.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

struct vma_struct_run {
     struct vma_struct_run *next;
};

struct {
    struct spinlock lock;
    struct vma_struct_run *freelist;
} vma_list;

struct vma_struct *vma_alloc(void){
    struct vma_struct_run *r;

    acquire(&vma_list.lock);
    r = vma_list.freelist;
    if(r){
        vma_list.freelist = r->next;
    }else{
        struct vma_struct *temp = (struct vma_struct *) kalloc();
        if(temp == 0)
            panic("vma_alloc");
        for(uint64 i = 1; i < (PGSIZE / vma_size); i++){
            r = (struct vma_struct_run *)(&temp[i]);
            r->next = vma_list.freelist;
            vma_list.freelist = r;
        }
        r = (struct vma_struct_run *)temp;
    }
    release(&vma_list.lock);

    memset((char*)r, 0, vma_size);

    return (struct vma_struct *)r;
}
void vma_free(struct vma_struct *vma){
    struct vma_struct_run *r;

    if(vma == 0)
        panic("vma_free");

    r = (struct vma_struct_run *)vma;

    acquire(&vma_list.lock);
    r->next = vma_list.freelist;
    vma_list.freelist = r;
    release(&vma_list.lock);
}
void free_all_vma(struct list *vma){
    struct list *pos = 0;
    struct vma_struct *free_vma = 0;
    list_del_for_each(pos, vma){
        vma_list_entry(free_vma, pos->prev);
        list_del(&free_vma->vma_list);
        vma_free(free_vma);
    }
}

int uvmacopy(struct list *pvma, struct list *npvma, struct proc *p, struct proc *np){
    struct list *pos = 0;
    struct vma_struct *free_vma = 0;
    list_for_each(pos, pvma){
        vma_list_entry(free_vma, pos);
        struct vma_struct *vma = vma_alloc();
        if(vma == 0){
            free_all_vma(npvma);
            return -1;
        }
        *vma = *free_vma;
        init_list(&vma->vma_list);
        vma->p = np;
        list_add_tail(npvma, &vma->vma_list);
        if(p->heap == free_vma)
            np->heap = vma;
    }
    return 0;
}
int do_page_error(uint64 scause, uint64 vaddr){
    struct proc *p = myproc();
    struct list *pos;
    struct vma_struct *vma = 0;
    list_for_each(pos, &p->vma){
        vma_list_entry(vma, pos);
        if((vma->vaddr_start <= vaddr) && (vaddr < vma->vaddr_end)){
            if((scause == 12) && ((vma->vma_port & VM_PROT_EXEC) == 0))
                return -1;
            if((scause == 13) && ((vma->vma_port & VM_PROT_READ) == 0))
                return -1;
            if((scause == 15) && ((vma->vma_port & VM_PROT_WRITE) == 0))
                return -1;
            switch (vma->vma_type){
                case VMA_DDR:
                    int xperm = 0;
                    if(vma->vma_port & VM_PROT_EXEC)
                        xperm |= PTE_X;
                    if(vma->vma_port & VM_PROT_WRITE)
                        xperm |= PTE_W;
                    if(uvmalloc(p->pagetable, PGROUNDDOWN(vaddr), PGSIZE, xperm) == 0)
                        return -1;
                    goto out;
                case VMA_FILE:
                    if(loadseg_from_vma_onepage(p->pagetable, PGROUNDDOWN(vaddr), vma) == 0)
                        return -1;
                    goto out;
                case VMA_MMAP:
                    return -1;
                case VMA_NONE:
                case VMA_START:
                default:
                    return -1;
                    break;  
            }
        }
    }
out:
    return 0;
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
    initlock(&vma_list.lock, "vma_list");
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  struct vma_struct *vma = vma_alloc();
  if(vma == 0)
    panic("uvmfirst couldn't get vma");
  vma->vaddr_start = 0;
  vma->vaddr_end   = sz;
  extern struct proc *initproc;
  vma->p           = initproc;
  init_list(&vma->vma_list);
  vma->off         = 0;
  vma->vma_type    = VMA_START;
  vma->filp        = 0;
  vma->vma_port    = VM_PROT_EXEC | VM_PROT_READ | VM_PROT_WRITE;
  list_add_head(&initproc->vma, &vma->vma_list);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to map process space from heap
// Returns 0 on true or -1 on error.
int vm_growproc(struct proc *p, int n){
    if(n > 0){
        uint64 end = PGROUNDUP(p->heap->vaddr_end);
        uint64 new_end = PGROUNDUP(p->heap->vaddr_end + n);
        if(new_end >= TRAPFRAME)
            return -1;
        p->heap->vaddr_end += n;
        if(new_end == end)
            return 0;
        if(uvmalloc(p->pagetable, end, (new_end - end), PTE_W) == 0){
            p->heap->vaddr_end -= n;
            return -1;
        }
    }else if(n < 0){
        uint64 end = PGROUNDUP(p->heap->vaddr_end);
        uint64 new_end = PGROUNDUP(p->heap->vaddr_end + n);
        p->heap->vaddr_end += n;
        if(new_end == end)
            return 0;
        uvmunmap(p->pagetable, new_end, ((end - new_end) / PGSIZE), 1);
    }

    return 0;
}

// Allocate PTEs and physical memory to map process space from start to start + sz
// which need be page aligned.  Returns 1 on true or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 start, uint64 sz, int xperm)
{
  char *mem;
  uint64 a;

  if((start % PGSIZE) != 0)
    return 0;

  for(a = start; a < (start + sz); a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmunmap(pagetable, start, ((a - start) / PGSIZE), 1);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmunmap(pagetable, start, ((a - start) / PGSIZE), 1);
      return 0;
    }
  }
  return 1;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// uint64
// uvmdealloc(pagetable_t pagetable, uint64 start, uint64 sz)
// {
//   if(newsz >= oldsz)
//     return oldsz;

//   if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
//     int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
//     uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
//   }

//   return newsz;
// }

// Recursively Free user memory pages,
void
freewalk_user_memory(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk_user_memory((pagetable_t)child);
    } else if(pte & PTE_V){
      kfree((void *)(PTE2PA(pte)));
      pagetable[i] = 0;
    }
  }
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable)
{
//   if(sz > 0)
//     uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk_user_memory(pagetable);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < TRAPFRAME; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
//   uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
// void
// uvmclear(pagetable_t pagetable, uint64 va)
// {
//   pte_t *pte;
  
//   pte = walk(pagetable, va, 0);
//   if(pte == 0)
//     panic("uvmclear");
//   *pte &= ~PTE_U;
// }

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0){
      struct list *pos = 0;
      struct proc *p = myproc();
      struct vma_struct *vma = 0;
      list_for_each(pos, &p->vma){
        vma_list_entry(vma, pos);
        if(vma && (vma->vaddr_start <= va0) && (va0 < vma->vaddr_end))
          break;
        vma = 0;
      }
      if(vma == 0)
        return -1;
      pa0 = loadseg_from_vma_onepage(pagetable, va0, vma);
      if(pa0 == 0)
        return -1;
      uint64 perm = PTE_U | PTE_V;
      if(vma->vma_port & VM_PROT_EXEC)
        perm |= PTE_X;
      if(vma->vma_port & VM_PROT_READ)
        perm |= PTE_R;
      if(vma->vma_port & VM_PROT_WRITE)
        perm |= PTE_W;
      if(pte == 0)
        pte = walk(pagetable, va0, 0);
      *pte = PA2PTE(pa0) | perm;
    }
    if((*pte & PTE_U) == 0 || (*pte & PTE_W) == 0)
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      struct list *pos = 0;
      struct proc *p = myproc();
      struct vma_struct *vma = 0;
      list_for_each(pos, &p->vma){
        vma_list_entry(vma, pos);
        if(vma && (vma->vaddr_start <= va0) && (va0 < vma->vaddr_end))
          break;
        vma = 0;
      }
      if(vma == 0)
        return -1;
      pa0 = loadseg_from_vma_onepage(pagetable, va0, vma);
      if(pa0 == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      struct list *pos = 0;
      struct proc *p = myproc();
      struct vma_struct *vma = 0;
      list_for_each(pos, &p->vma){
        vma_list_entry(vma, pos);
        if(vma && (vma->vaddr_start <= va0) && (va0 < vma->vaddr_end))
          break;
        vma = 0;
      }
      if(vma == 0)
        return -1;
      pa0 = loadseg_from_vma_onepage(pagetable, va0, vma);
      if(pa0 == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
