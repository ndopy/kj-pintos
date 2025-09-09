/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		/* 대기자 리스트를 우선순위 순으로 정렬하기 위해 ordered insert 사용 */
		list_insert_ordered(&sema->waiters, &thread_current()->elem, compare_priority, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();

	// 쓰레드를 깨우기 전에, 우선순위 기부로 인해 순서가 바뀌었을 수 있으므로
	// 대기자 리스트를 현재 우선순위 기준으로 다시 정렬한다.
	list_sort (&sema->waiters, compare_priority, NULL);

	if (!list_empty (&sema->waiters))
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	sema->value++;
	intr_set_level (old_level);

	/*
	 * thread_unblock 으로 ready_list 의 우선순위가 변경됐을 수 있으므로
	 * 현재 실행중인 스레드와 ready_list 의 첫 번째 스레드의 우선순위를 비교해서
	 * 선점 여부를 확인하고 CPU를 양보한다..
	 */
	if (should_preempt()) {
		thread_yield();
	}

}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */

/* 락을 획득하는 함수. 필요한 경우 락이 사용 가능해질 때까지 대기한다.
 * 현재 스레드가 이미 해당 락을 보유하고 있으면 안 된다.
 * 
 * 이 함수는 sleep 상태가 될 수 있으므로 인터럽트 핸들러 내에서 호출되면 안 된다.
 * 인터럽트가 비활성화된 상태에서 호출될 수 있지만, sleep이 필요한 경우
 * 다음 스케줄된 스레드가 인터럽트를 다시 활성화할 것이다.
 * 
 * 개선 제안:
 * 1. 우선순위 기부 체인이 너무 길어질 경우의 성능 문제 고려 필요
 * 2. 데드락 감지 및 예방 로직 추가 검토
 * 3. 재귀적 락(recursive lock) 지원 여부 검토 */
void
lock_acquire(struct lock *lock) {
	/* 기본적인 유효성 검사 수행 */
	ASSERT(lock != NULL);							// 락 포인터가 유효한지 확인
	ASSERT(!intr_context ());						// 인터럽트 컨텍스트가 아닌지 확인
	ASSERT(!lock_held_by_current_thread (lock));	// 현재 스레드가 이미 락을 보유하고 있지 않은지 확인

	struct thread *current = thread_current();		// 현재 실행 중인 스레드 정보 가져오기
	enum intr_level old_level;						// 이전 인터럽트 레벨 저장용 변수

	/* 락 상태 확인 및 기부 과정을 원자적으로 실행한다. */
	old_level = intr_disable();						// 인터럽트 비활성화하여 원자성 보장

	/* 다른 스레드가 락을 보유중이면 우선순위 기부를 수행한다. */
	if (lock->holder != NULL) {							// 락이 이미 점유되어 있는 경우
		current->wait_on_lock = lock;					// 현재 스레드가 대기 중인 락 정보 저장

		/* 우선순위 기부 체인을 따라 올라가며 우선순위를 갱신한다. */
		struct thread *target = lock->holder;			// 락을 보유한 스레드부터 시작
		while (target) {
			// 체인을 따라 올라가며
			if (target->priority < current->priority) {	// 현재 스레드의 우선순위가 더 높으면
				target->priority = current->priority;	// 우선순위를 기부

				// 우선순위가 변경된 target 이 만약 ready_list 에 있다면 재정렬 해야한다.
				if (target->status == THREAD_READY) {
					list_remove(&target->elem);
					list_insert_ordered(&ready_list, &target->elem, compare_priority, NULL);
				}
			}

			if (target->wait_on_lock) {					// 상위 락이 있다면
				target = target->wait_on_lock->holder;	// 체인을 따라 계속 진행
			} else {
				break;									// 체인의 끝에 도달하면 종료
			}
		}
	}
	intr_set_level(old_level); // 인터럽트 상태 복원

	/* 세마포어를 내려 락을 획득하려고 시도한다.
	 * 만약 다른 스레드가 락을 가지고 있다면, 여기서 잠들게 된다. */
	sema_down(&lock->semaphore); // 락 획득 시도

	/* 락 획득에 성공했거나, 잠에서 깨어났을 때 아래 로직이 실행된다.
	 * 이 부분도 원자적으로 처리해야 한다. */
	old_level = intr_disable(); // 다시 인터럽트 비활성화

	/* 이제 이 락은 더 이상 기다리는 락이 아니므로, wait_on_lock을 NULL로 초기화한다. */
	current->wait_on_lock = NULL; // 대기 중인 락 정보 초기화
	lock->holder = current; // 락 소유자를 현재 스레드로 설정
	list_push_back(&current->holding_locks, &lock->lock_elem); // 보유 중인 락 리스트에 추가
	intr_set_level(old_level); // 인터럽트 상태 복원
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	enum intr_level old_level = intr_disable();

	struct thread *current = thread_current();
	struct list_elem *e;

	/* 현재 락을 스레드의 '소유한 락' 리스트에서 제거한다. */
	list_remove(&lock->lock_elem);

	/* 우선순위를 재계산한다. */
	/* 먼저 자신의 원래 우선순위로 복원한다. */
	current->priority = current->original_priority;

	/* 아직 소유 중인 다른 락이 있다면, 그 락들을 통해 기부받은 우선순위 중
	 * 가장 높은 값으로 자신의 우선순위를 갱신한다. */
	if (!list_empty(&current->holding_locks)) {
		for (e = list_begin(&current->holding_locks); e != list_end(&current->holding_locks); e = list_next(e)) {
			struct lock *l = list_entry(e, struct lock, lock_elem);

			if (!list_empty(&l->semaphore.waiters)) {
				/* waiters 리스트가 항상 정렬되어 있다는 보장이 없으므로
				 * 전체 리스트를 순회하며 가장 높은 우선순위를 직접 찾는다.
				 */
				struct list_elem *waiter_e;
				struct thread *highest_waiter = NULL;

				for (waiter_e = list_begin(&l->semaphore.waiters); waiter_e != list_end(&l->semaphore.waiters); waiter_e = list_next(waiter_e)) {
					struct thread *t = list_entry(waiter_e, struct thread, elem);
					if (highest_waiter == NULL || t->priority > highest_waiter->priority) {
						highest_waiter = t;
					}
				}

				if (highest_waiter != NULL) {
					if (current->priority < highest_waiter->priority) {
						current->priority = highest_waiter->priority;
					}
				}
			}
		}
	}

	/* 락의 소유권을 포기하고, 기다리던 스레드 중 하나를 깨운다. */
	lock->holder = NULL;
	sema_up (&lock->semaphore);

	/* 우선순위가 변경되었으므로, 선점이 필요한지 확인한다. */
	if (should_preempt()) {
		thread_yield();
	}

	intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* condition variable의 대기자들을 우선순위로 비교하는 함수 */
static bool
compare_cond_waiter_priority (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED)
{
	const struct semaphore_elem *sema_a = list_entry (a, struct semaphore_elem, elem);
	const struct semaphore_elem *sema_b = list_entry (b, struct semaphore_elem, elem);

	struct thread *thread_a = list_entry (list_front (&sema_a->semaphore.waiters), struct thread, elem);
	struct thread *thread_b = list_entry (list_front (&sema_b->semaphore.waiters), struct thread, elem);

  return thread_a->priority > thread_b->priority;
}


/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		/* 대기자들을 우선순위 순으로 정렬하여 가장 높은 우선순위의 스레드를 깨운다. */
		list_sort(&cond->waiters, compare_cond_waiter_priority, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
