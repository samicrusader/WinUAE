
#ifndef CWJOY_USR_H
#define CWJOY_USR_H

#ifndef DRIVER
#ifndef LINUX
#include <winioctl.h>
#endif
#endif

// TODO: should work on CWCONFIG device

#define CW_JOY_GETOPTIONS      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +54,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:
//	0x00	u8	port
//output:
//	0x00	u8	port
//	0x01	u8	type 0: none
//			     1: c64/amiga joystick
//			     2: c64 paddle
//			     3: amiga mouse
//			     4: 1531 mouse
//			     5: atari mouse
//	0x02	u8	emubits
//			bit 5: paddle emulates mouse
//			bit 4: paddle emulates joy
//			bit 3: mouse emulates paddle
//			bit 2: mouse emulates joy
//			bit 1: joy emulates paddle
//			bit 0: joy emulates mouse
//	0x03	u8	autofire button 1 speed
//	0x04	u8	autofire button 2 speed
#define CW_JOY_SETOPTIONS      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +55,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:
//	0x00	u8	port
//	0x01	u8	type 0: none
//			     1: c64/amiga joystick
//			     2: c64 paddle
//			     3: amiga mouse
//			     4: 1531 mouse
//			     5: atari mouse
//	0x02	u8	emubits
//			bit 5: paddle emulates mouse
//			bit 4: paddle emulates joy
//			bit 3: mouse emulates paddle
//			bit 2: mouse emulates joy
//			bit 1: joy emulates paddle
//			bit 0: joy emulates mouse
//	0x03	u8	autofire button 1 speed
//	0x04	u8	autofire button 2 speed


/*
	currently highest syscall is

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
	in
catweasl_usr.h
		
	keep this in sync with catweasl.h, catweasl_usr.h cwfloppy.h cwjoystick.h cwkeyboard.h SID6581_usr.h
*/
#endif
