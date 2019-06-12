#include <ctype.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>

#include "breakpoint.h"
#include "dbgops.h"
#include "debuggee.h"
#include "exception.h"
#include "linkedlist.h"
#include "memutils.h"
#include "ptrace.h"
#include "sigsupport.h"
#include "strext.h"
#include "thread.h"
#include "trace.h"
#include "watchpoint.h"

#include "cmd/misccmd.h"

static const char *exc_str(exception_type_t exception){
    switch(exception){
        case EXC_BAD_ACCESS:
            return "EXC_BAD_ACCESS";
        case EXC_BAD_INSTRUCTION:
            return "EXC_BAD_INSTRUCTION";
        case EXC_ARITHMETIC:
            return "EXC_ARITHMETIC";
        case EXC_EMULATION:
            return "EXC_EMULATION";
        case EXC_SOFTWARE:
            return "EXC_SOFTWARE";
        case EXC_BREAKPOINT:
            return "EXC_BREAKPOINT";
        case EXC_SYSCALL:
            return "EXC_SYSCALL";
        case EXC_MACH_SYSCALL:
            return "EXC_MACH_SYSCALL";
        case EXC_RPC_ALERT:
            return "EXC_RPC_ALERT";
        case EXC_CRASH:
            return "EXC_CRASH";
        case EXC_RESOURCE:
            return "EXC_RESOURCE";
        case EXC_GUARD:
            return "EXC_GUARD";
        case EXC_CORPSE_NOTIFY:
            return "EXC_CORPSE_NOTIFY";
        default:
            return "<Unknown Exception>";
    }
}

static void set_single_step(struct machthread *t, int enabled){
    //printf("%s: setting single step for thread %d, enabled? %d\n",
      //      __func__, t->ID, enabled);
    get_debug_state(t);

    if(enabled)
        t->debug_state.__mdscr_el1 |= 1;
    else
        t->debug_state.__mdscr_el1 = 0;

    set_debug_state(t);
}

static void describe_hit_watchpoint(void *prev_data, void *cur_data,
        unsigned int sz, char **desc){
    long old_val = *(long *)prev_data;
    long new_val = *(long *)cur_data;

    if(sz == sizeof(char)){
        concat(desc, "Old value: %s%#x\nNew value: %s%#x\n\n", 
                (char)old_val < 0 ? "-" : "", 
                (char)old_val < 0 ? (char)-old_val : (char)old_val, 
                (char)new_val < 0 ? "-" : "", 
                (char)new_val < 0 ? (char)-new_val : (char)new_val);
    }
    else if(sz == sizeof(short)){
        concat(desc, "Old value: %s%#x\nNew value: %s%#x\n\n", 
                (short)old_val < 0 ? "-" : "", 
                (short)old_val < 0 ? (short)-old_val : (short)old_val, 
                (short)new_val < 0 ? "-" : "", 
                (short)new_val < 0 ? (short)-new_val : (short)new_val);

    }
    else if(sz == sizeof(int)){
        concat(desc, "Old value: %s%#x\nNew value: %s%#x\n\n", 
                (int)old_val < 0 ? "-" : "", 
                (int)old_val < 0 ? (int)-old_val : (int)old_val, 
                (int)new_val < 0 ? "-" : "", 
                (int)new_val < 0 ? (int)-new_val : (int)new_val);
    }
    else{
        concat(desc, "Old value: %s%#lx\nNew value: %s%#lx\n\n", 
                (long)old_val < 0 ? "-" : "", 
                (long)old_val < 0 ? (long)-old_val : (long)old_val, 
                (long)new_val < 0 ? "-" : "", 
                (long)new_val < 0 ? (long)-new_val : (long)new_val);
    }
}

static void handle_soft_signal(mach_port_t thread, long subcode, char **desc,
        int notify, int pass, int stop){
    char *sigstr = strdup(sys_signame[subcode]);
    size_t sigstrlen = strlen(sigstr);

    for(int i=0; i<sigstrlen; i++)
        sigstr[i] = toupper(sigstr[i]);

    concat(desc, "%ld, SIG%s. ", subcode, sigstr);

    free(sigstr);

    /* If we're passing signals, don't clear them. */
    if(pass)
        return;

    ptrace(PT_THUPDATE, debuggee->pid, (caddr_t)(unsigned long long)thread, 0);
}

// XXX remove later
static void resume_after_exception(void){
    //ops_resume();
    if(debuggee->currently_tracing){
        rl_already_prompted = 1;
        putchar('\n');
    }
}

