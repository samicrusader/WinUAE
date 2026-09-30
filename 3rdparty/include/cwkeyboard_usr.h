
#ifndef CWKEYB_USR_H
#define CWKEYB_USR_H

#ifndef DRIVER
#ifndef LINUX
#include <winioctl.h>
#endif
#endif

#define CW_KBD_SETKEYMAP      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +56,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define CW_KBD_GETKEYMAP      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +57,METHOD_BUFFERED,FILE_ANY_ACCESS)

/*
	currently highest syscall is

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
	in
catweasl_usr.h
		
	keep this in sync with catweasl.h, catweasl_usr.h cwfloppy.h cwjoystick.h cwkeyboard.h SID6581_usr.h
*/
#endif
