#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    int time;

    if (argc != 2){
        fprintf(2, "Usage: sleep time\n");
        exit(1);
    }

    time = atoi(argv[1]);

    int ret = sleep(time);
    if(ret < 0){
        fprintf(2, "sleep syscall err %d\n", ret);
        exit(1);
    }

    exit(0);
}
