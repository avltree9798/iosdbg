#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "breakpoint.h"
#include "convvar.h"
#include "dbgops.h"
#include "debuggee.h"
#include "exception.h"
#include "linkedlist.h"
#include "memutils.h"
#include "printutils.h"
#include "ptrace.h"
#include "queue.h"
#include "sigsupport.h"
#include "strext.h"
#include "thread.h"
#include "trace.h"
#include "watchpoint.h"

void ops_printsiginfo(void){
    printf("%-11s %-5s %-5s %-6s\n", "NAME", "PASS", "STOP", "NOTIFY");
    printf("=========== ===== ===== ======\n");

    int signo = 0;

    while(signo++ < (NSIG - 1)){
        int notify, pass, stop;
        char *e = NULL;

        sigsettings(signo, &notify, &pass, &stop, 0, &e);

        if(e){
            printf("error: %s\n", e);
            free(e);
        }

        char *sigstr = strdup(sys_signame[signo]);
        size_t sigstrlen = strlen(sigstr);

        for(int i=0; i<sigstrlen; i++)
            sigstr[i] = toupper(sigstr[i]);
        
        char *fullsig = NULL;
        concat(&fullsig, "SIG%s", sigstr);
        free(sigstr);

        const char *notify_str = notify ? "true" : "false";
        const char *pass_str = pass ? "true" : "false";
        const char *stop_str = stop ? "true" : "false";

        printf("%-11s %-5s %-5s %-6s\n",
                fullsig, pass_str, stop_str, notify_str);

        free(fullsig);
    }
}

void ops_detach(int from_death){
    pthread_mutex_lock(&HAS_REPLIED_MUTEX);

    debuggee->want_detach = 1;

    void_convvar("$_");
    void_convvar("$__");

    breakpoint_delete_all();
    watchpoint_delete_all();

    /* Disable hardware single stepping. */
    for(struct node_t *current = debuggee->threads->front;
            current;
            current = current->next){
        struct machthread *t = current->data;

        get_debug_state(t);
        t->debug_state.__mdscr_el1 = 0;
        set_debug_state(t);
    }

    /* Reply to any exceptions. */
    while(debuggee->pending_exceptions > 0){
        void *request = dequeue(debuggee->exc_requests);
        reply_to_exception(request, KERN_SUCCESS);
    }

    /* Send SIGSTOP to set debuggee's process status to
     * SSTOP so we can detach. Calling ptrace with PT_THUPDATE
     * to handle Unix signals sets this status to SRUN, and ptrace 
     * bails if this status is SRUN. See bsd/kern/mach_process.c
     */
    if(!from_death){
        kill(debuggee->pid, SIGSTOP);
        ptrace(PT_DETACH, debuggee->pid, 0, 0);
        kill(debuggee->pid, SIGCONT);
    }

    debuggee->deallocate_ports();
    debuggee->restore_exception_ports();

    debuggee->resume();
    debuggee->interrupted = 0;

    queue_free(debuggee->exc_requests);
    debuggee->exc_requests = NULL;

    linkedlist_free(debuggee->breakpoints);
    debuggee->breakpoints = NULL;

    linkedlist_free(debuggee->threads);
    debuggee->threads = NULL;
    
    linkedlist_free(debuggee->watchpoints);
    debuggee->watchpoints = NULL;

    debuggee->breakpoints = NULL;
    debuggee->watchpoints = NULL;
    debuggee->threads = NULL;

    debuggee->interrupted = 0;
    debuggee->last_hit_bkpt_ID = 0;
    debuggee->last_hit_wp_loc = 0;
    debuggee->last_hit_wp_PC = 0;
    debuggee->num_breakpoints = 0;
    debuggee->num_watchpoints = 0;
    debuggee->num_watchpoints = 0;
    debuggee->pid = -1;

    free(debuggee->debuggee_name);
    debuggee->debuggee_name = NULL;

    void_convvar("$ASLR");

    debuggee->want_detach = 0;
    pthread_mutex_unlock(&HAS_REPLIED_MUTEX);
}

void ops_resume(void){
    pthread_mutex_lock(&HAS_REPLIED_MUTEX);

    void *request = dequeue(debuggee->exc_requests);

    if(request){
        reply_to_exception(request, KERN_SUCCESS);
        HAS_REPLIED_TO_LATEST_EXCEPTION = 1;
    }

    /* Wake up the exception thread. */
    pthread_cond_signal(&MAIN_THREAD_CHANGED_REPLIED_VAR_COND);

    debuggee->resume();
    debuggee->interrupted = 0;

    pthread_mutex_unlock(&HAS_REPLIED_MUTEX);
}

void ops_threadupdate(void){
    thread_act_port_array_t threads;
    debuggee->update_threads(&threads);

    machthread_updatethreads(threads);

    struct machthread *focused = machthread_getfocused();

    if(!focused){
        printf("[Previously selected thread dead, selecting thread #1]\n\n");
        machthread_setfocused(threads[0]);
        focused = machthread_getfocused();
    }

    if(focused)
        machthread_updatestate(focused);
}
