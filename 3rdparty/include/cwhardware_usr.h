#ifndef CWHARDWARE_USR_H
#define CWHARDWARE_USR_H

// 128kb for mk4
#define CW_CONTROLLER_MEMSIZE	(128*1024)

/*
How to get the base address of the controller
---------------------------------------------

ISA:
	TODO

PCI: The PnP manager of the target system has mapped the board to a free space
in memory. Please consult your operating system manual for instructions on
how to determine the base address of a PCI PnP board.

PCI Vendor ID: $e159
PCI device ID: $0001
PCI subsystem ID: $00021212

The string for a Windows *.ini file would be:

PCI\VEN_E159&DEV_0001&SUBSYS_00021212

The board reserves two areas in memory, one in IO space and one ib memory
space. Both are 256 bytes, and both are identical. Writing/reading to/from
the memory space has the same effect as writing/reading to/from the IO space.
It's just a different base address, so it does not matter which space you
choose. However, it's recommended to use the IO space, because future versions
of the PCI bridge may not support the memory space any more. The offset for
the hardware registers is the same for both spaces: $c0. Do not access
the registers below, they control the behaviour of the PCI bridge, so writing
to these registers is a big no-no-no. Only run the given initialization
routine:

isa: (mk1)
no initialization needed

pci mk3:
Write $f1 to offset $00
write $00 to offset $01
write $00 to offset $02

write $00 to offset $04
write $00 to offset $05
write $00 to offset $29
write $00 to offset $2b

pci mk4:
Write $f1 to offset $00
write $00 to offset $01
write $e3 to offset $02

write $41 to offset $03 (mk3 register bank, set fpga reset bit)
upload fpga core
write $41 to offset $03 (clear legacy bit)

write $00 to offset $04
write $00 to offset $05
write $00 to offset $29
write $00 to offset $2b
*/

/*
mk4 only:
	upload fpga core

	TODO
*/

/*
Memory map
----------
The Catweasel MK3 has 16 registers that control the whole board. The following
table summarizes these registers. Please keep your source code portable by
using an offset-table for these registers. The register spacing and register
offsets may change in future versions of the controller, so a simple change
in your table will adapt the program to a new version of the hardware.

The MK4 has four different register banks, To access the MK3-compatible registers, 
write 0x41 to register base+3 of the controller.

*/

/*
PCI bridge registers

0x00 (write) ? init to 0xF1
0x01 (write) ? init to 0x00

0x02, 0x03 and 0x07 form a bi-directional 8-bit port. 

0x02 (write) 8-bit port data direction register. 
mk3: init to 0x00
mk4: Since all bits of this port have dedicated directions, please only write the recommended 
value into this register: 0xE3. This sets bits 7,6,5,1 and 0 to outputs and the other bits to 
inputs.

0x03 (write) 8-bit port write-port
		bank select (01: FIFO 0x41: mk3 regs)
	Bits 7 and 6 are write-only. They set the register bank of the FPGA. Before FPGA 
	configuration, only bank 01 is active (configuration bank). After configuration, 
	these two bits select one out of four register banks:
	
		00: Both bits=0 set the SID FIFO register bank
		01: Bit 7=0 and bit 6=1 sets the MK3-compatible register bank
		10: Bit 7=1 and bit 6=0 sets the IO register bank
		11: Bit 7=1 and bit 6=1 sets the IRQ register bank

	Bit 5 is write-only, the "MUX" bit. It controls the "Kylwalda" part of the Catweasel. 
	Writing a 0 to this bit grabs control of the floppy drives. While the Catweasel is in 
	control, the onboard-floppy controller still sees disk drives, but it is told that the 
	disk has been removed (diskchange signal).

	Bit 4 is read-only and should be written 0.
	Bit 3 is read-only and should be written 0.
	Bit 2 is read-only and should be written 0.

	Bit 1 is write-only and should be written 0 after the FPGA is configured. During 
	configuration, the LSB of the 8-bit data word must be mirrored here, because the path 
	to the FPGA configuration data port is only 7 bits wide. After configuration, this bit 
	has the "legacy" function that was never implemented. It still swaps a lot of bits in 
	the status and control registers, so it's recommended to keep it 0 unless you really 
	want to confuse everyone, including yourself.

	Bit 0 is write-only. It's the FPGA reset/erase bit: A 0 in this bit will erase the FPGA. 
	After setting this bit to 1, the FPGA will accept a new firmware program. The FPGA is 
	S-Ram based, so it can do any amount of write/erase cycles. Keep this bit set during 
	"everyday use", you normally do not have to fiddle around with FPGA re-configuration.

If you want to alter single bits, then read from register 0x07, modify your bits, then write the 
value to register 0x03.

0x04 (write) ? init to 0
0x05 (write) Write 0x08 to register base+5 to enable IRQ on the PCI Aux port bit 3.
0x07 (read) 8-bit port read-port		
	(mk4 only) joystick firebutton of port 2 in register base+7, bit 4.

	Bit 7 is write-only
	Bit 6 is write-only
	Bit 5 is write-only

	Bit 4 is read-only and should be written 0. It represents the joystick firebutton of 
	port 2. 0 means the button is pressed, 1 means that the button is released. This is a 
	realtime register. If you do not read often enough, you might miss a pressed button. 
	Do not read too often, as bouncing is also possible: Polling this bit at high frequency 
	can show bouncing effects of the buttons. Bouncing means that you get lots of consecutive 
	press and release events. A good polling frequency is between 25Hz and 100Hz.

	Bit 3 is read-only and should be written 0. This bit is only used during the 
	configuration phase of the FPGA. Check this bit before writing to the data port (0xc0): 
	Only write to the data port when this bit is 1. 
	
	Bit 2 is read-only and should be written 0. This bit shows the configuration status 
	of the FPGA: If you read a 1, then the FPGA firmware is installed and accepted by the 
	FPGA. If you read a 0, then the FPGA is not configured and the other register banks do 
	not have any function.
	
	Bit 1 is write-only
	Bit 0 is write-only


Setting/Resetting bits in this port
-----------------------------------
I'm sorry that I have "recommended" bad programming style with my Inside_MK4 file by recommending 
to write fixed values into register 0x03. You probably have lots of lines of code by now, where 
the register 0x03 is written fairly often. If you are altering the register bank, you mostly also 
alter the MUX bit, so programs that control the SID chip also affect the state of the Kylwalda 
part, which is really unnecessary. If you want to alter the register bank, please do it like 
this:

read from 0x07
AND with %00100011
OR with  %bb000000 (where bb ist the register bank)
write to 0x03

if you want to alter the state of the MUX bit, please do it like this:

read from 0x07
AND with 11000011
OR with 00m00000 (where m is the desired state of the MUX bit)
write to 0x03

Since any write to register 0x03 affects lots of functions, it should only be done selectively, 
not "in bulk" by using fixed values. I hope you will take the time to clean up your code with 
this information.


mk4:
The joystick fire for port 2 is in a different register. You can read the joystick firebutton of 
port 2 in register base+7, bit 4. If the read value of that register ANDed with 0x10 is 0x10, the 
button is not pressed. If the ANDed value is 0, then the button is pressed. 
*/



//#define CW_REG_BANK			0x03
//#define	CW_REG_7			0x07    // bit4: joystick 2 fire

#define	CW_REG_0			0x00	// ?
#define	CW_REG_1			0x01	// ?
#define	CW_REG_PORT_DDR			0x02	// (write) 8-bit port data direction register
#define	CW_REG_PORT_WRITE		0x03    // (write) 8-bit port write-port |mk4 only: bit 7,6: bank
#define	CW_REG_4			0x04	// ?
#define	CW_REG_5			0x05	// ?
#define	CW_REG_6			0x06	// ?
#define	CW_REG_PORT_READ		0x07    // (read) 8-bit port read-port |mk4 only: bit4: joystick 2 fire

// for CW_REG_PORT_DDR
#define CW_MK3_INITVALUE_PORT_DDR	0x00
#define CW_MK4_INITVALUE_PORT_DDR	0xE3

// for CW_REG_PORT_READ,CW_REG_PORT_WRITE
#define CW_MK4_FPGA_RESET	0x01	// bit 0	fpga reset
#define CW_MK4_LEGACY		0x02	// bit 1	legacy/fpga data bit 7 mirror
#define CW_MK4_KYWALDA		0x20	// bit 5
#define CW_MK4_BANK_FIFO	0x00	// bit 6,7
#define CW_MK4_BANK_MK3		0x40
#define CW_MK4_BANK_IO		0x80
#define CW_MK4_BANK_IRQ		0xC0

