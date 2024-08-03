
/*
 * RP2040 (Cortex M0+) Sub-task assembly functions. 'Now you are thinking with context switching!'
 */

.syntax unified       // ARM unified syntax, ie.

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

    // |Main| --> [Task]    (cases #1 through #1)
    // [Task] --> |Main|    (case  #4)
    ldr r3, [r0]    // r3 = sub_task->sp
    mov r4, sp      // using r4 is ok because we restore high-registers anyway.
    str r4, [r0]
    mov sp, r3

    cmp r1, #0
    beq sub_task_run_task_null
    // If task_function != null
                    mov r5, r0   // Save sub_stask* for after the task_function returns
                    mov r3, r1
                    mov r1, r2   // ready args
                    blx r3       // Call task_function!

                    // >>>===== Task RUNS =====>>>

                    // task_function returned
                    // TODO: Test this trap! (happens incase sub_task_continue is called)!
                    // FIXME: This trap causes the stack to leak 4 bytes every time it is re-used
                    //        with a new task. It's nice to have a trap, but maybe find a more global
                    //        way to put it there.
                    ldr	r3, =sub_task_trap
                    push {r3}

                    // [Task] --> |Main|
                    ldr r3, [r5]    // r3 = sub_task->sp
                    mov r4, sp      // using r4 is ok because we restore high-registers anyway.
                    str r4, [r5]
                    mov sp, r3

                    b sub_task_run_resume

    // else
    sub_task_run_task_null:
                    // Resume Task from yield!

                    mov r0, r2  // ready args

                    // Wow! What a coincidence! We are right above sub_task_run_resume
                    // so we didn't need to push and now pop sub_task_run_resume

                    // >>>===== Task RUNS =====>>>

    sub_task_run_resume:

    // We are now in one of four situations:
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
    //
    // #4   As above, so below. Unlike the other three cases, the *task* is resuming
    //      from yield. Not |main|. // w0 holds void* args

    // Restore the high-registers off the current (main) stack

    pop {r4-r7}     /*  Pop the high registers.  */
    mov r8, r4      /* Load the high registers. */
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc} // Pop/Load low registers and Pop/Load link register into PC

.global sub_task_trap
.thumb_func
sub_task_trap:
    pop {pc} // last time I did this, it caused a hard fault, so maybe it will here.
