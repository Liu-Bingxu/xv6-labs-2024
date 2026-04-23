#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int main(int argc, char *argv[]){
    char *end = 0;
    while(1){
        end = sbrk(PGSIZE);
        if(memcmp(end + 8, "very very secret pw is: ", 24) == 0){
            break;
        }
    }
    write(2, end + 32, 64);
    exit(0);
}
