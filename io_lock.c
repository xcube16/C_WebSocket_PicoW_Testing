
#include "sub_task.h"

#include "pico/mutex.h"


#define IOL_YIELD_REASON_END 0

// As mutex resources are limited, just have one global mutex to protect the
// lock/unlock mechanisms from multi-core and/or interrupt speggetti.
mutex_t global_core_lock;


typedef struct iol_lock_obj_t {
    // The task that is waiting to process an I/O operation
    sub_task* waiting_task;

    bool (*check_reason)(size_t reason);

    // What is the task waiting for?
    void* waiting_reason;

    // Protect the task from being continued while it is already running
    // This would result in upside-down-world! Don't go to upside-down-world;
    // don't continue an already running task.
    bool locked;
} iol_lock_obj;

/**
 * @brief Just an ugly global init function for global variables of horable globalness
 *
 * @return int non zero if stuff didn't work
 */
int iol_init() {
    // no mutex_free function exists yet. Kinda odd, but o well.
    mutex_init(&global_core_lock);
    return 0;
}

void iol_unlock(iol_lock_obj* lock) {
    // We should be safe as this function only gets called when the task has ended
    // Aka, a locked context basically.
    lock->locked = false;
}

bool iol_trylock(iol_lock_obj* lock) {
    bool locked = false;
    if (mutex_try_enter(&global_core_lock, NULL)) {
        if (!lock->locked) {
            lock->locked = locked = true;
        }
        mutex_exit(&global_core_lock);
    }
    return locked;
}

void iol_continue(iol_lock_obj* lock) {
    while (true) {

        if (!iol_trylock(lock)) {
            // did not aquire the lock. The task is still running and
            // will do a reason check by its self when its ready.
            break;
        }

        lock->waiting_reason = sub_task_continue(lock->waiting_task, 0/*no error*/);

        iol_unlock(lock);

        // Interrupts and other cores can now trigger notifications again.
        // Check and handle any we might have missed while locked.

        if (!lock->check_reason(lock->waiting_reason)) {
            // Still waiting for a valid reason to resume or the task has ended.
            break;
        }
    }
}

/**
 * @brief Tries to continue a task if it was waiting for the specified reason to continue.
 *
 * @param lock
 * @param reason The task might be waiting for this reason
 * @return true
 * @return false
 */
int iol_notify(iol_lock_obj* lock, size_t reason) {
    if (lock->waiting_reason == reason) {
        return 1;
    }

    iol_continue(lock);
}

/**
 * @brief Initialize the lock object and run the task with args
 *
 * @param lock
 * @return int
 */
int iol_task_run(iol_lock_obj* lock, bool (*check_reason)(size_t reason), sub_task* task, void* (*task_function)(sub_task*, void*), void* args) {
    lock->waiting_task = task;
    lock->check_reason = check_reason;
    lock->waiting_reason = 0;
    lock->locked = false;

    if (!iol_trylock(lock)) {
        // This should not happen. Something is very wrong.
        return 1;
    }

    lock->waiting_reason = sub_task_run(task, task_function, args);

    iol_unlock(lock);

    // Interrupts and other cores can now trigger notifications again.
    // Check and handle any we might have missed while locked.

    if (lock->check_reason(lock->waiting_reason)) {
        // An interrupt must have came in just as the task was finishing.
        iol_continue(lock);
    }

    return 0;
}
