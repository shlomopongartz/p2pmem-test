/*
 * Raithlin Consulting Inc. p2pmem test suite
 * Copyright (c) 2017, Raithlin Consulting Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

/* if this header fill is missing install linux-libc-dev
 * e.g. sudo apt install linux-libc-dev
 * linux-libc-devel on Fedora???
 * linux-libc-devel
 * liburing-dev?
 */
#include <linux/io_uring.h>
#include <liburing.h>

#define QD	64
#define BS	(32*1024)

#include <argconfig/argconfig.h>
#include <argconfig/report.h>
#include <argconfig/suffix.h>
#include <argconfig/timing.h>

#include "version.h"


#if defined(__x86_64__)
#define mb()	asm volatile("mfence" : : : "memory")
#define rmb()   asm volatile("lfence" : : : "memory")
#define wmb()   asm volatile("sfence" : : : "memory")
#define read_brrier()	asm volatile("lfence" : : : "memory")
#define write_barrier()	asm volatile("sfence" : : : "memory")
#else
#error "Define memory barrier to the architecture"
#endif

#define likely(x)   	__builtin_expect(!!(x), 1)
#define unlikely(x) 	__builtin_expect(!!(x), 0)

#define min(a, b)				\
	({ __typeof__ (a) _a = (a);		\
		__typeof__ (b) _b = (b);	\
		_a < _b ? _a : _b; })

#define max(a, b)				\
	({ __typeof__ (a) _a = (a);		\
		__typeof__ (b) _b = (b);	\
		_a > _b ? _a : _b; })

const char *def_str = "default string";
const char *desc = "Perform p2pmem and NVMe CMB testing (ver=" VERSION ")";

static struct {
	int nvme_read_fd;
	const char *nvme_read_filename;
	int nvme_write_fd;
	const char *nvme_write_filename;
	int p2pmem_fd;
	const char *p2pmem_filename;
	void     *buffer;
	unsigned check;
	size_t   chunk_size;
	size_t   chunks;
	int      duration;
	unsigned host_accesses;
	unsigned host_access_stop;
	int      host_access_sz;
	size_t   init_tot;
	unsigned init_stop;
	unsigned init_sz;
	size_t   offset;
	unsigned overlap;
	long     page_size;
	int      read_parity;
	uint64_t rsize;
	int      seed;
	size_t   size;
	size_t   size_mmap;
	unsigned skip_read;
	unsigned skip_write;
	struct timeval time_start;
	struct timeval time_end;
	struct rusage usage_start;
	struct rusage usage_end;
	size_t   threads;
	int      write_parity;
	uint64_t wsize;
	unsigned dump;
	unsigned chksum;
	unsigned fill;
	unsigned iodepth;
} cfg = {
	.buffer         = 0,
	.check          = 0,
	.chunk_size     = 4096,
	.chunks         = 1024,
	.duration       = -1,
	.host_accesses  = 0,
	.host_access_sz = 0,
	.host_access_stop = 0,
	.init_sz        = 0,
	.init_tot       = 0,
	.init_stop      = 0,
	.offset         = 0,
	.overlap        = 0,
	.seed           = -1,
	.skip_read      = 0,
	.skip_write     = 0,
	.threads        = 1,
	.dump           = 0,
	.chksum         = 0,
	.fill           = 0,
	.iodepth	= QD,
};

struct context;
struct thread_info {
	pthread_t thread_id;
	size_t    thread;
	size_t    total;
	struct context *ctx;
};

struct io_data {
	int read;
	off_t first_offset, offset;
	char *first_buf;
	size_t first_len;
	struct iovec iov;
};

struct context {
	struct io_uring ring;
	struct io_data *iod;
	struct io_data **iodp;
	unsigned iod_size;
	unsigned sp;
	char *buffer;
};

