#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"

#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜 핸들러 */
static void exit(int status) NO_RETURN;
static int read(int fd, void *buffer, unsigned size);
static int write(int fd, const void *buffer, unsigned size);
static void seek(int fd, unsigned position);
static unsigned tell(int fd);
static bool create(const char *file_name, unsigned int file_size);
static bool remove(const char *file_name);
static int open(const char *file_name);
static void close(int fd);
static int filesize(int fd);

/* 헬퍼 함수 */
static void check_address(void *addr);
static void check_string(const char *str);

/* 파일 시스템 동기화를 위한 전역 락 */
static struct lock filesys_lock;

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

	lock_init(&filesys_lock);
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

		case SYS_FORK:
			check_string((const char *) f->R.rdi);
			f->R.rax = process_fork((const char *) f->R.rdi, f);
			break;

		case SYS_EXEC:
			check_string((const char *) f->R.rdi);
			int result = process_exec((void *) f->R.rdi);
			if (result == -1) {
				exit(-1);
			}
			break;

		case SYS_WAIT:
			/* 자식 프로세스가 종료될 때까지 기다린다. */
			f->R.rax = process_wait((tid_t) f->R.rdi);
			break;

		case SYS_CREATE:
			check_string((const char *) f->R.rdi);
			f->R.rax = create((const char *)f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:
			check_string((const char *) f->R.rdi);
			f->R.rax = remove((const char *) f->R.rdi);
			break;

		case SYS_OPEN:
			check_string((const char *) f->R.rdi);
			f->R.rax = open((const char *)f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;

		case SYS_READ:
			check_address((void *) f->R.rsi);
			/* rdi, rsi, rdx -> fd, buffer, size */
			if (f->R.rdx > 0) { /* size > 0 일때만 검사한다. */
				check_address((void *)f->R.rsi + f->R.rdx - 1);
			}
			f->R.rax = read(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
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

		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;

		case SYS_CLOSE:
			close((int) f->R.rdi);
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

static int
read(int fd, void *buffer, unsigned size) {
	/* fd가 표준 출력이거나 유효하지 않은 범위일 경우 */
	if (fd == STDOUT_FILENO || fd < 0 || fd >= FDT_SIZE) {
		return -1;
	}

	if (fd == STDIN_FILENO) {
		// TODO: fd 가 표준 입력인 경우 : 키보드로부터 입력 받기
		uint8_t *buf = (uint8_t *) buffer;
		for (unsigned i = 0; i < size; i++) {
			buf[i] = input_getc();
		}

		return size;
	}

	struct thread *current = thread_current();
	struct file *file_obj = current->fd_table[fd];

	if (file_obj == NULL) {
		return -1;
	}

	/* 파일 시스템 접근 시에만 락을 사용한다. */
	lock_acquire(&filesys_lock);
	int bytes_read = file_read(file_obj, buffer, size);
	lock_release(&filesys_lock);

	return bytes_read;
}

static int
filesize(int fd) {
	/* 파일 디스크립터 유효성 검사 */
	if (fd < 2 || fd >= FDT_SIZE) {
		return -1;
	}

	lock_acquire(&filesys_lock);
	struct thread *current = thread_current();
	struct file *file_obj = current->fd_table[fd];

	/* 해당 fd에 열린 파일이 없는 경우 */
	if (file_obj == NULL) {
		lock_release(&filesys_lock);
		return -1;
	}

	int length = file_length(file_obj);
	lock_release(&filesys_lock);

	return length;
}

static int
write(int fd, const void *buffer, unsigned size) {
	if (fd == STDOUT_FILENO) {
		/* 표준 출력(stdout)에 버퍼의 내용을 size만큼 출력하는 로직 */
		lock_acquire(&filesys_lock);
		putbuf(buffer, size);
		lock_release(&filesys_lock);

		return size;
	}

	if (fd == STDIN_FILENO) {
		/* 표준 입력에 쓰는 것은 허용되지 않는다. */
		return -1;
	}

	if (fd < 0 || fd >= FDT_SIZE) {
		return -1;
	}

	// TODO: 일반 파일에 쓰는 경우
	struct thread *current = thread_current();
	struct file *file_obj = current->fd_table[fd];

	if (file_obj == NULL) {
		return -1;
	}

	lock_acquire(&filesys_lock);
	int bytes_written = file_write(file_obj, buffer, (off_t) size);
	lock_release(&filesys_lock);

	return bytes_written;
}

static void
seek(int fd, unsigned position) {
	if (fd < 2 || fd >= FDT_SIZE) {
		return;
	}

	struct thread *current = thread_current();
	struct file *file_obj = current->fd_table[fd];

	if (file_obj == NULL) {
		return;
	}

	file_seek(file_obj, position);
}

static unsigned
tell(int fd) {
	if (fd < 2 || fd >= FDT_SIZE) {
		return 0;
	}

	struct thread *current = thread_current();
	struct file *file_obj = current->fd_table[fd];

	if (file_obj == NULL) {
		return 0;
	}

	return file_tell(file_obj);
}

static bool
create(const char *file_name, unsigned int file_size) {
	/* 파일 이름이 비어있는 경우 -> 실패 처리 */
	if (*file_name == '\0') {
		return false;
	}

	lock_acquire(&filesys_lock);
	bool result = filesys_create(file_name, (off_t) file_size);
	lock_release(&filesys_lock);

	return result;
}

static bool
remove(const char *file_name) {
	/* 파일 이름이 비어있는 경우 -> 실패 처리 */
	if (*file_name == '\0') {
		return false;
	}

	lock_acquire(&filesys_lock);
	bool success = filesys_remove(file_name);
	lock_release(&filesys_lock);

	return success;
}

static int
open(const char *file_name) {
	/* 예외 처리 : 파일 이름이 비어있는 경우 */
	if (*file_name == '\0') {
		return -1;
	}

	lock_acquire(&filesys_lock);
	struct file *file_obj = filesys_open(file_name);

	if (file_obj == NULL) {
		lock_release(&filesys_lock);
		return -1;
	}

	struct thread *current = thread_current();

	/* 파일 디스크립터 테이블에서 비어있는 가장 작은 fd 를 찾는다. */
	int fd = -1;
	for (int i = 2; i < FDT_SIZE; i++) {
		if (current->fd_table[i] == NULL) {
			current->fd_table[i] = file_obj;
			fd = i;
			break;
		}
	}

	/* 빈 fd를 찾지 못한 경우 (테이블이 가득 찬 경우) 파일을 닫고 -1을 반환한다. */
	if (fd == -1) {
		file_close(file_obj);
	}

	lock_release(&filesys_lock);

	return fd;
}

static void
close(int fd) {
	/* 파일 디스크립트(fd) 유효성 검사 */
	/* 표준 입출력(0,1)이거나 유효 범위를 벗어난 fd는 처리하지 않는다. */
	if (fd < 2 || fd >= FDT_SIZE) {
		return;
	}

	struct thread *thread = thread_current();

	/* 파일 객체 포인터 조회 */
	struct file *file_obj = thread->fd_table[fd];

	/* 해당 fd에 열린 파일이 없는 경우 (이미 닫혔거나, 열린 적이 없는 경우) */
	if (file_obj == NULL) {
		return;
	}

	/* 파일 객체 닫기 */
	lock_acquire(&filesys_lock);
	file_close(file_obj);

	/* 파일 디스크립터 테이블을 정리해서 fd를 재사용할 수 있게 한다. */
	thread->fd_table[fd] = NULL;
	lock_release(&filesys_lock);
}


/* 헬퍼 함수 - 주소 유효성 검사 */
static void check_address(void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		exit(-1);
	}
}

/* 헬퍼 함수 - 문자열 주소 유효성 검사 */
static void check_string(const char *str) {
	/* check_address 함수는 NULL 포인터도 처리하므로, str 포인터 자체를 먼저 검사한다. */
	check_address((void *) str);

	while (*str != '\0') {
		str++;
		check_address((void *) str);
	}
}