/*
 * ext3 Filesystem Verification Tool.
 * This program tests the correct operation of ext3 filesystem.
 * This tool have two working modes
 * 1. full mode
 * 2. fast mode
 *	The full mode is basic mode in which program creates a subdirectory
 * in the test fileysytem, writes n(files_in_dir, default=16) large(4GB) files
 * to the directory with the test pattern at the start of each 4kb block.
 * The test pattern contains timestamp, relative file offset and per file
 * unique idenfifier(inode number). this continues until whole filesystem is
 * full and then this tooll verifies that the data in all of the test files
 * is correct.
 *	In the fast mode the tool creates a test directories with
 * EXT3_TOPDIR_FL flag set. the number of directories equals to the number
 * of block groups in the filesystem(e.g. 65536 directories for 8TB filesystem)
 * and then writes a single 1MB file in each directory. The tool then verifies
 * that the data in each file is correct.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef LUSTRE_UTILS
#define LUSTRE_UTILS
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <features.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <dirent.h>
#include <mntent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <gnu/stubs.h>
#include <gnu/stubs.h>

#ifdef HAVE_EXT2FS_EXT2FS_H
#  include <e2p/e2p.h>
#  include <ext2fs/ext2fs.h>
#endif

#define ONE_MB (1024 * 1024)
#define ONE_GB ((unsigned long long)(1024 * 1024 * 1024))
#define BLOCKSIZE 4096

/* Structure for writing test pattern */
struct block_data {
	unsigned long long bd_offset;
	unsigned long long bd_time;
	unsigned long long bd_inode;
};
static char *progname;		    /* name by which this program was run. */
static unsigned verbose = 1;	    /* prints offset in kB, operation rate */
static int readoption;		    /* run test in read-only (verify) mode */
static int writeoption;		    /* run test in write_only mode */
char *testdir;			    /* name of device to be tested. */
static unsigned full = 1;	    /* flag to full check */
static int errno_local;		    /* local copy of errno */
static unsigned long num_files;     /* Total number of files for read/write */
static loff_t file_size = 4*ONE_GB; /* Size of each file */
static unsigned files_in_dir = 32;  /* number of files in each directioy */
static unsigned num_dirs = 30000;   /* total number of directories */
const int dirmode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
static int fd = -1;
static int isatty_flag;
static int perms =  S_IRWXU | S_IRGRP | S_IROTH;

static struct option const longopts[] =
{
	{ "chunksize", required_argument, 0, 'c' },
	{ "help", no_argument, 0, 'h' },
	{ "offset", required_argument, 0, 'o' },
	{ "long", no_argument, 0, 'l' },
	{ "partial", required_argument, 0, 'p' },
	{ "quiet", required_argument, 0, 'q' },
	{ "read", no_argument, 0, 'r' },
	{ "timestamp", required_argument, 0, 't' },
	{ "verbose", no_argument, 0, 'v' },
	{ "write", no_argument, 0, 'w' },
	{ 0, 0, 0, 0}
};

/*
 * Usages: displays help information, whenever user supply --help option in
 * command or enters incorrect command line.
 */
void usage(int status)
{
	if (status != 0)
	{
	      printf("\nUsage: %s [OPTION]... <filesystem path> ...\n",
			progname);
	      printf("ext3 filesystem verification tool.\n"
		  "\t-t {seconds} for --timestamp,  set test time"
		  "(default=current time())\n"
		  "\t-o {offset}  for --offset, directory starting offset"
		  " from which tests should start\n"
		  "\t-r run test in read (verify) mode\n"
		  "\t-w run test in write (test-pattern) mode (default=r&w)\n"
		  "\t-v for verbose\n"
		  "\t-p for --partial, for partial check (1MB files)\n"
		  "\t-l for --long, full check (4GB file with 4k blocks)\n"
		  "\t-c for --chunksize, IO chunk size (default=1048576)\n"
		  "\t-h display this help and exit\n"
		  "\t--help display this help and exit\n");
	}
	exit(status);
}

