/*
An iOS debugger I'm making for fun and to learn.
Don't expect this to ever be as sophisticated as GDB or LLDB.
*/

#include "iosdbg.h"

// print every command with a description
void help(){
	printf("attach 						attach to a program with its PID\n");
	printf("aslr						show the ASLR slide (TODO)\n");
	printf("break <addr>				set a breakpoint at addr (TODO)\n");
	printf("clear						clear screen\n");
	printf("continue					resume debuggee execution\n");
	printf("detach						detach from the current program\n");
	printf("help						show this message\n");
	printf("quit						quit iosdbg\n");
	printf("regs <gen, float>			show registers (float: TODO)\n");
}

const char *get_exception_code(exception_type_t exception){
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

// Exceptions are caught here
// Breakpoints are handled here
kern_return_t catch_mach_exception_raise(
		mach_port_t exception_port,
		mach_port_t thread,
		mach_port_t task,
		exception_type_t exception,
		exception_data_t code,
		mach_msg_type_number_t code_count){

	// check PC to see if we are at an address we breakpointed at
	// keep a linked list of breakpoints?

	// interrupt debuggee so it doesn't crash
	interrupt(0);

	printf("\r\nThread %x received signal %d, %s.\r\n", thread, exception, get_exception_code(exception));
	printf("\r\r(iosdbg) ");

	return KERN_SUCCESS;
}

void *exception_server(void *arg){
	while(1){
		// shut down this thread once we detach
		if(debuggee->pid == -1)
			pthread_exit(NULL);

		kern_return_t err;

		if((err = mach_msg_server_once(mach_exc_server, 4096, debuggee->exception_port, 0)) != KERN_SUCCESS)
			printf("\r\nmach_msg_server_once: error: %s\r\n", mach_error_string(err));
	}

	return NULL;
}

// setup our exception related stuff
void setup_exception_handling(){
	// make an exception port for the debuggee
	kern_return_t err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &debuggee->exception_port);

	if(err){
		warn("setup_exception_handling: mach_port_allocate failed: %s\n", mach_error_string(err));
		return;
	}

	// be able to send messages on that exception port
	err = mach_port_insert_right(mach_task_self(), debuggee->exception_port, debuggee->exception_port, MACH_MSG_TYPE_MAKE_SEND);

	if(err){
		warn("setup_exception_handling: mach_port_insert_right failed: %s\n", mach_error_string(err));
		return;
	}

	// save the old exception ports
	err = task_get_exception_ports(debuggee->task, EXC_MASK_ALL, debuggee->original_exception_ports.masks, &debuggee->original_exception_ports.count, debuggee->original_exception_ports.ports, debuggee->original_exception_ports.behaviors, debuggee->original_exception_ports.flavors);

	if(err){
		warn("setup_exception_handling: task_get_exception_ports failed: %s\n", mach_error_string(err));
		return;
	}

	// add the ability to get exceptions on the debuggee exception port
	// OR EXCEPTION_DEFAULT with MACH_EXCEPTION_CODES so 64-bit safe exception messages will be provided 
	err = task_set_exception_ports(debuggee->task, EXC_MASK_ALL, debuggee->exception_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);

	if(err){
		warn("setup_exception_handling: task_set_exception_ports failed: %s\n", mach_error_string(err));
		return;
	}

	// start the exception server
	pthread_t exception_server_thread;
	pthread_create(&exception_server_thread, NULL, exception_server, NULL);
}

// SIGINT handler
void interrupt(int x1){
	if(debuggee->interrupted)
		return;

	// TODO: Provide a nice way of showing the client they interrupted the debuggee that doesn't screw up the limenoise prompt

	kern_return_t err = task_suspend(debuggee->task);

	if(err){
		warn("cannot interrupt: %s\n", mach_error_string(err));
		debuggee->interrupted = 0;

		return;
	}

	int result = suspend_threads();

	if(result != 0){
		warn("couldn't suspend threads for %d during interrupt\n", debuggee->pid);
		debuggee->interrupted = 0;

		return;
	}

	debuggee->interrupted = 1;
}

