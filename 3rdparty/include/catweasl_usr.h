#ifndef CATWEASL_USR_H
#define CATWEASL_USR_H

#ifndef DRIVER
#ifndef LINUX
#include <windows.h>
#include <winioctl.h>
#endif
#endif



#define CW_GET_VERSION     CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 0,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: version string

#define CW_REINIT_HW       CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +12,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: none

#define CW_GET_MEMORY      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 1,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: up to 128kb memory data

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  (optional) terminator mask (0x80 for mk3 style, 0xff for mk4 style data)
//output: up to 128kb memory data


#define CW_SET_MEMORY      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +13,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  up to 128kb memory data
//output: none

#define CW_GET_HW_REGS     CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +14,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: two unsigned int containing reg offsets. not for floppy.
//        0x00 - port base addr
//        0x01 - hw base
//        0x02 - unit number

#define CW_GET_HWVERSION   CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +20,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  nothing
//output: 16 Bytes
//        0(b) : Hardware version (0=MK1, 1=MK3, 2=MK4)
//        1(b) : can do 14MHz (0=No, 1=Yes)
//        2(b) : can do 28MHz (0=No, 1=Yes)
//        3(b) : can do 56MHz (0=No, 1=Yes)
//        4(b) : has joystick ports (bits: 0x1=joy, 0x2=mouse)
//        5(b) : dual ported memory (0=none, 1=supported)
//        6(b) : SID (bits: 0x1=supported, 0x2=FIFO)
//        7(b) : has keyboard (bits: 0x1=AMIGA kbd supported, 0x2=C64 kbd supported)
//        8(l) : SID FIFO size
//        12(l): reserved

#ifdef DRIVER
// FIXME: use proper types for 64bit Driver
// typedef unsigned char uint8_t;
// typedef unsigned long uint32_t;
#endif

typedef struct
{
	// output of CW_GET_HWVERSION: 16 Bytes
	uint8_t	hw_version;   // Hardware version (0=MK1, 1=MK3, 2=MK4)
	uint8_t	has_14mhz;    // can do 14MHz (0=No, 1=Yes)
	uint8_t	has_28mhz;    // can do 28MHz (0=No, 1=Yes)
	uint8_t	has_56mhz;    // can do 56MHz (0=No, 1=Yes)
	uint8_t joybits;      // has joystick ports (bits: 0x1=joy, 0x2=mouse)
	uint8_t	has_dportmem; // dual ported memory (0=none, 1=supported)
	uint8_t	sidbits;      // SID (bits: 0x1=supported, 0x2=FIFO)
	uint8_t	keybits;      // has keyboard (bits: 0x1=AMIGA kbd supported, 0x2=C64 kbd supported)
	uint32_t fifo_size;   // SID FIFO size
	uint32_t reserved;    // reserved
} CW_HW_INFO;

#define CW_REVISION_MK1			0
#define CW_REVISION_MK2			4
#define CW_REVISION_MK3			1
#define CW_REVISION_MK4			2

#define CW_HAS_SID			1
#define CW_HAS_FIFO			2

#define CW_HAS_JOYPORTS			1
#define CW_HAS_MOUSEPORTS		2

#define CW_HAS_AMIGAKBD			1
#define CW_HAS_C64KBD			2

#define CW_LOCK_EXCLUSIVE  CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +25,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output:  8 bytes:
//         0: uint32 HW baseaddress (not needed for CW_PEEKREG and CW_POKEREG)
//         4: uint32 HW type: 0=ISA, 1=PCI MK3, 2=PCI MK4

#define CW_UNLOCK_EXCLUSIVE CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +26,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none


#define CW_PEEKREG_FULL       CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +40,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  array of bytes with register offsets to peek.
//        address space includes full controller, including PCI config area.
//output: same size array of bytes read.

#define CW_POKEREG_FULL       CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +41,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  array of bytepairs with reg offset and value to poke.
//        address space includes full controller, including PCI config area.

#define CW_PEEKREG_MAP        CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +42,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  array of bytes with register offsets to peek.
//        address space is bank-mapped on PCI MK4 board and clipped on PCI MK3 board.
//output: same size array of bytes read.

#define CW_POKEREG_MAP        CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +43,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  array of bytepairs with reg offset and value to poke.
//        address space is bank-mapped on PCI MK4 board and clipped on PCI MK3 board.

/*
	currently highest syscall is

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
	in
catweasl_usr.h
		
	keep this in sync with catweasl.h, catweasl_usr.h cwfloppy.h cwjoystick.h cwkeyboard.h SID6581_usr.h
*/
#endif