/*
 * open_file: Opens file in specified mode and returns fd.
 */
static int open_file(const char *file, int flag)
{
	fd = open(file, flag, perms);
	if (fd < 0) {
		fprintf(stderr, "\n%s: Open '%s' failed:%s\n",
			progname, file, strerror(errno));
		exit(3);
	}
	return (fd);
}

/*
 * Verify_chunk: Verifies test pattern in each 4kB (BLOCKSIZE) is correct.
 * Returns 0 if test offset and timestamp is correct otherwise 1.
 */
int verify_chunk(char *chunk_buf, size_t chunksize,unsigned long long chunk_off,
		 unsigned long long time_st, unsigned long long inode_st,
		 char *file)
{
	struct block_data *bd;
	char *chunk_end;

	for (chunk_end = chunk_buf + chunksize - sizeof(*bd);
	     (char *)chunk_buf < chunk_end;
	     chunk_buf += BLOCKSIZE, chunk_off += BLOCKSIZE) {
		bd = (struct block_data *)chunk_buf;
		if ((bd->bd_offset == chunk_off) && (bd->bd_time == time_st) &&
		    (bd->bd_inode == inode_st))
			continue;
		fprintf(stderr,"\n%s: verify %s failed offset/timestamp/inode "
			"%llu/%llu/%llu: found %llu/%llu/%llu instead\n",
			progname, file, chunk_off, time_st, inode_st,
			bd->bd_offset, bd->bd_time, bd->bd_inode);
		return 1;
	}
	return 0;
}

/*
 * fill_chunk: Fills the chunk with current or user specified timestamp
 * and  offset. The test patters is filled at the beginning of
 * each 4kB(BLOCKSIZE) blocks in chunk_buf.
 */
void fill_chunk(char *chunk_buf, size_t chunksize, loff_t chunk_off,
		time_t time_st, ino_t inode_st)
{
	struct block_data *bd;
	char *chunk_end;

	for (chunk_end = chunk_buf + chunksize - sizeof(*bd);
	     (char *)chunk_buf < chunk_end;
	     chunk_buf += BLOCKSIZE, chunk_off += BLOCKSIZE) {
		bd = (struct block_data *)chunk_buf;
		bd->bd_offset = chunk_off;
		bd->bd_time = time_st;
		bd->bd_inode = inode_st;
	}
}

/*
 * write_chunk: write the chunk_buf on the device. The number of write
 * operations are based on the parameters write_end, offset, and chunksize.
 */
int write_chunks(int fd, unsigned long long offset,unsigned long long write_end,
		 char *chunk_buf, size_t chunksize, time_t time_st,
		 ino_t inode_st, const char *file)
{
	unsigned long long stride;

	stride = full ? chunksize : (ONE_GB - chunksize);
	for (offset = offset & ~(chunksize - 1); offset < write_end;
	     offset += stride) {
		if (lseek64(fd, offset, SEEK_SET) == -1) {
			fprintf(stderr, "\n%s: lseek64(%s+%llu) failed: %s\n",
				progname, file, offset, strerror(errno));
			return 1;
		}
		if (offset + chunksize > write_end)
			chunksize = write_end - offset;
		if (!full && offset > chunksize) {
			fill_chunk(chunk_buf, chunksize, offset, time_st,
				    inode_st);
			if (write(fd, chunk_buf, chunksize) < 0) {
				if (errno == ENOSPC) {
					errno_local = errno;
					return 0;
				}
				fprintf(stderr,
					"\n%s: write %s+%llu failed: %s\n",
					progname, file, offset,strerror(errno));
				return errno;
			}
			offset += chunksize;
			if (offset + chunksize > write_end)
				chunksize = write_end - offset;
		}
		fill_chunk(chunk_buf, chunksize, offset, time_st, inode_st);
		if (write(fd, (char *) chunk_buf, chunksize) < 0) {
			if (errno == ENOSPC) {
				errno_local = errno;
				return 0;
			}
			fprintf(stderr, "\n%s: write %s+%llu failed: %s\n",
				progname, file, offset, strerror(errno));
			return 1;
		}
	}
	return 0;
}

