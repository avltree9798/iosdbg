#ifndef _DEBUGGEE_H_
#define _DEBUGGEE_H_

#include <mach/mach.h>
#include <sys/types.h>

struct debuggee {
    /* Task port for the debuggee. */
    mach_port_t task;

    /* PID of the debuggee. */
    pid_t pid;

    /* Number of pending messages received at the debuggee's exception port. */
    int pending_exceptions;

    /* Exception requests from exception_server. */
    struct queue_t *exc_requests;

    int exc_num;

    /* If this variable is non-zero, tracing is not supported. */
    int tracing_disabled;

    /* Whether or not we are currently tracing. */
    int currently_tracing;

    /* Whether execution has been suspended or not. */
    int interrupted;

    /* Keeps track of the ID of the last breakpoint that hit. */
    int last_hit_bkpt_ID;

    /* Keeps track of the location of the data in the last watchpoint hit. */
    unsigned long last_hit_wp_loc;

    /* Keeps track of where the last watchpoint hit. */
    unsigned long last_hit_wp_PC;

    /* How many breakpoints are set. */
    int num_breakpoints;

    /* How many watchpoints are set. */
    int num_watchpoints;

    /* The debuggee's name. */
    char *debuggee_name;

    /* How many hardware breakpoints the device supports. */
    int num_hw_bps;

    /* How many hardware watchpoints the device supports. */
    int num_hw_wps;

    /* Whether or not the debuggee is single stepping. */
    int is_single_stepping;

    arm_thread_state64_t task_thread_state;
    arm_debug_state64_t task_debug_state;
    arm_neon_state64_t task_neon_state;

    /* Whether or not the debuggee wants to detach. */
    int want_detach;

    /* Count of threads for the debuggee. */
    mach_msg_type_number_t thread_count;

    /* Port to get exceptions from the debuggee. */
    mach_port_t exception_port;

    struct {
        mach_msg_type_number_t count;
#define MAX_EXCEPTION_PORTS 16
        exception_mask_t masks[MAX_EXCEPTION_PORTS];
        exception_handler_t ports[MAX_EXCEPTION_PORTS];
        exception_behavior_t behaviors[MAX_EXCEPTION_PORTS];
        thread_state_flavor_t flavors[MAX_EXCEPTION_PORTS];
    } original_exception_ports;

    /* List of breakpoints on the debuggee. */
    struct linkedlist *breakpoints;

    /* List of watchpoints on the debuggee. */
    struct linkedlist *watchpoints;

    /* List of threads on the debuggee. */
    struct linkedlist *threads;

    /* The debuggee's ASLR slide. */
    unsigned long long aslr_slide;

    /* The function pointer to find the debuggee's ASLR slide. */
    unsigned long long (*find_slide)(void);

    /* The function pointer to restore original exception ports. */
    kern_return_t (*restore_exception_ports)(void);

    /* The function pointer to task_resume. */
    kern_return_t (*resume)(void);

    /* The function pointer to set up exception handling. */
    kern_return_t (*setup_exception_handling)(void);

    /* The function pointer to deallocate needed ports on detach. */
    kern_return_t (*deallocate_ports)(void);

    /* The function pointer to task_suspend. */
    kern_return_t (*suspend)(void);

    /* The function pointer to update the list of the debuggee's threads. */
    kern_return_t (*update_threads)(thread_act_port_array_t *);

    kern_return_t (*get_task_thread_state)(void);
    kern_return_t (*set_task_thread_state)(void);
    kern_return_t (*get_task_debug_state)(void);
    kern_return_t (*set_task_debug_state)(void);
    kern_return_t (*get_task_neon_state)(void);
    kern_return_t (*set_task_neon_state)(void);
};

/* This structure represents what we are currently debugging. */
extern struct debuggee *debuggee;

#endif
