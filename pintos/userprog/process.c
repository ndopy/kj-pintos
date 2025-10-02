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

/** lazy_load_segment 함수에 필요한 정보를 담는 구조체
 * - 페이지 폴트 시 어떤 파일의 어느 위치에서 얼마만큼 읽어야 하는지에 대한 정보를 담는다.
 */
struct lazy_load_info {
	struct file *file;		/* 읽어올 데이터가 있는 파일 */
	off_t ofs;				/* 파일 내에서 데이터를 읽기 시작할 오프셋 */
	uint32_t read_bytes;	/* 파일에서 실제로 읽어야 할 바이트 수 */
	uint32_t zero_bytes;	/* 읽은 데이터 뒤에 0으로 채워야 할 바이트 수 */
	bool writable;			/* 페이지의 쓰기 가능 여부 */
};

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static struct thread *get_child_process (tid_t child_tid);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* file_name에서 프로그램 이름만 파싱하여 스레드 이름으로 사용 */
	char thread_name[128];
	strlcpy(thread_name, file_name, sizeof thread_name);
	char *save_ptr;
	strtok_r(thread_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *current = thread_current();
	tid_t child_tid;

	/* Clone current thread to new thread.*/
	child_tid = thread_create(name, PRI_DEFAULT, __do_fork, if_);
	if (child_tid == TID_ERROR) {
		return TID_ERROR;
	}

	/* 자식 프로세스가 리소스 복제를 마칠 때까지 대기 */
	sema_down(&current->fork_sema);

	if (current->fork_success) {
		return child_tid;
	} else {
		return TID_ERROR;
	}
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux UNUSED) {
	struct thread *current = thread_current ();
	struct thread *parent = current->parent;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 1. 커널 가상 주소 공간에 매핑된 페이지는 복제하지 않고 공유한다. */
	if (is_kernel_vaddr(va)) {
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) {
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	/* 3. 자식 프로세스를 위한 새 페이지를 할당한다. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL) {
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	/* 4. 부모 페이지의 내용을 새 페이지로 복사하고, 쓰기 가능 여부를 확인한다. */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		/* 6. 페이지 매핑에 실패하면 할당받은 페이지를 해제하고 에러를 반환한다. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *current = thread_current ();
	struct thread *parent = current->parent;
	struct intr_frame *parent_if = (struct intr_frame *) aux;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;	/* 자식 프로세스의 fork() 반환 값은 0이다. */

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
	if (!pml4_for_each (parent->pml4, duplicate_pte, NULL))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	/* 파일 디스크립터 테이블 복제 */
	current->fd_table = palloc_get_page(PAL_ZERO);
	if (current->fd_table == NULL) {
		goto error;
	}

	for (int i = 0; i < FDT_SIZE; i++) {
		struct file *file = parent->fd_table[i];
		if (file != NULL) {
			if (i < 2) {	/* 표준 입출력(0, 1)은 복제하지 않고 공유하기 */
				current->fd_table[i] = file;
			} else {
				current->fd_table[i] = file_duplicate(file);
			}
		}
	}

	/* Finally, switch to the newly created process. */
	parent->fork_success = true;
	sema_up(&parent->fork_sema);	/* 부모 프로세스 깨우기 */
	do_iret(&if_);

error:
	parent->fork_success = false;
	sema_up(&parent->fork_sema);	/* 부모 프로세스 깨우기 */
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	struct thread *curr = thread_current();
	uint64_t *old_pml4 = curr->pml4;

	/* FDT 가 없다면 (최초의 유저 프로세스) 할당해준다. */
	/* exec는 기존 FDT를 유지해야 하지만,
	 * 스레드가 처음으로 유저 프로세스가 되는 경우에는 FDT가 없으므로 생성해야 한다.
	 */
	if (curr->fd_table == NULL) {
		curr->fd_table = palloc_get_page(PAL_ZERO);

		if (curr->fd_table == NULL) {
			return -1;	/* FDT 할당 실패 */
		}
	}

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 명령어 문자열 복사 : f_name을 커널 메모리로 복사 */
	file_name = palloc_get_page(PAL_ZERO);
	if (file_name == NULL) {
		return -1;
	}
	strlcpy(file_name, f_name, PGSIZE);

	/* And then load the binary */
	/* load 함수는 성공 시 새로운 pml4 를 생성하고 활성화한다. */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	if (!success) {
		palloc_free_page (file_name);
		return -1;
	}

	/* load 성공 시, 더 이상 필요 없는 복사본 페이지를 해제한다. */
	palloc_free_page(file_name);

	/* 이전 프로그램이 사용하던 페이지 테이블을 파괴한다. */
	if (old_pml4 != NULL) {
		pml4_destroy(old_pml4);
	}

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
int
process_wait (tid_t child_tid) {
	/* 자식 프로세스 디스크립터를 찾는다. */
	struct thread *child = get_child_process(child_tid);

	/* 자식이 아니거나, 이미 wait한 자식이면 -1을 반환한다. */
	if (child == NULL) {
		return -1;
	}

	/* 자식 프로세스가 종료할 때까지 대기한다. (sema_down) */
	sema_down(&child->wait_sema);

	/* 자식의 종료 상태를 얻는다. */
	int status = child->exit_status;

	/* 자식 리스트에서 제거한다. (자식 프로세스 수확) */
	list_remove(&child->child_elem);

	/* 자식 프로세스가 완전히 종료될 수 있도록 신호를 보낸다. */
	sema_up(&child->reap_sema);

	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/*
	 * 프로세스 종료 메시지를 출력한다.
	 * curr->exit_status는 exit() 시스템 콜 핸들러에서 설정됩니다.
	 */
	printf("%s: exit(%d)\n", curr->name, curr->exit_status);

	process_cleanup ();

	/*
	 * 부모 프로세스가 process_wait()에서 기다리고 있다면 깨운다.
	 * (sema_up)
	 */
	sema_up(&curr->wait_sema);

	/* 부모가 wait() 을 호출하여 수확할 때까지 대기한다. */
	sema_down(&curr->reap_sema);
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	/* 스레드가 가진 모든 파일 디스크립터를 닫고 FDT 메모리를 해제하기 */
	if (curr->fd_table != NULL) {
		/* fd_table 을 순회하며 열려있는 모든 파일을 닫는다. (자원 누수 방지) */
		for (int i = 2; i < FDT_SIZE; i++) {
			if (curr->fd_table[i] != NULL) {
				file_close(curr->fd_table[i]);
			}
		}

		palloc_free_page(curr->fd_table);
		curr->fd_table = NULL;
	}

	/* 실행 중인 파일의 락을 해제한다. */
	if (curr->executable != NULL) {
		file_close(curr->executable);
		curr->executable = NULL;
	}

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
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

/*
 * 현재 프로세스의 자식 리스트에서 child_tid에 해당하는
 * 자식 스레드를 찾아 반환한다.
 */
static struct thread *
get_child_process (tid_t child_tid) {
	struct thread *cur = thread_current();
	struct list_elem *e;

	for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e)) {
		struct thread *child = list_entry(e, struct thread, child_elem);
		if (child->tid == child_tid) {
			return child;
		}
	}
	/* 자식을 찾지 못하면 NULL을 반환한다. */
	return NULL;
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
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
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	uint64_t *old_pml4;
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Argument Passing을 위한 변수들을 함수 시작 부분에 선언 */
	#define MAX_ARGS 64						/* 인자의 최대 개수: 64개 */
	char *argv[MAX_ARGS];
	char *arg_addresses[MAX_ARGS];
	int argc = 0;							/* 인자 개수 */
	char *token, *save_ptr;					/* strtok_r을 위한 변수들 */

	/* Allocate and activate page directory. */
	old_pml4 = t->pml4;
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* process_exec 에서 전달된 file_name 은 이미 복사본이므로 수정해도 안전하다. */
	// char *program_name = strtok_r(file_name, " ", &save_ptr);
	char *program_name = strtok_r((char *) file_name, " ", &save_ptr);

	/* Open executable file. */
	file = filesys_open (program_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", program_name);
		goto done;
	}

	/* 실행 중인 파일에 다른 프로세스가 쓰지 못하도록 막는다. */
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", program_name);
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
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	/* Argument Passing 구현 시작 */
	argv[argc] = program_name;		// 첫 번째 인자로 프로그램 이름 저장
	argc += 1;

	/* 공백을 기준으로 문자열을 파싱 */
	while ((token = strtok_r(NULL, " ", &save_ptr)) != NULL) {
		if (argc >= MAX_ARGS) {
			break;
		}

		argv[argc] = token;
		argc += 1;
	}

	/* 파싱된 문자열들을 스택에 저장 (문자열 내용) */
	for (i = argc-1; i >= 0; i--) {
		int arg_size = strlen(argv[i]) + 1;   // 문자열의 길이 (널 문자 포함)
		if_->rsp -= arg_size;				  // 스택 포인터 옮기기
		memcpy((void *)if_->rsp, argv[i], arg_size);
		arg_addresses[i] = (char *) if_->rsp;
	}

	/* 스택 정렬 : 스택 포인터를 8의 배수로 맞추기 */
	while (if_->rsp % 8 != 0) {
		// 1씩 빼주면서(스택을 아래로 내리면서) 공간을 0으로 채우기
		if_->rsp -= 1;
		*((char *)(if_->rsp)) = 0;
	}

	/* 널 포인터 쌓기 */
	if_->rsp -= 8;
	*((char **)(if_->rsp)) = 0;

	/* 문자열들의 주소 쌓기 */
	for (i = argc-1; i >= 0; i--) {
		if_->rsp -= 8;
		*((char **)(if_->rsp)) = arg_addresses[i];
	}

	/* 레지스터에 argc, argv 설정 */
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp;

	/* 가짜 return address */
	if_->rsp -= 8;
	*((char **)(if_->rsp)) = 0;
	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (success) {
		t->executable = file;
	} else {
		/* load가 실패하면 파일을 닫는다. */
		file_close(file);

		/* 새로 만든 페이지 테이블을 파괴하고, 이전 페이지 테이블로 복원 및 활성화한다. */
		pml4_destroy(t->pml4);
		t->pml4 = old_pml4;

		if (old_pml4 != NULL) {
			process_activate(t);
		}
	}
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

static bool
lazy_load_segment (struct page *page, void *aux) {
	struct lazy_load_info *info = (struct lazy_load_info *) aux;

	struct file *file = info->file;
	off_t ofs = info->ofs;
	size_t page_read_bytes = info->read_bytes;
	size_t page_zero_bytes = info->zero_bytes;

	/* 어느 파일(file)의 어디서부터(ofs) 읽어야 할지를 정한다. */
	file_seek(file, ofs);

	/* file 에서 read_bytes만큼 데이터를 읽어서 물리 메모리(kva)에 넣는다. (=로딩) */
	if (file_read(file, page->frame->kva, page_read_bytes) != (int) page_read_bytes) {
		/* 파일 읽기 실패 시, 보조 정보 메모리 해제 후 false 반환 */
		free(info);
		return false;
	}

	/* 남는 공간을 0으로 채우기 */
	memset(page->frame->kva + page_read_bytes, 0, page_zero_bytes);

	free(info);
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

/** 실행 파일의 세그먼트를 메모리에 로드하는 함수
 * 
 * @param file        로드할 세그먼트가 있는 파일
 * @param ofs         파일 내에서 세그먼트의 시작 위치
 * @param upage       세그먼트를 매핑할 가상 주소
 * @param read_bytes  파일에서 실제로 읽어야 할 바이트 수
 * @param zero_bytes  0으로 채워야 할 바이트 수
 * @param writable    쓰기 가능 여부
 * @return           성공하면 true, 실패하면 false
 */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	/* 페이지 정렬 검사 */
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 현재 페이지에 대해 파일에서 읽을 크기와 0으로 채울 크기 계산 
		 * PGSIZE보다 작은 경우 남은 크기만큼만 읽고 나머지는 0으로 채움 */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* lazy_load_segment에 전달할 보조 정보 설정 
		 * 파일 읽기에 필요한 정보를 담은 구조체를 생성해야 함 */
		struct lazy_load_info *info = malloc(sizeof(struct lazy_load_info));
		if (info == NULL) {
			return false;
		}

		info->file = file;
		info->ofs = ofs;
		info->read_bytes = page_read_bytes;
		info->zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer(VM_FILE, upage,
		                                    writable, lazy_load_segment, info)) {
			free(info);
			return false;
		}

		/* 다음 페이지 처리를 위해 카운터 갱신 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/** 사용자 프로세스의 스택을 설정하는 함수
 * 
 * @param if_ 인터럽트 프레임 구조체의 포인터
 * @return 스택 설정 성공 시 true, 실패 시 false
 * 
 * @details
 * USER_STACK 위치에 스택용 페이지를 생성하고 초기화한다.
 * VM_ANON 타입의 페이지를 할당하고 즉시 물리 메모리에 매핑한다.
 * 성공 시 인터럽트 프레임의 스택 포인터(rsp)를 USER_STACK으로 설정한다.
 */
static bool
setup_stack (struct intr_frame *if_) {
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* VM_ANON 타입으로, 쓰기 가능한 스택 페이지를 할당한다. */
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)) {
		/* 할당된 페이지를 즉시 물리 메모리에 올린다(claim). */
		if (vm_claim_page(stack_bottom)) {
			/* 성공 시 스택 포인터를 설정한다. */
			if_->rsp = USER_STACK;
			return true;
		} else {
			/* claim 실패 시, 이전에 SPT에 등록했던 (malloc으로 할당한) page를 해제하여 리소스 누수를 방지한다. */
			struct page *page = spt_find_page(&thread_current()->spt, stack_bottom);
			if (page) {
				vm_dealloc_page(page);
			}
		}
	}
	return false;
}
#endif /* VM */
