/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

static struct list mmap_file_list;

struct mmap_file_info{
	struct list_elem elem;
	uint64_t start;
	uint64_t end;
};

/* The initializer of file vm */
void
vm_file_init (void) {
	list_init(&mmap_file_list);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	struct file_page *file_page = &page->file;
	struct file* file = ((struct mmap_info*)page ->uninit.aux)->file;
	file_page -> file = file;
	
	page->operations = &file_ops;

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	if(file_page->file == NULL)
		return false;
	
	file_seek(file_page->file, file_page->ofs);
	off_t read_size = file_read(file_page->file, kva, file_page->size);
	if(read_size != file_page->size)
		return;
	if(read_size < PGSIZE)
		memset(kva + read_size, 0, PGSIZE - read_size);
	
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	struct thread *cur = thread_current();

	if(pml4_is_dirty(cur->pml4, page->va)){
		file_seek(file_page->file, file_page->ofs);
		file_write(file_page->file, page->va, file_page->size);
		pml4_set_dirty(cur->pml4, page->va, false);
	}

	pml4_clear_page(cur->pml4, page->va);
	page->frame = NULL;

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if(pml4_is_dirty(thread_current()->pml4, page->va)){
		file_seek(file_page->file, file_page->ofs);
		file_write(file_page->file, page->va, file_page->size);
	}
	file_close(file_page->file);

	if(page->frame != NULL){
		list_remove(&page->frame->elem);
		free(page->frame);
	}
}

static bool lazy_load_file(struct page *page, void *aux){
	struct mmap_info *mi = (struct mmap_info *)aux;
	file_seek(mi->file, mi->offset);
	page->file.size = file_read(mi->file, page->va, mi->read_bytes);
	page->file.ofs = mi->offset;
	if(page->file.size < PGSIZE)
		memset(page->va + page->file.size, 0, PGSIZE - page->file.size);
	pml4_set_dirty(thread_current()->pml4, page->va, false);
	free(mi);
	return true;
}
/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	off_t ofs = offset;
	uint64_t initaddr = addr;
	uint64_t read_bytes;
	uint64_t real_len = length > file_length(file) ? file_length(file) : length;
	for(uint64_t i = real_len; i > 0; i -= read_bytes){
		struct mmap_info *mi = malloc(sizeof(struct mmap_info));
		read_bytes = i >= PGSIZE ? PGSIZE : i;
		mi->file = file_reopen(file);
		mi->offset = ofs;
		mi->read_bytes = read_bytes;
		vm_alloc_page_with_initializer(VM_FILE, (void *)((uint64_t)initaddr), writable, lazy_load_file, (void *)mi);
		ofs += read_bytes;
		initaddr += PGSIZE;
	}
	struct mmap_file_info *mfi = malloc(sizeof(struct mmap_file_info));
	mfi->start = (uint64_t)addr;
	mfi->end = (uint64_t)pg_round_down((uint64_t)addr + real_len - 1);
	list_push_back(&mmap_file_list, &mfi->elem);
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	if(list_empty(&mmap_file_list))
		return;
	for(struct list_elem *i = list_front(&mmap_file_list); i != list_end(&mmap_file_list); i = list_next(i)){
		struct mmap_file_info *mfi = list_entry(i, struct mmap_file_info, elem);
		if(mfi->start == (uint64_t)addr){
			// for(uint64_t j = (uint64_t)addr; j <= mfi->end; j += PGSIZE){
			// 	struct page *page = spt_find_page(&thread_current()->spt, (void *)j);
			// 	spt_remove_page(&thread_current()->spt, page);
			// }
			// list_remove(&mfi->elem);
			// free(mfi);
			// return;
			struct page *page = spt_find_page(&thread_current()->spt, addr);
			struct file_page *file_page UNUSED = &page->file;
			list_remove(&mfi->elem);
			if(pml4_is_dirty(thread_current()->pml4, page->va)){
				file_seek(file_page->file, file_page->ofs);
				file_write(file_page->file, page->va, file_page->size);
				pml4_set_dirty(thread_current()->pml4, page->va, false);
			}
			pml4_clear_page(thread_current()->pml4, page->va);
		}
	}
}