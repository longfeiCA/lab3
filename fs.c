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

void fs_mount(char *new_disk_name) {
    // Close the current disk if already mounted
    if (disk != NULL) {
        fclose(disk);
        disk = NULL;
    }

    // Attempt to open the new disk file
    disk = fopen(new_disk_name, "rb+");
    if (disk == NULL) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    // Read the superblock from the disk
    if (fread(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to read superblock from disk %s\n", new_disk_name);
        fclose(disk);
        disk = NULL;
        return;
    }

    // Perform consistency checks on the file system
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];

        // Check for inconsistency based on provided rules
        if (!(inode->used_size & 0x80)) { // Unused inode
            if (inode->name[0] != '\0' || inode->start_block != 0 || inode->dir_parent != 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 1)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }
        } else { // Used inode
            // Check start_block range for files
            if (!(inode->dir_parent & 0x80) && (inode->start_block < 1 || inode->start_block > 127)) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }

            // Check size validity for files
            uint8_t size = inode->used_size & 0x7F; // Extract size (last 7 bits)
            if (!(inode->dir_parent & 0x80) && (inode->start_block + size - 1 > 127)) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }

            // Check directories for start_block and size = 0
            if ((inode->dir_parent & 0x80) && (inode->start_block != 0 || size != 0)) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 3)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }

            // Validate parent inode index
            uint8_t parent_index = inode->dir_parent & 0x7F;
            if (parent_index == 126 || (parent_index < 126 && !(superblock.inode[parent_index].used_size & 0x80))) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }
        }
    }

    // Check for unique file/directory names within each directory
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) continue;

        for (int j = i + 1; j < 126; j++) {
            if (!(superblock.inode[j].used_size & 0x80)) continue;

            if ((superblock.inode[i].dir_parent & 0x7F) == (superblock.inode[j].dir_parent & 0x7F) &&
                strncmp(superblock.inode[i].name, superblock.inode[j].name, 5) == 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 5)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }
        }
    }

    // Check the free-space list for discrepancies
    char block_usage[128] = {0};
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80) || (superblock.inode[i].dir_parent & 0x80)) continue;

        uint8_t start_block = superblock.inode[i].start_block;
        uint8_t size = superblock.inode[i].used_size & 0x7F;
        for (int j = 0; j < size; j++) {
            if (block_usage[start_block + j] || superblock.free_block_list[(start_block + j) / 8] & (1 << ((start_block + j) % 8)) == 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 6)\n", new_disk_name);
                fclose(disk);
                disk = NULL;
                return;
            }
            block_usage[start_block + j] = 1;
        }
    }

    // Set current working directory to root
    current_directory = 127;

    // Update current disk name
    strncpy(current_disk, new_disk_name, sizeof(current_disk) - 1);
    current_disk[sizeof(current_disk) - 1] = '\0';

    is_mounted = 1;

    printf("File system successfully mounted on %s\n", new_disk_name);
}

void fs_create(char name[5], int size) {
    // Check if the disk is mounted
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Validate file name
    if (strlen(name) > 5) {
        fprintf(stderr, "Error: File name exceeds maximum length of 5 characters\n");
        return;
    }

    // Validate file size
    if (size <= 0 || size > 127) {
        fprintf(stderr, "Error: Invalid file size %d\n", size);
        return;
    }

    // Check if there is an unused inode
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) { // Unused inode
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: No free inodes available\n");
        return;
    }

    // Check for name conflict in the current directory
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) && // File, not directory
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && 
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            fprintf(stderr, "Error: File with name %s already exists in the current directory\n", name);
            return;
        }
    }

    // Find a contiguous block of free space for the file
    int start_block = -1;
    int free_blocks = 0;
    for (int i = 1; i < 128; i++) {
        if (superblock.free_block_list[i / 8] & (1 << (i % 8))) {
            if (free_blocks == 0) {
                start_block = i; // Start of a potential block
            }
            free_blocks++;
            if (free_blocks == size) {
                break; // Found enough space
            } else if (i == 127) {
                start_block = -1;
                free_blocks = 0;
            }
        } else {
            start_block = -1;
            free_blocks = 0;
        }
    }

    if (free_blocks < size) {
        fprintf(stderr, "Error: Not enough free space available to create the file\n");
        return;
    }

    // Allocate the blocks in the free-space list
    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start_block + i) / 8] &= ~(1 << ((start_block + i) % 8));
    }

    // Initialize the inode
    Inode *inode = &superblock.inode[inode_index];
    strncpy(inode->name, name, 5);
    inode->name[5] = '\0'; // Ensure null-terminated
    inode->used_size = 0x80 | size; // Mark as used and set size
    inode->start_block = start_block;
    inode->dir_parent = current_directory & 0x7F; // Set parent directory

    // Update the superblock on disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to update the superblock on disk\n");
        return;
    }

    printf("File %s created successfully with size %d blocks\n", name, size);
}

