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

typedef struct {
    char free_block_list[16];
    Inode inode[NUM_INODES];
} Superblock;

Superblock superblock;
char disk[DISK_SIZE];
char buffer[BLOCK_SIZE];
char current_disk_name[100];
int current_working_directory = 0;

void load_superblock() {
    memcpy(&superblock, disk, sizeof(Superblock));
}

void write_superblock() {
    memcpy(disk, &superblock, sizeof(Superblock));
}

void fs_mount(char *new_disk_name) {
    FILE *file = fopen(new_disk_name, "rb+");
    if (file == NULL) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    fread(disk, 1, DISK_SIZE, file);
    fclose(file);

    load_superblock();

    // Check file system consistency
    bool is_consistent = true;
    int error_code = 0;

    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) == 0) {
            if (inode->used_size != 0 || inode->start_block != 0 || inode->dir_parent != 127) {
                is_consistent = false;
                error_code = 1;
                break;
            }
        } else {
            if ((inode->dir_parent & 0x80) == 0) { // File
                if (inode->start_block < 1 || inode->start_block > 127 || inode->used_size > 127) {
                    is_consistent = false;
                    error_code = 2;
                    break;
                }
            } else { // Directory
                if (inode->start_block != 0 || inode->used_size != 0) {
                    is_consistent = false;
                    error_code = 3;
                    break;
                }
            }
            if (inode->dir_parent == 126 || (inode->dir_parent < 127 && (superblock.inode[inode->dir_parent].used_size & 0x80) == 0)) {
                is_consistent = false;
                error_code = 4;
                break;
            }
            for (int j = 0; j < i; j++) {
                if (strcmp(superblock.inode[j].name, inode->name) == 0) {
                    is_consistent = false;
                    error_code = 5;
                    break;
                }
            }
            if (!is_consistent) break;
        }
    }

    for (int i = 1; i < NUM_BLOCKS; i++) {
        if ((superblock.free_block_list[i / 8] & (1 << (i % 8))) == 0) {
            for (int j = 0; j < NUM_INODES; j++) {
                if (superblock.inode[j].start_block <= i && i < superblock.inode[j].start_block + (superblock.inode[j].used_size & 0x7F)) {
                    is_consistent = false;
                    error_code = 6;
                    break;
                }
            }
            if (!is_consistent) break;
        }
    }

    if (!is_consistent) {
        fprintf(stderr, "Error: File system in %s is inconsistent (error code: %d)\n", new_disk_name, error_code);
        return;
    }

    strcpy(current_disk_name, new_disk_name);
    current_working_directory = 0;
    printf("File system %s mounted successfully.\n", new_disk_name);
}

void fs_create(char name[5], int size) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    if (size < 0 || size > 127) {
        fprintf(stderr, "Error: Invalid file size\n");
        return;
    }

    for (int i = 0; i < NUM_INODES; i++) {
        if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory) {
            fprintf(stderr, "Error: File or directory %s already exists\n", name);
            return;
        }
    }

    int free_inode = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        if ((superblock.inode[i].used_size & 0x80) == 0) {
            free_inode = i;
            break;
        }
    }

    if (free_inode == -1) {
        fprintf(stderr, "Error: Superblock in disk %s is full, cannot create %s\n", current_disk_name, name);
        return;
    }

    int start_block = -1;
    for (int i = 1; i <= NUM_BLOCKS - size; i++) {
        bool is_free = true;
        for (int j = 0; j < size; j++) {
            if ((superblock.free_block_list[(i + j) / 8] & (1 << ((i + j) % 8))) != 0) {
                is_free = false;
                break;
            }
        }
        if (is_free) {
            start_block = i;
            break;
        }
    }

    if (start_block == -1) {
        fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, current_disk_name);
        return;
    }

    Inode *inode = &superblock.inode[free_inode];
    strcpy(inode->name, name);
    inode->used_size = 0x80 | size;
    inode->start_block = start_block;
    inode->dir_parent = current_working_directory | 0x80;

    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start_block + i) / 8] |= 1 << ((start_block + i) % 8);
    }

    write_superblock();
    printf("File %s created successfully.\n", name);
}