static void handle_hit_watchpoint(struct machthread *t, char **desc){
    struct watchpoint *hit = find_wp_with_address(debuggee->last_hit_wp_loc);

    watchpoint_hit(hit);

    if(!hit){
        resume_after_exception();
        return;
    }

    unsigned int sz = hit->data_len;

    /* Save previous data for comparison. */
    void *prev_data = malloc(sz);
    memcpy(prev_data, hit->data, sz);

    read_memory_at_location((void *)hit->user_location, hit->data, sz);
    
    concat(desc, ": '%s': watchpoint %d at %#lx hit %d time(s).\n\n",
            t->tname, hit->id, hit->user_location, hit->hit_count);

    describe_hit_watchpoint(prev_data, hit->data, sz, desc);
    disassemble_at_location(debuggee->last_hit_wp_PC + 4, 4, desc);

    free(prev_data);
    
    debuggee->last_hit_wp_loc = 0;
    debuggee->last_hit_wp_PC = 0;
}

static void handle_single_step(struct machthread *t, int *should_auto_resume,
        int *should_print, char **desc){
    /* Re-enable all the breakpoints we disabled while performing the
     * single step. This function is called when the CPU raises the software
     * step exception after the single step occurs.
     */
    if(debuggee->is_single_stepping)
        breakpoint_enable_all();

    if(t->just_hit_breakpoint){
        if(t->just_hit_sw_breakpoint){
            breakpoint_enable(debuggee->last_hit_bkpt_ID, NULL);
            t->just_hit_sw_breakpoint = 0;
        }

        get_thread_state(t);

        /* If we caused a software step exception to get past a breakpoint,
         * just continue as normal. Otherwise, if we manually single step
         * right after a breakpoint hit, just print the disassembly.
         */
        if(!debuggee->is_single_stepping){
            *should_print = 0;
            // XXX should not print, should auto resume
            resume_after_exception();
        }
        else{
            // XXX should print, should not auto resume
            *should_auto_resume = 0;
            disassemble_at_location(t->thread_state.__pc, 4, desc);
        }

        t->just_hit_breakpoint = 0;

        debuggee->is_single_stepping = 0;

        return;
    }

   // putchar('\n');
    concat(desc, "\n");
    disassemble_at_location(t->thread_state.__pc, 4, desc);
    set_single_step(t, 0);

    debuggee->is_single_stepping = 0;

    // XXX should print, should not auto resume
    *should_auto_resume = 0;
}

static void handle_hit_breakpoint(struct machthread *t,
        long subcode, char **desc){
    struct breakpoint *hit = find_bp_with_address(subcode);

    /* This could be possible. If the user deletes all breakpoints before
     * we have a chance to handle an exception related to a breakpoint,
     * we'll end up with hit being NULL.
     */
    if(!hit)
        return;

    breakpoint_hit(hit);

    concat(desc, " breakpoint %d at %#lx hit %d time(s).\n",
            hit->id, hit->location, hit->hit_count);

    if(!hit->hw){
        t->just_hit_sw_breakpoint = 1;
        breakpoint_disable(hit->id, NULL);
    }

    debuggee->last_hit_bkpt_ID = hit->id;
}