void fs_delete(char name[5]) {
    // Check if the disk is mounted
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file in the current directory
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && // Inode is used
            !(superblock.inode[i].dir_parent & 0x80) && // It's a file, not a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && // In current directory
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File with name %s not found in the current directory\n", name);
        return;
    }

    // Get the inode and release allocated blocks
    Inode *inode = &superblock.inode[inode_index];
    int start_block = inode->start_block;
    int size = inode->used_size & 0x7F; // File size

    for (int i = 0; i < size; i++) {
        superblock.free_block_list[(start_block + i) / 8] |= (1 << ((start_block + i) % 8)); // Mark blocks as free
    }

    // Clear the inode
    memset(inode, 0, sizeof(Inode));

    // Update the superblock on disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to update the superblock on disk\n");
        return;
    }

    printf("File %s deleted successfully\n", name);
}

void fs_read(char name[5], int block_num) {
    // Check if the disk is mounted
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file in the current directory
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && // Inode is used
            !(superblock.inode[i].dir_parent & 0x80) && // It's a file, not a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && // In current directory
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File with name %s not found in the current directory\n", name);
        return;
    }

    // Get the inode
    Inode *inode = &superblock.inode[inode_index];
    int file_size = inode->used_size & 0x7F; // File size in blocks

    // Validate the requested block number
    if (block_num < 0 || block_num >= file_size) {
        fprintf(stderr, "Error: Invalid block number %d for file %s\n", block_num, name);
        return;
    }

    // Calculate the block's position on disk
    int start_block = inode->start_block;
    int block_to_read = start_block + block_num;

    // Read the block from disk
    char buffer[1024]; // Each block is 1024 bytes
    fseek(disk, block_to_read * 1024, SEEK_SET);
    if (fread(buffer, 1024, 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to read block %d of file %s from disk\n", block_num, name);
        return;
    }

    // Print the block contents to stdout
    printf("Contents of block %d of file %s:\n", block_num, name);
    for (int i = 0; i < 1024; i++) {
        printf("%c", buffer[i]);
    }
    printf("\n");
}

void fs_write(char name[5], int block_num) {
    // Check if the disk is mounted
    if (disk == NULL) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find the file in the current directory
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && // Inode is used
            !(superblock.inode[i].dir_parent & 0x80) && // It's a file, not a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && // In current directory
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File with name %s not found in the current directory\n", name);
        return;
    }

    // Get the inode
    Inode *inode = &superblock.inode[inode_index];
    int file_size = inode->used_size & 0x7F; // File size in blocks

    // Validate the requested block number
    if (block_num < 0 || block_num >= file_size) {
        fprintf(stderr, "Error: Invalid block number %d for file %s\n", block_num, name);
        return;
    }

    // Calculate the block's position on disk
    int start_block = inode->start_block;
    int block_to_write = start_block + block_num;

    // Write the data to the specified block
    fseek(disk, block_to_write * 1024, SEEK_SET);
    if (fwrite(buffer, 1024, 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to write to block %d of file %s on disk\n", block_num, name);
        return;
    }

    printf("Data successfully written to block %d of file %s.\n", block_num, name);
}

