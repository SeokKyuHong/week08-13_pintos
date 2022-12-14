#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
void argument_stack(char **argv, int argc, void **rsp);
bool lazy_load_segment (struct page *page, void *aux);
static bool setup_stack (struct intr_frame *if_);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) ;


/* General process initializer for initd and other process. */
/* initd 및 기타 프로세스를 위한 일반 프로세스 초기화. */
static void
process_init (void) {
	
	struct thread *current = thread_current ();
}



/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
/* FILE_NAME에서 로드된 "initd"라는 첫 번째 사용자 영역 프로그램을 시작합니다.
process_create_initd()가 반환되기 전에 새 스레드가 예약될 수 있으며 종료될 수도 있습니다.
initd의 스레드 ID를 반환하거나 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다.
이것은 한 번만 호출되어야 합니다. */
//grep foo bar -> foo랑 bar를 전달하여 grep를 실행
tid_t
process_create_initd (const char *file_name) { 
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	/* FILE_NAME의 복사본을 만듭니다.
	 그렇지 않으면 호출자와 load() 사이에 경쟁이 있습니다. */
	fn_copy = palloc_get_page (0); // 커널 가상 주소 반환
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE); 			// 2번을 1번으로 3번의 사이즈만큼 복사

	/*아몰랑*/
	char *save_ptr;  
	strtok_r (file_name, " ", &save_ptr);
	/*아몰랑*/
	
	/* Create a new thread to execute FILE_NAME. */
	/* FILE_NAME을 실행할 새 스레드를 만듭니다. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR){
		palloc_free_page (fn_copy);
	}
	

	return tid;
}

/* A thread function that launches first user process. */
/* 첫 번째 사용자 프로세스를 시작하는 스레드 함수. */
static void
initd (void *f_name) {

//새로운 페이지 테이블 초기화 
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	
	process_init ();
	
	
	if (process_exec (f_name) < 0){
		
		PANIC("Fail to launch initd\n");
	}
	
	NOT_REACHED ();
}

//실행중인 리스트의 자식들 중 인자(pid)와 같은 스레드의 pid를 반환
struct
thread *get_child (int pid){
	struct thread *curr = thread_current();
	struct list *child_list = &curr->child_list;
	for(struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, child_list_elem);
		if(t->tid == pid){
			
			return t;
		}
	}
	return NULL;
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
/* 현재 프로세스를 `name`으로 복제합니다.
  새 프로세스의 스레드 ID를 반환하거나 
  스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	
	/* Clone current thread to new thread.*/
	struct thread *parent = thread_current();
	memcpy(&parent->parent_if, if_, sizeof(struct intr_frame));	//부모의 if 를 부모의 parent_if에 저장
	tid_t pid = thread_create (name, PRI_DEFAULT, __do_fork, parent);
	if(pid == TID_ERROR){
		return TID_ERROR;
	}
	//생성된 스레드와 pid가 같은 스레드를 부모의 자식 리스트에서 가져온다.  
	struct thread *child = get_child(pid);
	
	//자식스레드를 만드는 동안 방해를 받게 하지 않기 위한 sema.  
	sema_down(&child->sema_fork);
	return pid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
/* 이 함수를 pml4_for_each에 전달하여 부모의 주소 공간을 복제합니다.
이것은 프로젝트 2에만 해당됩니다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* parent_page가 커널 페이지이면 즉시 반환합니다. */
	if (is_kernel_vaddr(va)){
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	/* 2. 부모의 페이지 맵 레벨 4에서 VA를 해결합니다. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL){
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
				자식에 대해 새 PAL_USER 페이지를 할당하고 결과를 NEWPAGE로 설정합니다.
	*/
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL){
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). 
	 * 	  부모 페이지를 새 페이지에 복제하고 부모 페이지가 쓰기 가능한지 여부를 확인합니다(결과에 따라 WRITABLE 설정). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
/* 부모의 실행 컨텍스트를 복사하는 스레드 함수.
  * 힌트) parent->tf는 프로세스의 사용자 영역 컨텍스트를 보유하지 않습니다.
  * 즉, 이 함수에 process_fork의 두 번째 인수를 전달해야 합니다. */
