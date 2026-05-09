struct page{
    struct list page_list;
    struct inode *ip;
    uint off;
    uint ref;
};

