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

// Helper functions declarations
static int get_block_bit(int block_num);
static void set_block_bit(int block_num, int value);
static int find_free_inode(void);
static int find_contiguous_blocks(int size);
static void mark_blocks(int start, int size, int mark);
static int compare_inode_names(const char* name1, const char* name2);
static void write_superblock(void);
static int check_consistency(void);
static void clean_name(char* name);

// Helper function implementations
static int find_free_inode() {
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            return i;
        }
    }
    return -1;
}

static int find_contiguous_blocks(int size) {
    if (size <= 0) return 0;
    
    for (int start = 1; start <= 127 - size + 1; start++) {
        int available = 1;
        for (int i = 0; i < size && available; i++) {
            if (get_block_bit(start + i)) {
                available = 0;
            }
        }
        if (available) {
            return start;
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

static void clean_name(char* name) {
    // Trim leading and trailing spaces
    int start = 0;
    int end = strlen(name) - 1;
    
    while(start < 5 && name[start] == ' ') start++;
    while(end >= 0 && name[end] == ' ') end--;
    
    if (start > end) {
        name[0] = '\0';
        return;
    }
    
    // Shift characters to remove leading spaces
    int j = 0;
    for(int i = start; i <= end && j < 5; i++) {
        name[j++] = name[i];
    }
    name[j] = '\0';
}

static int compare_inode_names(const char* name1, const char* name2) {
    // 6 >5 so the name will always end with '\0' to make this method safe
    char temp1[6] = {0};
    char temp2[6] = {0};
    
    // Copy and trim both names
    strncpy(temp1, name1, 5);
    strncpy(temp2, name2, 5);
    
    for (int i = 4; i >= 0; i--) {
        if (temp1[i] == ' ') temp1[i] = '\0';
        if (temp2[i] == ' ') temp2[i] = '\0';
    }
    
    return strcmp(temp1, temp2) == 0;
}

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

static void write_superblock() {
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        fwrite(&superblock, sizeof(Superblock), 1, disk);
        fclose(disk);
    }
}

static int check_consistency() {
    // Check 1: Verify free inodes
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            if (superblock.inode[i].start_block != 0) {
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

    // Check 3: Directory attributes
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x80)) {
            if (superblock.inode[i].start_block != 0 || 
                (superblock.inode[i].used_size & 0x7F) != 0) {
                return 3;
            }
        }
    }

    // Check 4: Parent directory validity
    for (int i = 0; i < 126; i++) {
        if (superblock.inode[i].used_size & 0x80) {
            int parent = superblock.inode[i].dir_parent & 0x7F;
            if (parent == 126) return 4;
            if (parent != 127) {
                if (parent < 0 || parent > 125) return 4;
                if (!(superblock.inode[parent].used_size & 0x80) ||
                    !(superblock.inode[parent].dir_parent & 0x80)) {
                    return 4;
                }
            }
        }
    }

    // Check 5: Unique names within directories
    for (int dir = 0; dir < 126; dir++) {
        if ((superblock.inode[dir].used_size & 0x80) && 
            (superblock.inode[dir].dir_parent & 0x80)) {
            for (int i = 0; i < 126; i++) {
                for (int j = i + 1; j < 126; j++) {
                    if ((superblock.inode[i].used_size & 0x80) &&
                        (superblock.inode[j].used_size & 0x80) &&
                        (superblock.inode[i].dir_parent & 0x7F) == dir &&
                        (superblock.inode[j].dir_parent & 0x7F) == dir &&
                        compare_inode_names(superblock.inode[i].name, 
                                         superblock.inode[j].name)) {
                        return 5;
                    }
                }
            }
        }
    }

    // Check 6: Block allocation consistency
    int block_usage[128] = {0};
    block_usage[0] = 1;  // Superblock

    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80)) {
            int size = superblock.inode[i].used_size & 0x7F;
            int start = superblock.inode[i].start_block;
                
            for (int b = start; b < start + size && b < 128; b++) {
                if (b >= 1) block_usage[b]++;
            }
        }
    }

    for (int i = 0; i < 128; i++) {
        int is_used = get_block_bit(i);
        if (i == 0) {
            if (!is_used) return 6;
        } else if (is_used != (block_usage[i] > 0)) {
            return 6;
        }
    }
    
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

    // Modify the free block finding logic
    int start_block = 0;
    if (size > 0) {  // File
        start_block = find_contiguous_blocks(size);
        if (start_block == -1) {
            fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, current_disk);
            return;
        }

        // Mark blocks as used
        for (int i = 0; i < size; i++) {
            set_block_bit(start_block + i, 1);
        }
    }

    // Initialize inode with proper values
    strncpy(superblock.inode[inode_idx].name, name, 5);
    superblock.inode[inode_idx].used_size = 0x80 | (size & 0x7F);
    superblock.inode[inode_idx].start_block = start_block;
    superblock.inode[inode_idx].dir_parent = (size == 0 ? 0x80 : 0) | 
                                           (current_dir_inode == 0 ? 127 : current_dir_inode);

    write_superblock();
}

