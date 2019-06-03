#define PCRE2_CODE_UNIT_WIDTH 8

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pcre2.h>

#include "argparse.h"

#include "../strext.h"

struct cmd_args_t *parse_and_create_args(char *_args, 
        const char *pattern,
        const char **groupnames,
        int num_groups,
        int unk_amount_of_args,
        char **error){
    struct cmd_args_t *arguments = malloc(sizeof(struct cmd_args_t));

    arguments->argqueue = queue_new();
    arguments->num_args = 0;
    arguments->argmaps = linkedlist_new();

    if(!_args)
        return arguments;

    char *args = strdup(_args);

    PCRE2_SIZE erroroffset;
    int errornumber;

    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern,
            PCRE2_ZERO_TERMINATED,
            0,
            &errornumber,
            &erroroffset,
            NULL);

    if(!re){
        PCRE2_UCHAR buf[2048];
        pcre2_get_error_message(errornumber, buf, sizeof(buf));

        concat(error, "regex compilation failed at offset %zu: %s",
                erroroffset, buf);

        argfree(arguments);
        free(args);

        return NULL;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    size_t arglen = strlen(args);

    int rc = pcre2_match(re,
            (PCRE2_SPTR)args,
            arglen,
            0,
            0,
            match_data,
            NULL);

    if(rc < 0){
        concat(error, "malformed arguments");

        pcre2_match_data_free(match_data);
        pcre2_code_free(re);

        argfree(arguments);
        free(args);

        return NULL;
    }

    /* If we have an unknown amount of arguments,
     * the group name for those arguments will
     * be the very last thing in the groupnames array.
     * Normally we'd just loop through the groupnames array,
     * but in this case, we have to do that but exclude the
     * last group name. Then we have to keep matching for that
     * group in another loop.
     */
    int idx_limit = num_groups;

    if(unk_amount_of_args)
        idx_limit--;
    
    for(int i=0; i<idx_limit; i++){
        PCRE2_SPTR current_group = (PCRE2_SPTR)groupnames[i];
        int substr_idx = pcre2_substring_number_from_name(re,
                current_group);

        PCRE2_UCHAR *substr_buf = NULL;
        PCRE2_SIZE substr_buf_len = 0;

        if(substr_idx == PCRE2_ERROR_NOUNIQUESUBSTRING){
            int copybyname_rc = pcre2_substring_get_byname(match_data,
                    current_group,
                    &substr_buf,
                    &substr_buf_len);

            //if(copybyname_rc < 0)
           //     continue;
        }
        else{
            int substr_rc = pcre2_substring_get_bynumber(match_data,
                    substr_idx,
                    &substr_buf,
                    &substr_buf_len);

            /* It is fine if this substring wasn't found. */
            //if(substr_rc < 0)
            //    continue;
        }

        printf("%s: not unlimited: current group: '%s', arg for it is '%s'\n",
                __func__, current_group, substr_buf ? substr_buf : "NULL");

        char *argument = NULL;

        if(substr_buf){
            arguments->num_args++;

            argument = strdup((char *)substr_buf);
        }

        argins(arguments, (const char *)current_group, argument);
        

        if(substr_buf){
            char *arg = strdup((char *)substr_buf);

            if(strlen(arg) > 0){
                //arguments->num_args++;
                enqueue(arguments->argqueue, arg);
            }
            else{
                free(arg);
            }
        }
    }

    if(!unk_amount_of_args){
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);

        free(args);

        return arguments;
    }

    /* Now we have to match the rest of the arguments. */
    for(;;){
        PCRE2_SPTR current_group = (PCRE2_SPTR)groupnames[idx_limit];
        int substr_idx = pcre2_substring_number_from_name(re,
                current_group);
        
        if(substr_idx < 0)
            break;

        PCRE2_UCHAR *substr_buf;
        PCRE2_SIZE substr_buf_len;

        if(substr_idx == PCRE2_ERROR_NOUNIQUESUBSTRING){
            int copybyname_rc = pcre2_substring_get_byname(match_data,
                    current_group,
                    &substr_buf,
                    &substr_buf_len);

            /* If there were no more matches for this group, we're done. */
            if(copybyname_rc < 0)
                break;
        }
        else{
            int substr_rc = pcre2_substring_get_bynumber(match_data,
                    substr_idx,
                    &substr_buf,
                    &substr_buf_len);

            if(substr_rc < 0)
                break;
        }

        printf("%s: unlimited: current group: '%s', arg for it is '%s'\n",
                __func__, current_group, substr_buf ? substr_buf : "NULL");

        char *argument = NULL;

        if(substr_buf){
            arguments->num_args++;
            argument = strdup((char *)substr_buf);
        }

        argins(arguments, (const char *)current_group, argument);

        char *arg = strdup((char *)substr_buf);

        if(strlen(arg) > 0){
            arguments->num_args++;
            enqueue(arguments->argqueue, arg);
        }
        else{
            free(arg);
        }

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        PCRE2_SIZE start_offset = ovector[1];

        rc = pcre2_match(re,
                (PCRE2_SPTR)args,
                arglen,
                start_offset,
                0,
                match_data,
                NULL);
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);

    free(args);

    return arguments;
}

void argins(struct cmd_args_t *args, const char *arggroup, char *argval){
    if(!args || !arggroup)
        return;

    for(struct node_t *current = args->argmaps->front;
            current;
            current = current->next){
        struct argmap *map = current->data;

        if(strcmp(map->arggroup, arggroup) == 0){
            printf("%s: existing argmap: enqueueing '%s' for group '%s'\n",
                    __func__, argval ? argval : "NULL", arggroup);
            enqueue(map->argvals, argval);
            return;
        }
    }

    struct argmap *map = malloc(sizeof(struct argmap));
    map->arggroup = strdup(arggroup);
    map->argvals = queue_new();

    if(argval)
        enqueue(map->argvals, argval);
    else
        enqueue(map->argvals, NULL);
    printf("%s: new argmap: creating linkedlist: adding '%s' for group '%s'\n",
            __func__, argval ? argval : "NULL", arggroup);

    linkedlist_add(args->argmaps, map);
}

char *argcopy(struct cmd_args_t *args, const char *group){
    if(!args || !group)
        return NULL;

    if(!args->argmaps)
        return NULL;

    for(struct node_t *current = args->argmaps->front;
            current;
            current = current->next){
        struct argmap *map = current->data;

        if(map->arggroup && strcmp(map->arggroup, group) == 0)
            return dequeue(map->argvals);
    }

    return NULL;
}

char *argnext(struct cmd_args_t *args){
    if(!args)
        return NULL;

    if(!args->argqueue)
        return NULL;

    return dequeue(args->argqueue);
}

void argfree(struct cmd_args_t *args){
    if(!args)
        return;

    char *arg = argnext(args);

    while(arg){
        free(arg);
        arg = argnext(args);
    }

    queue_free(args->argqueue);
    
    for(struct node_t *current = args->argmaps->front;
            current;
            current = current->next){
        struct argmap *map = current->data;
        free(map->arggroup);

        char *arg = dequeue(map->argvals);

        while(arg){
            free(arg);
            arg = dequeue(map->argvals);
        }

        linkedlist_delete(args->argmaps, map);
    }

    linkedlist_free(args->argmaps);
    free(args);
}