/*
0x29 (write) ? init to 0
0x2a (write) write 0x08 to register base+$2a to set active-low IRQ on bit 3 
0x2b (write) ? init to 0
*/

/*************************************************************************************************
	MK3 Register Bank
 *************************************************************************************************

Register     ! MK3	      ! MK4 (mk3 bank)	  ! MK1      !  MK3 Zorro offset ! MK2 Clockport
--------------------------------------------------------------------------------------------
JoyDat       ! $c0            ! $c0 		  ! n/a	     ! $c0               ! $00      upper 4bits: joystick2 lower 4bits: joystick1
PaddleSelect ! $c4            ! $c4 		  ! n/a      ! $c4               ! $04      
Joybutton    ! $c8            ! $c8               ! n/a      ! $c8               ! $08      bit6: joystick 1 fire
Joybuttondir ! $cc            ! $cc               ! n/a      ! $cc               ! $0c      
KeyDat       ! $d0            ! $d0               ! n/a	     ! $d0               ! $10      keyboard data
KeyStatus    ! $d4            ! $d4               ! n/a	     ! $d4               ! $14      keyboard status
SidDat       ! $d8            ! $d8               ! n/a	     ! $d8               ! $18      sid data
SidCommand   ! $dc            ! $dc               ! n/a	     ! $dc               ! $1c      sid command
CatMem       ! $e0            ! $e0               ! $00	     ! $e0               ! $20      read/write RAM (and increase addr counter)
CatAbort     ! $e4            ! $e4               ! $01	     ! $e4               ! $24      r: read reg w: clear addr counter(read) are both the same
CatControl   ! $e8            ! $e8               ! $02	     ! $e8               ! $28      r/w
CatOption    ! $ec            ! $ec               ! $03	     ! $ec               ! $2c      r:#7 reads version string w: #7 sets clock speed
CatStartA    ! $f0            ! $f0               ! $07	     ! $f0               ! $30      r: read normal  w: write indexed
CatStartB    ! $f4            ! $f4               ! $05	     ! $f4               ! $34      r: read indexed w: write normal
CatExtended  ! n/a            ! $f8               ! n/a      ! n/a               ! n/a      
CatIRQ       ! $fc            ! $fc               ! n/a      ! $fc               ! $3c      

mk3: Register $f8 (clockport $38) is not used.
mk4: Register $f8 is extended status/control register

*/

/*
Joystick functions
------------------
The JoyDat register contains information about the direction controls of
both joysticks:

bit 0 joystick 1 right
bit 1 joystick 1 left
bit 2 joystick 1 down
bit 3 joystick 1 up
bit 4 joystick 2 right
bit 5 joystick 2 left
bit 6 joystick 2 down
bit 7 joystick 2 up

A bit is 0 if the joystick is moved in the corresponding direction. The
mapping is "direct", no XORing (like in the Amiga) is needed. Connecting
an Amiga or Atari mouse may be possible, but since there are no hardware
counters, this would cause heavy CPU load because of the high rate that would
be necessary to poll the JoyDat register. 

The JoyButton register contains information about the firebuttons of a
digital joystick. The "second and third" buttons of some joysticks can
be read through the paddle registers in the SID chip.

bit 6 joystick 1 button (0=pressed)
bit 7 joystick 2 button (0=pressed)

Bits 0-5 of the register are unused and should be ANDed away for
compatibility with future versions of the Catweasel.

The button-lines of the joystick connectors can also be programmed to be
outputs, just like on the Amiga computer. The default value of the direction
bits after a reset is 0 (input). Bits 6 and 7 of the JoybuttonDir register
control the direction of the button lines.  It is recommended only to program
these lines to be outputs if it's absolutely sure that a hardware is connected
that accepts data through the Firebutton pin. Since a joystick shorts this
line to ground when the button is pressed, it may harm the Catweasel. If in
doubt, set both bits to 0. Emulator programmers could enable mapping of these
bits to the emulated machine with an option called "Dongle support", because
the only known hardware that needs the button line to be an output is dongles
on the Amiga. A warning should be displayed to the user when he's enabling
this option.
Bits 0-5 of the JoyButtonDir register are unused and should be written 0.
If the corresponding direction bits are set to 1, the state of the firebutton
line can be controlled by writing the desired value to the JoyButton register.
If only one of the two bits is set to output, the value that was written to
the input-bit will be stored in the Catweasel hardware, but not routed to the
joystick port.

There are two paddle inputs on each joystick port. The value of these paddles
can only be read if a SID chip is present in the socket of the Catweasel.
Just like in a C-64, only two of the four possible paddles can be read at a
time. The PaddleSelect register determines whether it's the two paddles in
port 1 or the other two paddles in port 2. The PaddleSelect register is a
toggle-register. That means the contents of the register do not matter; only
reading or writing the register will cause an action. Reading the register
gives a value that has no meaning, you can discard it. Reading causes the
analog switch to route the paddles of port 1 to the SID chip. Writing the
PaddleSelect register causes the analog switch to route the paddles of port 2
to the SID chip. It does not matter which value you write, but in order not
to cause any confusion, write a 0.
Please consult the SID section of this file on instructions how to read the
paddle registers of the SID chip.
*/

#define CW_REG_MK3_JOY_DATA	0xc0 	// upper 4bits: joystick2 lower 4bits: joystick1
#define CW_REG_MK3_C4		0xc4	// ?
#define CW_REG_MK3_C8		0xc8	// joystick1 bit6: fire
#define CW_REG_MK3_CC		0xcc	// ?

#define CW_BASE_MK3_JOY		0xc0
#define CW_OFFS_MK3_JOY_DATA	0x00 	// upper 4bits: joystick2 lower 4bits: joystick1
#define CW_OFFS_MK3_C4		0x04	// ?
#define CW_OFFS_MK3_C8		0x08	// joystick1 bit6: fire
#define CW_OFFS_MK3_CC		0x0c	// ?

/*
Keyboard functions
------------------
Amiga keyboards communicate through a serial interface with the computer.
Unfortunately, some keyboards don't behave like defined in the Amiga hardware
reference manual; sometimes they send more than 8 bits of data (trash bits,
they don't seem to have a meaning).

The keyboard interface is designed to provide maximum convenience for the
programmer. All timing-critical things are handled by the hardware, and trash-
bits are filtered automatically. If there is a keystroke value pending in the
data register, the keyboard is halted until the computer has read the value
out of the data register, so no keystroke will be lost. Even the necessary
bit-shifting is done by the hardware, so the value you read is a scancode
that is listed in the Amiga hardware reference manual. The MSB of the
scancode shows if it's a key-up or a key-down code.

The KeyStatus register only contains one valid bit. Bit 7 shows if data in the
KeyDat register is valid (bit 7=1). If this is the case, you can take all the
time you need to read the KeyDat register. After reading the data, you have
to tell the keyboard that it can continue sending data by writing 0 to the
KeyDat register. This will also set bit 7 of the KeyStatus register to 0.

Although every Amiga keyboard supports auto-resyncing, the Catweasel keyboard
interface can also do a manual resync. This is simply done by first writing
0 to KeyStatus, then writing 0 to KeyDat. This may be necessary for the early
A1000 keyboards that do not support auto-resyncing - rumors say that there
have been a few keyboards that lack this option.

Amiga keyboards support hot-plugging. As soon as the keyboard is powered up,
it sends an init-string. This can also be used to detect if a keyboard is
connected or not: Since the KeyStatus register is set to 0 on a reset, you
can tell that there's a keyboard connected with the first time that KeyStatus
bit 7 is 1.

Unlike PC keyboards, no Amiga keyboard supports recieving data. The only
information that is sent back to the keyboard is the handshaking.

Drawbacks of the interface: There is no IRQ for a pending keyboard value, and
the keyboard reset can only be detected from the string that is sent from the
keyboard prior to it's reset procedure.
*/
// keyboard
#define CW_REG_MK3_KBD_DATA		0xd0
#define CW_REG_MK3_KBD_STATUS		0xd4

#define CW_BASE_MK3_KBD			0xd0
#define CW_OFFS_MK3_KBD_DATA		0
#define CW_OFFS_MK3_KBD_STATUS		4