void fs_delete(char name[5], int inode_idx) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file/directory
    int target_inode = inode_idx;
    if (target_inode == -1) {
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

    // Recursively delete directory contents
    if (superblock.inode[target_inode].dir_parent & 0x80) {
        for (int i = 0; i < 126; i++) {
            if ((superblock.inode[i].used_size & 0x80) && 
                (superblock.inode[i].dir_parent & 0x7F) == target_inode) {
                fs_delete(superblock.inode[i].name, i);
            }
        }
    } else {
        int start = superblock.inode[target_inode].start_block;
        int size = superblock.inode[target_inode].used_size & 0x7F;
        
        // Mark blocks as free
        for (int i = 0; i < size; i++) {
            set_block_bit(start + i, 0);
        }

        // Zero out blocks
        FILE *disk = fopen(current_disk, "r+b");
        if (disk) {
            char zero_block[1024] = {0};
            for (int i = 0; i < size; i++) {
                fseek(disk, (start + i) * 1024, SEEK_SET);
                fwrite(zero_block, 1024, 1, disk);
            }
            fclose(disk);
        }
    }

    // Zero out the inode
    memset(&superblock.inode[target_inode], 0, sizeof(Inode));
    
    // Write changes back to disk
    write_superblock();
}

void fs_read(char name[5], int block_num) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

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

    int size = superblock.inode[found].used_size & 0x7F;
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    FILE *disk = fopen(current_disk, "rb");
    if (disk) {
        int actual_block = superblock.inode[found].start_block + block_num;
        fseek(disk, actual_block * 1024, SEEK_SET);
        fread(buffer, 1024, 1, disk);
        fclose(disk);
    }
}

void fs_write(char name[5], int block_num) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

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

    int size = superblock.inode[found].used_size & 0x7F;
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    FILE *disk = fopen(current_disk, "r+b");
    if (!disk) {
        return;
    }

    // Calculate actual block position and write
    int actual_block = superblock.inode[found].start_block + block_num;
    fseek(disk, actual_block * 1024, SEEK_SET);
    fwrite(buffer, 1024, 1, disk);
    fclose(disk);
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

    // Count valid entries in current directory
    int current_items = 0;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            current_items++;
        }
    }

    // Print current directory
    printf("%-5s %3d\n", ".", current_items + 2);

    // Print parent directory
    if (current_dir_inode == 0) {
        printf("%-5s %3d\n", "..", current_items + 2);
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

    // Print all other entries
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode) {
            if (superblock.inode[i].dir_parent & 0x80) { // Directory
                int dir_items = 0;
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) && 
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        dir_items++;
                    }
                }
                printf("%-5s %3d\n", superblock.inode[i].name, dir_items + 2);
            } else { // File
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
        // Try to expand in place
        int can_expand = 1;
        for (int i = start_block + current_size; i < start_block + new_size && can_expand; i++) {
            if (i >= 128 || get_block_bit(i)) {
                can_expand = 0;
            }
        }

        if (!can_expand) {
            // Find new location
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
                    fseek(disk, (start_block + i) * 1024, SEEK_SET);
                    fread(block, 1024, 1, disk);
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

                // Update block allocation
                for (int i = 0; i < current_size; i++) {
                    set_block_bit(start_block + i, 0);  // Free old blocks
                }
                for (int i = 0; i < new_size; i++) {
                    set_block_bit(new_start + i, 1);  // Mark new blocks as used
                }

                superblock.inode[found].start_block = new_start;
            }
        } else {
            // Mark additional blocks as used
            for (int i = current_size; i < new_size; i++) {
                set_block_bit(start_block + i, 1);
            }
        }
    } else if (new_size < current_size) {
        // Shrink file
        FILE *disk = fopen(current_disk, "r+b");
        if (disk) {
            char zero_block[1024] = {0};
            for (int i = new_size; i < current_size; i++) {
                fseek(disk, (start_block + i) * 1024, SEEK_SET);
                fwrite(zero_block, 1024, 1, disk);
                set_block_bit(start_block + i, 0);  
            }
            fclose(disk);
        }
    }

    // Update inode size
    superblock.inode[found].used_size = 0x80 | (new_size & 0x7F);
    write_superblock();
}

