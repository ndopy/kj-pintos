/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

#include "vm/vm.h"
#include "vm/uninit.h"

static bool uninit_initialize (struct page *page, void *kva);
static void uninit_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
/**
 * @brief 초기화되지 않은 새로운 페이지를 생성하는 함수
 * 
 * @param page			초기화할 페이지 구조체 포인터
 * @param va			가상 주소
 * @param init			페이지 초기화를 위한 콜백 함수
 * @param type			페이지의 타입 (anon, file, page_cache)
 * @param aux			초기화에 필요한 보조 데이터
 * @param initializer	페이지 초기화를 수행할 함수 포인터
 */
void
uninit_new(struct page *page, void *va, vm_initializer *init,
           enum vm_type type, void *aux,
           bool (*initializer)(struct page *, enum vm_type, void *)) {
	ASSERT(page != NULL);						/* 페이지 포인터가 NULL이 아닌지 확인 */

	*page = (struct page){
		.operations = &uninit_ops,				/* 초기화되지 않은 페이지의 기본 operations 설정 */
		.va = va,								/* 가상 주소 설정 */
		.frame = NULL,							/* 아직 프레임이 할당되지 않음 */
		.uninit = (struct uninit_page){
			.init = init,						/* 초기화 콜백 함수 설정 */
			.type = type,						/* 페이지 타입 설정 */
			.aux = aux,							/* 보조 데이터 설정 */
			.page_initializer = initializer,	/* 페이지 초기화 함수 설정 */
		}
	};
}

/* Initalize the page on first fault */
static bool
uninit_initialize (struct page *page, void *kva) {
	struct uninit_page *uninit = &page->uninit;

	/* Fetch first, page_initialize may overwrite the values */
	vm_initializer *init = uninit->init;
	void *aux = uninit->aux;

	/* TODO: You may need to fix this function. */
	return uninit->page_initializer (page, uninit->type, kva) &&
		(init ? init (page, aux) : true);
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
static void
uninit_destroy (struct page *page) {
	struct uninit_page *uninit UNUSED = &page->uninit;
	/* TODO: Fill this function.
	 * TODO: If you don't have anything to do, just return. */
}
