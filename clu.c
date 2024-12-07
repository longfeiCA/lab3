#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "fs-sim.h"

// Global variables
static Superblock superblock;
static char buffer[1024];
static char* current_disk = NULL;
static int current_dir_inode = 0;  // Root directory inode index

// Helper functions
static int find_free_inode() {
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            return i;
        }
    }
    return -1;
}

static int find_contiguous_blocks(int size) {
    int start = -1;
    int count = 0;
    
    for (int i = 1; i < 128; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        
        if (!(superblock.free_block_list[byte_idx] & (1 << bit_idx))) {
            if (start == -1) start = i;
            count++;
            if (count == size) return start;
        } else {
            start = -1;
            count = 0;
        }
    }
    return -1;
}

static void mark_blocks(int start, int size, int mark) {
    for (int i = start; i < start + size; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        
        if (mark) {
            superblock.free_block_list[byte_idx] |= (1 << bit_idx);
        } else {
            superblock.free_block_list[byte_idx] &= ~(1 << bit_idx);
        }
    }
}

static int check_consistency() {
    // Check 1: Free inodes must be zeroed
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            if (superblock.inode[i].name[0] != 0 || 
                superblock.inode[i].used_size != 0 ||
                superblock.inode[i].start_block != 0 || 
                superblock.inode[i].dir_parent != 0) {
                return 1;
            }
        }
    }

    // Check 2: Valid start block and size for files
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80)) {
            int size = superblock.inode[i].used_size & 0x7F;
            int start = superblock.inode[i].start_block;
            
            if (start < 1 || start > 127 || 
                start + size - 1 < 1 || start + size - 1 > 127) {
                return 2;
            }
        }
    }

    // Check 3: Directory size and start block must be zero
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x80)) {
            if (superblock.inode[i].start_block != 0 || 
                (superblock.inode[i].used_size & 0x7F) != 0) {
                return 3;
            }
        }
    }

    // Check 4: Parent inode validity
    for (int i = 0; i < 126; i++) {
        if (superblock.inode[i].used_size & 0x80) {
            int parent = superblock.inode[i].dir_parent & 0x7F;
            if (parent == 126) return 4;
            if (parent != 127 && (parent < 0 || parent > 125)) return 4;
            if (parent != 127 && (!(superblock.inode[parent].used_size & 0x80) || 
                !(superblock.inode[parent].dir_parent & 0x80))) {
                return 4;
            }
        }
    }

    // Check 5: Unique names in directories
    for (int dir = 0; dir < 126; dir++) {
        if ((superblock.inode[dir].used_size & 0x80) && 
            (superblock.inode[dir].dir_parent & 0x80)) {
            for (int i = 0; i < 126; i++) {
                if (i != dir && (superblock.inode[i].used_size & 0x80) && 
                    (superblock.inode[i].dir_parent & 0x7F) == dir) {
                    for (int j = i + 1; j < 126; j++) {
                        if ((superblock.inode[j].used_size & 0x80) && 
                            (superblock.inode[j].dir_parent & 0x7F) == dir) {
                            if (strncmp(superblock.inode[i].name, 
                                      superblock.inode[j].name, 5) == 0) {
                                return 5;
                            }
                        }
                    }
                }
            }
        }
    }

    // Check 6: Block allocation consistency
    // for (int i = 1; i < 128; i++) {
    //     int byte_idx = i / 8;
    //     int bit_idx = i % 8;
    //     int block_used = (superblock.free_block_list[byte_idx] & (1 << bit_idx)) != 0;
    //     int found = 0;
        
    //     for (int j = 0; j < 126; j++) {
    //         if ((superblock.inode[j].used_size & 0x80) && 
    //             !(superblock.inode[j].dir_parent & 0x80)) {
    //             int start = superblock.inode[j].start_block;
    //             int size = superblock.inode[j].used_size & 0x7F;
                
    //             if (i >= start && i < start + size) {
    //                 found++;
    //             }
    //         }
    //     }
        
    //     if ((found > 0 && !block_used) || (found == 0 && block_used)) {
    //         return 6;
    //     }
    //     if (found > 1) return 6;
    // }

    return 0;
}

