struct list{
    struct list *prev;
    struct list *next;
};

typedef uint64 size_t;

#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE*)0)->MEMBER)
#define container_of(ptr, type, member) ({          \
        const typeof( ((type *)0)->member ) *__mptr = (const typeof( ((type *)0)->member ) *)(ptr); \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_del_for_each(pos, head) \
    for (pos = (head)->next->next; pos->prev != (head); pos = pos->next)

#define INIT_LIST(head) struct list head = { \
                                    .prev = &head,\
                                    .next = &head,\
                                }

static inline void init_list(struct list *head){
    head->next = head;
    head->prev = head;
}

static inline int list_empty(struct list *head){
    return (head->next == head);
}

static inline void list_add_head(struct list *head, struct list *node){
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static inline void list_add_tail(struct list *head, struct list *node){
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static inline void list_del(struct list *node){
    node->next->prev = node->prev;
    node->prev->next = node->next;
    init_list(node);
}

// change head1 to head2
static inline void list_chg_head(struct list *head1, struct list *head2){
    if(list_empty(head1)){
        init_list(head2);
        return;
    }
    *head2 = *head1;
    head1->next->prev = head2;
    head1->prev->next = head2;
    init_list(head1);
}
