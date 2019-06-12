#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wpcmd.h"

#include "../debuggee.h"
#include "../expr.h"
#include "../interaction.h"
#include "../linkedlist.h"
#include "../printing.h"
#include "../strext.h"
#include "../watchpoint.h"

enum cmd_error_t cmdfunc_watchpoint_delete(struct cmd_args_t *args,
        int arg1, char **error){
    if(debuggee->num_watchpoints == 0){
        concat(error, "no watchpoints");
        return CMD_FAILURE;
    }

    char *ids = argcopy(args, WATCHPOINT_DELETE_COMMAND_REGEX_GROUPS[0]);

    if(!ids){
        char ans = answer("Delete all watchpoints? (y/n) ");

        if(ans == 'n'){
            WriteMessageBuffer("Nothing deleted.\n");
            return CMD_SUCCESS;
        }

        int num_deleted = debuggee->num_watchpoints;

        watchpoint_delete_all();

        WriteMessageBuffer("All watchpoint(s) removed. (%d watchpoint(s))\n", num_deleted);

        return CMD_SUCCESS;
    }

    int len = 0;
    char **all_ids = token_array(ids, " ", &len);

    free(ids);

    for(int i=0; i<len; i++){
        char *e = NULL;
        int id = (int)strtol_err(all_ids[i], &e);

        if(e){
            free(e);
            e = NULL;
        }

        watchpoint_delete(id, &e);

        if(e){
            WriteMessageBuffer("%s\n", e);
            free(e);
        }
        else{
            WriteMessageBuffer("Watchpoint %d deleted\n", id);
        }
    }

    token_array_free(all_ids, len);

    return CMD_SUCCESS;
}

enum cmd_error_t cmdfunc_watchpoint_list(struct cmd_args_t *args,
        int arg1, char **error){
    if(debuggee->num_watchpoints == 0){
        concat(error, "no watchpoints");
        return CMD_FAILURE;
    }

    WriteMessageBuffer("Current watchpoints:\n");

    for(struct node_t *current = debuggee->watchpoints->front;
            current;
            current = current->next){
        struct watchpoint *w = current->data;

        WriteMessageBuffer("%4s%d: address = %-16.16lx, hit count = %d, size = %d",
                "", w->id, w->user_location, w->hit_count, w->data_len);

        const char *type = "r";

        if(w->LSC == WP_WRITE)
            type = "w";
        else if(w->LSC == WP_READ_WRITE)
            type = "rw";

        WriteMessageBuffer(", type = %s\n", type);
    }
    
    return CMD_SUCCESS;
}

enum cmd_error_t cmdfunc_watchpoint_set(struct cmd_args_t *args, 
        int arg1, char **error){
    char *type_str = argcopy(args, WATCHPOINT_SET_COMMAND_REGEX_GROUPS[0]);
    int LSC = WP_WRITE;

    if(strcmp(type_str, "--r") == 0)
        LSC = WP_READ;
    else if(strcmp(type_str, "--w") == 0)
        LSC = WP_WRITE;
    else if(strcmp(type_str, "--rw") == 0)
        LSC = WP_READ_WRITE;

    free(type_str);

    char *location_str = argcopy(args, WATCHPOINT_SET_COMMAND_REGEX_GROUPS[1]);
    long location = eval_expr(location_str, error);

    free(location_str);

    if(*error)
        return CMD_FAILURE;

    char *data_len_str = argcopy(args, WATCHPOINT_SET_COMMAND_REGEX_GROUPS[2]);
    int data_len = (int)strtol_err(data_len_str, error);

    free(data_len_str);

    if(*error)
        return CMD_FAILURE;

    watchpoint_at_address(location, data_len, LSC, WP_ALL_THREADS, error);

    return *error ? CMD_FAILURE : CMD_SUCCESS;
}