/*
SID functions
-------------
The SID chip is programmed through a bottleneck. Since the amount of data that
can be written to the chip is very limited, this will not cause any reduction
of performance. The clock rate of the SID is generated by a MOS 8701 PLL chip
that has also been used in the C-64 and the C-128 computers. This will ensure
a genuine sound. The filter capacitors of the SID are fixed at 470pF, because
higher values that have been chosen by Commodore in some versions of the C64
cause the filters to take effect much too early.

There are two registers for communication with the SID. The data register
contains the byte that is read or that will be written to the chip, and the
command register tells the SID bridge which register of the SID is accessed
next. The following table shows the bitmap of the command register:

bit 0: address bit 0
bit 1: address bit 1
bit 2: address bit 2
bit 3: address bit 3
bit 4: address bit 4
bit 5: 0= command write, 1= command read
bit 6: Pitch select. 0=PAL (0,98Mhz), 1=NTSC (1,02Mhz)
bit 7: unused, should be written 0.

SidCommand is a write-only register.

write-byte procedure:

Write the value that you want to send to the SID to the SIDdat register. Then
write the command to the SIDcommand register, and leave the SID bridge
alone for at least 1 microsecond. There is no way to tell whether the SID
bridge is still active or not other than waiting 1 microsecond. This can
either be accomplished by reading the data register twice and discarding the
data, or by allowing context-switching in a multitasking system (this will
most probably take longer than 1 microsecond).

If for example you want to set the volume of the SID to maximum, write
$0f to SIDdata, then write $18 to the command register and wait for 1
microsecond. This also sets the clock rate of the SID to the PAL frequency
of 0,98Mhz. Add 0x40 to the command value to choose NTSC frequency.

read-byte procedure:

If you want to read a SID register, write the register number plus 0x20 to
the command register, then wait for 1 microsecond, then the value is present
in the SIDdata register.

It is not recommended to change the SID frequency during playback. It takes
up to 10ms until the oscillator is stable at the new frequency, so it might
sound "defective" during the transition. Please mind that reading from SID
can also alter the frequency, so you should keep the chosen frequency in a
shadow-register of your access-routine and, if necessary, add 0x40 to the
command value.
*/

// mk3 style SID
#define CW_REG_MK3_SID_DATA		0xd8
#define CW_REG_MK3_SID_CMD		0xdC

// bits for CW_REG_MK3_SID_CMD
#define CW_SID_CMD_READ   0x20U
#define CW_SID_CMD_TIMER  0x40U
/*
mk4:
The SID command register now uses Bit 7 to choose which SID the command goes to. 
Make sure that existing software either always writes a 0 to that bit, or add a 
configuration utility for the user, so the target SID can easily be chosen.
*/
#define CW_SID_CMD_SID2   0x80U

#define CW_BASE_MK3_SID			0xd8
#define CW_OFFS_MK3_SID_DATA		0
#define CW_OFFS_MK3_SID_CMD		4

/*
mk3:

Floppy functions
----------------
The floppy functions of the Catweasel MK3 are a grown structure since summer
1996. The layout of some registers and the methods of how to start an action
in the floppy part had to be adapted to older versions in order to make
existing software for the Catweasel MK2 work. Please mind that the ISA
Catweasel has always been a MK1 design (even the Y2K edition!), and the MK2
has never been documented to the public before (it was only available as
Amiga-hardware), so a lot of features of the floppy part will be completely
new to you if you've programmed the ISA version before.
*/

/*
memory access
-------------
The catweasel memory of 128KBytes is accessed through a bottleneck. Two
registers are used to read and write the memory. Every access to the CatMem
register reads or writes the memory. After every access, the internal memory
pointer of the controller is increased by 1, no matter if it was a read or
a write access. The memory pointer is reset to 0 by writing 0 to the CatAbort
register. No other actions are performed with this write access.
There is no way to read the state of the memory pointer. However, some actions
depend on the state of this pointer, so you MUST hold a shadow register of the
pointer in your program/driver.

The memory is accessed from two sides, either from the computer side (that is
your side!), or from the controller side. Before you access the memory, make
sure that the floppy controller state machines do not access the memory,
otherwise you might mess up the memory pointer. There is only one!
*/

#define	CW_MK3_REG_CATMEM	0xe0	// (rw)  read/write RAM (and increase addr counter)
#define	CW_MK3_REG_CATABORT	0xe4	// (rw)  r: read reg w: clear addr counter; addr counter(r/w) are both the same


/*
0xe8 Status/control register

The CatControl register is different on reads and on writes. Writing to the
register controls some lines on the shugart bus, but reading the CatControl
register reads the status register. If you write a value to the CatControl
register, you should keep it in a shadow register, this makes modification of
single bits a lot easier.

CatControl on read accesses:

              ! MK3	! MK4	! MK1
--------------------------------------------------------------
reading       |bit 7	|bit 7	|bit 0  | (0=controller is currently reading=memory access!)
writing       |bit 6	|bit 6	|bit 1	| (0=controller is currently writing=memory access!)
Diskchange    |bit 5	|bit 5	|bit 5	| (0=disk has been removed from drive)
Diskread      |bit 4	|bit 4	|-	| (will most probably not be used, should be masked away)
WProtect      |bit 3	|bit 3	|bit 3  | (0=disk is write protected)
Track0        |bit 2	|bit 2	|bit 4	| (0=head is on outermost position)
Index         |bit 1	|bit 1	|bit 6	| (0=drive motor is currently on index position)
Density select|bit 0	|bit 0	|-	| (pin 2 of shugart bus)

mk3/mk4:
Please note that bits 5, 4, 3, 2 and 1 are only valid if the corresponding
drive is selected. If no drive is selected, these bits will be 1 (if one of
the bits is still 0, something must be terribly wrong :-)).

CatControl on write accesses:

              ! MK3	! MK4	! MK1
--------------------------------------------------------------
Step          |bit 7	|bit 7	|bit 0  | OC
Sideselect    |bit 6	|bit 6	|bit 3	| TP pin 32
Motor0        |bit 5	|bit 5	|bit 7	| TP pin 16 (0=motor on)
Dir           |bit 4	|bit 4	|bit 1	| OC
Sel0          |bit 3	|bit 3	|bit 5  | TP pin 12 (0=selected)
Sel1          |bit 2	|bit 2	|bit 6	| TP pin 14 (0=selected)
Motor1        |bit 1	|bit 1	|bit 4	| OC (0=motor on)
Density select|bit 0	|bit 0	|bit 6	| OC (pin 2 of shugart bus, if in doubt, set to 1)

Consult the specifications of the floppy drive vendor to get information about
how to step, and about the timing constraints of the signals. For example,
the track 0 signal will be valid no earlier than 4ms after the step on most
drives. That means, even if the drive supports a step rate of 3ms, stepping
towards track 0 must be done at a rate of 4ms if you don't know the exact
position of the read/write head.

*/

#define	CW_MK3_REG_CATCONTROL	0xe8	// (rw)  r/w

// for writing
#define CW_CATCONTROL_STEP		(1<<7)
#define CW_CATCONTROL_SIDESELECT	(1<<6)
#define CW_CATCONTROL_MOTOR0		(1<<5)
#define CW_CATCONTROL_DIR		(1<<4)
#define CW_CATCONTROL_SEL0		(1<<3)
#define CW_CATCONTROL_SEL1		(1<<2)
#define CW_CATCONTROL_MOTOR1		(1<<1)
#define CW_CATCONTROL_DENSITY		(1<<0)

// for reading
#define CW_CATCONTROL_ISREADING		(1<<7)
#define CW_CATCONTROL_ISWRITING		(1<<6)
#define CW_CATCONTROL_DISKCHANGE	(1<<5)
#define CW_CATCONTROL_DISKREAD		(1<<4)
#define CW_CATCONTROL_WPROTECT		(1<<3)
#define CW_CATCONTROL_TRACK0		(1<<2)
#define CW_CATCONTROL_INDEX		(1<<1)
//#define CW_CATCONTROL_DENSITY		(1<<0)