/*
 * read_chunk: reads the chunk_buf from the device. The number of read
 * operations are based on the parameters read_end, offset, and chunksize.
 */
int read_chunks(int fd, unsigned long long offset, unsigned long long read_end,
		char *chunk_buf, size_t chunksize, time_t time_st,
		ino_t inode_st, char *file)
{
	unsigned long long stride;

	stride = full ? chunksize : (ONE_GB - chunksize);
	for (offset = offset & ~(chunksize - 1); offset < read_end;
	     offset += stride) {
		if (lseek64(fd, offset, SEEK_SET) == -1) {
			fprintf(stderr, "\n%s: lseek64(%s+%llu) failed: %s\n",
				progname, file, offset, strerror(errno));
			return 1;
		}
		if (offset + chunksize > read_end)
			chunksize = read_end - offset;
		if (!full && offset > chunksize) {
			if (read(fd, chunk_buf, chunksize) < 0) {
				fprintf(stderr,
					"\n%s: read %s+%llu failed: %s\n",
					progname, file, offset,strerror(errno));
				return 1;
			}
			if (verify_chunk(chunk_buf, chunksize, offset,
					 time_st, inode_st, file) != 0)
				return 1;
			offset += chunksize;
			if (offset + chunksize >= read_end)
				chunksize = read_end - offset;
		}
		if (read(fd, chunk_buf, chunksize) < 0) {
			fprintf(stderr, "\n%s: read %s+%llu failed: %s\n",
				progname, file, offset, strerror(errno));
			return 1;
		}
		if (verify_chunk(chunk_buf, chunksize, offset, time_st,
				 inode_st, file) != 0)
			return 1;
	}
	return 0;
}

/*
 * new_file: prepares new filename using file counter and current dir.
 */
char *new_file(char *tempfile, char *cur_dir, int file_num)
{
	sprintf(tempfile, "%s/file%03d", cur_dir, file_num);
	return tempfile;
}

/*
 * new_dir: prepares new dir name using dir counters.
 */
char *new_dir(char *tempdir, int dir_num)
{
	sprintf(tempdir, "%s/dir%05d", testdir, dir_num);
	return tempdir;
}

/*
 * show_filename: Displays name of current file read/write
 */
void show_filename(char *op, char *filename)
{
	static time_t last;
	time_t now;
	double diff;

	now = time(NULL);
	diff = now - last;
	if (diff > 4 || verbose > 2) {
		if (isatty_flag)
			printf("\r");
		printf("%s File name: %s          ", op, filename);
		if (isatty_flag)
			fflush(stdout);
		else
			printf("\n");
		last = now;
	}
}

/*
 * dir_write: This function writes directories and files on device.
 * it works for both full and fast modes.
 */
static int dir_write(char *chunk_buf, size_t chunksize,
		     time_t time_st, unsigned long dir_num)
{
	char tempfile[PATH_MAX];
	char tempdir[PATH_MAX];
	struct stat64 file;
	int file_num = 999999999;
	ino_t inode_st = 0;

#ifdef HAVE_EXT2FS_EXT2FS_H
	if (!full && fsetflags(testdir, EXT2_TOPDIR_FL))
		fprintf(stderr,
			"\n%s: can't set TOPDIR_FL on %s: %s (ignoring)\n",
			progname, testdir, strerror(errno));
#endif
	for (; dir_num < num_dirs; num_files++, file_num++) {
		if (file_num >= files_in_dir) {
			if (dir_num == num_dirs - 1)
				break;

			file_num = 0;
			if (mkdir(new_dir(tempdir, dir_num), dirmode) < 0) {
				if (errno == ENOSPC)
					break;
				if (errno != EEXIST) {
					fprintf(stderr, "\n%s: mkdir %s : %s\n",
						progname, tempdir,
						strerror(errno));
					return 1;
				}
			}
			dir_num++;
		}
		fd = open_file(new_file(tempfile, tempdir, file_num),
			       O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE);

		if (fd >= 0 && fstat64(fd, &file) == 0) {
			inode_st = file.st_ino;
		} else {
			fprintf(stderr, "\n%s: write stat64 to file %s: %s",
				progname, tempfile, strerror(errno));
			exit(1);
		}

		if (verbose > 1)
			show_filename("write", tempfile);

		if (write_chunks(fd, 0, file_size, chunk_buf, chunksize,
				 time_st, inode_st, tempfile)) {
			close(fd);
			return 1;
		}
		close(fd);

		if (errno_local == ENOSPC)
			break;
	}

	if (verbose) {
		verbose++;
		show_filename("write", tempfile);
		printf("\nwrite complete\n");
		verbose--;
	}

	return 0;
}

