#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "fs-sim.h"

#define DISK_SIZE 128 * 1024
#define BLOCK_SIZE 1024
#define NUM_BLOCKS 128
#define NUM_INODES 126

Superblock superblock;
FILE *disk;
char buffer[BLOCK_SIZE];
char current_directory[5] = ".";

// Forward declarations
bool check_consistency();
int find_free_blocks(int size);
bool is_block_used(int block);
void mark_blocks_used(int start, int size);
void mark_blocks_free(int start, int size);

void fs_mount(char *new_disk_name) {
    disk = fopen(new_disk_name, "rb+");
    if (disk == NULL) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    fread(&superblock, sizeof(Superblock), 1, disk);
    if (check_consistency()) {
        fclose(disk);
        disk = NULL;
        fprintf(stderr, "Error: File system in %s is inconsistent (error code: %d)\n", new_disk_name, check_consistency());
        return;
    }

    strcpy(current_directory, ".");
}

bool check_consistency() {
    // Check if free inodes are zero
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) == 0) {
            if (superblock.inode[i].used_size != 0 || superblock.inode[i].start_block != 0 || superblock.inode[i].dir_parent != 127) {
                return 1;
            }
        }
    }

    // Check start block and size of files
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && (superblock.inode[i].dir_parent & 0x80) == 0) {
            if (superblock.inode[i].start_block < 1 || superblock.inode[i].start_block > 127) {
                return 2;
            }
            if (superblock.inode[i].used_size & 0x7F > 0 && superblock.inode[i].start_block + (superblock.inode[i].used_size & 0x7F) - 1 > 127) {
                return 2;
            }
        }
    }

    // Check directories
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && (superblock.inode[i].dir_parent & 0x80) != 0) {
            if (superblock.inode[i].start_block != 0 || (superblock.inode[i].used_size & 0x7F) != 0) {
                return 3;
            }
        }
    }

    // Check parent inodes
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0) {
            if (superblock.inode[i].dir_parent == 126) {
                return 4;
            }
            if (superblock.inode[i].dir_parent < 126) {
                if ((superblock.inode[superblock.inode[i].dir_parent].used_size & 0x80) == 0 || (superblock.inode[superblock.inode[i].dir_parent].dir_parent & 0x80) == 0) {
                    return 4;
                }
            }
        }
    }

    // Check unique names
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0) {
            for (int j = i + 1; j < NUM_INODES; j++) {
                if ((superblock.inode[j].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == superblock.inode[j].dir_parent && strcmp(superblock.inode[i].name, superblock.inode[j].name) == 0) {
                    return 5;
                }
            }
        }
    }

    // Check free space list
    for (int i = 1; i < NUM_BLOCKS; i++) {
        bool is_free = (superblock.free_block_list[i / 8] & (1 << (i % 8))) == 0;
        bool is_used = false;
        for (int j = 0; j < NUM_INODES; j++) {
            if ((superblock.inode[j].used_size & 0x80) != 0 && (superblock.inode[j].dir_parent & 0x80) == 0) {
                for (int k = 0; k < (superblock.inode[j].used_size & 0x7F); k++) {
                    if (superblock.inode[j].start_block + k == i) {
                        is_used = true;
                        break;
                    }
                }
            }
            if (is_used) break;
        }
        if (is_free && is_used) return 6;
        if (!is_free && !is_used) return 6;
    }

    return 0;
}

void fs_create(char name[5], int size) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    if (strlen(name) == 0 || strlen(name) > 5 || name[0] == ' ' || name[strlen(name) - 1] == ' ') {
        fprintf(stderr, "Error: Invalid file name\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0) {
            fprintf(stderr, "Error: File or directory %s already exists\n", name);
            return;
        }
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) == 0) {
            superblock.inode[i].used_size = 0x80 | (size & 0x7F);
            superblock.inode[i].start_block = 0;
            superblock.inode[i].dir_parent = 127;
            strcpy(superblock.inode[i].name, name);
            if (size > 0) {
                int start_block = find_free_blocks(size);
                if (start_block == -1) {
                    superblock.inode[i].used_size = 0;
                    superblock.inode[i].start_block = 0;
                    superblock.inode[i].dir_parent = 127;
                    strcpy(superblock.inode[i].name, "");
                    fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, "disk0");
                    return;
                }
                superblock.inode[i].start_block = start_block;
                mark_blocks_used(start_block, size);
            }
            break;
        }
    }
    fseek(disk, 0, SEEK_SET);
    fwrite(&superblock, sizeof(Superblock), 1, disk);
}