/*
0xec CatOption

step    CatOption
 0	bit6-7: clock (write 0x00,0x80,0xc0 for 14,28,54Mhz)
 1	bit7: write enable (write 0x80 to enable write) mk4 has length of writepulse in bit0-4 (standard: 0x0a)
 2	bit 5: 0:Forbid IRQs 1:Allow IRQs bit 6: 0:Standard read mode 1: MFM-predecode (only set on reads!) (writing any value to allows index storing!)
 3	(write 0x00 to forbid index storing)
 4     n/a (reserved)
 5     n/a (reserved)
 6     n/a (reserved)
 7     <ready to write raw data bytes>
 
 ??? r:#7 reads version string
 ??? mk3 vs mk4 ?
 
*/

#define	CW_MK3_REG_CATOPTION	0xec

/*
write enable procedure:
Write 0 to the CatAbort register, this resets the memory pointer to 0.
Read the Catmem register once, this sets the memory pointer to 1.
Write 128 (0x80) to the CatOption register.
Advance the memory pointer to 7 by reading CatMem 6 times.
The Floppy controller is now ready for a write access. Please note that
the write enable procedure is different from the procedure that is necessary
on the ISA version of the Catweasel.

allow/forbid index storing:
The index signal can be stored in the catweasel memory in the MSB of each
byte. The MSB (bit 7) cannot be used for anything else. Allowing or
forbidding index storing only affects read accesses. If index storing is
allowed, bit 7 of the memory value will be set to 1 if the index signal was
active at the time of the memory-write. This is very precise if standard-
read without pre-decode is chosen, and a little less precise on read with
MFM-predecode.

To allow index storing, set the memory pointer to 2 and write any value to
the CatOption register. This is the default after a reset. Please mind that
writing to CatOption with the memory pointer at 2 also alters the Predecode
and the IRQ enable bits (see further down). To forbid index storing, set the
memory pointer to 3 and write 0 to the CatOption register.

Setting clock speed:
To set the clockspeed with the CatOption register, reset the memory counter
to 0 by writing 0 to the CatAbort register. Writing 0 to CatOption will set
the clock rate to 14Mhz. Writing 128 (0x80) to CatOption will set the clock
rate to 28Mhz, and writing 192 (0xc0) to CatOption will set the clock speed
to 56Mhz (to be precise, 56.644Mhz).

Predecode, IRQ enable:
To set the Predecode and the IRQenable bits with the CatOption register, set
the memory pointer to 2 by writing 0 to the CatAbort register and reading the
CatMem register twice. Bits 5 and 6 of the CatOption register now control
Predecode and IRQ enable:
        Bit 5=0: Forbid IRQs
        Bit 5=1: Allow IRQs
        Bit 6=0: Standard read mode
        Bit 6=1: MFM-predecode in lower nibble of memory (only set on reads!)
        
Please mind that any write to the CatOption register with the memory pointer
at 2 also allows index storing. If ths is not desired, forbid index storing
again after setting Predecode and IRQenable to the required values.

If installed in an Amiga, the IRQ will be an IRQ6. Since there is no way to
tell if the IRQ comes from this controller, and the Amiga only has shared
IRQs, this is practically useless. However, the software timers of the
Amiga OS are very precise, and the Catweasel floppy functions have always
worked without IRQs since october 1996, so there's no need to activate this
option on Amiga computers. It's mainly intended for PCs, where software
timers are rather unprecise. 
*/

/*
0xf0 CatStartA
0xf4 CatStartB

Since the CatStart A and B registers have no dedicated function, the
following section only contains sequences that start a read or a write access
to or from the disk. The difference between the accesses is the start and the
end condition. There are some things that all disk acceses have in common:

- any access can be aborted by reading the CatAbort register. The value that
  has been read can be discarded of, it's random and has no meaning. The
  state of the memory pointer will not be changed by this access.
- any access is track-wise. Unlike most other floppy controllers, the
  Catweasel always accesses a whole track in one go. 
- a read access will be stopped automatically if the memory is full, no matter
  what other condition you have chosen to stop the access.
- a write access will always be stopped by a stop command in memory, no matter
  what other condition you have chosen to stop the access. As opposed to a
  read access, a write access is not stopped at the end of memory. Instead,
  the memory pointer wraps around and starts again at 0.

unconditional read:
To start reading a track without any conditions, read the CatStartA register.
The controller will start reading and store the data starting from the
position that the memory pointer is currently at.

unconditional write:
To start writing a track without any contidions, perform the write enable
procedure and then write 0 to the CatStartB register. Do not access the memory
between the write enable procedure and the CatStartB access, because this
would change the memory pointer, invalidating the internal write enable bit.
The write enable procedure is described at the end of this file.

indexed write:
Indexed write waits for an index pulse before starting to write, and ends at
the next index pulse. Since the index pulse is very long (between 1ms and
8ms), you have to know that the start- and endpoint of the write is at the
beginning of the pulse (falling edge). This is very precise on 3,5" drives,
the position where the start and the end of the write come together is usually
only a stream of less than 10 bits of undefined data. However, you should
not rely on the exact position of the data, because the index pulse may be in
a slightly different position with a different drive. The precision on other
drive sizes has not been checked (yet).

indexed read:
Indexed read waits for the beginning of an index pulse before starting to
read. As opposed to indexed write, the indexed read stops at the end of the
following index pulse, so there's a small overlap.
To start an indexed read, forbid index storing in memory, and read the
CatStartB register. 

Sync read:
This option waits for an MFM sync before starting to read. The expected sync
pattern is $4489 and cannot be changed. This only works at fixed bitrates of
500KBit (DD), 1MBit (HD) and 2MBit (ED), depending on the clock rate that has
been set. See further down to learn how to set the clock rates. Reading starts
immediately after a sync has been recognized in the bit stream. The sync that
has been recognized will not be stored in memory. However, most disk formats
have at least two sync patterns in a row, so it's unlikely (but not
impossible) that there's no sync pattern at the beginning of the data in
memory.
To start a sync read, allow index storing in memory and read the CatStartB
register.
*/

#define	CW_MK3_REG_CATSTARTA	0xf0	// (rw)  r: read normal  w: write indexed
#define	CW_MK3_REG_CATSTARTB	0xf4	// (rw)  r: read indexed w: write normal