void fs_mount(char *new_disk_name) {
    FILE *disk = fopen(new_disk_name, "rb");
    if (!disk) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    // Read superblock
    fread(&superblock, sizeof(Superblock), 1, disk);
    fclose(disk);

    // Check consistency
    int consistency = check_consistency();
    if (consistency != 0) {
        fprintf(stderr, "Error: File system in %s is inconsistent (error code: %d)\n", 
                new_disk_name, consistency);
        return;
    }

    // Update current disk and directory
    if (current_disk) free(current_disk);
    current_disk = strdup(new_disk_name);
    current_dir_inode = 0;

    // Zero out buffer
    memset(buffer, 0, 1024);
}

void fs_create(char name[5], int size) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find free inode
    int inode_idx = find_free_inode();
    if (inode_idx == -1) {
        fprintf(stderr, "Error: Superblock in disk %s is full, cannot create %s\n", 
                current_disk, name);
        return;
    }

    // Check name uniqueness
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            if (strncmp(superblock.inode[i].name, name, 5) == 0) {
                fprintf(stderr, "Error: File or directory %s already exists\n", name);
                return;
            }
        }
    }

    // For files, find contiguous blocks
    int start_block = 0;
    if (size > 0) {
        start_block = find_contiguous_blocks(size);
        if (start_block == -1) {
            fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, current_disk);
            return;
        }
        mark_blocks(start_block, size, 1);
    }

    // Initialize inode
    memset(&superblock.inode[inode_idx], 0, sizeof(Inode));
    strncpy(superblock.inode[inode_idx].name, name, 5);
    superblock.inode[inode_idx].used_size = 0x80 | (size & 0x7F);
    superblock.inode[inode_idx].start_block = start_block;
    superblock.inode[inode_idx].dir_parent = (size == 0 ? 0x80 : 0) | 
                                           (current_dir_inode == 0 ? 127 : current_dir_inode);

    // Write superblock back to disk
    FILE *disk = fopen(current_disk, "r+b");
    fwrite(&superblock, sizeof(Superblock), 1, disk);
    fclose(disk);
}

void fs_delete(char name[5], int inode_idx) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file/directory
    int found = 0;
    for (int i = 0; i < 126 && !found; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            
            // If directory, recursively delete contents
            if (superblock.inode[i].dir_parent & 0x80) {
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) && 
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        fs_delete(superblock.inode[j].name, j);
                    }
                }
            } else {
                // Free blocks
                int size = superblock.inode[i].used_size & 0x7F;
                mark_blocks(superblock.inode[i].start_block, size, 0);
            }

            // Zero out inode
            memset(&superblock.inode[i], 0, sizeof(Inode));
            found = 1;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }

    // Write superblock back to disk
    FILE *disk = fopen(current_disk, "r+b");
    fwrite(&superblock, sizeof(Superblock), 1, disk);
    fclose(disk);
}

void fs_read(char name[5], int block_num) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file
    int found = 0;
    for (int i = 0; i < 126 && !found; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            
            int size = superblock.inode[i].used_size & 0x7F;
            if (block_num < 0 || block_num >= size) {
                fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
                return;
            }

            // Read block into buffer
            FILE *disk = fopen(current_disk, "rb");
            fseek(disk, (superblock.inode[i].start_block + block_num) * 1024, SEEK_SET);
            fread(buffer, 1024, 1, disk);
            fclose(disk);
            found = 1;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
    }
}

void fs_write(char name[5], int block_num) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file
    int found = 0;
    for (int i = 0; i < 126 && !found; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            
            int size = superblock.inode[i].used_size & 0x7F;
            if (block_num < 0 || block_num >= size) {
                fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
                return;
            }

            // Write buffer to block
            FILE *disk = fopen(current_disk, "r+b");
            fseek(disk, (superblock.inode[i].start_block + block_num) * 1024, SEEK_SET);
            fwrite(buffer, 1024, 1, disk);
            fclose(disk);
            found = 1;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
    }
}

void fs_buff(char buff[1024]) {
    memset(buffer, 0, 1024);
    strncpy(buffer, buff, 1024);
}

