/*
 * EasySandbox: an extremely simple sandbox for untrusted C/C++ programs
 * Copyright (c) 2012,2013 David Hovemeyer <david.hovemeyer@gmail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Minimum amount of memory to allocate when we use sbrk to extend the heap */
#define MIN_ALLOC 65536

/* Block flags */
#define ALLOCATED 1

/* Header found at the beginning of each block */
struct Header {
	struct Header *prev, *next; /* previous and next blocks */
	size_t size;                /* total size of block, including header */
	int flags;                  /* block flags */
};

/* List of blocks, sorted by order of increasing addresses */
static struct Header *s_head, *s_tail;

/*
 * Round given size up to the nearest multiple.
 */
static inline size_t round_to_multiple(size_t n, size_t multiple)
{
	if (n % multiple != 0) {
		n += multiple - (n % multiple);
	}
	return n;
}

/*
 * Predicate to test whether given block is allocated.
 */
static inline int is_allocated(struct Header *block)
{
	return (block->flags & ALLOCATED) != 0;
}

/*
 * Allocate a new block using sbrk and append it to the list of blocks.
 * Returns the pointer to the allocated block, or null if
 * sbrk couldn't allocate more memory
 */
static struct Header *alloc_block(size_t block_size)
{
	struct Header *block;

	/* ensure minimum allocation size */
	if (block_size < MIN_ALLOC) {
		block_size = MIN_ALLOC;
	}

	/* use sbrk to extend the heap */
	block = sbrk((intptr_t) block_size);
	if (block == (void *)-1) {
		return 0;
	}

	/* Append block to list */
	block->next = 0;
	if (s_head == 0) {
		/* first allocation */
		s_head = s_tail = block;
		block->prev = 0;
	} else {
		/* append block at tail of list */
		s_tail->next = block;
		block->prev = s_tail;
		s_tail = block;
	}
	block->size = block_size;
	block->flags = 0;

	return block;
}

/*
 * Split given block if its excess space beyond given required block size
 * is large enough to form a useful block.
 */
static void split_block_if_necessary(struct Header *block, size_t required_block_size)
{
	struct Header *excess;
	size_t left_over;

	/* is there enough room to form a useful block (larger than just a header)? */
	left_over = block->size - required_block_size;
	if (left_over <= sizeof(struct Header)) {
		return;
	}

	/* adjust size of the original block */
	block->size = required_block_size;

	/* compute address of block formed from excess memory in block */
	excess = (struct Header *) (((char *) block) + required_block_size);

	/* initialize the new block's header */
	excess->size = left_over;
	excess->flags = 0;

	/* graft the new block into the list as current block's successor */
	excess->next = block->next;
	excess->prev = block;
	if (block->next != 0) {
		block->next->prev = excess;
	} else {
		/* splitting the tail block, so excess is new tail */
		s_tail = excess;
	}
	block->next = excess;
}

/*
 * Coalesce given block with its successor if necesary.
 */
static void coalesce_if_necessary(struct Header *block)
{
	/* check whether successor exists and both block and successor are free */
	if (is_allocated(block) || block->next == 0 || is_allocated(block->next)) {
		return;
	}

	/* absorb successor into block */
	block->size += block->next->size;

	/* splice successor out of the list */
	if (block->next->next != 0) {
		block->next->prev = block;
	} else {
		/* successor was the tail block, so block becomes tail */
		s_tail = block;
	}
	block->next = block->next->next;
}

/*
 * Allocate a buffer of given size.
 */
void *malloc(size_t size)
{
	size_t required_block_size;
	struct Header *block;

	/* calculate the minimum block size needed for this allocation */
	required_block_size = round_to_multiple(size + sizeof(struct Header), sizeof(struct Header));

	/* search list for an unallocated block of sufficient size */
	for (block = s_head; block != 0; block = block->next) {
		if (block->size >= required_block_size && !is_allocated(block)) {
			break;
		}
	}

	/* if no sufficiently-large block was found, allocate one and append it to list */
	if (block == 0) {
		block = alloc_block(required_block_size);
		if (block == 0) {
			/* failed to allocate a new block */
			errno = ENOMEM;
			return 0;
		}
	}

	/* if block size exceeds required block size by more than the size of one header,
	 * then split it */
	split_block_if_necessary(block, required_block_size);

	/* mark the block as allocated */
	block->flags |= ALLOCATED;

	return (void*) (block + 1);
}

/*
 * Free a buffer allocated with malloc.
 */
void free(void *p)
{
	struct Header *block;

	if (p == 0) {
		return;
	}

	/* find header */
	block = ((struct Header *)p) - 1;

	/* ensure that this is actually an allocated block */
	if (!is_allocated(block)) {
		fprintf(stderr, "Invalid free at %p\n", p);
		return;
	}

	/* mark block as being free */
	block->flags &= ~(ALLOCATED);

	/* Attempt to coalesce with predecessor and successor blocks */
	coalesce_if_necessary(block);
	coalesce_if_necessary(block->prev);
}

/*
 * Allocate a zeroed buffer.
 */
void *calloc(size_t nmemb, size_t size)
{
	void *buf;

	buf = malloc(nmemb * size);
	if (buf != 0) {
		memset(buf, 0, nmemb * size);
	}
	return buf;
}

/*
 * Reallocate given buffer so that it has given size.
 */
void *realloc(void *ptr, size_t size)
{
	struct Header *block;
	size_t to_copy;
	void *buf;

	/* special case: if ptr is null, then allocate a new buffer
	 * using malloc */
	if (ptr == 0) {
		return malloc(size);
	}

	/* special case: if size is 0, then free the buffer */
	if (size == 0) {
		free(ptr);
		return 0;
	}

	/* find buffer's block header */
	block = ((struct Header *)ptr) - 1;

	/* allocate a new buffer */
	buf = malloc(size);
	if (buf == 0) {
		return 0;
	}

	/* copy data from old buffer to new buffer */
	to_copy = block->size - sizeof(struct Header); /* original buffer size */
	if (to_copy > size) {
		/* original size was larger than new size */
		to_copy = size;
	}
	memcpy(buf, ptr, to_copy);

	/* free the old buffer */
	free(ptr);

	/* return the new buffer */
	return buf;
}
