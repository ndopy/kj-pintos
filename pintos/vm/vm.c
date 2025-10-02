/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"

#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"

/* 전역 프레임 테이블과 락 */
static struct list frame_table;
static struct lock frame_table_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	/* 프레임 테이블과 락 초기화 */
	list_init(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static uint64_t page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED);

/* 가상 주소를 키로 사용해서 해시 값을 계산한다. */
static uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED) {
	const struct page *p = hash_entry(e, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
}

/* 해시 테이블에서 사용할 비교 함수 */
static bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry(a_, struct page, hash_elem);
	const struct page *b = hash_entry(b_, struct page, hash_elem);
	return a->va < b->va;
}


/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */

struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page p; /* 검색을 위한 임시 페이지 구조체 생성 */
	struct hash_elem *e; /* 해시 테이블 요소를 가리키는 포인터 선언 */

	/* pg_round_down : 특정 가상 주소(va)가 속한 가상 페이지의 시작 주소를 계산해준다. */
	p.va = pg_round_down(va);	/* 정렬된 주소를 페이지의 가상 주소로 설정 */

	/* 해시 테이블에서 해당 페이지 검색 */
	e = hash_find(&spt->pages, &p.hash_elem);

	/* 페이지를 찾은 경우 */
	if (e != NULL) {
		/* 해시 요소를 페이지 구조체로 변환하여 반환 */
		return hash_entry(e, struct page, hash_elem);
	}

	/* 페이지를 찾지 못한 경우 NULL 반환 */
	return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	return hash_insert(&spt->pages, &page->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/

/* vm_get_frame: 새로운 물리 프레임을 할당하고 초기화하는 함수
 *
 * 반환값:
 * - struct frame*: 새로 할당된 프레임의 포인터
 * - NULL: 메모리 할당 실패 시
 *
 * 동작:
 * 1. palloc_get_page를 사용하여 사용자 풀에서 새 페이지를 할당
 * 2. 할당 실패 시 페이지 교체(eviction) 수행
 * 3. 프레임 구조체를 생성하고 초기화
 * 4. 할당된 프레임 반환
 *
 * 주의사항:
 * - 반환된 프레임은 아직 특정 페이지와 연결되지 않은 상태임(frame->page = NULL)
 * - 메모리 부족 시 페이지 교체 정책에 따라 기존 페이지를 방출함
 */

static struct frame *
vm_get_frame (void) {
	/* 새로운 프레임 구조체를 가리킬 포인터를 NULL로 초기화 */
	struct frame *frame = NULL;

	/* 사용자 메모리 풀에서 새 페이지를 할당받고 0으로 초기화 */
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);

	/* 페이지 할당에 실패한 경우 */
	if (kva == NULL) {
		/* PANIC("TODO: Implement eviction"); */
		/* 지금은 스와핑이 구현되지 않았으므로 패닉을 발생시키거나
		 * vm_evict_frame을 호출하도록 미리 만들어 둘 수 있다.
		 */
		/* 페이지 교체 정책을 통해 새로운 프레임을 확보 */
		return vm_evict_frame();
	}

	/* 프레임 구조체를 위한 메모리 동적 할당 */
	frame = malloc(sizeof(struct frame));

	/* 프레임 구조체 할당 실패 시 이전에 할당받은 페이지를 반환하고 NULL 반환 */
	if (frame == NULL) {
		palloc_free_page(kva);
		return NULL;
	}

	frame->kva = kva;		/* 할당받은 커널 가상 주소를 프레임에 저장 */
	frame->page = NULL;		/* 프레임에 연결된 페이지가 없음을 표시 */
							/* 아직 어떤 페이지와도 연결되지 않음. */

	ASSERT(frame != NULL);			/* 프레임이 제대로 할당되었는지 확인 */
	ASSERT(frame->page == NULL);	/* 프레임에 연결된 페이지가 없는지 확인 */
	return frame;					/* 할당된 프레임 반환 */
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
/* vm_do_claim_page: 페이지에 물리 프레임을 할당하고 매핑하는 함수
 *
 * 인자:
 * - struct page *page: 물리 메모리를 할당받을 페이지 구조체
 * 
 * 반환값:
 * - bool: 성공 시 true, 실패 시 false
 *
 * 동작:
 * 1. 새로운 프레임을 할당받음
 * 2. 페이지와 프레임을 서로 연결
 * 3. 페이지의 가상 주소와 프레임의 물리 주소를 매핑
 * 4. 실패 시 할당받은 자원을 모두 해제
 */
static bool
vm_do_claim_page (struct page *page) {
	if (page == NULL) {
		return false;
	}

	/* 빈 프레임을 얻는다. */
	struct frame *frame = vm_get_frame ();

	/* 프레임 할당에 실패한 경우 */
	if (frame == NULL) {
		return false;
	}

	/* 페이지와 프레임을 서로 연결한다. */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 페이지의 가상 주소(VA)를 프레임의 물리 주소(PA)에 매핑 */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
		/* 자원은 할당받았지만 가상-물리 주소 매핑에 실패한 경우 */
		/* 할당받았던 자원들을 모두 해제한다. */
		palloc_free_page(frame->kva);
		free(frame);

		/* 페이지와 프레임의 연결을 끊어 댕글링 포인터를 방지한다. */
		page->frame = NULL;
		
		return false;
	}

	/* 페이지의 종류를 파악하고, 알맞은 위치에서 데이터를 읽어와 물리 프레임에 복사한다. */
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
