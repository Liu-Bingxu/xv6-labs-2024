#include "kernel/types.h"
#include "user/user.h"

void primes(int father_fd){
    int test_num = 0;
    int test_base = 0;
    uint16 test_cnt = 0;
    int ret = 0;
    int sonfd = -1;
    while(1){
        ret = read(father_fd, &test_num, sizeof(test_num));
        if(ret < 0){
            fprintf(2, "read err: %d\n", ret);
            exit(1);
        }else if(ret == 0){
            break;
        }
        if(test_cnt == 0){
            printf("prime %d\n", test_num);
            test_base = test_num;
        }else if((test_num % test_base) != 0){
            if(sonfd == -1){
                int msg[2];
                ret = pipe(msg);
                if (ret < 0){
                    fprintf(2, "pipe msg err: %d\n", ret);
                    exit(1);
                }
                int pid = fork();
                if (pid == 0){
                    close(father_fd);
                    close(msg[1]);
                    primes(msg[0]);
                }else if (pid < 0){
                    fprintf(2, "%d-fork err: %d\n", __LINE__, pid);
                    exit(1);
                }else{
                    close(msg[0]);
                    sonfd = msg[1];
                    write(sonfd, &test_num, sizeof(test_num));
                }
            }
            else{
                write(sonfd, &test_num, sizeof(test_num));
            }
        }
        test_cnt++;
    };
    close(father_fd);
    if(sonfd != -1){
        close(sonfd);
        do{
            ret = wait(0);
        }while(ret > 0);
    }
    exit(0);
}

int main(int argc, char *argv[]){
    int msg[2];

    int ret = pipe(msg);
    if (ret < 0){
        fprintf(2, "pipe msg err: %d\n", ret);
        exit(1);
    }

    int pid = fork();
    if (pid == 0){
        close(msg[1]);
        primes(msg[0]);
    }else if (pid < 0){
        fprintf(2, "fork err: %d\n", pid);
        exit(1);
    }else{
        close(msg[0]);
    }

    for(int i = 2; i < 281; i++){
        write(msg[1], &i, sizeof(i));
    }
    close(msg[1]);
    do{
        ret = wait(0);
    }while(ret > 0);
    exit(0);
}