int find_free_blocks(int size) {
    for (int i = 1; i <= 127 - size + 1; i++) {
        bool free = true;
        for (int j = 0; j < size; j++) {
            if (is_block_used(i + j)) {
                free = false;
                break;
            }
        }
        if (free) return i;
    }
    return -1;
}

bool is_block_used(int block) {
    return (superblock.free_block_list[block / 8] & (1 << (block % 8))) != 0;
}

void mark_blocks_used(int start, int size) {
    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start + i) / 8] |= (1 << ((start + i) % 8));
    }
}

void mark_blocks_free(int start, int size) {
    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start + i) / 8] &= ~(1 << ((start + i) % 8));
    }
}

void fs_delete(char name[5]) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0) {
            if (superblock.inode[i].used_size & 0x7F > 0) {
                mark_blocks_free(superblock.inode[i].start_block, superblock.inode[i].used_size & 0x7F);
            }
            superblock.inode[i].used_size = 0;
            superblock.inode[i].start_block = 0;
            superblock.inode[i].dir_parent = 127;
            strcpy(superblock.inode[i].name, "");
            break;
        }
    }
    fseek(disk, 0, SEEK_SET);
    fwrite(&superblock, sizeof(Superblock), 1, disk);
}

void fs_read(char name[5], int block_num) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0) {
            if (superblock.inode[i].used_size & 0x7F == 0) {
                fprintf(stderr, "Error: File %s does not exist\n", name);
                return;
            }
            if (block_num < 0 || block_num >= (superblock.inode[i].used_size & 0x7F)) {
                fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
                return;
            }
            fseek(disk, superblock.inode[i].start_block + block_num * BLOCK_SIZE, SEEK_SET);
            fread(buffer, BLOCK_SIZE, 1, disk);
            return;
        }
    }
    fprintf(stderr, "Error: File %s does not exist\n", name);
}

void fs_write(char name[5], int block_num) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0) {
            if (superblock.inode[i].used_size & 0x7F == 0) {
                fprintf(stderr, "Error: File %s does not exist\n", name);
                return;
            }
            if (block_num < 0 || block_num >= (superblock.inode[i].used_size & 0x7F)) {
                fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
                return;
            }
            fseek(disk, superblock.inode[i].start_block + block_num * BLOCK_SIZE, SEEK_SET);
            fwrite(buffer, BLOCK_SIZE, 1, disk);
            return;
        }
    }
    fprintf(stderr, "Error: File %s does not exist\n", name);
}

void fs_buff(char buff[1024]) {
    memset(buffer, 0, BLOCK_SIZE);
    strncpy(buffer, buff, BLOCK_SIZE);
}

void fs_ls(void) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    printf(". 6\n");
    printf(".. 6\n");

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127) {
            if (superblock.inode[i].used_size & 0x7F == 0) {
                printf("%-5s %3d\n", superblock.inode[i].name, 0);
            } else {
                printf("%-5s %3d KB\n", superblock.inode[i].name, superblock.inode[i].used_size & 0x7F);
            }
        }
    }
}

void fs_resize(char name[5], int new_size) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0) {
            if (superblock.inode[i].used_size & 0x7F == 0) {
                fprintf(stderr, "Error: File %s does not exist\n", name);
                return;
            }
            int current_size = superblock.inode[i].used_size & 0x7F;
            if (new_size > current_size) {
                int start_block = find_free_blocks(new_size - current_size);
                if (start_block == -1) {
                    fprintf(stderr, "Error: File %s cannot expand to size %d\n", name, new_size);
                    return;
                }
                for (int j = current_size; j < new_size; j++) {
                    fseek(disk, superblock.inode[i].start_block + j * BLOCK_SIZE, SEEK_SET);
                    fwrite(buffer, BLOCK_SIZE, 1, disk);
                }
                superblock.inode[i].used_size = 0x80 | (new_size & 0x7F);
                mark_blocks_used(superblock.inode[i].start_block + current_size, new_size - current_size);
            } else if (new_size < current_size) {
                for (int j = new_size; j < current_size; j++) {
                    fseek(disk, superblock.inode[i].start_block + j * BLOCK_SIZE, SEEK_SET);
                    fwrite(buffer, BLOCK_SIZE, 1, disk);
                }
                superblock.inode[i].used_size = 0x80 | (new_size & 0x7F);
                mark_blocks_free(superblock.inode[i].start_block + new_size, current_size - new_size);
            }
            fseek(disk, 0, SEEK_SET);
            fwrite(&superblock, sizeof(Superblock), 1, disk);
            return;
        }
    }
    fprintf(stderr, "Error: File %s does not exist\n", name);
}