void fs_buff(char buff[1024]) {
    // Load the provided buffer into the global buffer
    if (buff == NULL) {
        fprintf(stderr, "Error: Provided buffer is NULL\n");
        return;
    }

    // Copy the provided data into the global buffer
    memcpy(buffer, buff, 1024);

    printf("Buffer updated successfully.\n");
}

void fs_ls(void) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No disk is currently mounted.\n");
        return;
    }

    printf("File System Contents:\n");
    printf("---------------------\n");
    printf("%-6s %-6s\n", "Name", "Size");

    int file_count = 0;

    // Iterate through the inodes to find valid files and directories in the current directory
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_directory) {
            if (superblock.inode[i].dir_parent & 0x80) { // Directory
                int num_children = 0;
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) && 
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        num_children++;
                    }
                }
                printf("%-6s %3d\n", superblock.inode[i].name, num_children);
            } else { // File
                printf("%-6s %3d KB\n", superblock.inode[i].name, superblock.inode[i].used_size & 0x7F);
            }
            file_count++;
        }
    }

    if (file_count == 0) {
        printf("No files found in the current directory.\n");
    }
}

void fs_resize(char name[5], int new_size) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No disk is currently mounted.\n");
        return;
    }

    // Check for valid size
    if (new_size < 0 || new_size > MAX_BLOCKS_PER_FILE) {
        fprintf(stderr, "Error: Invalid new size. Must be between 0 and %d.\n", MAX_BLOCKS_PER_FILE);
        return;
    }

    // Find the file in the current directory
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            !(superblock.inode[i].dir_parent & 0x80) && // It's a file, not a directory
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && 
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: File '%s' not found.\n", name);
        return;
    }

    // Current size and start block of the file
    Inode *inode = &superblock.inode[inode_index];
    int current_size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    if (new_size == current_size) {
        printf("File '%s' is already of size %d. No resizing needed.\n", name, new_size);
        return;
    }

    if (new_size > current_size) {
        // Expanding the file
        int blocks_needed = new_size - current_size;
        int free_blocks = 0;
        int new_start_block = -1;

        // Find a contiguous block of free space for the file
        for (int i = 1; i < 128; i++) {
            if (superblock.free_block_list[i / 8] & (1 << (i % 8))) {
                if (free_blocks == 0) {
                    new_start_block = i; // Start of a potential block
                }
                free_blocks++;
                if (free_blocks == blocks_needed) {
                    break; // Found enough space
                }
            } else {
                new_start_block = -1;
                free_blocks = 0;
            }
        }

        if (free_blocks < blocks_needed) {
            fprintf(stderr, "Error: Not enough free space to expand file '%s' to size %d.\n", name, new_size);
            return;
        }

        // Allocate the blocks in the free-space list
        for (int i = 0; i < blocks_needed; i++) {
            superblock.free_block_list[(new_start_block + i) / 8] &= ~(1 << ((new_start_block + i) % 8));
        }

        // Copy data to new blocks
        for (int i = 0; i < current_size; i++) {
            fseek(disk, (start_block + i) * 1024, SEEK_SET);
            fread(buffer, 1024, 1, disk);
            fseek(disk, (new_start_block + i) * 1024, SEEK_SET);
            fwrite(buffer, 1024, 1, disk);
        }

        // Zero out old blocks
        for (int i = 0; i < current_size; i++) {
            fseek(disk, (start_block + i) * 1024, SEEK_SET);
            memset(buffer, 0, 1024);
            fwrite(buffer, 1024, 1, disk);
            superblock.free_block_list[(start_block + i) / 8] |= (1 << ((start_block + i) % 8));
        }

        // Update the inode
        inode->start_block = new_start_block;
        inode->used_size = 0x80 | new_size;

        printf("File '%s' resized to %d blocks.\n", name, new_size);
    } else {
        // Shrinking the file
        for (int i = current_size - 1; i >= new_size; i--) {
            int block_to_free = start_block + i;
            fseek(disk, block_to_free * 1024, SEEK_SET);
            memset(buffer, 0, 1024);
            fwrite(buffer, 1024, 1, disk);
            superblock.free_block_list[block_to_free / 8] |= (1 << (block_to_free % 8));
        }

        // Update the inode
        inode->used_size = 0x80 | new_size;

        printf("File '%s' resized to %d blocks.\n", name, new_size);
    }

    // Update the superblock on disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to update the superblock on disk\n");
        return;
    }
}

