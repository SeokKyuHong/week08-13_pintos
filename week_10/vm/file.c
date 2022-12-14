/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/malloc.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,		//페이지를 삭제하는 함수
	.type = VM_FILE,
};

/* The initializer of file vm */
/* 파일 vm의 초기화 */
void
vm_file_init (void) {
	// list_init(&mmap_file_list);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
    page -> operations = &file_ops;
	struct file_page *file_page = &page -> file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	if(page==NULL)
		return false;

	struct container *aux = (struct container *)page->uninit.aux;

	struct file *file = aux->file;
	off_t offset = aux->offset;
	size_t page_read_bytes = aux->page_read_bytes;
	// size_t page_zero_bytes = PGSIZE - page_read_bytes;
	
	file_seek (file, offset);

	if(file_read(file, kva, page_read_bytes) != (int)page_read_bytes) {
		return false;
	}

	memset(kva + page_read_bytes, 0, PGSIZE - page_read_bytes);

	return true;	
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
        
    if (page == NULL)
        return false;

    struct container * aux = (struct container *) page->uninit.aux;
    
    // 사용 되었던 페이지(dirty page)인지 체크
	// 체크 후 수정된 페이지(더티 비트1)는 파일에 업데이트
	// 이후 더티비트를 0으로 만듬
    if(pml4_is_dirty(thread_current()->pml4, page->va)){
        file_write_at(aux->file, page->va, aux->page_read_bytes, aux->offset);
        pml4_set_dirty (thread_current()->pml4, page->va, 0);
    }

	//present bit를 0으로 
    pml4_clear_page(thread_current()->pml4, page->va);
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	// if (page == NULL)
	// 	return false;
	
	// struct container* container = (struct container *)page->uninit.aux;

	// /* 수정된 페이지(더티 비트 1)는 파일에 업데이트 해 놓는다. 
	// 	그리고 더티 비트를 0으로 만들어둔다. */
	// if (pml4_is_dirty(thread_current()->pml4, page->va)){
	// 	file_write_at(container->file, page->va, 
	// 		container->page_read_bytes, container->offset);
	// 	pml4_set_dirty(thread_current()->pml4, page->va, 0);
	// }

	// /* present bit을 0으로 만든다. */
	// pml4_clear_page(thread_current()->pml4, page->va);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {

	// 파일과 동일한 inode의 새 파일을 연다. 
	struct file *mfile = file_reopen(file);
	
	void * start_addr = addr;
	size_t read_bytes = length > file_length(file) ? file_length(file) : length;
	size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;

	/* 파일을 페이지 단위로 잘라 해당 파일의 정보들을 container 구조체에 저장한다.
	   FILE-BACKED 타입의 UINIT 페이지를 만들어 lazy_load_segment()를 vm_init으로 넣는다. */
	while (read_bytes > 0 || zero_bytes > 0) {
		
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct container *container = (struct container*)malloc(sizeof(struct container));
		container->file = mfile;
		container->offset = offset;
		container->page_read_bytes = page_read_bytes;

		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable, lazy_load_segment, container)) {
			return NULL;
    	}
		
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		addr       += PGSIZE;
		offset     += page_read_bytes;
		
	}
	return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	
	/* ADDR부터 연속된 모든 페이지를 변경 사항을 업데이트하고 매핑 정보를 지운다.
	   가상 페이지가 free되는 것이 아니다. present bit을 0으로 만들어 주는 것이다. */
	
	while(true){
		struct page* page = spt_find_page(&thread_current()->spt, addr);

		if (page == NULL)
			break;;
		
		struct container* container = (struct container *)page->uninit.aux;

		/* 수정된 페이지(더티 비트 1)는 파일에 업데이트 해 놓는다. 
		   그리고 더티 비트를 0으로 만들어둔다. */
		if (pml4_is_dirty(thread_current()->pml4, page->va)){
			file_write_at(container->file, addr, 
				container->page_read_bytes, container->offset);
			pml4_set_dirty(thread_current()->pml4, page->va, 0);
		}

		/* present bit을 0으로 만든다. */
		pml4_clear_page(thread_current()->pml4, page->va);
		addr += PGSIZE; 
	}
}
