
/*
 * CONSTANTS
 */
.equ    THINGY,        25


/*
 * ASSEMBLY HEADER
 *
 * NOTE These macros need to be on separate lines
 */
.syntax unified                     // Use ARM unified syntax, ie.
                                    // mandate S suffix for lo registers
.cpu cortex-m0plus
.thumb

.text

.global sub_task_run
.thumb_func
sub_task_run:
    // r0 sub_task* task
    // r1 size_t (*task_function)(sub_task*, void*)
    // r2 void* args  <--- shove this into -- ^
    //                     or into the sub_task_yield's return

    // save link register
    // save high registers onto current (main) stack
    push {r4-r7, lr}   /* Store the low registers.  */
    mov r4, r8         /* Store the high registers. */
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}
    // Push main resume address
    ldr	r3, =sub_task_run_resume
    push {r3}

    //       ||||| --> [|||]
    //       temp = sub_task->sp
    //       Save our current SP to sub_task->sp
    //       Set SP to temp
    ldr r3, [r0]
    mov r4, sp      // using r4 is ok because we restore high-registers anyway.
    str r4, [r0]
    mov sp, r3


    // TODO: If task_function != null
    //             Call task_function // no need to set lr
    //       else
    //             restore high-registers from stack
    //             set w0 to args
    //             pop PC from stack ---> sub_task_yield will handle it from here

    // >>>===== Task RUNS =====>>>

    sub_task_run_resume:

    // We are now in one of three situations:
    //
    // #1   'Call task_function' finished in one-shot (no yields) and we
    //      are like a normal function. Although the high-registers are
    //      restored by task_function() according to the C call spec.
    //      We can safely dispose of the high-registers stored on the
    //      current (main) stack!
    //
    // #2   'Call task_function' finished although there were yields in the past.
    //      The old (from the very first sub_task_run) high-registers are
    //      preserved according to the C call spec. These are not what we want.
    //      Restore the correct high-registers off the stack (main) that the last
    //      sub_task_run caller expects.
    //      We must restore the high-registers off the current (main) stack! 
    //
    // #3   The task called sub_task_yield. Although the correct main high-registers
    //      get restored at the end of task_function(), we are not at the end,
    //      and task_function()'s efforts are futile (see #2).
    //      sub_task_yield already takes care of storing the task high-registers
    //      to the current (task) stack.
    //      We must restore the high-registers off the current (main) stack!
    //
    //      w0 holds sub_task_yield's pre_ret_code or task_function()'s return code

    //       [|||] --> |||||
    //       temp = sub_task->sp
    //       Save our current SP to sub_task->sp
    //       Set SP to temp
    ldr r3, [r0]
    mov r4, sp      // using r4 is ok because we restore high-registers anyway.
    str r4, [r0]
    mov sp, r3

    // Restore the high-registers off the current (main) stack
    
    pop {r4-r7}     /* Load the low registers.  */
    mov r4, r8      /* Load the high registers. */
    mov r5, r9
    mov r6, r10
    mov r7, r11
    pop {r4-r7, pc} // Restore link register into PC


.global sub_task_yield
.thumb_func
sub_task_yield:
    // w0 size_t pre_ret_code
    // w1 sub_task* current_task

    // Store high-registers on current (task) stack
    push {r4-r7, lr}   /* Store the low registers.  */
    mov r4, r8         /* Store the high registers. */
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}
    // Push task resume address
    ldr	r3, =sub_task_yield_resume
    push {r3}

    // TODO: Switch to main stack
    //       [|||] --> |||||
    //       temp = sub_task->sp
    //       Save our current SP to sub_task->sp
    //       Set SP to temp

    // TODO: pop PC from stack ---> sub_task_run will handle it from here

    // >>>===== Main RUNS =====>>>

    sub_task_yield_resume:
    // Restore the high-registers off the current (task) stack
    pop {r4-r7}     /* Load the low registers.  */
    mov r4, r8      /* Load the high registers. */
    mov r5, r9
    mov r6, r10
    mov r7, r11
    pop {r4-r7, pc} // Restore link register into PC
