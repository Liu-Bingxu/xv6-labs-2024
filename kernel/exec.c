#include "types.h"
#include "list.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "vm.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// int flags2perm(int flags)
// {
//     int perm = 0;
//     if(flags & 0x1)
//       perm = PTE_X;
//     if(flags & 0x2)
//       perm |= PTE_W;
//     return perm;
// }

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sp, ustack[MAXARG], stackbase, stack_botton;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  struct list oldvma;
  list_chg_head(&p->vma, &oldvma);

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  uint64 max_addr = 0;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    // if((ret = uvmalloc(pagetable, ph.vaddr, ph.memsz, flags2perm(ph.flags))) == 0)
    //   goto bad;
    // if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
    //   goto bad;
    struct vma_struct *vma = vma_alloc();
    if(vma == 0)
        goto bad;
    vma->vaddr_start = ph.vaddr;
    vma->vaddr_end   = PGROUNDUP(ph.vaddr + ph.memsz);
    vma->p           = p;
    init_list(&vma->vma_list);
    vma->vma_type    = VMA_FILE;
    vma->ip          = idup(ip);
    vma->off         = off;
    vma->vma_port    = VM_PROT_READ;
    if(ph.flags & 0x1)
      vma->vma_port |= VM_PROT_EXEC;
    if(ph.flags & 0x2)
      vma->vma_port |= VM_PROT_WRITE;
    list_add_head(&p->vma, &vma->vma_list);
    max_addr = (vma->vaddr_end > max_addr) ? vma->vaddr_end : max_addr;
    if((vma->vaddr_start <= elf.entry) && (elf.entry < vma->vaddr_end)){
      if((ph.vaddr + ph.filesz) < elf.entry)
        goto bad;
      if(uvmalloc(pagetable, PGROUNDDOWN(elf.entry), PGSIZE, PTE_X) == 0)
        goto bad;
      uint offset = ph.off + (PGROUNDDOWN(elf.entry) - ph.vaddr);
      uint sz = ((ph.vaddr + ph.filesz - PGROUNDDOWN(elf.entry)) > PGSIZE) ? PGSIZE : (ph.vaddr + ph.filesz - PGROUNDDOWN(elf.entry));
      loadseg(pagetable, PGROUNDDOWN(elf.entry), ip, offset, sz);
    }
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
//   uint64 sz1;
//   if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
//     goto bad;
//   uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  struct vma_struct *vma = vma_alloc();
  if(vma == 0)
      goto bad;
  vma->vaddr_start = max_addr + PGSIZE;
  vma->vaddr_end   = vma->vaddr_start + USERSTACK*PGSIZE;
  vma->p           = p;
  init_list(&vma->vma_list);
  vma->vma_type    = VMA_DDR;
  vma->ip          = 0;
  vma->off         = 0;
  vma->vma_port    = VM_PROT_READ | VM_PROT_WRITE;
  list_add_head(&p->vma, &vma->vma_list);

  vma = vma_alloc();
  if(vma == 0)
      goto bad;
  vma->vaddr_start = max_addr + PGSIZE + USERSTACK*PGSIZE;
  vma->vaddr_end   = vma->vaddr_start;
  vma->p           = p;
  init_list(&vma->vma_list);
  vma->vma_type    = VMA_DDR;
  vma->ip          = 0;
  vma->off         = 0;
  vma->vma_port    = VM_PROT_READ | VM_PROT_WRITE;
  list_add_head(&p->vma, &vma->vma_list);
  p->heap          = vma;

  sp = vma->vaddr_end;
  stackbase = sp - USERSTACK*PGSIZE;
  stack_botton = sp - PGSIZE;
  // only map one page stack in start
  if(uvmalloc(pagetable, stack_botton, PGSIZE, PTE_W) == 0)
    goto bad;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    while(sp < stack_botton){
      stack_botton = stack_botton - PGSIZE;
      if(uvmalloc(pagetable, stack_botton, PGSIZE, PTE_W) == 0)
        goto bad;
    }
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  while(sp < stack_botton){
    stack_botton = stack_botton - PGSIZE;
    if(uvmalloc(pagetable, stack_botton, PGSIZE, PTE_W) == 0)
      goto bad;
  }
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  free_all_vma(&oldvma);
  proc_freepagetable(oldpagetable);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  free_all_vma(&p->vma);
  list_chg_head(&oldvma, &p->vma);
  if(pagetable)
    proc_freepagetable(pagetable);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+PAGESIZE must not be mapped.
// Returns pa on success, 0 on failure.
uint64 loadseg_from_vma_onepage(pagetable_t pagetable, uint64 va, struct vma_struct *vma){
    uint n;
    uint64 pa, new_pa;
    struct proghdr ph;

    if((va % PGSIZE) != 0)
        panic("loadseg_from_vma_onepage: va must be page-aligned");

    pa = walkaddr(pagetable, va);
    if(pa != 0){
        panic("loadseg_from_vma_onepage: address should not exist");
    }
    int perm = 0;
    if(vma->vma_port & VM_PROT_EXEC)
        perm |= PTE_X;
    if(vma->vma_port & VM_PROT_WRITE)
        perm |= PTE_W;
    if(uvmalloc(pagetable, va, PGSIZE, perm) == 0)
        return 0;
    new_pa = walkaddr(pagetable, va);
    if(new_pa == 0)
        panic("loadseg_from_vma_onepage: address should exist");

    begin_op();
    ilock(vma->ip);
    if(readi(vma->ip, 0, (uint64)&ph, vma->off, sizeof(ph)) != sizeof(ph))
        goto bad;
    if((va < ph.vaddr) || ((ph.vaddr + ph.memsz) < va))
        panic("loadseg_from_vma_onepage: vma address error");
    if((ph.vaddr + ph.filesz) < va){
        iunlock(vma->ip);
        end_op();
        return new_pa;
    }else if((ph.vaddr + ph.filesz - va) < PGSIZE){
        n = (ph.vaddr + ph.filesz - va);
    }else{
        n = PGSIZE;
    }
    if(readi(vma->ip, 0, (uint64)new_pa, (va - ph.vaddr) + ph.off, n) != n)
        goto bad;
    iunlock(vma->ip);
    end_op();
    return new_pa;
bad:
    iunlock(vma->ip);
    end_op();
    return 0;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