static int setup_context(unsigned entries, struct context *ctx)
{
	int ret;
	unsigned iod_size;
	int i;
	char *buf;

	if (entries & (entries - 1)) {
		/* Not power of two */
		iod_size = 1 << (32 - __builtin_clz (entries - 1));
	} else {
		iod_size = entries;
	}
	ctx->iod = calloc(iod_size, sizeof(struct io_data) + sizeof(void *));
	if (!ctx->iod) {
		perror("calloc");
		return -1;
	}

	ret = io_uring_queue_init(entries, &ctx->ring, 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		free(ctx->iod);
		ctx->iod = NULL;
		return -1;
	}
	ctx->iodp = (struct io_data **) &ctx->iod[iod_size];
	buf = cfg.buffer;
	for (i = 0; i < iod_size; i++) {
		ctx->iodp[i] = &ctx->iod[i];
		ctx->iod[i].first_buf = buf;
		buf += cfg.chunk_size;
	}
	ctx->sp = ctx->iod_size = iod_size;
	
	return 0;
}

struct io_data* get_io_data(struct context *ctx)
{
	if (ctx->sp == 0)
		return NULL;
	ctx->sp--;
	return ctx->iodp[ctx->sp];
}

int put_io_data(struct context *ctx, struct io_data *id)
{
	if (ctx->sp == ctx->iod_size)
		return -1;
	ctx->iodp[ctx->sp] = id;
	ctx->sp++;
	return 0;
} 

#if 0
static int get_file_size(int fd, off_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		*size = st.st_size;
		return 0;
	} else if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -1;

		*size = bytes;
		return 0;
	}

	return -1;
}
#endif

static void queue_prepped(struct io_uring *ring, int fd, struct io_data *data)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	assert(sqe);

	if (data->read)
		io_uring_prep_readv(sqe, fd, &data->iov, 1, data->offset);
	else
		io_uring_prep_writev(sqe, fd, &data->iov, 1, data->offset);

	io_uring_sqe_set_data(sqe, data);
}

static int queue_io(struct context *ctx, int op, int fd, off_t size, off_t offset)
{
	struct io_uring_sqe *sqe;
	struct io_data *data;
	struct io_uring *ring = &ctx->ring;

	data = get_io_data(ctx);
	if (!data)
		return 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		free(data);
		return 1;
	}

	data->read = op;
	data->offset = data->first_offset = offset;

	data->iov.iov_base = data->first_buf;
	data->iov.iov_len = size;
	data->first_len = size;


	if (data->read)
		io_uring_prep_readv(sqe, fd, &data->iov, 1, offset);
	else
		io_uring_prep_writev(sqe, fd, &data->iov, 1, offset);

	io_uring_sqe_set_data(sqe, data);
	return 0;
}

static int queue_read(struct context *ctx, off_t size, off_t offset)
{
	return queue_io(ctx, 1, cfg.nvme_read_fd, size, offset);
}

static void queue_copy(struct context *ctx, struct io_data *data)
{
	struct io_uring *ring = &ctx->ring;

	data->read = 0;
	data->offset = data->first_offset;

	data->iov.iov_base = data->first_buf;
	data->iov.iov_len = data->first_len;

	queue_prepped(ring, cfg.nvme_write_fd, data);
	io_uring_submit(ring);
}

