#ifndef CWFLOPPY_USR_H
#define CWFLOPPY_USR_H

#ifndef DRIVER
#ifndef LINUX
#include <ntsecapi.h>
#include <winioctl.h>
#endif
#endif

// seek to cylinder
#define CW_SEEK            CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 3,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  one byte cylindernumber
//output: none

#define CW_CYLPOS_NOTIFY   CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 6,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
//output: none

// read raw track (index to index)
#define CW_READ_RAW        CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +21,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  5 Bytes
//        0 : Track lsb  (Cylinder?!)
//        1 : Track msb  (Cylinder?!)
//        2 : head (0-1)
//        3 : disk density (0=dd, 1=hd)   flag to set on floppy drive.
//        4 : clock (0=14MHz, 1=28MHz, 2=56MHz)
//output: data buffer (0-$20000 bytes)

// read raw track (given time)
#define CW_READ_RAW_NOINDEX        CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +45,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  6 Bytes
//        0 : Track lsb  (Cylinder?!)
//        1 : Track msb  (Cylinder?!)
//        2 : head (0-1)
//        3 : disk density (0=dd, 1=hd)   flag to set on floppy drive.
//        4 : clock (0=14MHz, 1=28MHz, 2=56MHz)
//	5/6 : reading time (0=read from index to index)
//output: data buffer (0-$20000 bytes)

// NOTE: When using with the ISA Catweasel, the end of the buffer
//       can contain junk or data from previous read operations. This
//       data can appears because the controller cannot place an
//       end-of-data mark in the read buffer.
//       The PCI version of the Catweasel does not have this problem.

#define CW_WRITE_RAW       CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +22,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define CW_WRITE_RAW_IDX2IDX   CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +60,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:
//        0 : Track lsb  (Cylinder?!)
//        1 : Track msb  (Cylinder?!)
//        2 : head (0-1)
//        3 : disk density (0=dd, 1=hd)   flag to set on floppy drive.
//        4 : clock (0=14MHz, 1=28MHz, 2=56MHz)
//	  followed by data
//output: none

// lock kywalda passthrough (mk4)
#define CW_LOCK_PASSTHROUGH  CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +46,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
// unlock kywalda passthrough (mk4)
#define CW_UNLOCK_PASSTHROUGH CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +47,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none

#define CW_ENABLE_DISKCHANGE  CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +50,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none
#define CW_DISABLE_DISKCHANGE CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +51,METHOD_BUFFERED,FILE_ANY_ACCESS)
//input:  none

//error codes (the same is in trackdisk.h Amiga include file, plus 0xE0000000UL .)
//use the normal macros for errordetection on results.

#define STATUS_CW_NOT_SPECIFIED     ((NTSTATUS)0xE0000014UL)
#define STATUS_CW_NO_SEC_HDR        ((NTSTATUS)0xE0000015UL)
#define STATUS_CW_BAD_SEC_PREAMBLE  ((NTSTATUS)0xE0000016UL)
#define STATUS_CW_BAD_SEC_ID        ((NTSTATUS)0xE0000017UL)
#define STATUS_CW_BAD_HDR_SUM       ((NTSTATUS)0xE0000018UL)
#define STATUS_CW_BAD_SEC_SUM       ((NTSTATUS)0xE0000019UL)
#define STATUS_CW_TOO_FEW_SECS      ((NTSTATUS)0xE000001AUL)
#define STATUS_CW_BAD_SEC_HDR       ((NTSTATUS)0xE000001BUL)
//#define STATUS_CW_WRITE_PROT        ((NTSTATUS)0xE000001CUL) use STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_CW_WRITE_PROT        STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_CW_DISK_CHANGED      ((NTSTATUS)0xE000001DUL)
#define STATUS_CW_SEEK_ERROR        ((NTSTATUS)0xE000001EUL)
#define STATUS_CW_NO_MEM            ((NTSTATUS)0xE000001FUL)
#define STATUS_CW_BAD_UNIT_NUM      ((NTSTATUS)0xE0000020UL)
#define STATUS_CW_BAD_DRIVE_TYPE    ((NTSTATUS)0xE0000021UL)
#define STATUS_CW_DRIVE_IN_USE      ((NTSTATUS)0xE0000022UL)
#define STATUS_CW_POST_RESET        ((NTSTATUS)0xE0000023UL)

#define CW_DRV_GETOPTIONS      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +58,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define CW_DRV_SETOPTIONS      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL +59,METHOD_BUFFERED,FILE_ANY_ACCESS)

/*
	currently highest syscall is

#define CW_GET_MEMORY_TERM      CTL_CODE(FILE_DEVICE_CONTROLLER,0x0800UL + 61,METHOD_BUFFERED,FILE_ANY_ACCESS)
	in
catweasl_usr.h
		
	keep this in sync with catweasl.h, catweasl_usr.h cwfloppy.h cwjoystick.h cwkeyboard.h SID6581_usr.h
*/


#endif