/*
0xf8 CatExtended (mk4 only)

0xf8 (read)		extended status register (mk4 only)

					bit 0: Onboard select 0
					bit 1: onboard select 1
					bit 2: eject signal feedback

					bits 3-7 are unused at this point.
0xf8 (write)		extended control register

					bit 0:	start reading on index, end by software or memory overflow
					bit 1:	start writing on index, end by software or memory overflow
					bit 2:	eject
					bit 3:	DMA to onboard FDC, select 0
					bit 4:	DMA to onboard FDC, select 1
					bit 5:	Stop on watermark memory value
					bit 6:	Continuous read operation

					Bit 7 is currently experimental.

read $f8: extended status register

The "onboard select" signals are active-low. They serve as a window to the onboard controller. 
Your software can determine if the native controller is trying to access a floppy drive by 
checking the lower bits of this register. If either nothing happens or the white connector is 
not used, the lower bits will be 1. If a drive is selected, and you think the onboard controller 
should have access to the drive(s), you can react by setting the mux bit to 1 in register base+3.

Bits 0 and 1 of this register are latched. If since the last read-access the onboard controller 
has tried to access the floppy, this attempt is stored in these two bits. This way, you won't 
miss any attempt of the onboard controller, even if you're polling this register at low 
frequencies. Reading this register will erase the two bits, so you have to watch both bits on 
the first read!

The eject feedback bit gives an idea if there's a drive attached that's capable of auto-ejecting.
If the eject bit is 0 in the extended control register (that's $f8 on a write, see further down), 
but you're reading a 1 from bit 2 in register $f8, then there's either no drive on the bus that 
can auto-eject, or a second drive is shorting the signal, so it cannot be used.

CAUTION: Some drives leave this pin completely unconnected, so it might also be floating. Since 
a floating pin gives unpredictable values, you can "charge" the line by setting, and immediately 
unsetting the bit before running the described detection of auto-eject drives. This pre-charging 
of the line should be done without selecting a drive!

If the eject signal is shorted by another drive, then other auto-eject drives on the cable will 
always eject a disk when they're selected. The user will have to cut a line 1 on the floppy 
cable (that's the red line on most cables). You just need to compare the bits: Write 0 to the 
eject bit, get 0 in the status, then eject might be possible. If you get 1 in the status although 
you pre-charged, eject is impossible. Notice that you can only say for sure that eject is 
impossible, but a 100% identification of the auto-eject feature is not possible. 

If auto-eject is supported by the drive, it's done by selecting the drive (unset corresponding 
SEL bit in the control register), and setting the eject bit to 1. Set the eject bit back to 0 
after a certain period of time - I don't have any documentation, but one millisecond should be 
sufficient, at least it is with my Mitsubishi MF355F-3792ME (my drive is defective, the motor 
doesn't run any more, only makes strange noises - but eject works!).

write o $f8: extended control register

Write $01 into this register to start reading on the falling index edge. In this read mode, 
the controller will read until the memory is full, or until the software will abort the transfer.
The read can be done with or without MFM predecode, and with or without index storing.

Write $02 into this register to start writing on the falling index edge. In this write mode, the 
controller will write until the memory pointer reaches the end, until the MSB of a byte in 
memory is set, or until the software will abort the transfer.

If for some reason (no drive selected, no disk inserted, motor off) the start-condition (falling 
edge of index) does not occur, you can abort the wait-condition by reading the CatAbort register 
($e4).

The DMA bits will route the write stream of the Catweasel floppy controller to the read line of 
the onboard FDC. The onbaord FDC also has two select lines, and each of these bits allows the 
Catweasel to output information on the FDC's read line if it has selected that drive. You can 
select both lines at the same time if you want to.
This will let you transfer data from the Catweasel memory into the onbaord floppy controller, 
even if the Catweasel itself has not selected any disk drive (notice that you can start a 
Catweasel-floppy-write operation without selecting a drive, this might be the option you want 
to use if you're talking to the onboard FDC through this channel).

If the "stop on watermark" bit is set, then a read or write operation is automatically halted 
if the watermark value (see IRQ register bank) and the memory pointer are equal. This bit will 
stay set until you reset it "manually" - don't forget to reset this bit after you have done a 
watermark-stop operation, otherwise a read or write might stop too early, or won't even start 
for no obvious reason!
The "stop on watermark" condition might never be met if you're doing a continuous write operation
with a reload command in the data stream. The reload command resets the memory pointer to 0, 
and if this reload command is located before the watermark value, the watermark is never reached.

Before setting the "stop on watermark" bit, make sure to clear the watermark IRQ status bit in 
the IRQ register bank. The comparator for 17 bits is fairly big, so I have only implemented it 
once. The stop condition is routed from the IRQ status bit, therefore with bit 5 of floppy 
register $f8 set, and watermark IRQ status set, read or write operations cannot even be 
started!

The IRQ mask bit has no effect on the watermark stop. Bit 5 of floppy register $f8 is the mask 
for "watermark stop". 

With bit 6 set, the controller will not stop reading when the memory is full, but wrap around 
and continue reading at the beginning of the memory. Write operations are never stopped at the 
end of the memory, so if there's no termination byte or other stop condition, the controller 
will write forever. Notice that this is not true for the ISA version of the Catweasel, that 
version wll stop when the end of the memory is reached (please mind if you want to support all 
versions of the controller).

bit 7:	ASync Read (experimental!)

If you set bit 7 of floppy register $f8, the read prescaler is not synced to the read pulses. 
This will hopefully improve (but not fix) the precision problems that Jorge experienced, because one 
problem can' be solved that easy: The state machine that writes to the memory is still syncronized to 
the read pulses (has to be, otherwise the data hold time to memory will be violated), so it's not 
certain how many count pulses are lost during the write-to-memory time. The value to add will depend 
on the prescale value itself: On "top speed", it'll be constant, and the 28Mhz/14Mhz settings will 
produce fractions to add to the read values in order to average out the statistical error.

This bit will switch on the 15-bit counter in future cores, so don't spend too much time playing with 
it.

*/

#define	CW_MK4_REG_EXTSTATUS	0xf8	// (read)  mk4 only: extended status register
#define	CW_MK4_REG_EXTCONTROL	0xf8	// (write) mk4 only: extended control register

// for CW_MK4_REG_EXTSTATUS (read)
#define	CW_MK4_EXTSTATUS_SE0	0x01	// bit 0: Onboard select 0
#define	CW_MK4_EXTSTATUS_SE1	0x02	// bit 1: onboard select 1
#define	CW_MK4_EXTSTATUS_ESF	0x04	// bit 2: eject signal feedback
					// ??? bits 3-7 are unused at this point.
// for CW_MK4_REG_EXTCONTROL (write)
#define	CW_MK4_EXTCONTROL_READ	0x01	// bit 0: start reading on index, end by software or memory overflow
#define	CW_MK4_EXTCONTROL_WRITE	0x02	// bit 1: start writing on index, end by software or memory overflow
#define	CW_MK4_EXTCONTROL_EJECT	0x04	// bit 2: eject
#define	CW_MK4_EXTCONTROL_DMA0	0x08	// bit 3: DMA to onboard FDC, select 0
#define	CW_MK4_EXTCONTROL_DMA1	0x10	// bit 4: DMA to onboard FDC, select 1
#define	CW_MK4_EXTCONTROL_MARK	0x20	// bit 5: Stop on watermark memory value
#define	CW_MK4_EXTCONTROL_CONT	0x40	// bit 6: Continuous read operation
#define	CW_MK4_EXTCONTROL_BIT7	0x80	// ??? Bit 7 is currently experimental.

/*
0xfc CatIRQ

To find the IRQ that the Catweasel will use on the PCI slot, consult your PnP software manual.
To stop an IRQ, write 0 to the CatIRQ register.
*/

#define	CW_MK3_REG_CATIRQ	0xfc	//


/*
mk4:
Floppy write commands
---------------------
The write stream for writing a track mainly consists of delay values between the write pulses. 

Any byte with the MSB cleared is interpreted as a delay byte. It is loaded into a 7-bit counter that counts up at the frequency you have chosen. If the counter rolls over, the next byte is fetched, and a write pulse is initiated. In other words: The smaller the number, the longer the delay. Delay values of $7e and $7f might confuse and lockup the state machine, they should be avoided (no floppy drive can take pulses that fast anyway).

Previous versions of the Catweasel accepted any byte that has the MSB set as a termination byte. Please review existing software for this termination byte: The Catweasel MK4 will ONLY accept $ff as a termination byte!

Any command will load a value of $7d into the counter, so the execution of a command takes exactly the same time as a $7d delay. 

list of commands:

$ff	terminates the write process and goes to idle state
$c0	set MK3 IRQ - read the IRQ status for this in $c0/bit 0 of the IRQ register bank
$80	reload: The memory counter will be reset when this command is executed. Notice that
	the memory pointer is also incremented after every access, so the next byte to be
	fetched after this command is 1, not 0.
$81	Inhibit write pulses
$82	allow write pulses
$83	wait for next hardsector
$84	disable writeGate-signal to floppy
$85	(re-)enable WriteGate-signal to floppy

Commands $81 and $82 can be used to generate longer delays between two write pulses than the 7-bit counter allows. Pulses are generated at the beginning of every delay command, if allowed. Pulses are allowed by default. No pulse is generated if command bytes are executed. 

Command $83 waits with executing the next byte from memory until the next falling edge of an index signal occurs. This is only useful for hard-sectored disks, and should be used with the timer/comparator write option of timer 1 (otherwise the write-start position is not correct!).

Commands $84 and $85 can be used to only write a track partially. The WriteGate signal to the floppy tells the drive to switch to recording mode, and you have full control over when it's activated and when it's de-activated with these commands. With the default (MK3) write-enable procedure, the WGate line is active immediately with the start of the state machine. If you change the write-enable procedure to:

- Write 0 to the CatAbort register, this resets the memory pointer to 0.
- Read the Catmem register once, this sets the memory pointer to 1.
- Write $e0 to the CatOption register  (this is the change!)
- Advance the memory pointer to 7 by reading CatMem 6 times.

After this, the Floppy controller will be ready to start the write state machine, but will not assert the WGate line right away. It will also not generate any write pulses until the data stream in memory tells it to do so, so using $e0 in the write-enable procedure acts like having executes $81 and $84 before the start of the state machine.
You can use these commands to write single sectors of a hard-sectored disk by starting with the timer-comparator option (remember to execute the write enable procedure before!), then skip sectors with the $83 command (one for every sector), and then enable WGate and write pulses with the $82 and $85 commands.
The value $e0 that you're writing to the CatOption register ($ec) is also kind of an opcode. The value that was recommended for the MK3 was $80, and that enables WriteGate and write pulses on the MK4, so existing software should work fine with the new controller. A value of $e0 inhibits both WGate and WPulses. A value of $c0 inhibits WGate on the start of the write process, but allows write pulses (this might not be useful at all, because no device should react to a write pulse without WGate asserted, but hey, you never know...).
Last not least, a value of $a0 allows WGate, bis disables write pulses. Any of these disable values can be changed within the write stream. The opcode that you're writing to the CatOption register will only determine the state of the start-value.
*/

