#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "fs-sim.h"

// Constants
#define MAX_BLOCKS_PER_FILE 127
#define MAX_FILES 126
#define BLOCK_SIZE 1024

// Global variables
FILE *disk = NULL;                 // File pointer for the virtual disk
Superblock superblock;             // Superblock in memory
uint8_t buffer[1024] = {0};        // 1 KB buffer
char current_disk[128] = "";       // Name of the currently mounted disk
int current_directory = 127;       // Root directory index (default)
int is_mounted = 0;                // Flag to check if a disk is mounted

// Function declarations
void fs_mount(char *new_disk_name);
void fs_create(char name[5], int size);
void fs_delete(char name[5]);
void fs_read(char name[5], int block_num);
void fs_write(char name[5], int block_num);
void fs_buff(char buff[1024]);
void fs_ls(void);
void fs_resize(char name[5], int new_size);
void fs_defrag(void);
void fs_cd(char name[5]);

// Helper functions (prototypes)
void execute_command(const char *line, const char *input_file, int line_num);
void handle_error(const char *input_file, int line_num);

// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *input_file = fopen(argv[1], "r");
    if (!input_file) {
        perror("Error opening input file");
        return EXIT_FAILURE;
    }

    char line[256];
    int line_num = 0;

    // Read commands from the input file
    while (fgets(line, sizeof(line), input_file)) {
        line_num++;
        execute_command(line, argv[1], line_num);
    }

    fclose(input_file);
    return EXIT_SUCCESS;
}

// Function to execute a command from the input file
void execute_command(const char *line, const char *input_file, int line_num) {
    char command;
    char arg1[256], arg2[256];
    int num_args = sscanf(line, "%c %255s %255s", &command, arg1, arg2);

    switch (command) {
        case 'M': // Mount
            if (num_args != 2) {
                handle_error(input_file, line_num);
                return;
            }
            fs_mount(arg1);
            break;

        case 'C': // Create
            if (num_args != 3) {
                handle_error(input_file, line_num);
                return;
            }
            fs_create(arg1, atoi(arg2));
            break;

        case 'D': // Delete
            if (num_args != 2) {
                handle_error(input_file, line_num);
                return;
            }
            fs_delete(arg1);
            break;

        case 'R': // Read
            if (num_args != 3) {
                handle_error(input_file, line_num);
                return;
            }
            fs_read(arg1, atoi(arg2));
            break;

        case 'W': // Write
            if (num_args != 3) {
                handle_error(input_file, line_num);
                return;
            }
            fs_write(arg1, atoi(arg2));
            break;

        case 'B': // Update buffer
            if (num_args != 2) {
                handle_error(input_file, line_num);
                return;
            }
            // Truncate arg1 to 1024 bytes
            char truncated_buffer[1024];
            strncpy(truncated_buffer, arg1, 1023);
            truncated_buffer[1023] = '\0'; // Ensure null-termination
            fs_buff((uint8_t *)truncated_buffer);
            break;

        case 'L': // List
            if (num_args != 1) {
                handle_error(input_file, line_num);
                return;
            }
            fs_ls();
            break;

        case 'E': // Resize
            if (num_args != 3) {
                handle_error(input_file, line_num);
                return;
            }
            fs_resize(arg1, atoi(arg2));
            break;

        case 'O': // Defrag
            if (num_args != 1) {
                handle_error(input_file, line_num);
                return;
            }
            fs_defrag();
            break;

        case 'Y': // Change directory
            if (num_args != 2) {
                handle_error(input_file, line_num);
                return;
            }
            fs_cd(arg1);
            break;

        default:
            handle_error(input_file, line_num);
            break;
    }
}

// Function to handle errors in command parsing
void handle_error(const char *input_file, int line_num) {
    fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
}

