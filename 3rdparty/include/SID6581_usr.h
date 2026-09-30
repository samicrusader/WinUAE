#ifndef SID6581_USR_H
#define SID6581_USR_H

#ifndef DRIVER
#ifndef LINUX
#include <winioctl.h>
#endif
#endif

//device name for opening

//device name:
// '\Device\SID6581_0'
//user-mode name:
// '\DosDevices\SID6581_1'


#define SID_GET_VERSION     CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 0,METHOD_BUFFERED,FILE_ANY_ACCESS)

//using the peek/poke command:
//
//as input, supply an array of byte-pairs that makes up the command.
//The first byte is a command, the second byte an operand
//possible values for the first byte:
// 0 to 31       write to register 0...31, operand is the data.
// SID_CMD_READ  read from register, operand is register number
// SID_CMD_TIMER wait for next occurence of the timer. This can also be used as first or last command.
//
//output is a buffer filled with the data from all the read commands found in the block.
//
//Note: The SID does not like writing to the read-only registers. Therefore, write-attempts to these
//      are ignored by the driver and not passed to the SID.

#define SID_SID_PEEK_POKE   CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 1,METHOD_BUFFERED,FILE_ANY_ACCESS)

#define SID_CMD_READ   32
#define SID_CMD_TIMER  64

//using the set timer command:
//
//as input, supply a buffer containing a single unsigned integer with a number of microseconds
//to use for periodic timer. The timer is implemented using the kernel timers or using the
//hardware if possible. The minimum delay is one millisecond.
//Note that there is only one timer available. If it is set again, the old timer is cancelled.
//To stop the timer, call the stop timer function without parameters.

#define SID_SET_TIMER       CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 2,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  4 unsigned int timer
//output: none
#define SID_STOP_TIMER      CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 3,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: none

//using the set clock command:
//
//as input, supply a single byte. If this byte is zero, the clock is set to the PAL frequency,
//if the byte is one, the clock is set to the NTSC frequency. Note that it may take up to 2
//milliseconds for the oscillator to stabilize on the changed frequency.

#define SID_SET_CLOCK       CTL_CODE(FILE_DEVICE_SOUND,0x0800UL + 4,METHOD_BUFFERED,FILE_ANY_ACCESS)

/*
	currently highest syscall is

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
	in
catweasl_usr.h
		
	keep this in sync with catweasl.h, catweasl_usr.h cwfloppy.h cwjoystick.h cwkeyboard.h SID6581_usr.h
*/
#endif