static int io_file(struct context *ctx, int fd, int op, off_t filesize)
{
	unsigned long ios;
	struct io_uring_cqe *cqe;
	off_t offset;
	int ret;
	struct io_uring *ring = &ctx->ring;

	ios = offset = 0;

	while (filesize) {
		unsigned long had_ios;
		int got_comp;

		/*
		 * Queue up as many reads as we can
		 */
		had_ios = ios;
		while (filesize) {
			off_t this_size = filesize;

			if (ios >= cfg.iodepth)
				break;
			if (this_size > cfg.chunk_size)
				this_size = cfg.chunk_size;
			else if (!this_size)
				break;

			if (queue_io(ctx, op, fd, this_size, offset))
				break;

			filesize -= this_size;
			offset += this_size;
			ios++;
		}

		if (had_ios != ios) {
			ret = io_uring_submit(ring);
			if (ret < 0) {
				fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
				break;
			}
		}

		/*
		 * Queue is full at this point. Find at least one completion.
		 */
		got_comp = 0;
		while (filesize) {
			struct io_data *data;

			if (!got_comp) {
				ret = io_uring_wait_cqe(ring, &cqe);
				got_comp = 1;
			} else {
				ret = io_uring_peek_cqe(ring, &cqe);
				if (ret == -EAGAIN) {
					cqe = NULL;
					ret = 0;
				}
			}
			if (ret < 0) {
				fprintf(stderr, "io_uring_peek_cqe: %s\n",
							strerror(-ret));
				return 1;
			}
			if (!cqe)
				break;

			data = io_uring_cqe_get_data(cqe);
			if (cqe->res < 0) {
				if (cqe->res == -EAGAIN) {
					queue_prepped(ring, fd, data);
					io_uring_submit(ring);
					io_uring_cqe_seen(ring, cqe);
					continue;
				}
				fprintf(stderr, "cqe failed: %s\n",
						strerror(-cqe->res));
				return 1;
			} else if ((size_t)cqe->res != data->iov.iov_len) {
				/* Short read/write, adjust and requeue */
				data->iov.iov_base += cqe->res;
				data->iov.iov_len -= cqe->res;
				data->offset += cqe->res;
				queue_prepped(ring, fd, data);
				io_uring_submit(ring);
				io_uring_cqe_seen(ring, cqe);
				continue;
			}

			/*
			 * All done. if write, nothing else to do. if read,
			 * queue up corresponding write.
			 */
			put_io_data(ctx, data);
			ios--;
			io_uring_cqe_seen(ring, cqe);
		}
	}

	/* wait out pending reads */
	while (ios) {
		struct io_data *data;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d ios=%ld\n", ret, ios);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "io res=%d ios=%ld\n", cqe->res, ios);
			return 1;
		}
		data = io_uring_cqe_get_data(cqe);
		put_io_data(ctx, data);
		ios--;
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

static int read_file(struct context *ctx, off_t filesize)
{
	return io_file(ctx, cfg.nvme_read_fd, 1, filesize);
}

static int write_file(struct context *ctx, off_t filesize)
{
	return io_file(ctx, cfg.nvme_write_fd, 0, filesize);
}

static int copy_file(struct context *ctx, off_t filesize)
{
	unsigned long reads, writes;
	struct io_uring_cqe *cqe;
	off_t write_left, offset;
	int ret;
	struct io_uring *ring = &ctx->ring;

	write_left = filesize;
	writes = reads = offset = 0;

	while (filesize || write_left) {
		unsigned long had_reads;
		int got_comp;
	
		/*
		 * Queue up as many reads as we can
		 */
		had_reads = reads;
		while (filesize) {
			off_t this_size = filesize;

			if (reads + writes >= cfg.iodepth)
				break;
			if (this_size > cfg.chunk_size)
				this_size = cfg.chunk_size;
			else if (!this_size)
				break;

			if (queue_read(ctx, this_size, offset))
				break;

			filesize -= this_size;
			offset += this_size;
			reads++;
		}

		if (had_reads != reads) {
			ret = io_uring_submit(ring);
			if (ret < 0) {
				fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
				break;
			}
		}

		/*
		 * Queue is full at this point. Find at least one completion.
		 */
		got_comp = 0;
		while (write_left) {
			struct io_data *data;

			if (!got_comp) {
				ret = io_uring_wait_cqe(ring, &cqe);
				got_comp = 1;
			} else {
				ret = io_uring_peek_cqe(ring, &cqe);
				if (ret == -EAGAIN) {
					cqe = NULL;
					ret = 0;
				}
			}
			if (ret < 0) {
				fprintf(stderr, "io_uring_peek_cqe: %s\n",
							strerror(-ret));
				return 1;
			}
			if (!cqe)
				break;

			data = io_uring_cqe_get_data(cqe);
			if (cqe->res < 0) {
				if (cqe->res == -EAGAIN) {
					queue_prepped(ring, cfg.nvme_read_fd, data);
					io_uring_submit(ring);
					io_uring_cqe_seen(ring, cqe);
					continue;
				}
				fprintf(stderr, "cqe failed: %s\n",
						strerror(-cqe->res));
				return 1;
			} else if ((size_t)cqe->res != data->iov.iov_len) {
				/* Short read/write, adjust and requeue */
				data->iov.iov_base += cqe->res;
				data->iov.iov_len -= cqe->res;
				data->offset += cqe->res;
				queue_prepped(ring, cfg.nvme_read_fd, data);
				io_uring_submit(ring);
				io_uring_cqe_seen(ring, cqe);
				continue;
			}

			/*
			 * All done. if write, nothing else to do. if read,
			 * queue up corresponding write.
			 */
			if (data->read) {
				queue_copy(ctx, data);
				write_left -= data->first_len;
				reads--;
				writes++;
			} else {
				put_io_data(ctx, data);
				writes--;
			}
			io_uring_cqe_seen(ring, cqe);
		}
	}

	/* wait out pending writes */
	while (writes) {
		struct io_data *data;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d writes=%ld\n", ret, writes);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "write res=%d writes=%ld\n", cqe->res, writes);
			return 1;
		}
		data = io_uring_cqe_get_data(cqe);
		put_io_data(ctx, data);
		writes--;
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

static void randfill(void *buf, size_t len)
{
	uint8_t *cbuf = buf;

	for (int i = 0; i < len; i++)
		cbuf[i] = rand();
}

static void zerofill(void *buf, size_t len)
{
	uint8_t *cbuf = buf;

	for (int i = 0; i < len; i++)
		cbuf[i] = 0;
}

static int written_later(int idx, size_t *offsets, size_t count)
{
	for (int i = idx + 1; i < count; i++) {
		if (offsets[idx] == offsets[i]) {
			return 1;
		}
	}

	return 0;
}

static void print_buf(void *buf, size_t len)
{
	uint8_t *cbuf = buf;
	for (int i = len-1; i >= 0; i--)
		printf("%02X", cbuf[i]);
}


#if 0
static void print_buf16(void *buf, size_t len)
{
	uint8_t *cbuf = buf;

	for (int i = len-1; i >= 0; i--) {
		printf("%02X", cbuf[i]);
		if ((i & 0xf) == 0)
			printf("\n");
	}
}

static uint64_t checksum_buf16(void *buf, uint64_t prev, size_t len)
{
	uint64_t *cbuf = (uint64_t *) buf;
	uint64_t *ebuf = (uint64_t *) (buf + len);

	for (; cbuf < ebuf; ++cbuf)
		prev ^= *cbuf;

	return prev;
}
#endif


static int hostinit(void) {

	struct hostaccess {
		uint8_t entry[cfg.init_sz];
	} __attribute__ (( packed ));

	struct hostaccess wdata;
	struct hostaccess *mem = cfg.buffer;
	size_t count = cfg.init_tot / sizeof(struct hostaccess);

	zerofill(&wdata, sizeof(struct hostaccess));
	for(size_t i = 0; i < count; i++)
		mem[i] = wdata;

	return 0;
}

static int hosttest(void)
{
	size_t *offsets;
	struct hostaccess {
		uint8_t entry[abs(cfg.host_access_sz)];
	} __attribute__ (( packed ));

	struct hostaccess *wdata, *rdata;
	struct hostaccess *mem = cfg.buffer;
	size_t count = cfg.chunk_size / sizeof(struct hostaccess);

	offsets = (size_t *)malloc(cfg.host_accesses*sizeof(size_t));
	wdata = (struct hostaccess*)
		malloc(cfg.host_accesses*sizeof(struct hostaccess));
	rdata = (struct hostaccess*)
		malloc(cfg.host_accesses*sizeof(struct hostaccess));

	if (offsets == NULL || wdata == NULL ||
	    rdata == NULL || sizeof(wdata) > cfg.size) {
		errno = ENOMEM;
		return -1;
	}

	for (int i = 0; i < cfg.host_accesses; i++)
		offsets[i] = rand() % count;

	if (cfg.host_access_sz > 0) {
		randfill(wdata, sizeof(wdata));

		for(size_t i = 0; i < cfg.host_accesses; i++)
			mem[offsets[i]] = wdata[i];
	}

	for(size_t i = 0; i < cfg.host_accesses; i++)
		rdata[i] = mem[offsets[i]];

	if (cfg.host_access_sz <= 0)
		return 0;

	for (size_t i = 0; i < cfg.host_accesses; i++) {
		if (written_later(i, offsets, cfg.host_accesses))
			continue;

		if (memcmp(&rdata[i], &wdata[i], sizeof(rdata[i])) == 0)
			continue;

		printf("MISMATCH on host_access %04zd : ", i);
		print_buf(&wdata[i], sizeof(wdata[i]));
		printf(" != ");
		print_buf(&rdata[i], sizeof(rdata[i]));
		printf("\n");
		errno = EINVAL;
		return -1;
	}
	//fprintf(stdout, "MATCH on %d host accesses.\n",
	//	cfg.host_accesses);

	free(wdata);
	free(rdata);
	free(offsets);

	return 0;
}

static int writedata(void)
{
	int *buffer;
	ssize_t count;
	int ret = 0;

	if (posix_memalign((void **)&buffer, cfg.page_size, cfg.size))
		return -1;

	cfg.write_parity = 0;
	for (size_t i=0; i<(cfg.size/sizeof(int)); i++) {
		buffer[i] = rand();
		cfg.write_parity ^= buffer[i];
	}
	count = write(cfg.nvme_read_fd, (void *)buffer, cfg.size);
	if (count == -1) {
		ret = -1;
		goto out;
	}

out:
	free(buffer);
	return ret;
}

static int readdata(void)
{
	int *buffer;
	ssize_t count;
	int ret = 0;

	if (posix_memalign((void **)&buffer, cfg.page_size, cfg.size))
		return -1;

	count = read(cfg.nvme_write_fd, (void *)buffer, cfg.size);
	if (count == -1) {
		ret = -1;
		goto out;
	}

	cfg.read_parity = 0;
	for (size_t i=0; i<(cfg.size/sizeof(int)); i++)
		cfg.read_parity ^= buffer[i];

out:
	free(buffer);
	return ret;
}

static void *thread_run(void *args)
{
	struct thread_info *tinfo = (struct thread_info *)args;
	struct context *ctx = tinfo->ctx;

	if (cfg.skip_read) {
		write_file(ctx, cfg.size);
	} else if (cfg.skip_write) {
		read_file(ctx, cfg.size);
	} else {
		copy_file(ctx, cfg.size);
	}

	return NULL;
}

static size_t get_suffix(char *string) {

	size_t val;

	val = suffix_binary_parse(string);
	if (errno) {
		fprintf(stderr,"error in get_suffix().\n");
		exit(-1);
	}
	return val;
}

static void get_init(char *init) {

	char *token;

	if (init == def_str)
		return;

	token = strtok(init, ":");
	if (token == NULL)
		return;
	cfg.init_sz = atoi(token);
	if (cfg.init_sz == 0) {
		cfg.init_tot = 0;
		return;
	}
	token = strtok(NULL, ":");
	if (token == NULL)
		cfg.init_tot = 4096;
	else
		cfg.init_tot = get_suffix(token);
	token = strtok(NULL, ":");
	if (token == NULL)
		cfg.init_stop = 0;
	else
		cfg.init_stop = 1;
}

static void get_hostaccess(char *host_access) {

	char *token;

	if (host_access == def_str)
		return;

	token = strtok(host_access, ":");
	if (token == NULL)
		return;
	cfg.host_access_sz = atoi(token);
	if (cfg.host_access_sz == 0) {
		cfg.host_accesses = 0;
		return;
	}
	token = strtok(NULL, ":");
	if (token == NULL)
		cfg.host_accesses = 64;
	else
		cfg.host_accesses = get_suffix(token);
	token = strtok(NULL, ":");
	if (token == NULL)
		cfg.host_access_stop = 0;
	else
		cfg.host_access_stop = 1;

}

int main(int argc, char **argv)
{
	//double rval, wval, val;
	double val;
	//const char *rsuf, *wsuf, *suf;
	char *host_access, *init;
	struct timeval delta;
	struct context ctx;
	size_t total = 0;

	host_access = (char *)def_str;
	init = (char*)def_str;

	const struct argconfig_options opts[] = {
		{"nvme-read", .cfg_type=CFG_FD_RDWR_DIRECT_NC,
		 .value_addr=&cfg.nvme_read_fd,
		 .argument_type=required_positional,
		 .force_default="/dev/nvme0n1",
		 .help="NVMe device to read"},
		{"nvme-write", .cfg_type=CFG_FD_RDWR_DIRECT_NC,
		 .value_addr=&cfg.nvme_write_fd,
		 .argument_type=required_positional,
		 .force_default="/dev/nvme1n1",
		 .help="NVMe device to write"},
		{"p2pmem", .cfg_type=CFG_FD_RDWR_NC,
		 .value_addr=&cfg.p2pmem_fd,
		 .argument_type=optional_positional,
		 .help="p2pmem device to use as buffer (omit for sys memory)"},
		{"check", 0, "", CFG_NONE, &cfg.check, no_argument,
		 "perform checksum check on transfer (slow)"},
		{"chunks", 'c', "", CFG_LONG_SUFFIX, &cfg.chunks, required_argument,
		 "number of chunks to transfer"},
		{"chunk_size", 's', "", CFG_LONG_SUFFIX, &cfg.chunk_size, required_argument,
		 "size of data chunk"},
		{"duration", 'D', "", CFG_INT, &cfg.duration, required_argument,
		 "duration to run test for (-1 for infinite)"},
		{"host_access", 0, "", CFG_STRING, &host_access, required_argument,
		 "alignment/size and (: sep [optional]) count for host access test "
		 "(alignment/size: 0 = no test, < 0 = read only test)"},
		{"init", 0, "", CFG_STRING, &init, required_argument,
		 "initialize memory buffer with zeros using this size/alignment and "
		 " (optional : sep) total bytes to init"},
		{"offset", 'o', "", CFG_LONG_SUFFIX, &cfg.offset, required_argument,
		 "offset into the p2pmem buffer"},
		{"overlap", 0, "", CFG_NONE, &cfg.overlap, no_argument,
		 "Allow overlapping of read and/or write files."},
		{"seed", 0, "", CFG_INT, &cfg.seed, required_argument,
		 "seed to use for random data (-1 for time based)"},
		{"skip-read", 0, "", CFG_NONE, &cfg.skip_read, no_argument,
		 "skip the read (can't be used with --check)"},
		{"skip-write", 0, "", CFG_NONE, &cfg.skip_write, no_argument,
		 "skip the write (can't be used with --check)"},
		{"threads", 't', "", CFG_POSITIVE, &cfg.threads, required_argument,
		 "number of read/write threads to use"},
		{"dump", 0, "", CFG_NONE, &cfg.dump, no_argument,
		 "dump 1K of read data"},
		{"chksum", 0, "", CFG_NONE, &cfg.chksum, no_argument,
		 "Print checsum"},
		{"fill", 0, "", CFG_NONE, &cfg.fill, no_argument,
		 "Print fill random data"},
		{"iodepth", 0, "", CFG_POSITIVE, &cfg.iodepth, required_argument,
		 "iodpeth (default 1)"},
		{NULL}
	};

	argconfig_parse(argc, argv, desc, opts, &cfg, sizeof(cfg));
	cfg.page_size = sysconf(_SC_PAGESIZE);
	cfg.size = cfg.chunk_size*cfg.chunks;
#if 0
	cfg.size_mmap = cfg.chunk_size*cfg.threads;
#else
	/* Should be cfg.chunk_size * cfg.threads* cfg.iodepth*/
	cfg.size_mmap = cfg.chunk_size*cfg.iodepth;
#endif
	get_hostaccess(host_access);
	get_init(init);

	/* Currently no support for multithreads */
	if (cfg.threads > 1) {
		fprintf(stderr, "Multithread is not yet supported\n");
		cfg.threads = 1;
	}

	if (ioctl(cfg.nvme_read_fd, BLKGETSIZE64, &cfg.rsize)) {
		perror("ioctl-read");
		goto fail_out;
	}
	if (ioctl(cfg.nvme_write_fd, BLKGETSIZE64, &cfg.wsize)) {
		perror("ioctl-write");
		goto fail_out;
	}

	if ((cfg.skip_read || cfg.skip_write) && cfg.check) {
		fprintf(stderr, "can not set --skip-read or --skip-write and "
			"--check at the same time (skip-* kills check).\n");
		goto fail_out;
	}

	if (cfg.overlap && cfg.check) {
		fprintf(stderr, "can not set --overlap and --check at the "
			"same time (overlap kills check).\n");
		goto fail_out;
	}

	if (cfg.overlap && (min(cfg.rsize, cfg.wsize) >  cfg.size)) {
		fprintf(stderr, "do not set --overlap when its not needed "
			"(%lu, %lu, %zd).\n", cfg.rsize, cfg.wsize, cfg.size);
		goto fail_out;
	}

	if (!cfg.overlap && (min(cfg.rsize, cfg.wsize) <  cfg.size)) {
		fprintf(stderr, "read and write files must be at least "
			"as big as --chunks*--chunks_size (or use --overlap).\n");
		goto fail_out;
	}

	if (cfg.p2pmem_fd && (cfg.chunk_size % cfg.page_size)){
		fprintf(stderr, "--size must be a multiple of PAGE_SIZE in p2pmem mode.\n");
		goto fail_out;
	}

	if (!cfg.p2pmem_fd && cfg.offset) {
		fprintf(stderr,"Only use --offset (-o) with p2pmem!\n");
		goto fail_out;
	}

	if (cfg.chunks % cfg.threads) {
		fprintf(stderr,"--chunks not evenly divisable by --threads!\n");
		goto fail_out;
	}

	if (cfg.init_stop)
		cfg.size_mmap = max(cfg.size_mmap, cfg.init_tot);

	if (cfg.init_tot > cfg.size) {
		fprintf(stderr,"--init init_tot exceeds mmap()'ed size!\n");
		goto fail_out;
	}

	if ( cfg.seed == -1 )
		cfg.seed = time(NULL);
	srand(cfg.seed);

	char tmp[24];
	if (cfg.p2pmem_fd) {
		cfg.buffer = mmap(NULL, cfg.size_mmap, PROT_READ | PROT_WRITE, MAP_SHARED,
				  cfg.p2pmem_fd, cfg.offset);
		if (cfg.buffer == MAP_FAILED) {
			perror("mmap");
			goto fail_out;
		}
	} else {
		if (posix_memalign(&cfg.buffer, cfg.page_size, cfg.size_mmap)) {
			perror("posix_memalign");
			goto fail_out;
		}
	}

	if (setup_context(cfg.iodepth, &ctx))
		return -1;

	sprintf(tmp, "%d", cfg.duration);
	//rval = cfg.rsize;
	//rsuf = suffix_si_get(&rval);
	//wval = cfg.wsize;
	//wsuf = suffix_si_get(&wval);
	//fprintf(stdout,"Running p2pmem-test: reading %s (%.4g%sB): writing %s (%.4g%sB): "
	//	"p2pmem buffer %s.\n",cfg.nvme_read_filename, rval, rsuf,
	//	cfg.nvme_write_filename, wval, wsuf, cfg.p2pmem_filename);
	val = cfg.size;
	//suf = suffix_si_get(&val);
	//fprintf(stdout,"\tchunk size = %zd : number of chunks =  %zd: total = %.4g%sB : "
	//	"thread(s) = %zd : overlap = %s.\n", cfg.chunk_size, cfg.chunks, val, suf,
	//	cfg.threads, cfg.overlap ? "ON" : "OFF");
	fprintf(stdout,"%zd , %zd, %.4g , ",
		cfg.chunk_size, cfg.chunks, val);
	//fprintf(stdout,"\tskip-read = %s : skip-write =  %s : duration = %s sec.\n",
	//	cfg.skip_read ? "ON" : "OFF", cfg.skip_write ? "ON" : "OFF",
	//	(cfg.duration <= 0) ? "INF" : tmp);
	//rval = cfg.size_mmap;
	//rsuf = suffix_si_get(&rval);
	//fprintf(stdout,"\tbuffer = %p (%s): mmap = %.4g%sB\n", cfg.buffer,
	//	cfg.p2pmem_fd ? "p2pmem" : "system memory", rval, rsuf);
	//fprintf(stdout,"\tPAGE_SIZE = %ldB\n", cfg.page_size);
	//rval = cfg.init_tot;
	//rsuf = suffix_si_get(&rval);
	//if (cfg.init_tot)
	//	fprintf(stdout,"\tinitializing %.4g%sB of buffer with zeros: alignment "
	//		"and size = %dB (STOP = %s)\n", rval, rsuf, cfg.init_sz,
	//		cfg.init_stop ? "ON" : "OFF");
	//rval = cfg.host_accesses;
	//rsuf = suffix_si_get(&rval);
	//if (cfg.host_accesses)
	//	fprintf(stdout,"\tchecking %.4g%sB host accesses %s: alignment and size = %dB"
	//		" (STOP = %s)\n", rval, rsuf, cfg.host_access_sz < 0 ? "(read only) " : "",
	//		abs(cfg.host_access_sz), cfg.host_access_stop ? "ON" : "OFF");
	//if (cfg.check)
	//	fprintf(stdout,"\tchecking data with seed = %d\n", cfg.seed);

	if (cfg.init_tot) {
		if (hostinit()) {
			perror("hostinit");
			goto free_fail_out;
		}
		if (cfg.init_stop) {
			fprintf(stdout, "stopping at hostinit()\n");
			goto out;
		}
	}
	if (cfg.host_accesses) {
		if (hosttest()) {
			perror("hosttest");
			goto free_fail_out;
		}
		if (cfg.host_access_stop) {
			fprintf(stdout, "stopping at hosttest()\n");
			goto out;
		}
		srand(cfg.seed);
	}

	if (cfg.check)
		if (writedata()) {
			perror("writedata");
			goto free_fail_out;
		}

	if (lseek(cfg.nvme_read_fd, 0, SEEK_SET) == -1) {
		perror("writedata-lseek");
		goto free_fail_out;
	}

	struct thread_info *tinfo;
	tinfo = calloc(cfg.threads, sizeof(*tinfo));
	if (tinfo == NULL) {
		perror("calloc");
		goto free_fail_out;
	}

	gettimeofday(&cfg.time_start, NULL);
	getrusage(RUSAGE_SELF, &cfg.usage_start);
	if (cfg.threads == 1) {
		tinfo[0].thread = 0;
		tinfo[0].ctx = &ctx;
		thread_run(&tinfo[0]);
		total += tinfo[0].total;
	} else {
		for (size_t t = 0; t < cfg.threads; t++) {
			tinfo[t].thread = t;
			tinfo[t].ctx = &ctx;
			int s = pthread_create(&tinfo[t].thread_id, NULL,
					       &thread_run, &tinfo[t]);
			if (s != 0) {
				perror("pthread_create");
				goto free_free_fail_out;
			}
		}
		for (size_t t = 0; t < cfg.threads; t++) {
			int s = pthread_join(tinfo[t].thread_id, NULL);
			if (s != 0) {
				perror("pthread_join");
				goto free_free_fail_out;
			}
			total += tinfo[t].total;
		}
	}
	getrusage(RUSAGE_SELF, &cfg.usage_end);
	gettimeofday(&cfg.time_end, NULL);

	if (cfg.check) {
		if (lseek(cfg.nvme_write_fd, 0, SEEK_SET) == -1) {
			perror("readdata-lseek");
			goto free_fail_out;
		}
		if (readdata()) {
			perror("readdata");
			goto free_fail_out;
		}
	}

	//if (cfg.check)
	//	fprintf(stdout, "%s on data check, 0x%x %s 0x%x.\n",
	//		cfg.write_parity==cfg.read_parity ? "MATCH" : "MISMATCH",
	//		cfg.write_parity,
	//		cfg.write_parity==cfg.read_parity ? "=" : "!=",
	//		cfg.read_parity);

	//fprintf(stdout, "Transfer:\n");
	//report_transfer_rate(stdout, &cfg.time_start, &cfg.time_end, total);
	report_transfer_rate(stdout, &cfg.time_start, &cfg.time_end, cfg.size);
	//fprintf(stdout, "\n");
	//fprintf(stdout, "User CPU time used per:");
	timersub(&cfg.usage_end.ru_utime, &cfg.usage_start.ru_utime, &delta);
	//fprintf(stdout, "%ld.%06ld\n", delta.tv_sec, delta.tv_usec);
	fprintf(stdout, "%ld.%06ld, ", delta.tv_sec, delta.tv_usec);
	//fprintf(stdout, "System CPU time used: per");
	timersub(&cfg.usage_end.ru_stime, &cfg.usage_start.ru_stime, &delta);
	//fprintf(stdout, "%ld.%06ld\n", delta.tv_sec, delta.tv_usec);
	fprintf(stdout, "%ld.%06ld, \n", delta.tv_sec, delta.tv_usec);

	free(tinfo);
	if (cfg.p2pmem_fd)
		munmap(cfg.buffer, cfg.chunk_size);
	else
		free(cfg.buffer);


	io_uring_queue_exit(&ctx.ring);

	return EXIT_SUCCESS;

free_free_fail_out:
	free(tinfo);
out:
free_fail_out:
	if (cfg.p2pmem_fd)
		munmap(cfg.buffer, cfg.chunk_size);
	else
		free(cfg.buffer);
fail_out:
	return EXIT_FAILURE;
}