// Function implementations
void fs_mount(char *new_disk_name) {
    // Open the disk file
    FILE *new_disk = fopen(new_disk_name, "rb");
    if (!new_disk) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    // Read the superblock into memory
    Superblock temp_superblock;
    if (fread(&temp_superblock, sizeof(Superblock), 1, new_disk) != 1) {
        fclose(new_disk);
        fprintf(stderr, "Error: Failed to read superblock from %s\n", new_disk_name);
        return;
    }

    // Perform consistency checks
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &temp_superblock.inode[i];

        // Check 1: Free inodes must have all bits set to 0
        if (!(inode->used_size & 0x80)) { // Check if inode is free
            if (memcmp(inode, (Inode[1]){0}, sizeof(Inode)) != 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 1)\n", new_disk_name);
                fclose(new_disk);
                return;
            }
        } else {
            // Check 2: start_block and size for files
            if (!(inode->dir_parent & 0x80)) { // Not a directory
                if (inode->start_block < 1 || inode->start_block > MAX_BLOCKS_PER_FILE ||
                    (inode->start_block + (inode->used_size & 0x7F) - 1) > MAX_BLOCKS_PER_FILE) {
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                    fclose(new_disk);
                    return;
                }
            }

            // Check 3: Directories must have zero size and start_block
            if ((inode->dir_parent & 0x80) && (inode->start_block != 0 || (inode->used_size & 0x7F) != 0)) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 3)\n", new_disk_name);
                fclose(new_disk);
                return;
            }

            // Check 4: Parent inode validation
            int parent_index = inode->dir_parent & 0x7F;
            if (parent_index != 127 && (parent_index < 0 || parent_index >= MAX_FILES ||
                !(temp_superblock.inode[parent_index].used_size & 0x80) ||
                !(temp_superblock.inode[parent_index].dir_parent & 0x80))) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                fclose(new_disk);
                return;
            }
        }
    }

    // Check 5: Ensure unique names in each directory
    for (int i = 0; i < MAX_FILES; i++) {
        if (!(temp_superblock.inode[i].used_size & 0x80)) continue; // Skip free inodes

        for (int j = i + 1; j < MAX_FILES; j++) {
            if (!(temp_superblock.inode[j].used_size & 0x80)) continue;

            if (strncmp(temp_superblock.inode[i].name, temp_superblock.inode[j].name, 5) == 0 &&
                (temp_superblock.inode[i].dir_parent & 0x7F) == (temp_superblock.inode[j].dir_parent & 0x7F)) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 5)\n", new_disk_name);
                fclose(new_disk);
                return;
            }
        }
    }

    // Check 6: Free-space list consistency
    uint8_t block_usage[16] = {0};
    for (int i = 0; i < MAX_FILES; i++) {
        if (!(temp_superblock.inode[i].used_size & 0x80)) continue;

        int start_block = temp_superblock.inode[i].start_block;
        int size = temp_superblock.inode[i].used_size & 0x7F;

        for (int b = start_block; b < start_block + size; b++) {
            if (b < 1 || b > MAX_BLOCKS_PER_FILE || block_usage[b / 8] & (1 << (b % 8))) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 6)\n", new_disk_name);
                fclose(new_disk);
                return;
            }
            block_usage[b / 8] |= (1 << (b % 8));
        }
    }

    // Final free space list check
    for (int i = 0; i < 16; i++) {
        if (temp_superblock.free_block_list[i] != ~block_usage[i]) {
            fprintf(stderr, "Error: File system in %s is inconsistent (error code: 6)\n", new_disk_name);
            fclose(new_disk);
            return;
        }
    }

    // Update global state
    if (disk) fclose(disk);
    disk = new_disk;
    memcpy(&superblock, &temp_superblock, sizeof(Superblock));
    strncpy(current_disk, new_disk_name, sizeof(current_disk));
    current_directory = 127;
    is_mounted = 1;

    printf("File system mounted successfully from %s\n", new_disk_name);
}

void fs_create(char name[5], int size) {
    // Step 1: Validate name uniqueness in the current directory
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x7F) == current_directory &&
            strncmp(inode->name, name, 5) == 0) {
            fprintf(stderr, "Error: File or directory %s already exists\n", name);
            return;
        }
    }

    // Step 2: Find a free inode
    int free_inode_index = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) {
            free_inode_index = i;
            break;
        }
    }

    if (free_inode_index == -1) {
        fprintf(stderr, "Error: Superblock in disk %s is full, cannot create %s\n", current_disk, name);
        return;
    }

    // Step 3: Allocate contiguous blocks if size > 0
    int start_block = -1;
    if (size > 0) {
        int free_blocks = 0;
        for (int i = 1; i <= MAX_BLOCKS_PER_FILE; i++) {
            if (!(superblock.free_block_list[i / 8] & (1 << (i % 8)))) {
                free_blocks++;
                if (free_blocks == size) {
                    start_block = i - size + 1;
                    break;
                }
            } else {
                free_blocks = 0;
            }
        }

        if (start_block == -1) {
            fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, current_disk);
            return;
        }

        // Mark the allocated blocks in the free block list
        for (int i = start_block; i < start_block + size; i++) {
            superblock.free_block_list[i / 8] |= (1 << (i % 8));
        }
    }

    // Step 4: Populate the inode
    Inode *new_inode = &superblock.inode[free_inode_index];
    memset(new_inode, 0, sizeof(Inode));
    strncpy(new_inode->name, name, 5);
    new_inode->used_size = (0x80 | size); // Mark as used and set size
    new_inode->start_block = size > 0 ? start_block : 0;
    new_inode->dir_parent = (0x80 | current_directory); // Set as a directory if size == 0

    // Step 5: Write the updated superblock back to disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to write superblock to disk\n");
        return;
    }

    printf("Created %s successfully\n", name);
}


