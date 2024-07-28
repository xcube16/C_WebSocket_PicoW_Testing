
#ifndef SUB_TASK_H
#define SUB_TASK_H

#include <stddef.h>

typedef struct sub_task_ {
    void* stack_ptr;
    char stack[];
} sub_task;

/**
 * @brief Allocates a sub-task stack on the current stack
 *
 */
#define SUB_TASK_ON_STACK(name, size) \
    u8_t _on_stack_##name[size + sizeof(sub_task)]; \
    sub_task* name = (sub_task*) _on_stack_##name; \
    name->stack_ptr = (void*) name + size + sizeof(sub_task);

/**
 * @brief Allocates a sub-task stack in global unititialized memory.
 * SUB_TASK_GLOBAL_INIT(name) must be called before it's first use.
 */
#define SUB_TASK_GLOBAL(name, size) \
    u8_t _on_bss_##name[size + sizeof(sub_task)]; \
    sub_task* name = (sub_task*) _on_bss_##name; \
     \
    static const size_t _on_bss_size_##name = size + sizeof(sub_task); // Keep track of the size for the init at runtime

#define SUB_TASK_GLOBAL_INIT(name) \
    name->stack_ptr = (void*) name + _on_bss_size_##name;


/**
 * @brief Run or resume a subtask. It's like a thread. Use sub_task_yield to pause, pre-return data,
 * and have new data when resumed.
 *
 *  TODO: How TF does the caller know when the task is done!?!??!?!?
 *        Had this crazy idea. What happens if it gets called anyway? Instead of sub_task_yield?
 *        Turns out it actually almost works! sub_task_yield's assembly was very much like sub_task_run.
 *        So I combined them and it even saves some bytes on the stack as everything resumes
 *        to the exact same location! Wow!
 *  TODO: Rename low level sub_task_run to sub_task_switch or something.
 *
 * @param task
 * @param task_function task function pointer or null for resume
 * @param args Put data in here ...
 * @return size_t
 */
size_t sub_task_run(sub_task* task, size_t (*task_function)(sub_task*, void*), void* args);

/**
 * @brief Yield to the colling procedure. Maybe we will have more data when we continue.
 * Because we are clever, we put pre_ret_code as the first arg. That way it falls
 * in the same register as a return code.
 *
 * @param pre_ret_code size_t, it could even be a void* pointer. hmmm
 * @param current_task The current task.
 * @return void* ... and the data comes out here!
 */
static inline void* sub_task_yield(size_t pre_ret_code, sub_task* current_task) {
    return (void*) sub_task_run(current_task, NULL, (void*) pre_ret_code);
}

static inline size_t sub_task_continue(sub_task* task, void* args) {
    return sub_task_run(task, NULL, args);
}

#endif