void fs_ls(void) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Print current directory (.)
    int num_items = 0;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            num_items++;
        }
    }
    printf("%-5s %3d\n", ".", num_items + 2);  // +2 for . and ..

    // Print parent directory (..)
    if (current_dir_inode == 0) {
        printf("%-5s %3d\n", "..", num_items + 2);
    } else {
        int parent_idx = superblock.inode[current_dir_inode].dir_parent & 0x7F;
        int parent_items = 0;
        for (int i = 0; i < 126; i++) {
            if ((superblock.inode[i].used_size & 0x80) && 
                (superblock.inode[i].dir_parent & 0x7F) == parent_idx) {
                parent_items++;
            }
        }
        printf("%-5s %3d\n", "..", parent_items + 2);
    }

    // Print files and directories
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            if (superblock.inode[i].dir_parent & 0x80) {
                // Directory
                int dir_items = 0;
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) && 
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        dir_items++;
                    }
                }
                printf("%-5s %3d\n", superblock.inode[i].name, dir_items + 2);
            } else {
                // File
                printf("%-5s %3d KB\n", superblock.inode[i].name, 
                       superblock.inode[i].used_size & 0x7F);
            }
        }
    }
}

void fs_resize(char name[5], int new_size) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file
    int found = 0;
    for (int i = 0; i < 126 && !found; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            
            int current_size = superblock.inode[i].used_size & 0x7F;
            int start_block = superblock.inode[i].start_block;

            if (new_size > current_size) {
                // Try to expand in place
                int can_expand = 1;
                for (int b = start_block + current_size; 
                     b < start_block + new_size && can_expand; b++) {
                    int byte_idx = b / 8;
                    int bit_idx = b % 8;
                    if (superblock.free_block_list[byte_idx] & (1 << bit_idx)) {
                        can_expand = 0;
                    }
                }

                if (can_expand) {
                    // Mark new blocks as used
                    mark_blocks(start_block + current_size, new_size - current_size, 1);
                } else {
                    // Try to relocate file
                    int new_start = find_contiguous_blocks(new_size);
                    if (new_start == -1) {
                        fprintf(stderr, "Error: File %s cannot expand to size %d\n", 
                                name, new_size);
                        return;
                    }

                    // Copy data to new location
                    FILE *disk = fopen(current_disk, "r+b");
                    char temp_buffer[1024];
                    for (int b = 0; b < current_size; b++) {
                        // Read old block
                        fseek(disk, (start_block + b) * 1024, SEEK_SET);
                        fread(temp_buffer, 1024, 1, disk);
                        
                        // Write to new block
                        fseek(disk, (new_start + b) * 1024, SEEK_SET);
                        fwrite(temp_buffer, 1024, 1, disk);
                        
                        // Zero out old block
                        memset(temp_buffer, 0, 1024);
                        fseek(disk, (start_block + b) * 1024, SEEK_SET);
                        fwrite(temp_buffer, 1024, 1, disk);
                    }
                    fclose(disk);

                    // Update free block list
                    mark_blocks(start_block, current_size, 0);
                    mark_blocks(new_start, new_size, 1);
                    
                    // Update inode
                    superblock.inode[i].start_block = new_start;
                }
            } else if (new_size < current_size) {
                // Zero out freed blocks
                FILE *disk = fopen(current_disk, "r+b");
                char zero_buffer[1024] = {0};
                for (int b = new_size; b < current_size; b++) {
                    fseek(disk, (start_block + b) * 1024, SEEK_SET);
                    fwrite(zero_buffer, 1024, 1, disk);
                }
                fclose(disk);

                // Update free block list
                mark_blocks(start_block + new_size, current_size - new_size, 0);
            }

            // Update inode size
            superblock.inode[i].used_size = 0x80 | (new_size & 0x7F);
            found = 1;

            // Write superblock back to disk
            FILE *disk = fopen(current_disk, "r+b");
            fwrite(&superblock, sizeof(Superblock), 1, disk);
            fclose(disk);
        }
    }

    if (!found) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
    }
}