void fs_defrag(void) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Create sorted array of files
    typedef struct {
        int inode_idx;
        int start_block;
        int size;
    } FileInfo;
    
    FileInfo files[126];
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
                FileInfo temp = files[j];
                files[j] = files[j + 1];
                files[j + 1] = temp;
            }
        }
    }

    // Move files toward beginning
    int next_free = 1;  // Start after superblock
    FILE *disk = fopen(current_disk, "r+b");
    if (disk) {
        char *block_buffer = malloc(1024);
        if (!block_buffer) {
            fclose(disk);
            return;
        }

        for (int i = 0; i < file_count; i++) {
            if (files[i].start_block != next_free) {
                // Move each block of the file
                for (int j = 0; j < files[i].size; j++) {
                    // Read original block
                    fseek(disk, (files[i].start_block + j) * 1024, SEEK_SET);
                    fread(block_buffer, 1024, 1, disk);
                    
                    // Write to new location
                    fseek(disk, (next_free + j) * 1024, SEEK_SET);
                    fwrite(block_buffer, 1024, 1, disk);
                }

                // Update free space list and zero out old blocks
                for (int j = 0; j < files[i].size; j++) {
                    set_block_bit(files[i].start_block + j, 0);
                    set_block_bit(next_free + j, 1);
                }

                // Zero out old blocks
                memset(block_buffer, 0, 1024);
                for (int j = 0; j < files[i].size; j++) {
                    fseek(disk, (files[i].start_block + j) * 1024, SEEK_SET);
                    fwrite(block_buffer, 1024, 1, disk);
                }

                // Update inode
                superblock.inode[files[i].inode_idx].start_block = next_free;
            }
            next_free += files[i].size;
        }

        free(block_buffer);
        fclose(disk);
        write_superblock();
    }
}

void fs_cd(char name[5]) {
    if (!current_disk) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    if (strcmp(name, ".") == 0) {
        return;  
    }
    
    if (strcmp(name, "..") == 0) {
        if (current_dir_inode != 0) {  // Not root directory
            int parent = superblock.inode[current_dir_inode].dir_parent & 0x7F;
            if (parent != 127) {  // Not root
                current_dir_inode = parent;
            }
        }
        return;
    }

    // Find directory in current directory
    int found = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) &&  // Used inode
            (superblock.inode[i].dir_parent & 0x80) &&  // Is a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_dir_inode &&  // In current directory
            compare_inode_names(superblock.inode[i].name, name)) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        fprintf(stderr, "Error: Directory %s does not exist\n", name);
        return;
    }

    current_dir_inode = found;
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
        line[strcspn(line, "\n")] = 0; // Remove newline
        if (line[0] == '\0') continue; // Skip empty lines

        char cmd = line[0];
        char args[1024];
        
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
                    if (strlen(line) < 2) {  // Just "B" 
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