void fs_defrag(void) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No disk is currently mounted.\n");
        return;
    }

    printf("Starting defragmentation...\n");

    int free_block_index = 1;  // Tracks the next available free block for compaction

    // Find the first free block
    while (free_block_index < 128 && (superblock.free_block_list[free_block_index / 8] & (1 << (free_block_index % 8)))) {
        free_block_index++;
    }

    for (int i = 0; i < 126; i++) {
        if (superblock.inode[i].used_size & 0x80 && !(superblock.inode[i].dir_parent & 0x80)) {  // Active file
            int start_block = superblock.inode[i].start_block;
            int size = superblock.inode[i].used_size & 0x7F;

            for (int j = 0; j < size; j++) {
                int block = start_block + j;

                if (block > free_block_index) {
                    // Move the block to the free position
                    printf("Moving block %d (file '%s', part %d) to %d...\n", 
                        block, superblock.inode[i].name, j, free_block_index);

                    // Read the block from the old position
                    fseek(disk, block * 1024, SEEK_SET);
                    fread(buffer, 1024, 1, disk);

                    // Write the block to the new position
                    fseek(disk, free_block_index * 1024, SEEK_SET);
                    fwrite(buffer, 1024, 1, disk);

                    // Update the free-space list
                    superblock.free_block_list[block / 8] |= (1 << (block % 8)); // Mark old block as free
                    superblock.free_block_list[free_block_index / 8] &= ~(1 << (free_block_index % 8)); // Mark new block as used

                    // Update the inode's start block if necessary
                    if (j == 0) {
                        superblock.inode[i].start_block = free_block_index;
                    }

                    // Find the next free block
                    while (free_block_index < 128 && (superblock.free_block_list[free_block_index / 8] & (1 << (free_block_index % 8)))) {
                        free_block_index++;
                    }
                }
            }
        }
    }

    // Update the superblock on disk
    fseek(disk, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk) != 1) {
        fprintf(stderr, "Error: Failed to update the superblock on disk\n");
        return;
    }

    printf("Defragmentation completed. All files are now stored contiguously.\n");
}

void fs_cd(char name[5]) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No disk is currently mounted.\n");
        return;
    }

    // Check if the name is "." or ".."
    if (strcmp(name, ".") == 0) {
        // Current directory
        return;
    } else if (strcmp(name, "..") == 0) {
        // Parent directory
        if (current_directory == 127) {
            // Already in the root directory
            return;
        }

        // Find the parent directory
        for (int i = 0; i < 126; i++) {
            if ((superblock.inode[i].used_size & 0x80) && 
                (superblock.inode[i].dir_parent & 0x80) && 
                (superblock.inode[i].dir_parent & 0x7F) == current_directory) {
                current_directory = superblock.inode[i].dir_parent & 0x7F;
                break;
            }
        }

        return;
    }

    // Find the directory in the current directory
    int inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && 
            (superblock.inode[i].dir_parent & 0x80) && 
            (superblock.inode[i].dir_parent & 0x7F) == current_directory && 
            strncmp(superblock.inode[i].name, name, 5) == 0) {
            inode_index = i;
            break;
        }
    }

    if (inode_index == -1) {
        fprintf(stderr, "Error: Directory %s does not exist\n", name);
        return;
    }

    // Update the current directory
    current_directory = inode_index;
    printf("Changed directory to %s\n", name);
}