/*************************************************************************************************
  MK4 FIFO register bank (MK4 style SID with FIFO)
 *************************************************************************************************

To access this bank, write 0x01 to register base+3.

Remember to set the desired clock frequency for the SIDs before switching the register bank to 
the FIFO. This can either be done with a dummy write, or with a dummy read (both set the PAL/NTSC 
selection bit!). You can change the PAL/NTSC frequency in the MK3-compatible register while the 
Fifo is running. All other MK3 SID actions are suppressed while the FIFO is enabled, just the 
PAL/NTSC switch works. Don't forget to switch banks before/after you've accessed a register in 
a different bank! Avoid writing to the SIDdat register while the Fifo is working. It might mess 
up write-data from the Fifo to the SID. Reading the SIDdat register might be interesting, but 
doesn' really give valuable information (you can see data flying by on it's way into the SID).

0xc0 (read)       Read fifo  register
     (write)      write a byte into the FIFO
0xc4 (read only)  WRITE fifo: number of used words divided by 4. Stop writing to the FIFO when this value reaches 0xff.
0xc8 (read)       READ fifo: number of used words divided by 2.
     (write)      bit 0: FIFO transfer gate. 0 stops transfer to SID, 1 starts transfer to SID 
     		  bit 1: Turbo bit.
0xcc (read)       MSB is set when FIFO is empty, cleared when data is left in the FIFO
     (write)      any write to this register triggers a complete erase of the Fifo.
0xd0              idle cycles counter (highbyte)
0xd4              idle cycles counter (lowbyte)
0xd8              direct SID feedback byte
*/

/* The SID write FIFO size is 1024 bytes. */
#define MK4_SID_FIFO_SIZE 1024

#define CW_REG_FIFO_IN		0xc0		// (read)       Read fifo  register
#define CW_REG_FIFO_OUT		0xc0		// (write)      write a byte into the FIFO
#define CW_REG_FIFO_OUTPTR	0xc4		// (read)	WRITE fifo: number of used words divided by 4.
#define CW_REG_FIFO_INPTR	0xc8		// (read)       READ fifo: number of used words divided by 2.
#define CW_REG_FIFO_CTRL	0xc8		// (write)      bit 0: FIFO transfer gate. 0 stops transfer to SID, 1 starts transfer to SID bit 1: Turbo bit.
#define CW_REG_FIFO_STAT	0xcc		// (read)       MSB is set when FIFO is empty, cleared when data is left in the FIFO
#define CW_REG_FIFO_CLEAR	0xcc		// (write)      any write to this register triggers a complete erase of the Fifo.
#define CW_REG_FIFO_IDLEHI	0xd0		// idle cycles counter (highbyte)
#define CW_REG_FIFO_IDLELO	0xd4		// idle cycles counter (lowbyte)
#define CW_REG_FIFO_FEEDBACK 	0xd8		// direct SID feedback byte
// for CW_REG_FIFO_OUT
#define CW_FIFO_CMD_WRITE	0x80		// msb set and bit 6 cleared, bit 5 is sid number (next byte is value)
#define CW_FIFO_CMD_READ	0xc0		// msb set and bit 6 set    , bit 5 is sid number (next byte is read identifier)
#define CW_FIFO_SID0		0x00		// sid nr 1
#define CW_FIFO_SID1		0x20		// sid nr 2
#define CW_FIFO_CMD_WAIT	0x00		// msb cleared, bit 6 is multiplier
#define CW_FIFO_MUL_1		0x00		// multiplier 1 
#define CW_FIFO_MUL_64		0x40		// multiplier 64
// for CW_REG_FIFO_CTRL
#define CW_FIFO_CTRL_START	0x01
#define CW_FIFO_CTRL_STOP	0x00
#define CW_FIFO_CTRL_TURBO	0x02
// CW_REG_FIFO_STAT
#define CW_FIFO_STAT_EMPTY	0x80
#define CW_FIFO_STAT_FULL	0x00

/*
Syntax: There are three opcodes, "write byte", "read byte", and "wait". Write-byte and 
read-byte are two-byte commands:

The first byte of a "write byte" must have MSB set and bit 6 cleared to indicate the start of 
a write-byte commad. The lower 5 bits set the target register number of the SID, bit 5 sets 
the sid number.
The next byte holds the data that you want to write into that register.

You can do any number of byte-writes in a row. Each of them will take one cycle.

The first byte of a "read byte" must have MSB and bit 6 set to indicate the start of a 
read-byte commad. The lower 5 bits set the target register number of the SID, bit 5 sets 
the sid number.
The next byte holds the read identifier. This identifier is written to the read FIFO, and has 
no effect on the SID at all. You can choose this value freely in order to identify the byte 
that comes next. You will always get the identifier first, then the data byte that was read 
out of the SID. The contents of the read fifo can be read in register $c0. Before reading 
that register, make sure that there's data in the pipe: Register $c8 bust be greater than  0.

You can do any number of byte-reads in a row. Each of them will take one cycle.

The wait command is a one-byte command. It is indicated by a 0 in the MSB. Bit 6 has a special 
meaning, it is a multiplier for the lower six bits. If bit 6 is 0, the multiplier is 1. If 
bit 6 is 1, the multiplier is 64. The wait time is always in SID clock cycles plus one. Set the 
SID clock the known way - the FIFO cannot alter the clock speed.

Some examples of wait time:

0 will wait 1 cycle
1 will wait 2 cycles
2 will wait 3 cycles
..
..
62 will wait 63 cycles
63 will wait 64 cycles
64 will wait 64 cycles 
65 will wait 128 cycles 
66 will wait 192 cycles 
..
..
127 will wait 4096 cycles

Wait-time 63 will have a special meaning in the future, so I recommend not to use it. Feedback 
will also change, to it's not documented here.

turbo bit:
Set this bit to 1 to make all delay values 1 cycle long. This is to quickly flush the FIFO, but 
not miss any data that's supposed to be sent to the SID. Can either be used for fast-forwarding, 
or for changes in filters/voice muting to take effect quicker. There are three conditions that 
clear the turbo bit: 

1) The Fifo is empty, 
2) a delay value of 0x3f occurs in the data stream, 
3) the user writes 0 to the bit.

If a delay value of 0x3f is encountered, that delay will be processed normally, that means, 64 
cycles will be waited until the next byte is fetched from the FIFO.

idle cycles counter:
This counter is 16 bit wide, and it's counted up for every cycle that the FIFO is empty AND the 
last delay byte has been processed, so even if the Fifo is empty, the counter will only start 
counting for cycles that the FIFO state machine is idle.

Read the state of the idle-cycle counter in $d0 (highbyte) and $d4 (lowbyte). To reset the idle-
cycle counter to 0, write any value to one of these registers.
The idle cycles counter can be used to correct an unwanted empty state of the FIFO. If for 
example a multi-tasking operating system had tasks of higher priorities blocking the SID 
playback routine, the value of this register can be used to correct the timing. Just shorten 
future waits by the number of cycles that the counter is showing. The result will be that some 
notes will be played late, but the overall listening experience will most probably stay in-sync 
with the beat of the music.

direct feedback:
Any write to register $1f of any SID (both work) will be stored in the direct feedback register 
($d8) of this bank. The write to the SID is not actually done, but it still takes one cycle if 
executed from the fifo. The select line of the SID is only pulled low for SID registers 0 to 
$1e. If you have software that writes to $1f of the SID, you'll have to map it to a different 
register in order to cause the few-milliseconds-hold-effect.
*/