void setup_initial_debuggee(){
	debuggee = malloc(sizeof(struct debuggee));

	// if we aren't attached to anything, debuggee's pid is -1
	debuggee->pid = -1;
	debuggee->interrupted = 0;
	debuggee->breakpoints = linkedlist_new();
}

// try and attach to the debuggee, and get its ASLR slide
// Returns: 0 on success, -1 on fail
int attach(pid_t pid){
	kern_return_t err = task_for_pid(mach_task_self(), pid, &debuggee->task);

	if(err){
		warn("attach: couldn't get task port for pid %d: %s\n", pid, mach_error_string(err));
		return -1;
	}

	err = task_suspend(debuggee->task);

	if(err){
		warn("attach: task_suspend call failed: %s\n", mach_error_string(err));
		return -1;
	}

	int result = suspend_threads();

	if(result != 0){
		warn("attach: couldn't suspend threads for %d while attaching, detaching...\n", debuggee->pid);

		detach();

		return -1;
	}

	debuggee->pid = pid;
	debuggee->interrupted = 1;

	vm_region_basic_info_data_64_t info;
	vm_address_t address = 0;
	vm_size_t size;
	mach_port_t object_name;
	mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
	
	vm_region_64(debuggee->task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &info_count, &object_name);

	debuggee->aslr_slide = address - 0x100000000;

	printf("Attached to %d, ASLR slide is %llx. Do not worry about adding ASLR to addresses, it is already accounted for.\n", debuggee->pid, debuggee->aslr_slide);

	return 0;
}

// resume the debuggee's execution
// Returns: 0 on success, -1 on fail
int resume(){
	if(!debuggee->interrupted)
		return -1;

	kern_return_t err = task_resume(debuggee->task);

	if(err){
		warn("resume: couldn't continue: %s\n", mach_error_string(err));
		return -1;
	}

	resume_threads();

	debuggee->interrupted = 0;

	return 0;
}

// try and resume every thread in the debuggee
void resume_threads(){
	for(int i=0; i<debuggee->thread_count; i++)
		thread_resume(debuggee->threads[i]);
}

// try and suspend every thread in the debuggee
// Return: 0 on success, -1 on fail
int suspend_threads(){
	kern_return_t err = task_threads(debuggee->task, &debuggee->threads, &debuggee->thread_count);

	if(err){
		warn("suspend_threads: couldn't get the list of threads for %d: %s\n", debuggee->pid, mach_error_string(err));
		return -1;
	}

	for(int i=0; i<debuggee->thread_count; i++)
		thread_suspend(debuggee->threads[i]);

	return 0;
}

// show general registers of thread 0
// Return: 0 on success, -1 on fail
int show_general_registers(){
	arm_thread_state64_t thread_state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

	kern_return_t err = thread_get_state(debuggee->threads[0], ARM_THREAD_STATE64, (thread_state_t)&thread_state, &count);

	if(err){
		warn("show_general_registers: thread_get_state failed: %s\n", mach_error_string(err));
		return -1;
	}

	// print general purpose registers
	for(int i=0; i<29; i++)
		printf("X%d 				0x%llx\n", i, thread_state.__x[i]);

	// print the other ones
	printf("FP 				0x%llx\n", thread_state.__fp);
	printf("LR 				0x%llx\n", thread_state.__lr);
	printf("SP 				0x%llx\n", thread_state.__sp);
	printf("PC 				0x%llx\n", thread_state.__pc);
	printf("CPSR 				0x%x\n", thread_state.__cpsr);

	return 0;
}

