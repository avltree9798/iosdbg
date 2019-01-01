/*
Implementation for every command.
*/

#include "dbgcmd.h"

cmd_error_t cmdfunc_attach(const char *args, int arg1){
	if(debuggee->pid != -1)
		return CMD_FAILURE;

	if(!args){
		cmdfunc_help("attach", 0);
		return CMD_FAILURE;
	}

	pid_t pid = pid_of_program((char *)args);

	if(pid == -1)
		return CMD_FAILURE;

	kern_return_t err = task_for_pid(mach_task_self(), pid, &debuggee->task);

	if(err){
		printf("attach: couldn't get task port for pid %d: %s\n", pid, mach_error_string(err));
		return CMD_FAILURE;
	}

	thread_act_port_array_t threads;
	debuggee->update_threads(&threads);
	
	// update PC
	arm_thread_state64_t thread_state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	err = thread_get_state(threads[0], ARM_THREAD_STATE64, (thread_state_t)&thread_state, &count);
	
	debuggee->PC = thread_state.__pc;

	err = debuggee->suspend();

	if(err){
		printf("attach: task_suspend call failed: %s\n", mach_error_string(err));
		return CMD_FAILURE;
	}
	
	debuggee->pid = pid;
	debuggee->interrupted = 1;
	
	debuggee->aslr_slide = debuggee->find_slide();
	
	printf("Attached to %d, slide: %#llx.\n", debuggee->pid, debuggee->aslr_slide);

	debuggee->breakpoints = linkedlist_new();
	debuggee->threads = linkedlist_new();

	setup_exceptions();

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_aslr(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	printf("%#llx\n", debuggee->aslr_slide);
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_backtrace(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;
	
	// get FP register
	arm_thread_state64_t thread_state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

	struct machthread *focused = machthread_getfocused();
	
	if(!focused){
		printf("We are not focused on any thread.\n");
		return CMD_FAILURE;
	}

	kern_return_t err = thread_get_state(focused->port, ARM_THREAD_STATE64, (thread_state_t)&thread_state, &count);

	if(err){
		printf("cmdfunc_backtrace: thread_get_state failed: %s\n", mach_error_string(err));
		return CMD_FAILURE;
	}

	// print frame 0, which is where we are currently at
	printf("  * frame #0: %#llx\n", thread_state.__pc);
	
	// frame 1 is what is in LR
	printf("     frame #1: %#llx\n", thread_state.__lr);

	int frame_counter = 2;

	// there's a linked list-like thing of frame pointers
	// so we can unwind the stack by following this linked list
	struct frame_t {
		struct frame_t *next;
		unsigned long long frame;
	};

	struct frame_t *current_frame = malloc(sizeof(struct frame_t));
	err = memutils_read_memory_at_location((void *)thread_state.__fp, current_frame, sizeof(struct frame_t));
	
	if(err){
		printf("Backtrace failed\n");
		return CMD_FAILURE;
	}

	while(current_frame->next){
		printf("     frame #%d: %#llx\n", frame_counter, current_frame->frame);

		memutils_read_memory_at_location((void *)current_frame->next, (void *)current_frame, sizeof(struct frame_t));	
		frame_counter++;
	}

	printf(" - cannot unwind past frame %d -\n", frame_counter);

	free(current_frame);

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_break(const char *args, int arg1){
	if(!args){
		printf("Location?\n");
		return CMD_FAILURE;
	}

	if(debuggee->pid == -1)
		return CMD_FAILURE;

	char *tok = strtok((char *)args, " ");

	while(tok){
		unsigned long long location = strtoul(tok, NULL, 16);
		breakpoint_at_address(location);

		tok = strtok(NULL, " ");
	}
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_continue(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	if(!debuggee->interrupted)
		return CMD_FAILURE;

	kern_return_t err = debuggee->resume();

	if(err){
		printf("resume: couldn't continue: %s\n", mach_error_string(err));
		return CMD_FAILURE;
	}

	debuggee->interrupted = 0;
	breakpoint_enable_all();

	rl_printf(RL_NO_REPROMPT, "Continuing.\n");

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_delete(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;
	
	if(!args){
		printf("We need a breakpoint ID\n");
		return CMD_FAILURE;
	}
	
	bp_error_t error = breakpoint_delete(atoi(args));

	if(error == BP_FAILURE){
		printf("Couldn't delete breakpoint\n");
		return CMD_FAILURE;
	}

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_detach(const char *args, int from_death){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	// delete all breakpoints on detach so the original instruction is written back to prevent a crash
	// TODO: instead of deleting them, maybe disable all of them and if we are attached to the same thing again re-enable them?
	breakpoint_delete_all();

	if(!from_death){
		debuggee->restore_exception_ports();

		if(debuggee->interrupted){
			cmd_error_t result = cmdfunc_continue(NULL, 0);

			if(result != CMD_SUCCESS){
				printf("detach: couldn't resume execution before we detach?\n");
				return CMD_FAILURE;
			}
		}
	}

	debuggee->interrupted = 0;

	linkedlist_free(debuggee->breakpoints);
	debuggee->breakpoints = NULL;

	linkedlist_free(debuggee->threads);
	debuggee->threads = NULL;

	printf("Detached from %d\n", debuggee->pid);

	debuggee->pid = -1;

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_disassemble(const char *args, int arg1){
	if(!args){
		cmdfunc_help("disassemble", 0);
		return CMD_FAILURE;
	}

	char *tok = strtok((char *)args, " ");
	
	if(!tok){
		cmdfunc_help("disassemble", 0);
		return CMD_FAILURE;
	}

	// first thing is the location
	char *loc_str = malloc(strlen(tok) + 1);
	strcpy(loc_str, tok);

	unsigned long long location = strtoull(loc_str, NULL, 16);

	free(loc_str);

	// then the amount of instructions to disassemble
	tok = strtok(NULL, " ");

	if(!tok){
		cmdfunc_help("disassemble", 0);
		return CMD_FAILURE;
	}

	int base = 10;

	if(strstr(tok, "0x"))
		base = 16;
	
	int amount = strtol(tok, NULL, base);

	if(amount <= 0){
		cmdfunc_help("disassemble", 0);
		return CMD_FAILURE;
	}

	// finally, whether or not '--no-aslr' was given
	tok = strtok(NULL, " ");

	// if it is NULL, nothing is there, so add ASLR to the location
	if(!tok)
		location += debuggee->aslr_slide;

	kern_return_t err = memutils_disassemble_at_location(location, amount);
	
	if(err){
		printf("Couldn't disassemble\n");
		return CMD_FAILURE;
	}
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_examine(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	if(!args){
		cmdfunc_help("examine", 0);
		return CMD_FAILURE;
	}

	// x <location> <amount> {--no-aslr}
	
	char *tok = strtok((char *)args, " ");
	
	if(!tok){
		cmdfunc_help("examine", 0);
		return CMD_FAILURE;
	}

	// first thing will be the location
	char *loc_str = malloc(strlen(tok) + 1);
	strcpy(loc_str, tok);
	
	int base = 10;

	if(strstr(loc_str, "0x"))
		base = 16;

	unsigned long long location = strtoull(loc_str, NULL, base);

	free(loc_str);

	// next thing will be however many bytes is wanted
	tok = strtok(NULL, " ");

	if(!tok){
		cmdfunc_help("examine", 0);
		return CMD_FAILURE;
	}

	char *amount_str = malloc(strlen(tok) + 1);
	strcpy(amount_str, tok);

	base = 10;

	if(strstr(amount_str, "0x"))
		base = 16;

	unsigned long amount = strtol(amount_str, NULL, base);

	free(amount_str);

	// check if --no-aslr was given
	tok = strtok(NULL, " ");

	kern_return_t ret;

	if(tok){
		if(strcmp(tok, "--no-aslr") == 0){
			ret = memutils_dump_memory_new(location, amount);
			
			if(ret)
				return CMD_FAILURE;
		}
		else{
			cmdfunc_help("examine", 0);
			return CMD_FAILURE;
		}

		return CMD_SUCCESS;
	}

	ret = memutils_dump_memory_new(location + debuggee->aslr_slide, amount);

	if(ret)
		return CMD_FAILURE;
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_regsfloat(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	if(!args){
		printf("Register?\n");
		return CMD_FAILURE;
	}

	// Iterate through and show all the registers the user asked for
	char *tok = strtok((char *)args, " ");

	while(tok){
		char reg_type = tok[0];
		// move up a byte for the register number
		tok++;
		int reg_num = atoi(tok);

		if(reg_num < 0 || reg_num > 31)
			continue;

		arm_neon_state64_t neon_state;
		mach_msg_type_number_t count = ARM_NEON_STATE64_COUNT;
		
		struct machthread *focused = machthread_getfocused();

		if(!focused){
			printf("We are not focused on any thread.\n");
			return CMD_FAILURE;
		}

		kern_return_t err = thread_get_state(focused->port, ARM_NEON_STATE64, (thread_state_t)&neon_state, &count);

		if(err){
			printf("show_neon_registers: thread_get_state failed: %s\n", mach_error_string(err));
			return CMD_FAILURE;
		}

		union intfloat {
			int i;
			float f;
		} IF;

		if(reg_type == 'v'){
			// TODO figure this out...
			// print each byte in this 128 bit integer (16)
		
			//void *upper = neon_state.__v[reg_num] >> 64;
			//void *lower = neon_state.__v[reg_num] << 64;
			
			//memutils_dump_memory_from_location(upper, 8, 8, 16);
			//memutils_dump_memory_from_location(lower, 8, 8, 16);	

		//	printf("%llx %llx\n", upper, lower);

		}
		else if(reg_type == 'd'){
			// D registers, bottom 64 bits of each Q register
			IF.i = neon_state.__v[reg_num] >> 32;
			printf("D%d 				%f\n", reg_num, IF.f);
		}
		else if(reg_type == 's'){
			// S registers, bottom 32 bits of each Q register
			IF.i = neon_state.__v[reg_num] & 0xFFFFFFFF;
			printf("S%d\t\t\t%f (0x%x)\n", reg_num, IF.f, IF.i);
		}
	
		tok = strtok(NULL, " ");
	}

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_regsgen(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;
	
	arm_thread_state64_t thread_state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	
	struct machthread *focused = machthread_getfocused();

	if(!focused){
		printf("We are not focused on any thread.\n");
		return CMD_FAILURE;
	}

	kern_return_t err = thread_get_state(focused->port, ARM_THREAD_STATE64, (thread_state_t)&thread_state, &count);

	if(err){
		printf("Failed\n");
		return CMD_FAILURE;
	}
	
	// if there were no arguments, print every register
	if(!args){
		for(int i=0; i<29; i++)
			printf("X%d\t\t\t%#llx\n", i, thread_state.__x[i]);
		
		printf("FP\t\t\t%#llx\n", thread_state.__fp);
		printf("LR\t\t\t%#llx\n", thread_state.__lr);
		printf("SP\t\t\t%#llx\n", thread_state.__sp);
		printf("PC\t\t\t%#llx\n", thread_state.__pc);
		printf("CPSR\t\t\t%#x\n", thread_state.__cpsr);

		return CMD_SUCCESS;
	}

	// otherwise, print every register they asked for
	char *tok = strtok((char *)args, " ");

	while(tok){
		if(tok[0] != 'x'){
			tok = strtok(NULL, " ");
			continue;
		}

		// move up one byte to get to the "register number"
		tok++;
		int reg_num = atoi(tok);
		
		if(reg_num < 0 || reg_num > 29){
			tok = strtok(NULL, " ");
			continue;
		}

		printf("X%d\t\t\t%#llx\n", reg_num, thread_state.__x[reg_num]);

		tok = strtok(NULL, " ");
	}
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_kill(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	//cmdfunc_detach(NULL, 0);
	//kill(debuggee->pid, SIGKILL);
	
	printf("Disabled for safety right now\n");
	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_help(const char *args, int arg1){
	if(!args)
		return CMD_FAILURE;
	
	// it does not make sense for the command to be autocompleted here
	// so just search through the command table until we find the argument
	int num_cmds = sizeof(COMMANDS) / sizeof(struct dbg_cmd_t);
	int cur_cmd_idx = 0;

	while(cur_cmd_idx < num_cmds){
		struct dbg_cmd_t *cmd = &COMMANDS[cur_cmd_idx];
	
		// must not be an ambigious command
		if(strcmp(cmd->name, args) == 0 && cmd->function){
			printf("	%s\n", cmd->desc);
			return CMD_SUCCESS;
		}

		cur_cmd_idx++;
	}
	
	// not found
	return CMD_FAILURE;
}

cmd_error_t cmdfunc_quit(const char *args, int arg1){
	if(debuggee->pid != -1)
		cmdfunc_detach(NULL, 0);

	free(debuggee);
	exit(0);
}

cmd_error_t cmdfunc_set(const char *args, int arg1){
	if(!args){
		cmdfunc_help("set", 0);
		return CMD_FAILURE;
	}
	
	// check for offset
	if(args[0] == '*'){
		// move past the '*'
		args++;
		args = strtok((char *)args, " ");
	
		// get the location, an equals sign follows it
		char *location_str = malloc(64);
		char *equals = strchr(args, '=');

		if(!equals){
			printf("\t * No new value\n\n");
			cmdfunc_help("set", 0);
			return CMD_FAILURE;
		}

		strncpy(location_str, args, equals - args);

		char *zero_x = strstr(location_str, "0x");
		if(!zero_x){
			printf("\t * Need '0x' before location\n\n");
			cmdfunc_help("set", 0);
			return CMD_FAILURE;
		}

		// TODO allow math on the location and the value

		int base = 16;
		unsigned long long location = strtoll(location_str, NULL, base);

		// find out what they want the location set to
		char *value_str = malloc(64);

		// equals + 1 to get past the actual equals sign
		strcpy(value_str, equals + 1);
		
		if(strlen(value_str) == 0){
			printf("Need a value\n");
			return CMD_FAILURE;
		}

		// see how they want their new value interpreted
		int value_base = 16;

		// no "0x", so base 10
		if(!strstr(value_str, "0x"))
			value_base = 10;

		unsigned long long value = strtoll(value_str, NULL, value_base);

		location += debuggee->aslr_slide;

		args = strtok(NULL, " ");
		if(args && strstr(args, "--no-aslr"))
			location -= debuggee->aslr_slide;

		kern_return_t result = memutils_write_memory_to_location((vm_address_t)location, (vm_offset_t)value);
		
		if(result){
			printf("Error: %s\n", mach_error_string(result));
			return CMD_FAILURE;
		}
	}

	// if they're not modifing an offset, they're setting a config variable
	// to be implemented

	
	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_threadlist(const char *args, int arg1){
	if(!debuggee)
		return CMD_FAILURE;

	if(!debuggee->threads)
		return CMD_FAILURE;
	
	if(!debuggee->threads->front)
		return CMD_FAILURE;
	
	struct node_t *current = debuggee->threads->front;

	while(current){
		struct machthread *t = current->data;

		printf("\t%sthread #%d, tid = %#llx, name = '%s', where = %#llx\n", t->focused ? "* " : "", t->ID, t->tid, t->tname, t->thread_state.__pc);
		
		current = current->next;
	}

	return CMD_SUCCESS;
}

cmd_error_t cmdfunc_threadselect(const char *args, int arg1){
	if(debuggee->pid == -1)
		return CMD_FAILURE;

	if(!debuggee->threads)
		return CMD_FAILURE;

	if(!debuggee->threads->front)
		return CMD_FAILURE;

	int thread_id = atoi(args);

	if(thread_id < 1 || thread_id > debuggee->thread_count){
		printf("Out of bounds, must be in between [1, %d]\n", debuggee->thread_count);
		printf("Threads:\n");
		cmdfunc_threadlist(NULL, 0);
		return CMD_FAILURE;
	}

	int result = machthread_setfocusgivenindex(thread_id);
	
	if(result){
		printf("Failed");
		return CMD_FAILURE;
	}

	printf("Selected thread %d\n", thread_id);
	
	return CMD_SUCCESS;
}

// Given user input, autocomplete their command and find the arguments.
// If the command is valid, call the function pointer from the correct command struct.
cmd_error_t execute_command(char *user_command){
	int num_commands = sizeof(COMMANDS) / sizeof(struct dbg_cmd_t);

	// make a copy of the parameters so we don't modify them
	char *user_command_copy = malloc(128);
	strcpy(user_command_copy, user_command);

	char *token = strtok(user_command_copy, " ");
	if(!token)
		return CMD_FAILURE;

	// this string will hold all the arguments for the command passed in
	// if it is still NULL by the time we exit the main loop, there were no commands
	char *cmd_args = NULL;

	char *piece = malloc(128);
	strcpy(piece, token);

	struct cmd_match_result_t *current_result = malloc(sizeof(struct cmd_match_result_t));
	current_result->num_matches = 0;
	current_result->match = NULL;
	current_result->matched_cmd = NULL;
	current_result->matches = malloc(512);
	current_result->ambigious = 0;
	current_result->perfect = 0;

	bzero(current_result->matches, 512);

	// will hold the best command we've found
	// this compensates for command arguments being included
	// in the command string we're testing for
	struct cmd_match_result_t *final_result = NULL;

	while(token){
		int cur_cmd_idx = 0;
		struct dbg_cmd_t *cmd = &COMMANDS[cur_cmd_idx];

		char *prev_piece = NULL;

		while(cur_cmd_idx < num_commands){
			int use_prev_piece = prev_piece != NULL;
			int piecelen = strlen(use_prev_piece ? prev_piece : piece);
			
			// check if the command is an alias
			// check when a command is shorter than another command
			if((cmd->function && strcmp(cmd->name, piece) == 0) || (cmd->alias && strcmp(cmd->alias, user_command_copy) == 0)){
				final_result = malloc(sizeof(struct cmd_match_result_t));

				final_result->num_matches = 1;
				final_result->match = malloc(64);
				strcpy(final_result->match, cmd->name);
				final_result->matches = NULL;
				final_result->matched_cmd = cmd;
				final_result->perfect = 1;

				break;
			}

			if(strncmp(cmd->name, use_prev_piece ? prev_piece : piece, piecelen) == 0){
				current_result->num_matches++;

				// guaranteed ambigious command
				if(!cmd->function){
					if(current_result->match)
						current_result->match = NULL;
					
					// strlen(cmd->name) + ' ' + '\0'
					char *updated_piece = malloc(strlen(cmd->name) + 1 + 1);
					strcpy(updated_piece, cmd->name);
					strcat(updated_piece, " ");

					strcpy(piece, updated_piece);

					free(updated_piece);

					current_result->ambigious = 1;
				}
				else
					current_result->ambigious = current_result->num_matches > 1 ? 1 : 0;

				// tack on any other matches we've found
				if(cmd->function){
					strcat(current_result->matches, cmd->name);
					strcat(current_result->matches, ", ");
				}

				if(current_result->num_matches == 1){
					// strlen(cmd->name) + ' ' + '\0'
					char *updated_piece = malloc(strlen(cmd->name) + 1 + 1);
					
					// find the end of the current word in the command
					char *cmdname_copy = (char *)cmd->name;

					// advance to where piece leaves off
					cmdname_copy += strlen(piece);

					// find the nearest space after that
					char *space = strchr(cmdname_copy, ' ');

					if(space){
						// if we found a space, we need to know how many bytes of cmd->name to copy
						// because we aren't at the end of this command yet
						int byte_amount = space - cmdname_copy;

						strncpy(updated_piece, cmd->name, strlen(piece) + byte_amount);
					}
					else
						// otherwise, we've reached the end of the command
						// and it's safe to copy the entire thing
						strcpy(updated_piece, cmd->name);
					
					strcat(updated_piece, " ");

					// we need to check for ambiguity but we are modifing piece
					// make a backup and use this in the strncmp call when it is not NULL
					if(!prev_piece)
						prev_piece = malloc(64);

					strcpy(prev_piece, piece);
					strcpy(piece, updated_piece);

					free(updated_piece);
					
					if(!current_result->match)
						current_result->match = malloc(64);

					strcpy(current_result->match, cmd->name);
					strcpy(current_result->matches, current_result->match);
					strcat(current_result->matches, ", ");

					final_result = current_result;
					final_result->matched_cmd = cmd;
				}

				if(current_result->ambigious){
					if(current_result->match){
						free(current_result->match);
						current_result->match = NULL;
					}

					if(final_result)
						final_result->matched_cmd = NULL;
				}
			}

			cur_cmd_idx++;
			cmd = &COMMANDS[cur_cmd_idx];
		}

		cur_cmd_idx = 0;

		token = strtok(NULL, " ");

		if(token && (final_result && !final_result->perfect)){
			if(!current_result->ambigious)
				strcat(piece, token);
			else if(token && current_result->ambigious){
				// find the last space in the command string we're building
				char *lastspace = strrchr(piece, ' ');

				// append the next piece to the command string
				if(lastspace){
					char *updated_piece = malloc(strlen(piece) + strlen(token) + 1);
					strcpy(updated_piece, piece);
					strcat(updated_piece, token);
					strcpy(piece, updated_piece);

					free(updated_piece);
				}
			}
		}

		// once final_result->match is not NULL, we've found a match
		// this means we can assume anything `token` contains is an argument
		if(token && final_result && final_result->match){
			if(!cmd_args){
				cmd_args = malloc(128);
				bzero(cmd_args, 128);
			}

			strcat(cmd_args, token);
			strcat(cmd_args, " ");
		}

		current_result->num_matches = 0;
	}

	if(final_result){
		// trim the extra comma and space from the ambiguous commands string
		if(final_result->matches)
			final_result->matches[strlen(final_result->matches) - 2] = '\0';

		if(!final_result->match)
			printf("Ambigious command '%s': %s\n", user_command, final_result->matches);

		// do the same with the cmd_args string
		if(cmd_args && strlen(cmd_args) > 0)
			cmd_args[strlen(cmd_args) - 1] = '\0';

		if(final_result->matched_cmd && final_result->matched_cmd->function)
			final_result->matched_cmd->function(cmd_args, 0);
		
		if(cmd_args)
			free(cmd_args);
	}
	else
		printf("Unknown command '%s'\n", user_command);

	free(current_result->match);
	free(current_result->matches);
	free(current_result);
	free(piece);
	free(user_command_copy);

	return CMD_SUCCESS;
}
