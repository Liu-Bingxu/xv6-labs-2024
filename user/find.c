#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void find(char *dirpath, char *filename){
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if((fd = open(dirpath, O_RDONLY)) < 0){
        fprintf(2, "find: cannot open %s\n", dirpath);
        return;
    }

    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", dirpath);
        close(fd);
        return;
    }
    switch(st.type){
        case T_DEVICE:
        case T_FILE:
            fprintf(2, "find: dirpath %s is no a dir\n", dirpath);
            close(fd);
            return;

        case T_DIR:
            if(strlen(dirpath) + 1 + DIRSIZ + 1 > sizeof(buf)){
                printf("find: path too long\n");
                break;
            }
            strcpy(buf, dirpath);
            p = buf + strlen(buf);
            *p++ = '/';
            while(read(fd, &de, sizeof(de)) == sizeof(de)){
                if(de.inum == 0)
                    continue;
                if(strcmp(de.name, ".") == 0)
                    continue;
                if(strcmp(de.name, "..") == 0)
                    continue;
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;
                if(stat(buf, &st) < 0){
                    printf("find: cannot stat %s\n", buf);
                    continue;
                }
                switch(st.type){
                    case T_DEVICE:
                    case T_FILE:
                        if(filename == 0){
                            printf("%s\n", buf);
                        }else if(strcmp(filename, de.name) == 0){
                            printf("%s\n", buf);
                        }
                        break;

                    case T_DIR:
                        find(buf, filename);
                }
            }
            break;
    }
    close(fd);
}

int main(int argc, char *argv[]){
    if(argc < 2){
        find(".", 0);
        exit(0);
    }else if(argc < 3){
        find(argv[1], 0);
        exit(0);
    }else if(argc == 3){
        find(argv[1], argv[2]);
        exit(0);
    }else if(argc > 3){
        fprintf(2, "Usage: find [dirname] [filename]\n");
        exit(0);
    }
}
