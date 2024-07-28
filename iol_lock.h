
#ifndef IOL_LOCK_H
#define IOL_LOCK_H

#include <stdbool.h>
#include "sub_task.h"

#define IOL_YIELD_REASON_END 0

typedef struct iol_lock_obj_t {
    // The task that is waiting to process an I/O operation
    sub_task* waiting_task;

    bool (*check_reason)(size_t reason);

    // What is the task waiting for?
    size_t waiting_reason;

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
int iol_init();

/**
 * @brief Tries to continue a task if it was waiting for the specified reason to continue.
 *
 * @param lock
 * @param reason The task might be waiting for this reason
 * @return true
 * @return false
 */
int iol_notify(iol_lock_obj* lock, size_t reason);

/**
 * @brief Initialize the lock object and run the task with args
 *
 * @param lock
 * @return int
 */
int iol_task_run(iol_lock_obj* lock, bool (*check_reason)(size_t reason), sub_task* task, size_t (*task_function)(sub_task*, void*), void* args);

#endif