void fs_defrag(void) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Sort files by start block
    int file_count = 0;
    struct FileInfo {
        int inode_idx;
        int start_block;
        int size;
    } files[126];

    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80)) {
            files[file_count].inode_idx = i;
            files[file_count].start_block = superblock.inode[i].start_block;
            files[file_count].size = superblock.inode[i].used_size & 0x7F;
            file_count++;
        }
    }

    // Bubble sort files by start block
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = 0; j < file_count - i - 1; j++) {
            if (files[j].start_block > files[j + 1].start_block) {
                struct FileInfo temp = files[j];
                files[j] = files[j + 1];
                files[j + 1] = temp;
            }
        }
    }

    // Move files towards block 1
    int next_free = 1;
    FILE *disk = fopen(current_disk, "r+b");
    char buffer[1024];

    for (int i = 0; i < file_count; i++) {
        if (files[i].start_block != next_free) {
            // Copy file data
            for (int b = 0; b < files[i].size; b++) {
                // Read block
                fseek(disk, (files[i].start_block + b) * 1024, SEEK_SET);
                fread(buffer, 1024, 1, disk);
                
                // Write to new location
                fseek(disk, (next_free + b) * 1024, SEEK_SET);
                fwrite(buffer, 1024, 1, disk);
                
                // Zero out old block
                memset(buffer, 0, 1024);
                fseek(disk, (files[i].start_block + b) * 1024, SEEK_SET);
                fwrite(buffer, 1024, 1, disk);
            }

            // Update free block list
            mark_blocks(files[i].start_block, files[i].size, 0);
            mark_blocks(next_free, files[i].size, 1);
            
            // Update inode
            superblock.inode[files[i].inode_idx].start_block = next_free;
        }
        next_free += files[i].size;
    }

    // Write superblock back to disk
    fseek(disk, 0, SEEK_SET);
    fwrite(&superblock, sizeof(Superblock), 1, disk);
    fclose(disk);
}

void fs_cd(char name[5]) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Handle special cases
    if (strcmp(name, ".") == 0) {
        return;
    }
    
    if (strcmp(name, "..") == 0) {
        if (current_dir_inode != 0) {
            current_dir_inode = superblock.inode[current_dir_inode].dir_parent & 0x7F;
        }
        return;
    }

    // Find directory
    int found = 0;
    for (int i = 0; i < 126 && !found; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            current_dir_inode = i;
            found = 1;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: Directory %s does not exist\n", name);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <command_file>\n", argv[0]);
        return 1;
    }

    FILE *cmd_file = fopen(argv[1], "r");
    if (!cmd_file) {
        fprintf(stderr, "Error: Cannot open command file %s\n", argv[1]);
        return 1;
    }

    char line[1024];
    int line_num = 0;
    
    while (fgets(line, sizeof(line), cmd_file)) {
        line_num++;
        char cmd;
        char arg1[256];
        int arg2;
        
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Parse command
        if (sscanf(line, "%c %s %d", &cmd, arg1, &arg2) < 1) {
            continue;  // Empty line
        }

        // Process commands
        switch (cmd) {
            case 'M':  // Mount
                if (sscanf(line, "M %s", arg1) != 1) {
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                    continue;
                }
                fs_mount(arg1);
                break;

            case 'C':  // Create
                {
                    char name[6];  // Extra byte for null terminator
                    int size;
                    if (sscanf(line, "C %5s %d", name, &size) != 2 || 
                        size < 0 || size > 127 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_create(name, size);
                }
                break;

            case 'D':  // Delete
                {
                    char name[6];
                    if (sscanf(line, "D %5s", name) != 1 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_delete(name, -1);
                }
                break;

            case 'R':  // Read
                {
                    char name[6];
                    int block;
                    if (sscanf(line, "R %5s %d", name, &block) != 2 || 
                        block < 0 || block > 126 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_read(name, block);
                }
                break;

            case 'W':  // Write
                {
                    char name[6];
                    int block;
                    if (sscanf(line, "W %5s %d", name, &block) != 2 || 
                        block < 0 || block > 126 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_write(name, block);
                }
                break;

            case 'B':  // Buffer
                {
                    char *buffer_content = line + 2;  // Skip "B "
                    if (strlen(buffer_content) > 1024) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_buff(buffer_content);
                }
                break;

            case 'L':  // List
                if (strlen(line) != 1) {
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                    continue;
                }
                fs_ls();
                break;

            case 'E':  // Resize
                {
                    char name[6];
                    int new_size;
                    if (sscanf(line, "E %5s %d", name, &new_size) != 2 || 
                        new_size <= 0 || new_size > 127 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_resize(name, new_size);
                }
                break;

            case 'O':  // Defragment
                if (strlen(line) != 1) {
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                    continue;
                }
                fs_defrag();
                break;

            case 'Y':  // Change directory
                {
                    char name[6];
                    if (sscanf(line, "Y %5s", name) != 1 || strlen(name) > 5) {
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                        continue;
                    }
                    fs_cd(name);
                }
                break;

            default:
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                break;
        }
    }

    fclose(cmd_file);
    if (current_disk) {
        free(current_disk);
    }
    return 0;
}