/*************************************************************************************************
   MK4 IO Register bank
 *************************************************************************************************

To access this register bank, write 129 (0x81) to base+3.

$c0: mouse Y1 (write to upper 6 bits also affect register $c8)
$c4: mouse X1 (write to upper 6 bits also affect register $cc)
$C8: mouse Y2 (write to upper 6 bits also affect register $c0)
$cc: mouse X2 (write to upper 6 bits also affect register $c4)
$d0: read: pot inputs to the joystickports (lower 4 bits)
$d0: write: Bits 0 and 1 are for Atari mouse selection (set to 1 for Atari)

*/

/*
The mouse registers can now be written just like in the Amiga. The upper six bits can be set by 
just writing to the registers, where both X and both Y registers are written at the same time 
(again, just like the JOYTEST register of the Amiga). The lower two bits cannot be set, because 
they directly reflect the state of the port signal lines.

Writing to $d0 will change the mode of the mouse counters for each port. Bit 0 is for port 1, 
and bit 1 is for port 2. If these "Atari select" bits are set, the mouse ports will accept a 
mouse from the Atari ST computer. Set the bits to 0 for Amiga mice and trackballs. Atari and 
Amiga mice can be mixed, each port has it's own mode.
*/

#define CW_REG_IO_MOUSEY1		0xc0
#define CW_REG_IO_MOUSEX1		0xc4
#define CW_REG_IO_MOUSEY2		0xc8
#define CW_REG_IO_MOUSEX2		0xcc
/*
The Pot inputs can only be used when there's no SID chips installed. If SID chips are installed, 
the value of the middle/right mouse buttons must be read from the POT registers of the SIDs.

This is due to the nature of the A/D converters inside the SIDs: They discharge a capacitor, 
and count the time until the capacitor is charged over a certain level again. If the SID is 
installed, you will only read toggeling values from the $d0 register.
You can use this as a feature: If you're reading toggeling values out of these bits, you can be 
sure that there's a SID installed.
*/
#define CW_REG_IO_POTIN			0xd0	// (read)	/* mk4 only: lower 4 bits=digital(!)status of pot lines if no sid installed */
#define CW_REG_IO_MOUSECTRL		0xd0	// (write)

#define CW_MOUSECTRL0_ATARI		0x01
#define CW_MOUSECTRL1_ATARI		0x02
#define CW_MOUSECTRL0_PULLUP		0x04
#define CW_MOUSECTRL1_PULLUP		0x08


/*************************************************************************************************
	IRQ Register Bank
 *************************************************************************************************/

#define CW_REG_IRQ_STATUS1		0xc0	// (read)
#define CW_REG_IRQ_STATUS2		0xc4	// (read)

#define CW_REG_IRQ_MASK1		0xc8	// 
#define CW_REG_IRQ_MASK2		0xcc	// 

#define CW_REG_IRQ_PTR0			0xd0	// (read) bit 0 is the MSB of the memory pointer (bit 16)
#define CW_REG_IRQ_PTR1			0xd4	// (read) bits 8 to 15 of the memory pointer
#define CW_REG_IRQ_PTR2			0xd8	// (read) bits 0 to 7 of the memory pointer

/*
To access the IRQ register bank, write 0xc1 to register base+3.

$C0 is the IRQ source status register on a read. A 1 in a bit indicates that the IRQ source 
signalled an IRQ since you last reset the bit. This status is not affected by the IRQ mask bits, 
you can always see the IRQ sources, regardless of the mask status.
You can also reset the IRQ status in this register by writing a 1 to the bit that you want to 
reset. Writing a 0 to the bit will leave it unchanged. Example: To clear all IRQ bits, write 
$ff to $c0. To only clear the SID_write_Fifo IRQ bit, write $10 to $c0.

	bit 0:	MK3 floppy IRQ
	bit 1:	Index IRQ. This IRQ is set whenever the falling edge of an index signal passed by.
	bit 2;	This bit is set whenever a floppy read or write operation has started
	bit 3:	This bit is set whenever a floppy read or write operation has ended
	bit 4:	joystick change 1
	bit 5:	joystick change 2
	bit 6:	Catweasel memory address equal ("watermark")
	bit 7:	keyboard IRQ


$c4 is the second IRQ status register:

	bit 0:	SID write fifo empty
	bit 1:	direct SID feedback
	bit 2:	timer 1 underrun
	bit 3:	timer 2 underrun
	bit 4:	Sid write fifo half empty: Set when the fifo contents are getting less then 512
		bytes, and if the fifo contents were above 768 bytes since the last IRQ.
		will NOT be set whan coming from the other direction, only on the transition from 512
		to 511 bytes.
		Automatically cleared when more than 768 bytes are in fifo.
	bit 5:	Sid read about to take place

Bit 0 is set whnever the Fifo is empty AND the fifo is running. It must be cleared manually by 
writing $01 to $c4. If you can use an automatic clear, for example when a certain fill-level is 
reached, let me now.

Bit 1 is set when a (virtual) write to register $1f of any SID chip is in progress. This write 
access can come either from the Fifo, or it can be made directly with the MK3-compatible 
registers; both will set this bit! The write to the SIDs is not passed to the chips, but it 
still takes one cycle when executed from the FIFO.

Bit 2 is set whenever timer 1 underruns. Both modes (one-shot or continuous mode) are supported. 
Same for bit 3, which applies to timer 2.

Bit 4 will only toggle automatically if the Fifo is enabled. If you disable the Fifo, the value 
in the IRQ register remains until you reset it by writing a 1 to bit 4 of $c0. This is a a real 
schmitt-trigger. The Fifo half empty IRQ is only set if the Fifo is running, and the contents 
have been above 768 bytes since the last IRQ. There is no IRQ when the Fifo is empty, and there 
is no IRQ on the transition 511->512->511. 


$c8 is the IRQ mask register to $c0. Write a 1 to each bit to route the IRQ source signal to the 
IRQ pin, or write a 0 to disable routing to the IRQ pin. The bit layout is the same as the $c0 
register. The mask register can also be read, so you do not have to keep track of which option 
has been enabled/disabled. Any routine that wants to enable/disable IRQs can first read the byte,
do the necessary bit operations, and write the value back to $c8. 

$cc is the IRQ mask register to $c4.

The floppy memory pointer can be read in these addresses of the IRQ register bank:

$d0: bit 0 is the MSB of the memory pointer (bit 16)
$d4: bits 8 to 15 of the memory pointer
$d8: bits 0 to 7 of the memory pointer

When reading, these three bytes represent the state of the memory pointer. When writing, you can 
set the "watermark" value where an IRQ shall happen. A one-strobe-transfer from the memory
pointer to the watermark register was skipped in order to save a bit of logic. 

timers
------

Every timer has a 16-bit counter, a 16-bit latch, a 16-bit reload value, and a control register.
The counter will always count backwards, and re-load itself with the value in the reload register. 
The state of the control register determines what happens next: Either the timer stops running 
(one-shot mode), or it starts over again (continuous mode).


timer 1

$dc:	8-bit prescaler for 28.322 Mhz clock (0=fastest, 255=slowest)
$e0:	read: timer 1 counter highbyte
	write: timer 1 control
		bit 0:	timer 1 run. (1=run, 0=stop)
		bit 1:	one-shot (1=one shot, 0=continuous run)
		bit 2:	clock source (0= prescaler, 1= SID clock)
		bit 3:	auto-start in index
		bit 4:	auto-stop on index
		bit 5:	start floppy-write on timer underrun
		bit 6:	transfer current timer state to latch (snapshot-strobe)
		bit 7:	transfer reload to timer coutner (forced reload-strobe)

$e4:	read: timer 1 counter lowbyte
	write: timer 1 latch/comparator triggers
		bit 0:	auto-latch on index
		bit 1:	auto-reload on index
		bit 2:	start floppy write on index if timer1 counter =>32768
		bit 3:	start floppy write on index if timer1 counter <32768
	
Bits 0 and 1 will always keep their values, bits 2 and 3 will auto-reset after the action has 
been triggered. Notice that bits 2 and 3 will also be auto-cleared if something else has 
triggered a write operation!

If bit 0 is set, the latch will automatically takeover the current value of the timer counter on 
every index pulse. Use this option in combination with the index IRQ to measure the average 
rotation time over several rotations.

If bit 1 is set, every index pulse will force the timer to reload on every index pulse. If the 
right values are chosen and the disk keeps spinning, a timer underrun will never occur, so this 
mode can be used to detect a non-spinning disk on drives that do not support the diskchange 
feature.

Bits 2 and 3 are to be used in combination with setting bit 1, and are mainly intended for use 
with hard-sectored disks. Hard-sectored disks use one index hole for every sector, and another 
index hole in the middle of two sector-holes to indicate the start of a track. Setting bits 1 
and 2 of this register in combination with the right timer settings will automatically find the 
shorter distance betwen the sector- and the track-index hole, and start writing when the counter 
did not cross the threshold value of 32768. This threshold is fixed. You'll have to tweak the 
reload and prescale value in order to adjust the trigger-time.

Setting bits 1 and 3 at the same time will look for a longer distance between two index pulses 
before starting to write. Not sure if this will be useful, but it's "almost free" in the logic, 
and you never know what kinds of disks we'll encounter in the future.

Don't set bits 2 and 3 at the same time, as it will cause a write operation independant of the 
timer. Any index pulse will trigger writing, and this is something that you can also triger with
a different command (-combination). Besides, I might want to use this combination to trigger 
something else, so just avoid it.	
	
$e8:	read: timer 1 latch highbyte
	write: timer 1 reload highbyte
$ec:	read: timer 1 latch lowbyte
	write: timer 1 reload lowbyte
	
timer 2	
	
$f0:	read: timer 2 counter highbyte
	write: timer 2 control
		bit 0:	timer 2 run. (1=run, 0=stop)
		bit 1:	one-shot (1=one shot, 0=continuous run)
		bit 2+3: clock source
		bit 6:	1 in this bit transfers current counter state to latch
		bit 7:	1 in this bit forces a reload (transfer from reload to counter register)
		
		clock source combinations:
		bit 2=0, bit 3=0:	prescaler of timer 2
		bit 2=1, bit 3=0:	SID clock
		bit 2=0, bit 3=1:	timer 1 underrun
		bit 2=1, bit 3=1:	currently unused
		
		Bits 4 and 5 are currently unused.
	
$f4:	read: timer 2 counter lowbyte
	write: timer 2 prescaler for 28.322Mhz
$f8:	read: timer 2 latch highbyte
	write: timer 2 reload highbyte
$fc:	read: timer 2 latch lowbyte
	write: timer 2 reload lowbyte

*/


