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
	 * 
	 * 인터럽트 컨텍스트에서는 직접 thread_yield()를 호출할 수 없으므로
	 * 지연된 yield를 사용한다.
	 */
	if (should_preempt()) {
		if (intr_context()) {
			/* 인터럽트 컨텍스트에서는 지연된 yield 사용 */
			intr_yield_on_return();
		} else {
			/* 일반 컨텍스트에서는 즉시 yield */
			thread_yield();
		}
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


/* 단일 우선순위 기부를 수행하는 함수
 * 
 * @param donor     우선순위를 기부하는 스레드
 * @param recipient 우선순위를 기부받는 스레드
 * 
 * donor의 우선순위가 recipient의 우선순위보다 높은 경우에만 
 * recipient의 우선순위를 donor의 우선순위로 높이고,
 * recipient가 READY 상태인 경우 ready_list의 순서를 재조정한다.
 */
static void donate_priority(struct thread *donor, struct thread *recipient) {
	if (recipient->priority < donor->priority) {
		recipient->priority = donor->priority;
		if (recipient->status == THREAD_READY) {
			list_remove(&recipient->elem);
			list_insert_ordered(&ready_list, &recipient->elem, compare_priority, NULL);
		}
	}
}

/* 우선순위 기부를 연쇄적으로 수행하는 함수
 * 
 * @param lock    현재 스레드가 획득하려는 락
 * @param current 우선순위를 기부하려는 현재 스레드
 * 
 * 현재 스레드가 획득하려는 락을 소유한 스레드부터 시작하여,
 * 해당 스레드가 대기하고 있는 다른 락의 소유자들에게까지
 * 연쇄적으로 우선순위를 기부한다.
 */
static void donate_priority_chain(struct lock *lock, struct thread *current) {
	struct thread *target = lock->holder;
	while (target) {
		donate_priority(current, target);
		if (target->wait_on_lock) {
			target = target->wait_on_lock->holder;
		} else {
			break;
		}
	}
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire(struct lock *lock) {
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	struct thread *current = thread_current();
	enum intr_level old_level = intr_disable();

	if (lock->holder != NULL) {
		current->wait_on_lock = lock;
		donate_priority_chain(lock, current);
	}
	intr_set_level(old_level);

	sema_down(&lock->semaphore);

	old_level = intr_disable();
	current->wait_on_lock = NULL;
	lock->holder = current;
	list_push_back(&current->holding_locks, &lock->lock_elem);
	intr_set_level(old_level);
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
		if (intr_context()) {
			/* 인터럽트 컨텍스트에서는 지연된 yield 사용 */
			intr_yield_on_return();
		} else {
			/* 일반 컨텍스트에서는 즉시 yield */
			thread_yield();
		}
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
