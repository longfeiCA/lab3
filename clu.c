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

// Helper function to compare file/directory names
static int compare_inode_names(const char* name1, const char* name2) {
    for (int i = 0; i < 5; i++) {
        if (name1[i] != name2[i]) {
            return 0;
        }
        if (name1[i] == '\0' && name2[i] == '\0') {
            return 1;
        }
    }
    return 1;  // Both names used all 5 characters
}

// Helper functions for bit manipulation
static void set_block_bit(int block_num, int value) {
    int byte_idx = block_num / 8;
    int bit_idx = block_num % 8;
    if (value) {
        superblock.free_block_list[byte_idx] |= (1 << bit_idx);
    } else {
        superblock.free_block_list[byte_idx] &= ~(1 << bit_idx);
    }
}

static int get_block_bit(int block_num) {
    int byte_idx = block_num / 8;
    int bit_idx = block_num % 8;
    return (superblock.free_block_list[byte_idx] & (1 << bit_idx)) != 0;
}

// Helper function to write superblock to disk
static void write_superblock() {
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        fwrite(&superblock, sizeof(Superblock), 1, disk);
        fclose(disk);
    }
}

static int check_consistency() {
    // Check 1: Free inodes must be zeroed
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            // If inode is free, all bits must be zero
            if (superblock.inode[i].used_size != 0 ||
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
    int block_usage[128] = {0};  // Count how many files use each block
    
    // First block (superblock) must be marked as used
    if (!(superblock.free_block_list[0] & 0x01)) {
        return 6;
    }
    
    // Count block usage by files
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80)) {
            int start = superblock.inode[i].start_block;
            int size = superblock.inode[i].used_size & 0x7F;

            if (start < 1 || start + size > 128) continue;
            
            for (int b = start; b < start + size; b++) {
                block_usage[b]++;
            }
        }
    }
    
    // Compare with free-block list
    for (int i = 0; i < 128; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        int is_marked_used = (superblock.free_block_list[byte_idx] & (1 << bit_idx)) != 0;

        if (i == 0) {  // Superblock must be marked used
            if (!is_marked_used) return 6;
        } else {  // Other blocks
            if (is_marked_used && block_usage[i] != 1) return 6;  // Marked used but not used by exactly one file
            if (!is_marked_used && block_usage[i] != 0) return 6;  // Marked free but used by some file
        }
    }

    return 0;  // File system is consistent
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
            if (compare_inode_names(superblock.inode[i].name, name)) {
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
    int target_inode = -1;
    if (inode_idx >= 0) {
        target_inode = inode_idx;
    } else {
        for (int i = 0; i < 126; i++) {
            if ((superblock.inode[i].used_size & 0x80) && 
                (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
                compare_inode_names(superblock.inode[i].name, name)) {
                target_inode = i;
                break;
            }
        }
    }

    if (target_inode == -1) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }

    // If directory, recursively delete contents
    if (superblock.inode[target_inode].dir_parent & 0x80) {
        for (int i = 0; i < 126; i++) {
            if ((superblock.inode[i].used_size & 0x80) && 
                (superblock.inode[i].dir_parent & 0x7F) == target_inode) {
                fs_delete(superblock.inode[i].name, i);
            }
        }
    } else {
        // Free blocks
        int start = superblock.inode[target_inode].start_block;
        int size = superblock.inode[target_inode].used_size & 0x7F;
        
        // Mark blocks as free in free-space list
        for (int i = start; i < start + size; i++) {
            int byte_idx = i / 8;
            int bit_idx = i % 8;
            superblock.free_block_list[byte_idx] &= ~(1 << bit_idx);
        }

        // Zero out the blocks
        FILE *disk = fopen(current_disk, "r+b");
        if (disk) {
            char zero_block[1024] = {0};
            for (int i = start; i < start + size; i++) {
                fseek(disk, i * 1024, SEEK_SET);
                fwrite(zero_block, 1024, 1, disk);
            }
            fclose(disk);
        }
    }

    // Zero out inode
    memset(&superblock.inode[target_inode], 0, sizeof(Inode));

    // Write superblock back to disk
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        fwrite(&superblock, sizeof(Superblock), 1, disk);
        fclose(disk);
    }
}

