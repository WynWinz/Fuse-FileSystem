/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.

	gcc -Wall `pkg-config fuse --cflags --libs` cs1550.c -o cs1550
*/

/*
 * Edwin Mellett
 * CS 1550
 * Project 4
 * Date Started: 4/15/16
 * Date Finished: 4/24/16
 * Fuse Filesystem Implementation
 */

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

//150 bytes is 1200 bits which can represent ~4.9 MB
#define BITMAP_SIZE 150

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define	MAX_FILES_IN_DIR (BLOCK_SIZE - (MAX_FILENAME + 1) - sizeof(int)) / \
	((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(unsigned long))

struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_directory_entry cs1550_directory_entry;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

struct cs1550_disk_block
{
	//The first 4 bytes will be the value 0xF113DA7A 
	unsigned long magic_number;
	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

//How many pointers in an inode?
#define NUM_POINTERS_IN_INODE (BLOCK_SIZE - sizeof(unsigned int) - sizeof(unsigned long))/sizeof(unsigned long)

struct cs1550_inode
{
	//The first 4 bytes will be the value 0xFFFFFFFF
	unsigned long magic_number;
	//The number of children this node has (either other inodes or data blocks)
	unsigned int children;
	//An array of disk pointers to child nodes (either other inodes or data)
	unsigned long pointers[NUM_POINTERS_IN_INODE];
};

typedef struct cs1550_inode cs1550_inode;

/******************************************************************************
 *
 *  HELPER FUNCTIONS BELOW
 *
 *****************************************************************************/

/*
 * retrieves first block from .disk
 * returns 1 on success -1 on failure
 */
static int get_root(cs1550_root_directory *root) {
	int value;

	FILE *f = fopen(".disk", "rb");
	value = fread(root, sizeof(cs1550_root_directory), 1, f);
	fclose(f);
	return value;	
}
/*
 * goes to index block of .disk to read in a directory
 * returns 1 on success -1 on failure
 */
static int get_directory(cs1550_directory_entry *directory, int start_block) {
	int value = -1;
	int seek;
	FILE *f = fopen(".disk", "rb");
	if(f == NULL) {
		return value;
	}
	seek = fseek(f, start_block, SEEK_SET);
	if(seek == -1) {
		return value;
	}
	//value is -1 on failure
	value = fread(directory, sizeof(cs1550_directory_entry), 1, f);
	fclose(f);
	return value;
}

/*
 * searches through root directory contained in block 0 of .disk
 * return index of directory, -1 on failure
 */
static int find_directory(char *directory) {
	cs1550_root_directory root;
	long startBlock = -1;
	int i, value;
	int found = 0;

	value = get_root(&root);
	if(value == -1) {
		return value;
	}
	//search through subdirectories
	for(i = 0; i < root.nDirectories && !found; i++) {
		if(strcmp(root.directories[i].dname, directory) == 0) {
			startBlock = root.directories[i].nStartBlock;
			found = 1;
		}
	}

	return (int) startBlock;	
}

/*
 * searches through free space structure
 * returns free block contents can be written to
 * can never return first of last block 
 * (1 and 10240)
 */
static int allocate_block(void) {
	int block = -1;
	FILE *f = fopen(".disk", "rb+");
	fseek(f, 0, SEEK_END);
	int disk_size = ftell(f);
	int offset = disk_size - BITMAP_SIZE;
	rewind(f);
	fseek(f, offset, SEEK_SET);
	char *buf = (char *) malloc(BITMAP_SIZE);
	fread(buf, BITMAP_SIZE, 1, f);
	fclose(f);

	int i;
	int j;
	unsigned char c;
	unsigned char comparison = 128;
	unsigned char temp;
	int found = 0;
	for(i = 0; i < BITMAP_SIZE && !found; i++) {
		c = buf[i];
		//if byte has free space in bitmap
		if(c != 0xff) {
			for(j = 0; j < 8; j++) {
				temp = c;
				temp &= comparison;
				if(temp != 0) {
					comparison = comparison >> 1;
				}
				else {
					//calculates block based on bitmap
					block = 1 + j + i * 8;
					if(block == 1) {
						comparison = comparison >> 1;
						block = -1;
						continue;
					}
					//check that block isn't last block, saved for bitmap
					else if(block == (disk_size / BLOCK_SIZE)) {
						block = -1;
						return block;
					}
					else {
						found = 1;
						break;
					}
				}
			}
			comparison = 128;
		}
	}
	free(buf);

	return block;
}

/*
 * update contents of bitmap
 * choice: allocate or free
 */
static void update_bitmap(const char *choice, int block_index) {
	FILE *f = fopen(".disk", "rb+");
	fseek(f, 0, SEEK_END);
	int disk_size = ftell(f);
	int offset = disk_size - BITMAP_SIZE;
	rewind(f);
	fseek(f, offset, SEEK_SET);
	char *buf = (char *) malloc(BITMAP_SIZE);
	fread(buf, BITMAP_SIZE, 1, f);
	int byte_index = (int) ((block_index + 8 - 1) / 8);
	int bit_offset = (block_index-1) % 8;
	unsigned char update = buf[byte_index-1];

	if(strcmp(choice, "allocate") == 0) {
		// we | the bit with 1
		unsigned char updateValue = 128;
		updateValue = (updateValue >> bit_offset) | (updateValue << (8 - bit_offset));
		update |= updateValue; 
	}
	if(strcmp(choice, "free") == 0) {
		// we & the bit with 0
		unsigned char updateValue = 127;
		updateValue = (updateValue >> bit_offset) | (updateValue << (8 - bit_offset));
		update &= updateValue;
	}

	buf[byte_index-1] = update;
	//write updated value back to disk
	rewind(f);
	fseek(f, offset, SEEK_SET);
	fwrite(buf, BITMAP_SIZE, 1, f);
	fclose(f);
	//free bitmap buffer
	free(buf);
}

/*
 * searches the given directory for a specific file
 * returns -1 on failure, file index on success
 */
static int find_file(cs1550_directory_entry *cur_directory, char *filename, char *extension) {
	cs1550_directory_entry directory;
	directory = *cur_directory;
	//check if file exists
	int i; 
	for(i = 0; i < directory.nFiles; i++) {
		if(strcmp(directory.files[i].fname, filename) == 0) {
			if(strcmp(directory.files[i].fext, extension) == 0) {
				//file index in directory
				return i;
			}	
		}
	}

	return -1;
}
/*
 * finds data block on disk
 * returns 1 on success, -1 on failure
 */
static int get_disk_block(cs1550_disk_block* cur_disk_block, int start_block) {
	FILE *f = fopen(".disk", "rb+");
	fseek(f, start_block, SEEK_SET);
	int result = fread(cur_disk_block, sizeof(cs1550_disk_block), 1, f);
	fclose(f);
	return result;
}

/******************************************************************************
 *
 *  END OF HELPER FUNCTIONS
 *
 *****************************************************************************/

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	cs1550_directory_entry cur_directory;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
	int directory_block;
	int dir_found;
	int found = 0;
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} 
	else {

		//initialize directories to null character
		directory[0] = '\0';
		filename[0] = '\0';
		extension[0] = '\0';

		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

		//check if subdirectory
		if(strcmp(directory, "\0") != 0 && strcmp(filename, "\0") == 0) {
			directory_block = find_directory(directory);
			if(directory_block != -1) {
				dir_found = get_directory(&cur_directory, directory_block);
				//if get directory fails, return error no entry
				if(dir_found == -1) {
					return -ENOENT;
				}
				else {
					//Might want to return a structure with these fields
					stbuf->st_mode = S_IFDIR | 0755;
					stbuf->st_nlink = 2;
					res = 0; //no error
				}
			}
			else {
				res = -ENOENT;
			}
		}
		//regular files
		else {
			directory_block = find_directory(directory);
			dir_found = get_directory(&cur_directory, directory_block);
			int i;
			res = -ENOENT;
			//search through files in directory
			//for filename and extension match
			for(i = 0; i < cur_directory.nFiles && !found; i++) {
				if(strcmp(cur_directory.files[i].fname, filename) == 0) {
					if(strcmp(cur_directory.files[i].fext, extension) == 0) {
						//regular file, probably want to be read and write
						stbuf->st_mode = S_IFREG | 0666; 
						stbuf->st_nlink = 1; //file links
						stbuf->st_size = cur_directory.files[i].fsize; //file size
						res = 0; // no error
						found = 1;	
					}
				}
			}
		}
	}
	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	cs1550_directory_entry cur_directory;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
	char file[(MAX_FILENAME+1) + 1 + (MAX_EXTENSION+1)];
	int i, directory_block, dir_found;
	int res;

	//initialize directories to null character
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//add contents of subdirectory
	if (strcmp(path, "/") != 0) {
		directory_block = find_directory(directory);
		if(directory_block != -1) {
			dir_found = get_directory(&cur_directory, directory_block);
			if(dir_found == -1) {
				return -ENOENT;
			}

			filler(buf, ".", NULL, 0);
			filler(buf, "..", NULL, 0);
			for(i = 0; i < cur_directory.nFiles; i++) {
				//file is filename.ext
				if(strlen(cur_directory.files[i].fext) > 0) {
					sprintf(file, "%s.%s", cur_directory.files[i].fname, cur_directory.files[i].fext);
				}
				else {
					sprintf(file, "%s", cur_directory.files[i].fname);
				}
				filler(buf, file, NULL, 0);
			}

			res = 0;
		}
		else {
			return -ENOENT;
		}
	}
	//add contents of root
	else {
		cs1550_root_directory root;
		int value;
		i = 0;

		value = get_root(&root);
		if(value == -1) {
			return -ENOENT;
		}

		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		for(i = 0; i < root.nDirectories; i++) {
			filler(buf, root.directories[i].dname, NULL, 0);
		}
		res = 0;		
	}

	return res;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) mode;

	cs1550_root_directory root;
	//char directory[MAX_FILENAME+1];
	char directory[20];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
	int value;
	int start_block;

	//initialize directories to null character
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	if(strlen(directory) > MAX_FILENAME) {
		return -ENAMETOOLONG;
	}

	if(strcmp(path, "/") == 0 || strcmp(filename, "\0") != 0) {
		return -EPERM;
	}
	value = find_directory(directory);
	if(value == -1) {
		if(directory != NULL) {
			//check if name is valid
			value = get_root(&root);
			if(value == -1 || root.nDirectories >= MAX_DIRS_IN_ROOT) {
				return -EPERM;
			}
			strcpy(root.directories[root.nDirectories].dname, directory);

			//allocate block for new directory
			start_block = allocate_block();
			update_bitmap("allocate", start_block);
			root.directories[root.nDirectories].nStartBlock = (long) (BLOCK_SIZE * start_block);
			root.nDirectories = root.nDirectories + 1;
			
			//create emptry directory
			cs1550_directory_entry new_directory;
			new_directory.nFiles = 0;
			FILE *f = fopen(".disk", "rb+");
			int seek;
			int offset = root.directories[root.nDirectories-1].nStartBlock;

			//update root
			seek = fseek(f, offset, SEEK_SET);
			if(seek == -1) {
				return value;
			}
			fwrite(&new_directory, sizeof(cs1550_directory_entry), 1, f);
			fseek(f, 0, SEEK_SET);
			
			fwrite(&root, sizeof(cs1550_root_directory), 1, f);
			fclose(f);
		}
	}
	else {
		return -EEXIST;
	}
	return 0;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	(void) path;

	cs1550_directory_entry cur_directory;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
	int start_block;

	//initialize directories to null character	
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';
	

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//check if directory is root
	if(strcmp(filename, "\0") == 0) {
		return -EPERM;
	}
	//check if name is valid
	if(strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) {
		return -ENAMETOOLONG;
	}
	start_block = find_directory(directory);
	//do I need to check if directory exists?
	if(start_block == -1) {
		return -ENOENT;
	}
	get_directory(&cur_directory, start_block);
	//check that new file can be made
	if(cur_directory.nFiles >= MAX_FILES_IN_DIR) {
		return -EPERM;
	}
	//check if file exists
	int i; 
	for(i = 0; i < cur_directory.nFiles; i++) {
		if(strcmp(cur_directory.files[i].fname, filename) == 0) {
			if(strcmp(cur_directory.files[i].fext, extension) == 0) {
				//file exists
				return -EEXIST;
			}	
		}
	}

	//allocate a new inode
	cs1550_inode new_inode;
	new_inode.children = 0;
	new_inode.magic_number = 0XFFFFFFFF;
	int inode_block = allocate_block();
	update_bitmap("allocate", inode_block);

	//update directory
	int fileNum = cur_directory.nFiles;
	strcpy(cur_directory.files[fileNum].fname, filename);
	strcpy(cur_directory.files[fileNum].fext, extension);
	cur_directory.files[fileNum].fsize = 0;
	cur_directory.files[fileNum].nStartBlock = (long) inode_block * BLOCK_SIZE;
	cur_directory.nFiles = fileNum + 1;

	FILE *f = fopen(".disk", "rb+");
	int seek;
	int offset = start_block;
	//write directory to disk
	seek = fseek(f, offset, SEEK_SET);
	if(seek == -1) {
		return seek;
	}
	fwrite(&cur_directory, sizeof(cs1550_directory_entry), 1, f);
	rewind(f);
	//write inode to disk
	offset = cur_directory.files[fileNum].nStartBlock;
	seek = fseek(f, offset, SEEK_SET);
	fwrite(&new_inode, sizeof(cs1550_inode), 1, f);
	fclose(f);

	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;
	int f_size;

	cs1550_directory_entry cur_directory;
	cs1550_inode inode;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	//initialize directories to null character
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//read in data
	int directory_index = find_directory(directory);
	get_directory(&cur_directory, directory_index);
	int index = find_file(&cur_directory, filename, extension);
	f_size = cur_directory.files[index].fsize;

	//check that size is > 0
	if(f_size < 0) {
		return -ENOENT;
	}
	//check if path is a directory
	if(strcmp(filename, "\0") == 0) {
		return -EISDIR;
	}

	//get inode for file
	int inode_start = cur_directory.files[index].nStartBlock;
	//read in inode from disk
	FILE *f = fopen(".disk", "rb+");
	fseek(f, inode_start, SEEK_SET);
	fread(&inode, sizeof(cs1550_inode), 1, f);
	fclose(f); 

	int i;
	int block_index = 0;
	//free data blocks
 	for(i = 0; i < inode.children; i++) {
 		block_index = ((long)inode.pointers[i]) / BLOCK_SIZE;
 		update_bitmap("free", block_index);
 	}

 	//free inode
	update_bitmap("free", inode_start / BLOCK_SIZE);

	//update directory
	cur_directory.nFiles = cur_directory.nFiles - 1;
	int j;
	for(j = index; j < cur_directory.nFiles; j++) {
		strcpy(cur_directory.files[j].fname, cur_directory.files[j+1].fname);
		strcpy(cur_directory.files[j].fext, cur_directory.files[j+1].fext);
		cur_directory.files[j].fsize = cur_directory.files[j+1].fsize;
		cur_directory.files[j].nStartBlock = cur_directory.files[j+1].nStartBlock;	
	}	

	//write updated directory
	f = fopen(".disk", "rb+");
	fseek(f, directory_index, SEEK_SET);
	fwrite(&cur_directory, sizeof(cs1550_directory_entry), 1, f);
	fclose(f);
	  
    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	int f_size;
	int d_block = -1;

	cs1550_directory_entry cur_directory;
	cs1550_inode inode;
	cs1550_disk_block cur_disk_block;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	//initialize directories to null character
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//read in data
	int directory_index = find_directory(directory);
	get_directory(&cur_directory, directory_index);
	int index = find_file(&cur_directory, filename, extension);
	f_size = cur_directory.files[index].fsize;

	//check that offset is <= to the file size
	if(offset > f_size) {
		//printf("Too big, return\n");
		return -EFBIG;
	}
	//check that size is > 0
	if(f_size < 0) {
		return -ENOENT;
	}
	//check if path is a directory
	if(strcmp(filename, "\0") == 0) {
		return -EISDIR;
	}

	//get inode for file
	int inode_start = cur_directory.files[index].nStartBlock;
	//read in inode from disk
	FILE *f = fopen(".disk", "rb+");
	fseek(f, inode_start, SEEK_SET);
	fread(&inode, sizeof(cs1550_inode), 1, f);
	fclose(f); 

	int i;
	int block_index = 0;
	if(inode.children > 0) {
		block_index = offset / (MAX_DATA_IN_BLOCK);
		d_block = ((long)inode.pointers[block_index]) / BLOCK_SIZE;
		get_disk_block(&cur_disk_block, d_block * BLOCK_SIZE);
	}	

	int read_start = offset % (MAX_DATA_IN_BLOCK-1);
	int bytes_read = 0;

	strcpy(buf, &cur_disk_block.data[read_start]);
	bytes_read += (MAX_DATA_IN_BLOCK-read_start);

	int numReads = (size + offset + MAX_DATA_IN_BLOCK-1) / (MAX_DATA_IN_BLOCK); 

	int start = block_index + 1;
	
	for(i = start; i < numReads && i < inode.children; i++) {
		d_block = ((long)inode.pointers[i]) / BLOCK_SIZE;
		get_disk_block(&cur_disk_block, d_block * BLOCK_SIZE);
		strcat(buf, cur_disk_block.data);
	}

	//set size and return
	return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	int f_size;
	int result;
	int d_block = -1;

	cs1550_directory_entry cur_directory;
	cs1550_inode inode;
	cs1550_disk_block cur_disk_block;
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	//initialize directories to null character
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	
	int directory_index = find_directory(directory);
	get_directory(&cur_directory, directory_index);
	int index = find_file(&cur_directory, filename, extension);
	//check to make sure path exists
	if(index == -1) {
		return -ENOENT;
	}
	f_size = cur_directory.files[index].fsize;
	//check that offset is <= to the file size
	if(offset > f_size) {
		result = -EFBIG;
		offset = f_size;
	}
	else {
		result = size;
	}
	//get inode for file
	int inode_start = cur_directory.files[index].nStartBlock;
	//read in inode from disk
	FILE *f = fopen(".disk", "rb+");
	fseek(f, inode_start, SEEK_SET);
	fread(&inode, sizeof(cs1550_inode), 1, f); 

	//equivalent to ceil((size+offset)/(MAX_DATA_IN_BLOCK-1))
	int blocks_needed = (size + offset + MAX_DATA_IN_BLOCK-2) / (MAX_DATA_IN_BLOCK-1);

	//check if inode contains children
	printf("Blocks needed: %d\n", blocks_needed);
	while(blocks_needed > inode.children) {
		cur_disk_block.magic_number = 0xF113DA7A;
		d_block = allocate_block();
		update_bitmap("allocate", d_block);
		inode.pointers[inode.children] = (unsigned long) d_block * BLOCK_SIZE;
		inode.children = inode.children + 1;

		//write new disk block
		fseek(f, d_block * BLOCK_SIZE, SEEK_SET);
		fwrite(&cur_disk_block, sizeof(cs1550_disk_block), 1, f);
		rewind(f);
	}

	//get disk block and write first block
	int block_index = offset / (MAX_DATA_IN_BLOCK-1);
	d_block = ((long)inode.pointers[block_index]) / BLOCK_SIZE;
	get_disk_block(&cur_disk_block, d_block * BLOCK_SIZE);

	int start_point = offset % MAX_DATA_IN_BLOCK;
	strncpy(&cur_disk_block.data[start_point], buf, MAX_DATA_IN_BLOCK-start_point-1);	
	buf+=(MAX_DATA_IN_BLOCK-1);

	fseek(f, d_block * BLOCK_SIZE, SEEK_SET);
	fwrite(&cur_disk_block, sizeof(cs1550_disk_block), 1, f);
	rewind(f);

	int start = block_index + 1;
	int i;
	//write the rest of the blocks
	for(i = start; i < blocks_needed; i++) {
		d_block = ((long)inode.pointers[i]) / BLOCK_SIZE;
		get_disk_block(&cur_disk_block, d_block * BLOCK_SIZE);
		strncpy(cur_disk_block.data, buf, MAX_DATA_IN_BLOCK-1);

		fseek(f, d_block * BLOCK_SIZE, SEEK_SET);
		fwrite(&cur_disk_block, sizeof(cs1550_disk_block), 1, f);
		rewind(f);

		buf+=(MAX_DATA_IN_BLOCK-1);
	}
	
	//update file size
	if(offset == 0) {
		cur_directory.files[index].fsize = size;
	}
	else {
		cur_directory.files[index].fsize += size;	
	}
	
	//write updated file size
	fseek(f, directory_index, SEEK_SET);
	fwrite(&cur_directory, sizeof(cs1550_directory_entry), 1, f);
	rewind(f);

	//write updated inode
	fseek(f, inode_start, SEEK_SET);
	fwrite(&inode, sizeof(cs1550_inode), 1, f);
	rewind(f);	
	fclose(f);
	
	//set size (should be same as input) and return, or error
	return result;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}