// Work in progress
// show floating point registers of thread 0
// Return: 0 on success, -1 on fail
int show_neon_registers(){
	arm_neon_state64_t neon_state;
	mach_msg_type_number_t count = ARM_NEON_STATE64_COUNT;

	kern_return_t err = thread_get_state(debuggee->threads[0], ARM_NEON_STATE64, (thread_state_t)&neon_state, &count);

	if(err){
		warn("show_neon_registers: thread_get_state failed: %s\n", mach_error_string(err));
		return -1;
	}

	// print floating point registers
	for(int i=0; i<32; i++){
		uint64_t lower_bits = neon_state.__v[i];
		//printf("%lx\n", sizeof(neon_state.__v[i]));
		printf("V%d 				%llx\n", i, (uint64_t)(neon_state.__v[i] >> 32));
	}

	return 0;
}

// try and detach from the debuggee
// Returns: 0 on success, -1 on fail
int detach(){
	// restore original exception ports
	for(mach_msg_type_number_t i=0; i<debuggee->original_exception_ports.count; i++){
		kern_return_t err = task_set_exception_ports(debuggee->task, debuggee->original_exception_ports.masks[i], debuggee->original_exception_ports.ports[i], debuggee->original_exception_ports.behaviors[i], debuggee->original_exception_ports.flavors[i]);
		
		if(err)
			warn("detach: task_set_exception_ports: %s, %d\n", mach_error_string(err), i);
	}

	if(debuggee->interrupted){
		int result = resume();

		if(result != 0){
			warn("detach: couldn't resume execution before we detach?\n");
			return -1;
		}
	}

	debuggee->interrupted = 0;

	printf("detached from %d\n", debuggee->pid);

	debuggee->pid = -1;

	linkedlist_free(debuggee->breakpoints);
	debuggee->breakpoints = NULL;

	return 0;
}

int main(int argc, char **argv, const char **envp){
	setup_initial_debuggee();

	signal(SIGINT, interrupt);

	char *line = NULL;

	while((line = linenoise("(iosdbg) ")) != NULL){
		// add the command to history
		linenoiseHistoryAdd(line);

		// update the debuggee's list of threads
		if(debuggee->pid != -1){
			kern_return_t err = task_threads(debuggee->task, &debuggee->threads, &debuggee->thread_count);

			if(err){
				warn("we couldn't update the list of threads for %d: %s\n", debuggee->pid, mach_error_string(err));
				continue;
			}
		}

		if(strcmp(line, "quit") == 0){
			if(debuggee->pid != -1)
				detach();

			free(debuggee);
			exit(0);
		}
		else if(strcmp(line, "continue") == 0)
			resume();
		else if(strcmp(line, "clear") == 0)
			linenoiseClearScreen();
		else if(strcmp(line, "help") == 0)
			help();
		else if(strcmp(line, "detach") == 0)
			detach();
		else if(strstr(line, "attach")){
			if(debuggee->pid != -1){
				warn("Already attached to %d\n", debuggee->pid);
				continue;
			}

			char *tok = strtok(line, " ");
			pid_t potential_target_pid;
			
			while(tok){
				potential_target_pid = atoi(tok);
				tok = strtok(NULL, " ");
			}

			if(potential_target_pid == 0)
				warn("we cannot attach to the kernel!\n");
			else if(potential_target_pid == getpid())
				warn("do not try and debug me!\n");
			else{
				int result = attach(potential_target_pid);

				if(result != 0)
					warn("Couldn't attach to %d\n", potential_target_pid);
				else
					setup_exception_handling();
			}
		}
		else if(strstr(line, "regs") && debuggee->pid != -1){
			char *reg_type = malloc(1024);
			char *tok = strtok(line, " ");

			while(tok){
				strcpy(reg_type, tok);
				tok = strtok(NULL, " ");
			}

			if(strcmp(reg_type, "gen") != 0 && strcmp(reg_type, "float") != 0){
				warn("Bad argument. try `gen` or `float`\n");
				free(reg_type);
				continue;
			}

			if(strcmp(reg_type, "float") == 0)
				show_neon_registers();
			else
				show_general_registers();

			free(reg_type);
		}
		else
			printf("Invalid command.\n");

		free(line);
	}

	return 0;
}