void fs_read(char name[5], int block_num) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file
    int found = 0;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            compare_inode_names(superblock.inode[i].name, name)) {
            
            int size = superblock.inode[i].used_size & 0x7F;
            if (block_num < 0 || block_num >= size) {
                fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
                return;
            }

            // Read block into buffer
            FILE *disk = fopen(current_disk, "rb");
            if (disk) {
                fseek(disk, (superblock.inode[i].start_block + block_num) * 1024, SEEK_SET);
                fread(buffer, 1024, 1, disk);
                fclose(disk);
            }
            found = 1;
            break;
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
            compare_inode_names(superblock.inode[i].name, name)) {
            
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
    if (buff) {
        strncpy(buffer, buff, 1024);
    }
}

void fs_ls(void) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Count items in current directory
    int num_items = 0;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            num_items++;
        }
    }

    // Print current directory (.)
    printf("%-5s %3d\n", ".", num_items + 2);  // +2 for . and ..

    // Print parent directory (..)
    if (current_dir_inode == 0) {  // Root directory
        printf("%-5s %3d\n", "..", num_items + 2);  // Same as . for root
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

    // Print files and directories in sorted order by inode index
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            if (superblock.inode[i].dir_parent & 0x80) {  // Directory
                int dir_items = 0;
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) && 
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        dir_items++;
                    }
                }
                printf("%-5s %3d\n", superblock.inode[i].name, dir_items + 2);
            } else {  // File
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
    int found = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) &&
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&
            compare_inode_names(superblock.inode[i].name, name)) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    int current_size = superblock.inode[found].used_size & 0x7F;
    int start_block = superblock.inode[found].start_block;

    if (new_size > current_size) {
        // Check if we can expand in place
        int can_expand = 1;
        for (int i = start_block + current_size; i < start_block + new_size && can_expand; i++) {
            if (i >= 128) {
                can_expand = 0;
                break;
            }
            int byte_idx = i / 8;
            int bit_idx = i % 8;
            if (superblock.free_block_list[byte_idx] & (1 << bit_idx)) {
                can_expand = 0;
            }
        }

        if (!can_expand) {
            // Try to find new contiguous space
            int new_start = find_contiguous_blocks(new_size);
            if (new_start == -1) {
                fprintf(stderr, "Error: File %s cannot expand to size %d\n", name, new_size);
                return;
            }

            // Copy data to new location
            FILE *disk = fopen(current_disk, "r+b");
            if (disk) {
                char block[1024];
                // Copy existing blocks
                for (int i = 0; i < current_size; i++) {
                    // Read old block
                    fseek(disk, (start_block + i) * 1024, SEEK_SET);
                    fread(block, 1024, 1, disk);
                    
                    // Write to new location
                    fseek(disk, (new_start + i) * 1024, SEEK_SET);
                    fwrite(block, 1024, 1, disk);
                }

                // Zero out old blocks
                memset(block, 0, 1024);
                for (int i = 0; i < current_size; i++) {
                    fseek(disk, (start_block + i) * 1024, SEEK_SET);
                    fwrite(block, 1024, 1, disk);
                }
                fclose(disk);

                // Update free space list
                // Mark old blocks as free
                for (int i = start_block; i < start_block + current_size; i++) {
                    int byte_idx = i / 8;
                    int bit_idx = i % 8;
                    superblock.free_block_list[byte_idx] &= ~(1 << bit_idx);
                }

                // Mark new blocks as used
                for (int i = new_start; i < new_start + new_size; i++) {
                    int byte_idx = i / 8;
                    int bit_idx = i % 8;
                    superblock.free_block_list[byte_idx] |= (1 << bit_idx);
                }

                superblock.inode[found].start_block = new_start;
            }
        } else {
            // Mark additional blocks as used
            for (int i = start_block + current_size; i < start_block + new_size; i++) {
                int byte_idx = i / 8;
                int bit_idx = i % 8;
                superblock.free_block_list[byte_idx] |= (1 << bit_idx);
            }
        }
    } else if (new_size < current_size) {
        // Free excess blocks
        FILE *disk = fopen(current_disk, "r+b");
        if (disk) {
            // Zero out freed blocks
            char zero_block[1024] = {0};
            for (int i = start_block + new_size; i < start_block + current_size; i++) {
                fseek(disk, i * 1024, SEEK_SET);
                fwrite(zero_block, 1024, 1, disk);
                
                // Mark block as free
                int byte_idx = i / 8;
                int bit_idx = i % 8;
                superblock.free_block_list[byte_idx] &= ~(1 << bit_idx);
            }
            fclose(disk);
        }
    }

    // Update inode size
    superblock.inode[found].used_size = 0x80 | (new_size & 0x7F);

    // Write superblock back to disk
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        fwrite(&superblock, sizeof(Superblock), 1, disk);
        fclose(disk);
    }
}