static void
__do_fork (void *aux) {
	
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();  //자식 프로세스임
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;

	parent_if = &parent->parent_if;

	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
	
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	/* 힌트) 파일 객체를 복제하려면 include/filesys/file.h에서 `file_duplicate`를 사용하세요.
	이 함수가 부모의 리소스를 성공적으로 복제할 때까지 부모는 fork()에서 반환해서는 안 됩니다.
	*/
	if(parent -> fdidx >= MAX_FD_NUM){
		goto error;
	}

	current -> file_descriptor_table[0] = parent->file_descriptor_table[0];
	current -> file_descriptor_table[1] = parent->file_descriptor_table[1];
	for (int i = 2; i < MAX_FD_NUM; i++){
		struct file *f = parent->file_descriptor_table[i];
		if (f == NULL){
			continue;
		}
		current -> file_descriptor_table[i] = file_duplicate(f);
	}

	current -> fdidx = parent -> fdidx;

	// sema_up(&current -> sema_fork);
	//자식을 다 만들었으니 업하여 활성화 
	process_init ();
	
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	// thread_exit ();
	
	// sema_up(&current -> sema_fork);
	exit_syscall(-1);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
/* current execution 컨텍스트(행위)를 f_name으로 전환합니다.
  * 실패 시 -1을 반환합니다. */
int
process_exec (void *f_name) { 				//실행함수
	char *file_name = f_name;
	bool success;
	
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	/* 스레드 구조에서 intr_frame을 사용할 수 없습니다.
	* 현재 쓰레드가 recheduled 될 때 멤버에게 실행 정보를 저장하기 때문이다. */
	struct intr_frame _if;					//사용 권한 설정?
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();
	
	#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);  // 추가!!
	#endif
	
	/* And then load the binary */
	success = load (file_name, &_if);
	
	
	if (!success){
		
		return -1;
		// exit_syscall(-1);
	}
	
	/* If load failed, quit. */
	palloc_free_page (file_name);
	/* Start switched process. */
	
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
/* 스레드 TID가 죽을 때까지 기다렸다가 종료 상태를 반환합니다.
커널에 의해 종료된 경우(예: 예외로 인해 종료됨) -1을 반환합니다.
TID가 유효하지 않거나 호출 프로세스의 자식이 아니거나 주어진 TID에 대해 process_wait()가 이미 성공적으로 호출된 경우 기다리지 않고 즉시 -1을 반환합니다.

이 기능은 문제 2-2에서 구현될 것이다. 현재로서는 아무 작업도 수행하지 않습니다. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: 힌트) pintos exit if process_wait(initd), process_wait를 구현하기 전에 여기에 
	무한 루프를 추가하는 것이 좋습니다. */
	struct thread *child = get_child(child_tid);	//넘어온 tid 값과 같은 자식 리스트의 스레드를 가져온다.
	
	if (child == NULL){							//없다면 리턴 -1
		
		return -1;
	}
	// if (child->is_waited){						//아직 기다리라고 한 자식이면 리턴 -1
	// 	return -1;
	// }
	// else {										//자식이 있고 기다리라고 했던 적이 없다면 
		
	// 	child -> is_waited = true;				//자식을 기다리라고 한다. 
	// }	
	
	sema_down(&child -> sema_wait);				//자식이 wait 상태인동안 인터럽트 활성화
	list_remove(&child->child_list_elem);		//자식 제거
	sema_up(&child -> sema_free);				//free할 수 있도록 인터럽트 해제
	 
	
	// while (1){}
	// thread_set_priority(thread_get_priority()-1);
	
	return child -> exit_status;			// 종료 상태를 리턴
	
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current () ;
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	/* TODO: 프로세스 종료 메시지 구현(project2/process_termination.html 참조).
	 * TODO: 여기에서 프로세스 리소스 정리를 구현하는 것이 좋습니다. */
	
		
	for (int i = 2; i < MAX_FD_NUM; i ++){
		close_syscall(i);
		// return -1;
	}

	palloc_free_multiple(curr->file_descriptor_table, FDT_PAGES); //멀티풀로 교체 
	
	process_cleanup ();

	sema_up(&curr -> sema_wait);
	sema_up(&curr -> sema_fork);
	sema_down(&curr -> sema_free);  
}

/* Free the current process's resources. */
/* 현재 프로세스의 리소스를 해제합니다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();
	
#ifdef VM
	if(!hash_empty(&curr->spt.hashs))
		supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	/* 현재 프로세스의 페이지 디렉토리를 파괴하고 커널 전용 페이지 디렉토리로 다시 전환합니다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the next thread.
 * This function is called on every context switch. */
/* 넥스트 스레드에서 사용자 코드를 실행하기 위해 CPU를 설정합니다.
  * 이 함수는 모든 컨텍스트 전환에서 호출됩니다. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	/* 스레드의 페이지 테이블을 활성화합니다. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정합니다. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
/* 실행 헤더. [ELF1] 1-4 ~ 1-8 참조.
  * 이것은 ELF 바이너리의 맨 처음에 나타납니다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
/* FILE_NAME에서 현재 스레드로 ELF 실행 파일을 로드합니다.
  * 실행 파일의 진입점을 *RIP에 저장하고 초기 스택 포인터를 *RSP에 저장합니다.
  * 성공하면 true, 그렇지 않으면 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	
	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
	
		goto done;
	process_activate (thread_current ());

	/*---------------P2-----------------*/

	char *token, *argv[64], *save_ptr;  
	int argc = 0;

	token = strtok_r (file_name, " ", &save_ptr);
    while (token)
	{
		argv[argc] = token;
		token = strtok_r ('\0', " ", &save_ptr);
		
		argc ++;
	}

	/*--------------P2-------------------*/
	/* Open executable file. */
	file = filesys_open (file_name);
	
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}
	
	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}
	
	
	/* Set up stack. */
	if (!setup_stack (if_)){
		
		goto done;
	}
	
	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	//1. 첫 주소 부터 글자의 길이(끝에 \0포함) 만큼 넣어준다
	//	글자 길이 만큼 저장 위치가 감소 해야 한다. (거꾸로)
	//	argv[0]까지 = RDI: 4
	uintptr_t start_p = (if_ -> rsp);	//초기 시작 포인터 저장
	uintptr_t curr = 0;	//계속 갱신 되는 임시 포인터 
	char *address[64];	//argv의 주소값을 저장할 배열

	/*argc 자른 마디의 개수만큼 for문 돌린다.*/
	for (int i = argc - 1; i != -1; i--)
	{
		size_t argv_size = (strlen(argv[i]) + 1);	//memcpy를 위한 사이즈(뒤에 '/0'을 하나씩 넣기위한 +1)
		curr += argv_size;	//임시 포인터 갱신
		address[i] = (start_p - curr);	//나중에 주소를 저장해야 하기에 지금 주소를 저장해둠
		
		memcpy ((start_p-curr), argv[i], argv_size);	//argv[i]의 데이터를 start 포인터에 저장 
	}

	//2. 마지막에 word-align = 0 으로 채운다 (8의 배수로 체운다)
	size_t word_align_size = (start_p-curr) % 8;	//8의 배수로 채워야 하므로 나머지 만큼만 공백으로 채움
	curr += word_align_size;
	memset(start_p - curr, '\0', word_align_size);

	//3. argv 배열의 마지막은 0으로 채운다. 
	curr += 8;
	memset(start_p - curr, 0, 8);

	//3-1. argv의 주소값을 뒤에서부터 하나씩 넣는다
	//	주소는 8바이트씩 넣는다. 
	for (int j = argc-1; j != -1; j--)
	{
		size_t argv_size = 8;
		curr += argv_size;
		
		memcpy ((start_p-curr), &address[j], argv_size);
	}

	//4. 마지막에 Return address 를 0으로 넣는다. 
	curr += 8;
	memset(start_p-curr, 0, 8);

	//5. 저장했던 임시 포인터를 실제 rsp에 적용하고 나머지 포인터들도 세팅 한다. 
	if_->rsp -= curr;
	if_->R.rdi = argc;
	if_->R.rsi = (if_->rsp)+8;
	