void fs_delete(char name[5]) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    int inode_index = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_index];
    int size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start_block + i) / 8] &= ~(1 << ((start_block + i) % 8));
    }

    memset(inode, 0, sizeof(Inode));

    write_superblock();
    printf("File %s deleted successfully.\n", name);
}

void fs_read(char name[5], int block_num) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    int inode_index = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_index];
    int size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    int block_index = start_block + block_num;
    memcpy(buffer, &disk[block_index * BLOCK_SIZE], BLOCK_SIZE);
    printf("Block %d of file %s read successfully.\n", block_num, name);
}

void fs_write(char name[5], int block_num) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    int inode_index = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_index];
    int size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    int block_index = start_block + block_num;
    memcpy(&disk[block_index * BLOCK_SIZE], buffer, BLOCK_SIZE);
    write_superblock();
    printf("Block %d of file %s written successfully.\n", block_num, name);
}

void fs_buff(char buff[1024]) {
    memcpy(buffer, buff, BLOCK_SIZE);
}

void fs_ls() {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    printf(". 2\n");
    printf(".. 2\n");

    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->dir_parent & 0x7F) == current_working_directory) {
            if ((inode->dir_parent & 0x80) == 0) {
                printf("%-5s %3d KB\n", inode->name, inode->used_size & 0x7F);
            } else {
                int num_children = 0;
                for (int j = 0; j < NUM_INODES; j++) {
                    if ((superblock.inode[j].dir_parent & 0x7F) == i) {
                        num_children++;
                    }
                }
                printf("%-5s %3d\n", inode->name, num_children);
            }
        }
    }
}

void fs_resize(char name[5], int new_size) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    int inode_index = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_index];
    int size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    if (new_size < 0 || new_size > 127) {
        fprintf(stderr, "Error: Invalid new size\n");
        return;
    }

    if (new_size > size) {
        int free_blocks = 0;
        for (int i = 1; i < NUM_BLOCKS; i++) {
            if ((superblock.free_block_list[i / 8] & (1 << (i % 8))) == 0) {
                free_blocks++;
            }
        }

        if (free_blocks < (new_size - size)) {
            fprintf(stderr, "Error: File %s cannot expand to size %d\n", name, new_size);
            return;
        }

        int new_start_block = -1;
        for (int i = 1; i <= NUM_BLOCKS - new_size; i++) {
            bool is_free = true;
            for (int j = 0; j < new_size; j++) {
                if ((superblock.free_block_list[(i + j) / 8] & (1 << ((i + j) % 8))) != 0) {
                    is_free = false;
                    break;
                }
            }
            if (is_free) {
                new_start_block = i;
                break;
            }
        }

        if (new_start_block == -1) {
            fprintf(stderr, "Error: File %s cannot expand to size %d\n", name, new_size);
            return;
        } else if (new_start_block != start_block) {
            for (int i = 0; i < size; i++) {
                memcpy(&disk[(new_start_block + i) * BLOCK_SIZE], &disk[(start_block + i) * BLOCK_SIZE], BLOCK_SIZE);
                superblock.free_block_list[(start_block + i) / 8] &= ~(1 << ((start_block + i) % 8));
                superblock.free_block_list[(new_start_block + i) / 8] |= 1 << ((new_start_block + i) % 8);
            }
            inode->start_block = new_start_block;
        }

        for (int i = size; i < new_size; i++) {
            superblock.free_block_list[(new_start_block + i) / 8] |= 1 << ((new_start_block + i) % 8);
        }

        inode->used_size = 0x80 | new_size;
    } else if (new_size < size) {
        for (int i = new_size; i < size; i++) {
            superblock.free_block_list[(start_block + i) / 8] &= ~(1 << ((start_block + i) % 8));
        }
        inode->used_size = 0x80 | new_size;
    }

    write_superblock();
    printf("File %s resized successfully to %d blocks.\n", name, new_size);
}

