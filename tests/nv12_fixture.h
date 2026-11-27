/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "render/gpu_dmabuf.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct nv12_fixture {
	struct gpu_dmabuf frame;
	uint8_t *pixels;
	size_t size;
	int refs;
};

static void fixture_retain(void *refs)
{
	(*(int *)refs)++;
}

static void fixture_release(void *refs)
{
	(*(int *)refs)--;
}

static bool nv12_fixture_sync(const struct nv12_fixture *f, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_WRITE | flags };
	int r;

	do {
		r = ioctl(f->frame.fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (r && errno == EINTR);
	return r == 0;
}

/* udmabuf wraps test-owned, page-aligned sealed storage in a real dma-buf. */
static bool nv12_fixture_open(struct nv12_fixture *f, unsigned int width,
			      unsigned int height)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t used = (size_t)width * height * 3 / 2;
	int control = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	int mem = memfd_create("avernella-test-nv12",
			       MFD_ALLOW_SEALING | MFD_CLOEXEC);
	f->size = page > 0 ? (used + page - 1) / page * page : 0;
	f->frame = (struct gpu_dmabuf){
		.fd = -1,
		.width = width,
		.height = height,
		.offset = { 0, width * height },
		.stride = { width, width },
		.lease = &f->refs,
		.retain = fixture_retain,
		.release = fixture_release,
	};
	if (control >= 0 && mem >= 0 && f->size && !ftruncate(mem, f->size) &&
	    fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) >= 0) {
		struct udmabuf_create create = {
			.memfd = mem,
			.size = f->size,
			.flags = UDMABUF_FLAGS_CLOEXEC,
		};
		f->frame.fd = ioctl(control, UDMABUF_CREATE, &create);
		if (f->frame.fd >= 0) {
			void *p = mmap(NULL, f->size, PROT_READ | PROT_WRITE,
				       MAP_SHARED, mem, 0);
			if (p != MAP_FAILED)
				f->pixels = p;
			else {
				close(f->frame.fd);
				f->frame.fd = -1;
			}
		}
	}
	if (control >= 0)
		close(control);
	if (mem >= 0)
		close(mem);
	if (f->pixels && !nv12_fixture_sync(f, DMA_BUF_SYNC_START)) {
		munmap(f->pixels, f->size);
		close(f->frame.fd);
		f->pixels = NULL;
	}
	return f->pixels != NULL;
}

static void nv12_fixture_close(struct nv12_fixture *f)
{
	if (f->pixels) {
		munmap(f->pixels, f->size);
		close(f->frame.fd);
		f->pixels = NULL;
	}
}
