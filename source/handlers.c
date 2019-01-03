#include "handlers.h"

unsigned long long find_slide(void){
	vm_region_basic_info_data_64_t info;
	vm_address_t address = 0;
	vm_size_t size;
	mach_port_t object_name;
	mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
	
	kern_return_t err = vm_region_64(debuggee->task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &info_count, &object_name);

	if(err)
		return err;
	
	return address - 0x100000000;
}

kern_return_t restore_exception_ports(void){
	for(mach_msg_type_number_t i=0; i<debuggee->original_exception_ports.count; i++)
		task_set_exception_ports(debuggee->task, 
								debuggee->original_exception_ports.masks[i], 
								debuggee->original_exception_ports.ports[i], 
								debuggee->original_exception_ports.behaviors[i], 
								debuggee->original_exception_ports.flavors[i]);

	return KERN_SUCCESS;
}

kern_return_t resume(void){
	return task_resume(debuggee->task);
}

kern_return_t setup_exception_handling(void){
	// make an exception port for the debuggee
	kern_return_t err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &debuggee->exception_port);
	// be able to send messages on that exception port
	err = mach_port_insert_right(mach_task_self(), debuggee->exception_port, debuggee->exception_port, MACH_MSG_TYPE_MAKE_SEND);
	/*
	mach_port_t port_set;

	err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &port_set);

	err = mach_port_move_member(mach_task_self(), debuggee->exception_port, port_set);

	// allocate port to notify us of termination
	err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &debuggee->death_port);

	err = mach_port_move_member(mach_task_self(), debuggee->death_port, port_set);
	
	mach_port_t p;
	err = mach_port_request_notification(mach_task_self(), debuggee->task, MACH_NOTIFY_DEAD_NAME, 0, debuggee->death_port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &p);
	*/
	// save the old exception ports
	err = task_get_exception_ports(debuggee->task, EXC_MASK_ALL, debuggee->original_exception_ports.masks, &debuggee->original_exception_ports.count, debuggee->original_exception_ports.ports, debuggee->original_exception_ports.behaviors, debuggee->original_exception_ports.flavors);

	// add the ability to get exceptions on the debuggee exception port
	// OR EXCEPTION_DEFAULT with MACH_EXCEPTION_CODES so 64-bit safe exception messages will be provided 
	err = task_set_exception_ports(debuggee->task, EXC_MASK_ALL, debuggee->exception_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);

	return err;
}

kern_return_t suspend(void){
	return task_suspend(debuggee->task);
}

kern_return_t update_threads(thread_act_port_array_t *threads){
	mach_msg_type_number_t thread_count;
	
	kern_return_t err = task_threads(debuggee->task, threads, &thread_count);
	
	debuggee->thread_count = thread_count;

	return err;
}