void fs_defrag() {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    int free_blocks[127];
    int free_count = 0;

    for (int i = 1; i < NUM_BLOCKS; i++) {
        if ((superblock.free_block_list[i / 8] & (1 << (i % 8))) == 0) {
            free_blocks[free_count++] = i;
        }
    }

    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) != 0) {
            int size = inode->used_size & 0x7F;
            int start_block = inode->start_block;

            for (int j = 0; j < size; j++) {
                if (free_blocks[j] != start_block + j) {
                    memcpy(&disk[free_blocks[j] * BLOCK_SIZE], &disk[(start_block + j) * BLOCK_SIZE], BLOCK_SIZE);
                    superblock.free_block_list[(start_block + j) / 8] &= ~(1 << ((start_block + j) % 8));
                    superblock.free_block_list[free_blocks[j] / 8] |= 1 << (free_blocks[j] % 8);
                    free_blocks[j] = start_block + j;
                }
            }

            inode->start_block = free_blocks[0];
        }
    }

    write_superblock();
    printf("Disk defragmented successfully.\n");
}

void fs_cd(char name[5]) {
    if (strlen(current_disk_name) == 0) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    if (strcmp(name, ".") == 0) {
        return;
    } else if (strcmp(name, "..") == 0) {
        if (current_working_directory == 0) {
            return;
        }
        current_working_directory = superblock.inode[current_working_directory].dir_parent & 0x7F;
    } else {
        int inode_index = -1;
        for (int i = 0; i < NUM_INODES; i++) {
            if (strcmp(superblock.inode[i].name, name) == 0 && (superblock.inode[i].dir_parent & 0x7F) == current_working_directory && (superblock.inode[i].dir_parent & 0x80) != 0) {
                inode_index = i;
                break;
            }
        }

        if (inode_index == -1) {
            fprintf(stderr, "Error: Directory %s does not exist\n", name);
            return;
        }

        current_working_directory = inode_index;
    }

    printf("Current directory changed to %s.\n", name);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    FILE *input_file = fopen(argv[1], "r");
    if (input_file == NULL) {
        fprintf(stderr, "Error: Cannot open input file %s\n", argv[1]);
        return 1;
    }

    char command[100];
    int line_number = 1;

    while (fgets(command, 100, input_file) != NULL) {
        command[strcspn(command, "\n")] = 0; // Remove newline character

        char *token = strtok(command, " ");
        if (token == NULL) {
            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            line_number++;
            continue;
        }

        if (strcmp(token, "M") == 0) {
            char *disk_name = strtok(NULL, " ");
            if (disk_name == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            fs_mount(disk_name);
        } else if (strcmp(token, "C") == 0) {
            char *file_name = strtok(NULL, " ");
            char *file_size_str = strtok(NULL, " ");
            if (file_name == NULL || file_size_str == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            int file_size = atoi(file_size_str);
            fs_create(file_name, file_size);
        } else if (strcmp(token, "D") == 0) {
            char *file_name = strtok(NULL, " ");
            if (file_name == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            fs_delete(file_name);
        } else if (strcmp(token, "R") == 0) {
            char *file_name = strtok(NULL, " ");
            char *block_num_str = strtok(NULL, " ");
            if (file_name == NULL || block_num_str == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            int block_num = atoi(block_num_str);
            fs_read(file_name, block_num);
        } else if (strcmp(token, "W") == 0) {
            char *file_name = strtok(NULL, " ");
            char *block_num_str = strtok(NULL, " ");
            if (file_name == NULL || block_num_str == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            int block_num = atoi(block_num_str);
            fs_write(file_name, block_num);
        } else if (strcmp(token, "B") == 0) {
            char *new_buffer = strtok(NULL, " ");
            if (new_buffer == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            fs_buff(new_buffer);
        } else if (strcmp(token, "L") == 0) {
            fs_ls();
        } else if (strcmp(token, "E") == 0) {
            char *file_name = strtok(NULL, " ");
            char *new_size_str = strtok(NULL, " ");
            if (file_name == NULL || new_size_str == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            int new_size = atoi(new_size_str);
            fs_resize(file_name, new_size);
        } else if (strcmp(token, "O") == 0) {
            fs_defrag();
        } else if (strcmp(token, "Y") == 0) {
            char *dir_name = strtok(NULL, " ");
            if (dir_name == NULL) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                line_number++;
                continue;
            }
            fs_cd(dir_name);
        } else {
            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
        }

        line_number++;
    }

    fclose(input_file);
    return 0;
}