void fs_defrag(void) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) != 0 && (superblock.inode[i].dir_parent & 0x80) == 0) {
            int new_start_block = find_free_blocks(superblock.inode[i].used_size & 0x7F);
            if (new_start_block != superblock.inode[i].start_block) {
                for (int j = 0; j < (superblock.inode[i].used_size & 0x7F); j++) {
                    fseek(disk, superblock.inode[i].start_block + j * BLOCK_SIZE, SEEK_SET);
                    fread(buffer, BLOCK_SIZE, 1, disk);
                    fseek(disk, new_start_block + j * BLOCK_SIZE, SEEK_SET);
                    fwrite(buffer, BLOCK_SIZE, 1, disk);
                    fseek(disk, superblock.inode[i].start_block + j * BLOCK_SIZE, SEEK_SET);
                    memset(buffer, 0, BLOCK_SIZE);
                    fwrite(buffer, BLOCK_SIZE, 1, disk);
                }
                superblock.inode[i].start_block = new_start_block;
                mark_blocks_used(new_start_block, superblock.inode[i].used_size & 0x7F);
                mark_blocks_free(superblock.inode[i].start_block, superblock.inode[i].used_size & 0x7F);
            }
        }
    }
    fseek(disk, 0, SEEK_SET);
    fwrite(&superblock, sizeof(Superblock), 1, disk);
}

void fs_cd(char name[5]) {
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    if (strcmp(name, "..") == 0) {
        if (strcmp(current_directory, ".") == 0) {
            return;
        }
        for (int i = 0; i < NUM_INODES; i++) {
            if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, current_directory) == 0) {
                current_directory[0] = '.';
                current_directory[1] = '\0';
                return;
            }
        }
    } else if (strcmp(name, ".") == 0) {
        return;
    } else {
        for (int i = 0; i < NUM_INODES; i++) {
            if ((superblock.inode[i].used_size & 0x80) != 0 && superblock.inode[i].dir_parent == 127 && strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x80) != 0) {
                strcpy(current_directory, name);
                return;
            }
        }
    }
    fprintf(stderr, "Error: Directory %s does not exist\n", name);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    FILE *input = fopen(argv[1], "r");
    if (input == NULL) {
        fprintf(stderr, "Error: Cannot open input file %s\n", argv[1]);
        return 1;
    }

    char command[10];
    char name[6];
    int size, block_num, new_size;
    while (fscanf(input, "%s", command) != EOF) {
        if (strcmp(command, "M") == 0) {
            fscanf(input, "%s", name);
            fs_mount(name);
        } else if (strcmp(command, "C") == 0) {
            fscanf(input, "%s %d", name, &size);
            fs_create(name, size);
        } else if (strcmp(command, "D") == 0) {
            fscanf(input, "%s", name);
            fs_delete(name);
        } else if (strcmp(command, "R") == 0) {
            fscanf(input, "%s %d", name, &block_num);
            fs_read(name, block_num);
        } else if (strcmp(command, "W") == 0) {
            fscanf(input, "%s %d", name, &block_num);
            fs_write(name, block_num);
        } else if (strcmp(command, "B") == 0) {
            fscanf(input, "%1024s", buffer);
            fs_buff(buffer);
        } else if (strcmp(command, "L") == 0) {
            fs_ls();
        } else if (strcmp(command, "E") == 0) {
            fscanf(input, "%s %d", name, &new_size);
            fs_resize(name, new_size);
        } else if (strcmp(command, "O") == 0) {
            fs_defrag();
        } else if (strcmp(command, "Y") == 0) {
            fscanf(input, "%s", name);
            fs_cd(name);
        } else {
            fprintf(stderr, "Command Error: %s, %ld\n", argv[1], ftell(input) / sizeof(command));
        }
    }

    fclose(input);
    if (disk != NULL) {
        fclose(disk);
    }

    return 0;
}