#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int get_one_arg(char *arg, uint64 buf_len){
    uint8 pos = 0;
    int ret = 0;
    while(1){
        ret = read(0, &arg[pos], 1);
        if(ret == 0){
            arg[pos] = '\0';
            return 2;
        }
        else if(ret != 1){
            fprintf(2, "xargs: read error %d\n", ret);
            return -1;
        }
        else if(arg[pos] == ' '){
            arg[pos] = '\0';
            return 0;
        }
        else if(arg[pos] == '\n'){
            arg[pos] = '\0';
            return 1;
        }
        pos++;
        if(pos == buf_len){
            fprintf(2, "xargs: read arg too big\n");
            return -1;
        }
    };
}

int start_one_progam(char *new_argv[MAXARG], int argc){
    int ret = 0;
    int has_arg = 0;
    char arg[MAXARG][128];
    while(1){
        ret = get_one_arg(arg[argc], sizeof(arg[argc]));
        switch (ret){
            case -1:
                return -1;
            case 0:
                if(argc == (MAXARG - 2)){
                    fprintf(2, "xargs%d: arg too many\n", __LINE__);
                    return -1;
                }
                new_argv[argc] = arg[argc];
                argc++;
                break;
            case 2:
                if(has_arg == 0)
                    return 2;
            case 1:
                if(argc == (MAXARG - 1)){
                    fprintf(2, "xargs%d: arg too many\n", __LINE__);
                    return -1;
                }
                new_argv[argc] = arg[argc];
                new_argv[argc + 1] = 0;
                ret = fork();
                if(ret == 0){
                    exec(new_argv[0], new_argv);
                }else if(ret > 0){
                    wait(0);
                    return ret;
                }else{
                    fprintf(2, "xargs: fork error\n");
                    return -1;
                }
                break;
            default:
                break;
        }
        has_arg = 1;
    }
    return -2;
}

int main(int argc, char *argv[]){
    if(argc < 2){
        fprintf(2, "Usage: xargs COMMAND [INITIAL-ARGS]\n");
        exit(-1);
    }
    char *new_argv[MAXARG];
    for(uint16 i = 1; i < argc; i++){
        new_argv[i - 1] = argv[i];
    }
    int ret;
    while(1){
        ret = start_one_progam(new_argv, (argc - 1));
        switch (ret){
            case -1:
                exit(-1);
            case -2:
                fprintf(2, "xargs: fork unknow error\n");
                exit(-1);
            case 1:
                break;
            case 2:
                exit(0);
            default:
                break;
        }
    }
}