void fs_defrag(void) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Create array of files sorted by start block
    struct FileInfo {
        int inode_idx;
        int start_block;
        int size;
    };
    struct FileInfo files[126];
    int file_count = 0;

    // Collect file information
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80)) {
            files[file_count].inode_idx = i;
            files[file_count].start_block = superblock.inode[i].start_block;
            files[file_count].size = superblock.inode[i].used_size & 0x7F;
            file_count++;
        }
    }

    // Sort files by start block
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = 0; j < file_count - i - 1; j++) {
            if (files[j].start_block > files[j + 1].start_block) {
                struct FileInfo temp = files[j];
                files[j] = files[j + 1];
                files[j + 1] = temp;
            }
        }
    }

    // Move files toward beginning of disk
    int next_free = 1;  // Start after superblock
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        char block[1024];
        
        // Process each file
        for (int i = 0; i < file_count; i++) {
            if (files[i].start_block != next_free) {
                // Read and write each block
                for (int j = 0; j < files[i].size; j++) {
                    // Read block
                    fseek(disk, (files[i].start_block + j) * 1024, SEEK_SET);
                    fread(block, 1024, 1, disk);
                    
                    // Write to new location
                    fseek(disk, (next_free + j) * 1024, SEEK_SET);
                    fwrite(block, 1024, 1, disk);
                }

                // Update free space list
                for (int j = 0; j < files[i].size; j++) {
                    // Mark old block as free
                    int old_byte = (files[i].start_block + j) / 8;
                    int old_bit = (files[i].start_block + j) % 8;
                    superblock.free_block_list[old_byte] &= ~(1 << old_bit);

                    // Mark new block as used
                    int new_byte = (next_free + j) / 8;
                    int new_bit = (next_free + j) % 8;
                    superblock.free_block_list[new_byte] |= (1 << new_bit);
                }

                // Update inode
                superblock.inode[files[i].inode_idx].start_block = next_free;
            }
            next_free += files[i].size;
        }
        
        // Zero out all blocks after the last file
        char zero_block[1024] = {0};
        for (int i = next_free; i < 128; i++) {
            fseek(disk, i * 1024, SEEK_SET);
            fwrite(zero_block, 1024, 1, disk);
        }
        
        fclose(disk);
        
        // Write updated superblock
        disk = fopen(current_disk, "r+b");
        if (disk) {
            fwrite(&superblock, sizeof(Superblock), 1, disk);
            fclose(disk);
        }
    }
}

void fs_cd(char name[5]) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Handle special cases
    if (strcmp(name, ".") == 0) {
        return;  // Stay in current directory
    }
    
    if (strcmp(name, "..") == 0) {
        if (current_dir_inode != 0) {  // Not root directory
            current_dir_inode = superblock.inode[current_dir_inode].dir_parent & 0x7F;
        }
        return;
    }

    // Look for directory in current directory
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) &&  // Used inode
            (superblock.inode[i].dir_parent & 0x80) &&  // Is a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&  // In current directory
            compare_inode_names(superblock.inode[i].name, name)) {
            current_dir_inode = i;
            return;
        }
    }

    fprintf(stderr, "Error: Directory %s does not exist\n", name);
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
        
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Skip empty lines
        if (line[0] == '\0') continue;

        char cmd = line[0];
        char args[1024];
        
        // Extract arguments
        if (strlen(line) > 2) {
            strcpy(args, line + 2);
        } else {
            args[0] = '\0';
        }

        switch (cmd) {
            case 'M': {  // Mount
                char disk_name[256];
                if (sscanf(args, "%s", disk_name) != 1) {
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                    continue;
                }
                fs_mount(disk_name);
                break;
            }

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
                    if (strlen(line) < 2) {  // Just "B" with no content
                        memset(buffer, 0, 1024);
                    } else {
                        char *buffer_content = line + 2;  // Skip "B "
                        if (strlen(buffer_content) > 1024) {
                            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                            continue;
                        }
                        fs_buff(buffer_content);
                    }
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