/*
 * dir_read: This function reads directories and files on device.
 * it works for both full and fast modes.
 */
static int dir_read(char *chunk_buf, size_t chunksize,
		    time_t time_st, unsigned long dir_num)
{
	char tempfile[PATH_MAX];
	char tempdir[PATH_MAX];
	unsigned long count = 0;
	struct stat64 file;
	int file_num = 0;
	ino_t inode_st = 0;

	for (count = 0; count < num_files && dir_num < num_dirs; count++) {
		if (file_num == 0) {
			if (dir_num == num_dirs - 1)
				break;

			new_dir(tempdir, dir_num);
			dir_num++;
		}

		fd = open_file(new_file(tempfile, tempdir, file_num),
			       O_RDONLY | O_LARGEFILE);
		if (fd >= 0 && fstat64(fd, &file) == 0) {
			inode_st = file.st_ino;
		} else {
			fprintf(stderr, "\n%s: read stat64 file '%s': %s\n",
				progname, tempfile, strerror(errno));
			return 1;
		}

		if (verbose > 1)
			show_filename("read", tempfile);

		if (count == num_files)
			file_size = file.st_size;
		if (read_chunks(fd, 0, file_size, chunk_buf, chunksize,
				time_st, inode_st, tempfile)) {
			close(fd);
			return 1;
		}
		close(fd);

		if (++file_num >= files_in_dir)
			file_num = 0;
	}
	if (verbose > 1){
		verbose++;
		show_filename("read", tempfile);
		printf("\nread complete\n");
		verbose--;
	}
	return 0;
}

