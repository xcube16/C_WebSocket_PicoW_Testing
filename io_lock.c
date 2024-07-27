
#include "sub_task.h"

#include "pico/mutex.h"

// As mutex resources are limited, just have one global mutex to protect the
// lock/unlock mechanisms from multi-core and/or interrupt speggetti.
mutex_t global_core_lock;


typedef struct io_lock_obj_t {
    // The task that is waiting to process an I/O operation
    sub_task* waiting_task;
    void* io_object;
    uint8_t locked;
} io_lock_obj;

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

/**
 * @brief Tries to aquire the lock
 *
 * @param lock
 * @return true
 * @return false
 */
bool iol_trylock(io_lock_obj* lock) {
    bool locked = false;

    //
    if (mutex_try_enter(&global_core_lock, NULL)) {
        if (!lock->locked) {
            lock->locked = locked = true;
        }
        mutex_exit(&global_core_lock);
    }

    return locked;
}

void iol_unlock(io_lock_obj* lock) {
    // We should be safe as this function only gets called when the task has ended
    // Aka, a locked context basically.
    lock->locked = false;
}
