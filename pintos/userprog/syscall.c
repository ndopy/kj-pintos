#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"

#include "filesys/filesys.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜 핸들러 */
static void exit(int status) NO_RETURN;
static int write(int fd, const void *buffer, unsigned size);
static bool create(const char *file_name, unsigned int file_size);

/* 헬퍼 함수 */
static void check_address(void *addr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// TODO: Your implementation goes here.
	switch (f->R.rax) {
		case SYS_HALT:
			/* Pintos를 종료시킨다. */
			power_off();
			break;

		case SYS_EXIT:
			/* 현재 프로세스를 종료시킨다. */
			exit(f->R.rdi);
			break;

		case SYS_CREATE:
			check_address((void *) f->R.rdi);
			f->R.rax = create((void *)f->R.rdi, f->R.rsi);
			break;

		case SYS_WRITE:
			// 인자 유효성 검사
			check_address((void *) f->R.rsi);

			// size가 0보다 클 때만 버퍼의 끝 주소를 검사한다.
			if (f->R.rdx > 0) {
				check_address((void *) f->R.rsi + f->R.rdx - 1);
			}

			f->R.rax = write(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
			break;

		default:
			/* 아직 구현되지 않은 시스템 콜이 호출되면, 비정상 종료로 처리한다. */
			printf ("system call %lld not implemented!\n", f->R.rax);
			thread_current()->exit_status = -1;
			thread_exit();
			break;
	}
}

static void
exit(int status) {
	thread_current()->exit_status = status;
	thread_exit();
}

/* 헬퍼 함수 - 주소 유효성 검사 */
static void check_address(void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		exit(-1);
	}
}

static int
write(int fd, const void *buffer, unsigned size) {
	if (fd == 1) {
		/* TODO: 표준 출력(stdout)에 버퍼의 내용을 size만큼 출력하는 로직 */
		putbuf(buffer, size);
		return size;
	}

	// TODO: fd가 1이 아닌 경우 처리 (파일에 쓰는 경우)
	return -1;
}

static bool
create(const char *file_name, unsigned int file_size) {
	bool result = filesys_create(file_name, (off_t) file_size);

	return result;
}