void handle_exception(Request *request, int *should_auto_resume,
        int *should_print, char **desc){
    /* Finish printing everything while tracing so
     * we don't get caught in the middle of it.
     */
    wait_for_trace();

    mach_port_t task = request->task.name;
    mach_port_t thread = request->thread.name;
    exception_type_t exception = request->exception;
    const char *exc = exc_str(exception);
    long code = ((long *)request->code)[0];
    long subcode = ((long *)request->code)[1];

    //printf("%s: exc '%s', code %#lx, subcode %#lx\n", __func__, exc, code, subcode);

    /* Give focus to whatever caused this exception. */
    struct machthread *focused = machthread_getfocused();

    if(!focused || focused->port != thread){
        machthread_setfocused(thread);
        focused = machthread_getfocused();
    }

    get_thread_state(focused);

    concat(desc, "\n * Thread #%d (tid = %#llx)", focused->ID, focused->tid);

    /* A number of things could have happened to cause an exception:
     *      - hardware breakpoint
     *      - hardware watchpoint
     *      - software breakpoint
     *      - software single step exception
     *      - Unix soft signal
     */
    /* Unix soft signal. */
    if(exception == EXC_SOFTWARE && code == EXC_SOFT_SIGNAL){
        int notify, pass, stop;
        char *error = NULL;

        sigsettings(subcode, &notify, &pass, &stop, 0, &error);

        if(error)
            free(error);
        
        concat(desc, ", '%s' received signal ", focused->tname);
        handle_soft_signal(focused->port, subcode, desc, notify, pass, stop);
        
        if(stop){
            // XXX should print, should not auto resume
            *should_auto_resume = 0;
            
            concat(desc, "%#llx in debuggee.", focused->thread_state.__pc);
        }
        else{
            concat(desc, "Resuming execution.");

            // XXX should print, should auto resume

            /*pthread_mutex_unlock(&HAS_REPLIED_MUTEX);
            ops_resume();
            pthread_mutex_lock(&HAS_REPLIED_MUTEX);
            */
        }
        
        /* Don't print any of this if we're detaching. */
        if(notify && !debuggee->want_detach){
            //printf("%s", desc);

            concat(desc, "\n");

            if(stop){
                disassemble_at_location(focused->thread_state.__pc, 4, desc);
            }

            // XXX should print, should not auto resume
            *should_auto_resume = 0;
        }
        else if(!notify){
            // XXX should not print, should not auto resume
            *should_auto_resume = 0;
            *should_print = 0;
        }

        //free(desc);
    }
    /* A hardware watchpoint hit. However, we need to single step in 
     * order for the CPU to execute the instruction at this address
     * so the value actually changes.
     */
    else if(code == EXC_ARM_DA_DEBUG){
        focused->just_hit_watchpoint = 1;

        debuggee->last_hit_wp_loc = subcode;
        debuggee->last_hit_wp_PC = focused->thread_state.__pc;

        set_single_step(focused, 1);
        
        /* Continue execution so the software step exception occurs. */
        resume_after_exception();
        
        // XXX should not print, should auto resume
        *should_print = 0;
        

        //free(desc);
    }
    /* A hardware/software breakpoint hit, or the software step
     * exception has occured.
     */
    else if(exception == EXC_BREAKPOINT && code == EXC_ARM_BREAKPOINT){
        if(subcode == 0){
            if(focused->just_hit_watchpoint){
                handle_hit_watchpoint(focused, desc);
                focused->just_hit_watchpoint = 0;

                //free(desc);

                // XXX should print, should not auto resume
                *should_auto_resume = 0;

                return;
            }

            /* If we single step over where a breakpoint is set,
             * we should report it and count it as hit.
             */ 
            struct breakpoint *hit = find_bp_with_address(
                    focused->thread_state.__pc);

            if(debuggee->is_single_stepping && hit){
                breakpoint_hit(hit);

                concat(desc, ": '%s': breakpoint %d at %#lx hit %d time(s).\n",
                        focused->tname, hit->id, hit->location, hit->hit_count);

                //printf("%s", desc);
            }
            else{
                concat(desc, ": '%s': single step.\n", focused->tname);
            }
    
            handle_single_step(focused, should_auto_resume, should_print, desc);
            //free(desc);

            // XXX should print, should not auto resume
            //*should_auto_resume = 0; XXX handled in handle_single_step

            return;
        }
        
        focused->just_hit_breakpoint = 1;

        concat(desc, ": '%s':", focused->tname);
        handle_hit_breakpoint(focused, subcode, desc);

        //printf("%s", desc);

        //free(desc);

        disassemble_at_location(focused->thread_state.__pc, 4, desc);
        set_single_step(focused, 1);

        // XXX should print, should not auto resume
        *should_auto_resume = 0;
    }
}

void reply_to_exception(Request *req, kern_return_t retcode){
    Reply reply;
    
    mach_msg_header_t *rpl_head = &reply.Head;

    /* This is from mach_excServer.c. */
    rpl_head->msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(
                req->Head.msgh_bits), 0);
    rpl_head->msgh_remote_port = req->Head.msgh_remote_port;
    rpl_head->msgh_size = (mach_msg_size_t)sizeof(mig_reply_error_t);
    rpl_head->msgh_local_port = MACH_PORT_NULL;
    rpl_head->msgh_id = req->Head.msgh_id + 100;
    rpl_head->msgh_reserved = 0;

    reply.NDR = req->NDR;
    reply.RetCode = retcode;

    mach_msg(&reply.Head,
            MACH_SEND_MSG,
            reply.Head.msgh_size,
            0,
            MACH_PORT_NULL,
            MACH_MSG_TIMEOUT_NONE,
            MACH_PORT_NULL);

    debuggee->pending_exceptions--;
}
