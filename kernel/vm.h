struct vma_struct{
    enum{VMA_NONE, VMA_START, VMA_FILE, VMA_DDR, VMA_MMAP}vma_type;
#define VM_PROT_READ  0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXEC  0x4
    char vma_port;
#define VM_FLAGS_SHARE   0x1
#define VM_FLAGS_PRIVATE 0x2
    char vma_flags;
    uint64 vaddr_start;
    uint64 vaddr_end;
    struct proc *p;
    union{
        struct inode *ip;
        struct file *filp;
    };
// 如果是VMA_FILE，表示elf文件中加载段头的偏移
// 如果是VMA_MMAP，表示文件中的偏移
    uint64 off;
    struct list vma_list;
};

#define vma_list_entry(vma, node) \
    vma = container_of(node, struct vma_struct, vma_list)

#define vma_size (sizeof(struct vma_struct))

struct vma_struct  *vma_alloc(void);
void                vma_free(struct vma_struct  *);
void                free_all_vma(struct list *);
int                 uvmacopy(struct list *, struct list *, struct proc *, struct proc *);
int                 do_page_error(uint64, uint64);
