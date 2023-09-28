
#ifndef SUB_TASK_H
#define SUB_TASK_H

#include <stddef.h>

struct sub_task {
    void* stack_ptr;
    char stack[];
};

//#define IM_A_STACK_ON_THE_STACK(size,)

/**
 * @brief Run or resume a subtask. It's like a thread. Use sub_task_yield to pause, pre-return data,
 * and have new data when resumed.
 * 
 *  TODO: How TF does the caller know when the task is done!?!??!?!?
 * 
 * @param task 
 * @param task_function task function pointer or null for resume
 * @param args Put data in here ...
 * @return size_t
 */
size_t sub_task_run(struct sub_task* task, size_t (*task_function)(struct sub_task*, void*), void* args);

/**
 * @brief Yield to the colling procedure. Maybe we will have more data when we continue.
 * Because we are clever, we put pre_ret_code as the first arg. That way it falls
 * in the same register as a return code.
 * 
 * @param pre_ret_code size_t, it could even be a void* pointer. hmmm
 * @param current_task The current task.
 * @return void* ... and the data comes out here!
 */
void* sub_task_yield(size_t pre_ret_code, struct sub_task* current_task);

#endif