/*
mk4:

When reading the keyboard scancode from the keydat register, you had to write a 0 to that 
same register in order to tell the keyboard that it can send the next byte on the MK3 
controller. This can be done on the MK4 by un-setting the IRQ status bit (writing 0x80 to $c0). 
Make sure not to do both write accesses, and to read the scancode _before_ unsetting the IRQ 
status bit because this might cause data loss!
*/
/*
The two joystick IRQs will be set whenever a joystick line changes. Joystick/mouse port IRQ1 
watches the four direction lines and the fire button (left mouse button), while josystick/mouse 
IRQ bit 2 only watches the four direction lines. If you need an IRQ on the change of the 
firebutton (left mouse button) of joystick/mouse 2, you'll have to program the PCI bridge to 
generate an IRQ (that line is not connected to the FPGA, but directly to the PCI bridge).
*/
/*
The SID read IRQ is an "early IRQ" - it alerts you when the next command in the FIFO pipe is a 
read command. It becomes active as soon as the last byte before the read command has been 
fetched. This is pretty much "just before the read" if the command before was a write, but it's 
fairly early if the command before the read is a delay, because the IRQ already becomes active 
when the delay is in progress. If you feel that the read IRQ comes too early, you can split the 
delay before the read command into two bytes. This will wait with the IRQ until the last delay
byte before the read command. If you feel that the IRQ comes too late, you can insert a 
dummy-read before the real read, and wait in the IRQ service routine for the "used read words" 
counter to reach 2 (=4 bytes) instead of 1 (=2 bytes). Since this IRQ is thrown before the 
action takes place, you *always* have to check if there's data in the read fifo!
*/
/*
The IRQ pin of the FPGA is connected to the data port of the PCI bridge. The status of the pin 
can be read in register base+7, bit 3 (FYI: this is also the Ready bit during configuration). 
This bit is 1 if there's no IRQ, and it's 0 if there's an IRQ (active-low line). 
Mind that you're dealing with two chips on the Catweasel MK4. The status and mask registers 
above $c0 in the Catweasel register space are in the FPGA, and that's connected to the PCI 
bridge with a few lines. One of them is the IRQ line that can be read back in register base+7, 
bit 3. Even if IRQs are un-masked in the FPGA (IRQ bank, register $c8), they are still masked 
away by the PCI bridge, and don't reach the host computer yet. To un-mask IRQs in the PCI bridge,
do these writes (mind the order!):

write 0x08 to register base+$2a to set active-low IRQ on bit 3 
Write 0x08 to register base+5 to enable IRQ on the PCI Aux port bit 3.

Make sure that you have an IRQ service routine setup _before_ you're enabling IRQs. Since the 
IRQ will most probably be shared with other IRQ sources on the PCI bus, your service routine 
first has to find out if the Catweasel was really the source of the IRQ. Check bit 3 of register 
base+7: If it's set, then this IRQ was not for you! Only if bit 3 of that register is 0, the 
FPGA has triggered an IRQ. Check registers $c0 and $c4 of the IRQ register bank to find out 
what reason the IRQ had in detail, then jump to the service (sub-)routine.
*/


/*
The 8-bit prescaler value is a divisor for the 28.322Mhz clock. The value that you're writing 
into this register cannot be read back, a read from $dc will always return 0, and a read 
from $f4 will return the counter lowbyte of timer 2. The output frequency in Mhz of the 
prescaler will be 28.322/(n+1). A value of 0 will result in an output frequency of 28.322Mhz, 
1 will output 14,161 Mhz, and 255 will output 110,63khz. The prescaler is not syncronized to 
anything, it's always free-running. Even a write to the prescaler does not reset the counter!
*/
/*
Reading from registers $e0 and $e4 will give a current snapshot of the timer. Notice that the 
timer continues running between the two read acceses for lowbyte and highbyte. You might get a 
rollover of the lowbyte after reading the highbyte, so reading these registers should either 
only be done to have a rough estimate, or fo 8-operations only. For precise reading of counter 
state, transfer the value to the latch, and read from there.
*/
/*
Writing to register $e0 sets the timer 1 control register. The control register cannot be read:

bit 0:	timer 1 run. (1=run, 0=stop)
bit 1:	one-shot (1=one shot, 0=continuous run)
bit 2:	clock source (0= prescaler, 1= SID clock)
bit 3:	auto-start in index
bit 4:	auto-stop on index
bit 5:	start floppy-write on timer underrun
bit 6:	transfer current timer state to latch (snapshot-strobe)
bit 7:	transfer reload to timer coutner (forced reload-strobe)

Notice that choosing the SID clock as source for the timer depends on the PAL/NTSC clock 
selection of the chip. PAL is 0,985Mhz, NTSC is 1,023Mhz.

Bits 3 and 4 will start/stop the timer automatically on a falling edge of an index signal. 
You can set both bits with one write cycle to the control register. This wil result in the 
timer waiting for an index signal, then run for a full revolution of the drive, and stop on 
the next falling edge of an index signal. This can be used for measuring the time between two 
index signals without corrupting the contents of the memory.

If bit 5 is set, the timer underrun will start a floppy write operation. In combination with bit 
3, this can be used to move the track gap to a different position than the index signal.

examples:
If you want to start a one-shot timer, set the time in the reload register, the prescaler value 
(if needed), and then $83 to register $e0. This will transfer the reload register to the timer 
counter (bit 7), tell the timer to stop after re-loading (bit 1), and start the action (bit 0).

If you want to create a snapshot of a running counter, write $41 (or $45, depending on the clock 
source)  to register $e0. Remember that you can always change the clock source, (de-)activate 
autostart/stop, and start floppy-write on underrun while the timer is running. Make sure that 
this is not happening accidently if you want to make a snapshot of the counter!

Shift reload->counter->latch contents: Write %11xxxxxx to $e0 (X depending on the other options 
you want - remember not to set/reset anything accidently!). This pattern will transfer the 
current counter state to the latch, and the contents of the reload register to the counter 
in one go. This can be used to set the contents of the latch register or the counter, even 
if the timer is not runnung. use this option to pre-set all register contents before starting 
comparator-triggered writes:

Timer 2 is a little less complex than timer 1, and it only has one control register in $f0, 
which is comparable to register $e0 for timer 1:

*/

#endif /* CWHARDWARE_USR_H */