int main(int argc, char **argv)
{
	time_t time_st = 0;		/* Default timestamp */
	size_t chunksize = ONE_MB;	/* IO chunk size(defailt=1MB) */
	char *chunk_buf;		/* chunk buffer */
	int error = 0;
	FILE *countfile = NULL;
	char filecount[PATH_MAX];
	unsigned long dir_num = 0, dir_num_orig = 0;/* starting directory */
	char c;

	progname = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	while ((c = (char)getopt_long(argc, argv, "t:rwvplo:h",
				      longopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			chunksize = (strtoul(optarg, NULL, 0) * ONE_MB);
			if (!chunksize) {
				fprintf(stderr, "%s: Chunk size value should be"
					"a multiple of 1MB\n", progname);
				return -1;
			}
			break;
		case 'l':
			full = 1;
			break;
		case 'o': /* offset */
			dir_num = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			full = 0;
			break;
		case 'q':
			verbose = 0;
			break;
		case 'r':
			readoption = 1;
			break;
		case 't':
			time_st = (time_t)strtoul(optarg, NULL, 0);
			break;
		case 'w':
			writeoption = 1;
			break;
		case 'v':
			verbose++;
			break;

		case 'h':
		default:
			usage(1);
			return 0;
		}
	}
	testdir = argv[optind];

	if (!testdir) {
		fprintf(stderr, "%s: pathname not given\n", progname);
		usage(1);
		return -1;
	}
	if (!readoption && !writeoption) {
		readoption = 1;
		writeoption = 1;
	}
	if (!time_st)
		(void) time(&time_st);
	printf("Timestamp: %lu\n", (unsigned long )time_st);
	isatty_flag = isatty(STDOUT_FILENO);

	if (!full) {
#ifdef HAVE_EXT2FS_EXT2FS_H
		struct mntent *tempmnt;
		FILE *fp = NULL;
		ext2_filsys fs;

		if ((fp = setmntent("/etc/mtab", "r")) == NULL){
			fprintf(stderr, "%s: fail to open /etc/mtab in read"
				"mode :%s\n", progname, strerror(errno));
			goto guess;
		}

		/* find device name using filesystem */
		while ((tempmnt = getmntent(fp)) != NULL) {
			if (strcmp(tempmnt->mnt_dir, testdir) == 0)
				break;
		}

		if (tempmnt == NULL) {
			fprintf(stderr, "%s: no device found for '%s'\n",
				progname, testdir);
			endmntent(fp);
			goto guess;
		}

		if (ext2fs_open(tempmnt->mnt_fsname, 0, 0, 0,
				unix_io_manager, &fs)) {
			fprintf(stderr, "%s: unable to open ext3 fs on '%s'\n",
				progname, testdir);
			endmntent(fp);
			goto guess;
		}
		endmntent(fp);

		num_dirs = (fs->super->s_blocks_count +
			    fs->super->s_blocks_per_group - 1) /
			fs->super->s_blocks_per_group;
		if (verbose)
			printf("ext3 block groups: %u, fs blocks: %u "
			       "blocks per group: %u\n",
			       num_dirs, fs->super->s_blocks_count,
			       fs->super->s_blocks_per_group);
		ext2fs_close(fs);
#else
                goto guess;
#endif
		if (0) { /* ugh */
			struct statfs64 statbuf;
		guess:
			if (statfs64(testdir, &statbuf) == 0) {
				num_dirs = (long long)statbuf.f_blocks *
					statbuf.f_bsize / (128ULL << 20);
				if (verbose)
					printf("dirs: %u, fs blocks: %llu\n",
					       num_dirs,
					       (long long)statbuf.f_blocks);
			} else {
				fprintf(stderr, "%s: unable to stat '%s': %s\n",
					progname, testdir, strerror(errno));
				if (verbose)
					printf("dirs: %u\n", num_dirs);
			}
		}

		file_size = ONE_MB;
		chunksize = ONE_MB;
		files_in_dir = 1;
	}
	chunk_buf = (char *)calloc(chunksize, 1);
	if (chunk_buf == NULL) {
		fprintf(stderr, "Memory allocation failed for chunk_buf\n");
		return 4;
	}
	sprintf(filecount, "%s/%s.filecount", testdir, progname);
	if (writeoption) {
		(void)mkdir(testdir, dirmode);

		unlink(filecount);
		if (dir_num != 0) {
			num_files = dir_num * files_in_dir;
			if (verbose)
				printf("\n%s: %lu files already written\n",
				       progname, num_files);
		}
		if (dir_write(chunk_buf, chunksize, time_st, dir_num)) {
			error = 3;
			goto out;
		}
		countfile = fopen(filecount, "w");
		if (countfile != NULL) {
			if (fprintf(countfile, "%lu", num_files) < 1 ||
			    fflush(countfile) != 0) {
				fprintf(stderr, "\n%s: writing %s failed :%s\n",
					progname, filecount, strerror(errno));
			}
			fclose(countfile);
		}
		dir_num = dir_num_orig;
	}
	if (readoption) {
		if (!writeoption) {
			countfile = fopen(filecount, "r");
			if (countfile == NULL ||
			    fscanf(countfile, "%lu", &num_files) != 1) {
				fprintf(stderr, "\n%s: reading %s failed :%s\n",
					progname, filecount, strerror(errno));
				num_files = num_dirs * files_in_dir;
			} else {
				num_files -= (dir_num * files_in_dir);
			}
			if (countfile)
				fclose(countfile);
		}
		if (dir_read(chunk_buf, chunksize, time_st, dir_num)) {
			fprintf(stderr, "\n%s: Data verification failed\n",
				progname) ;
			error = 2;
			goto out;
		}
	}
	error = 0;
out:
	free(chunk_buf);
	return error;
}
