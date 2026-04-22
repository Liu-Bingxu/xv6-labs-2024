#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    int send[2];
    int recv[2];

    char test = 'c';

    int ret = pipe(send);
    if (ret < 0){
        fprintf(2, "pipe send err: %d\n", ret);
        exit(1);
    }
    ret = pipe(recv);
    if (ret < 0){
        fprintf(2, "pipe recv err: %d\n", ret);
        exit(1);
    }

    int pid = fork();
    if (pid == 0){
        int sonpid = getpid();
        read(send[0], &test, 1);
        printf("%d: received ping\n", sonpid);
        write(recv[1], &test, 1);
        exit(0);
    }
    else if (pid > 0){
        int parentpid = getpid();
        write(send[1], &test, 1);
        read(recv[0], &test, 1);
        printf("%d: received pong\n", parentpid);
        exit(0);
    }
    else{
        fprintf(2, "fork err: %d\n", pid);
        exit(1);
    }
}