/*--------------------------------------------------*/
	
	success = true;

	
done:
	/* We arrive here whether the load is successful or not. */
	
	// file_close (file);
	
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;
	
	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;
	
	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		/*------------------------------------*/
		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}
		/*------------------------------------*/	
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을 페이지 테이블에 추가합니다.
  * WRITABLE이 참이면 사용자 프로세스가 페이지를 수정할 수 있습니다.
  * 그렇지 않으면 읽기 전용입니다.
  * UPAGE는 이미 매핑되어 있지 않아야 합니다.
  * KPAGE는 아마도 palloc_get_page()를 사용하여 사용자 풀에서 얻은 페이지여야 합니다.
  * 성공 시 true를 반환하고, UPAGE가 이미 매핑되었거나 메모리 할당에 실패하면 false를 반환합니다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	
	struct file *file = ((struct container*)aux)->file;
	off_t offset = ((struct container*)aux)->offset;
	size_t page_read_bytes = ((struct container*)aux)->page_read_bytes;
	size_t page_zero_bytes = PGSIZE - page_read_bytes;
	
	file_seek(file, offset);  // file의 오프셋을 offset으로 바꾼다. 이제 offset부터 읽기 시작한다.

	/* 페이지에 매핑된 물리 메모리(frame, 커널 가상 주소)에 파일의 데이터를 읽어온다. */
	/* 제대로 못 읽어오면 페이지를 FREE시키고 FALSE 리턴 */
	if (file_read(file, page->frame->kva, page_read_bytes) != (int)page_read_bytes){
		palloc_free_page(page->frame->kva);
		return false;
	}
	/* 만약 1페이지 못 되게 받아왔다면 남는 데이터를 0으로 초기화한다. */
	memset(page->frame->kva + page_read_bytes, 0, page_zero_bytes); 
	
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* 주소 UPAGE에 있는 FILE의 오프셋 OFS에서 시작하는 세그먼트를 로드합니다. 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화됩니다.
  *
  * - UPAGE의 READ_BYTES 바이트는 오프셋 OFS에서 시작하는 FILE에서 읽어야 합니다.
  *
  * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트를 0으로 설정해야 합니다.
  *
  * 이 함수에 의해 초기화된 페이지는 WRITABLE이 참이면 사용자 프로세스에 의해 쓰기 가능해야 하고 그렇지 않으면 읽기 전용이어야 합니다.
  *
  * 성공하면 true를 반환하고 메모리 할당 오류 또는 디스크 읽기 오류가 발생하면 false를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);
	// file_seek(file,ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 1 Page보다 같거나 작은 메모리를 한 단위로 해서 읽어 온다.
		   페이지보다 작은 메모리를 읽어올때 (페이지 - 메모리) 공간을 0으로 만들 것이다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 새 UNINIT 페이지를 만들어서 현재 프로세스의 spt에 넣는다. 
		   페이지에 해당하는 파일의 정보들을 container 구조체에 담아서 AUX로 넘겨준다.
		   타입에 맞게 initializer를 설정해준다. */
		struct container *container = (struct container *)malloc(sizeof(struct container));
		container->file = file;
		container->page_read_bytes = page_read_bytes;
		container->page_zero_bytes = page_zero_bytes;
		container->offset = ofs;
		
		if (!vm_alloc_page_with_initializer (VM_ANON, upage, 
				writable, lazy_load_segment, container))
		{
			return false;
		}
		// page fault가 호출되면 페이지가 타입별로 초기화되고 lazy_load_segment()가 실행된다. 
		
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	
	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, 1)) {
		success = vm_claim_page(stack_bottom);

		if (success){
			if_->rsp = USER_STACK;
			thread_current()->stack_bottom = stack_bottom;
		}
	}
	return success;
}

struct file* process_get_file(int fd){
	struct thread *curr = thread_current();
	struct file* fd_file = curr->file_descriptor_table[fd];

	if (fd_file)
		return fd_file;
	else
		return NULL;
}

#endif /* VM */