void fs_delete(char name[5]) {
    // Step 1: Locate the file or directory in the current working directory
    int inode_index = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x7F) == current_directory &&
            strncmp(inode->name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    // Step 2: Check if the file or directory exists
    if (inode_index == -1) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }

    // Step 3: Recursive deletion if it is a directory
    Inode *target_inode = &superblock.inode[inode_index];
    int is_directory = (target_inode->used_size & 0x7F) == 0;
    if (is_directory) {
        for (int i = 0; i < MAX_FILES; i++) {
            Inode *child_inode = &superblock.inode[i];
            if ((child_inode->used_size & 0x80) && (child_inode->dir_parent & 0x7F) == inode_index) {
                fs_delete(child_inode->name); // Recursively delete files/directories inside
            }
        }
    }

    // Step 4: Free the data blocks (if it's a file)
    int size = target_inode->used_size & 0x7F; // Extract file size
    if (size > 0) {
        int start_block = target_inode->start_block;
        for (int i = start_block; i < start_block + size; i++) {
            superblock.free_block_list[i / 8] &= ~(1 << (i % 8)); // Mark block as free
        }
    }

    // Step 5: Zero out the inode
    memset(target_inode, 0, sizeof(Inode));

    // Step 6: Write the updated superblock back to disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to write superblock to disk\n");
    }

    printf("Deleted %s successfully\n", name);
}

void fs_read(char name[5], int block_num) {
    // Step 1: Locate the file in the current working directory
    int inode_index = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x7F) == current_directory &&
            strncmp(inode->name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    // Step 2: Check if the file exists
    if (inode_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *target_inode = &superblock.inode[inode_index];
    int size = target_inode->used_size & 0x7F; // Extract file size
    int is_directory = (size == 0);

    // Step 3: Check if it is a directory
    if (is_directory) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    // Step 4: Validate block number
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    // Step 5: Read the block from disk
    int start_block = target_inode->start_block;
    int block_to_read = start_block + block_num;
    char buffer[BLOCK_SIZE];

    fseek(disk, block_to_read * BLOCK_SIZE, SEEK_SET);
    if (fread(buffer, BLOCK_SIZE, 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to read block %d of file %s\n", block_num, name);
        return;
    }

    // Step 6: Output the buffer content
    printf("Contents of block %d of file %s:\n", block_num, name);
    fwrite(buffer, 1, BLOCK_SIZE, stdout);
    printf("\n");
}


void fs_write(char name[5], int block_num) {
    // Step 1: Locate the file in the current working directory
    int inode_index = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x7F) == current_directory &&
            strncmp(inode->name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    // Step 2: Check if the file exists
    if (inode_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *target_inode = &superblock.inode[inode_index];
    int size = target_inode->used_size & 0x7F; // Extract file size
    int is_directory = (size == 0);

    // Step 3: Check if it is a directory
    if (is_directory) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    // Step 4: Validate block number
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    // Step 5: Write the buffer to the block
    int start_block = target_inode->start_block;
    int block_to_write = start_block + block_num;
    char buffer[BLOCK_SIZE];

    // Fill the buffer with user input
    printf("Enter content to write to block %d of file %s:\n", block_num, name);
    fgets(buffer, BLOCK_SIZE, stdin);

    // Write buffer to the specified block
    fseek(disk, block_to_write * BLOCK_SIZE, SEEK_SET);
    if (fwrite(buffer, BLOCK_SIZE, 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to write to block %d of file %s\n", block_num, name);
        return;
    }

    printf("Successfully wrote to block %d of file %s.\n", block_num, name);
}


void fs_buff(char buff[1024]) {
    // Zero out the buffer
    memset(buff, 0, 1024);

    // Write new bytes into the buffer
    printf("Enter new content for the buffer (up to 1024 bytes): ");
    fgets((char *)buff, 1024, stdin);

    // Remove trailing newline character if present
    size_t len = strlen((char *)buff);
    if (len > 0 && buff[len - 1] == '\n') {
        buff[len - 1] = '\0';
    }
}

void fs_ls(void) {
    // Print the special entries "." and ".."
    printf(".\n");
    if (current_directory == 127) {
        printf("..\n"); // Root directory: .. points to itself
    } else {
        printf("..\n");
    }

    // Iterate through the inodes to list files and directories
    for (int i = 0; i < MAX_FILES; i++) {
        Inode *inode = &superblock.inode[i];

        // Skip unused inodes
        if (!(inode->used_size & 0x80)) {
            continue;
        }

        // Check if the inode belongs to the current directory
        if ((inode->dir_parent & 0x7F) == current_directory) {
            if ((inode->used_size & 0x7F) == 0) { // Directory
                // Count the number of entries in the directory
                int count = 0;
                for (int j = 0; j < MAX_FILES; j++) {
                    if ((superblock.inode[j].used_size & 0x80) &&
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        count++;
                    }
                }
                printf("%s (dir, %d entries)\n", inode->name, count);
            } else { // File
                int size = inode->used_size & 0x7F;
                printf("%s (file, %d blocks)\n", inode->name, size);
            }
        }
    }
}
