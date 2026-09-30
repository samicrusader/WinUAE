/*
///////////////////////////////////////////////////////////////////////////////
// 
// C interface of ParaPort
//
// copyright (c) 2002, 2003 by Paul R. ADAM
// all rights reserved
// read the "http://www.ParaPort.net/TermsOfUse.html"
//
// more information on "http://www.ParaPort.net"
//
///////////////////////////////////////////////////////////////////////////////
*/

#ifndef ParaPort_h
#define ParaPort_h

#pragma pack( 1 )

/*
///////////////////////////////////////////////////////////////////////////////
// interface for executeCycle
*/

#define                                PARAPORT_MASK_DATA_D0                   0x01
#define                                PARAPORT_MASK_DATA_D1                   0x02
#define                                PARAPORT_MASK_DATA_D2                   0x04
#define                                PARAPORT_MASK_DATA_D3                   0x08
#define                                PARAPORT_MASK_DATA_D4                   0x10
#define                                PARAPORT_MASK_DATA_D5                   0x20
#define                                PARAPORT_MASK_DATA_D6                   0x40
#define                                PARAPORT_MASK_DATA_D7                   0x80
#define                                PARAPORT_MASK_DATA                      0xFF

#define                                PARAPORT_MASK_CONTROL_STROBE            0x01
#define                                PARAPORT_MASK_CONTROL_LINEFEED          0x02
#define                                PARAPORT_MASK_CONTROL_INIT              0x04
#define                                PARAPORT_MASK_CONTROL_SELECTIN          0x08
#define                                PARAPORT_MASK_CONTROL_DIRECTION         0x20      /* only for internal use */
#define                                PARAPORT_MASK_CONTROL                   ( PARAPORT_MASK_CONTROL_INIT     | \
                                                                                 PARAPORT_MASK_CONTROL_LINEFEED | \
                                                                                 PARAPORT_MASK_CONTROL_SELECTIN | \
                                                                                 PARAPORT_MASK_CONTROL_STROBE   )

#define                                PARAPORT_MASK_STATUS_ERROR              0x08
#define                                PARAPORT_MASK_STATUS_SELECT             0x10
#define                                PARAPORT_MASK_STATUS_PAPEREND           0x20
#define                                PARAPORT_MASK_STATUS_ACKNOWLEDGE        0x40
#define                                PARAPORT_MASK_STATUS_BUSY               0x80
#define                                PARAPORT_MASK_STATUS                    ( PARAPORT_MASK_STATUS_ACKNOWLEDGE | \
                                                                                 PARAPORT_MASK_STATUS_BUSY        | \
                                                                                 PARAPORT_MASK_STATUS_ERROR       | \
                                                                                 PARAPORT_MASK_STATUS_PAPEREND    | \
                                                                                 PARAPORT_MASK_STATUS_SELECT      )

typedef struct
{
 UCHAR Data; 
 UCHAR Status;
 UCHAR Control;
 UCHAR MaskData;
 UCHAR MaskStatus;
 UCHAR MaskControl;
 UCHAR RepeatFactor; 
}
PARAPORT_CYCLE; 

/*
///////////////////////////////////////////////////////////////////////////////
// interface for getPortInfo
*/

typedef unsigned char*  PARAPORT_ADDRESS;

typedef struct
{
 PARAPORT_ADDRESS PortAddress; 
}
PARAPORT_INFO; 

/*
///////////////////////////////////////////////////////////////////////////////
// interface for error handling
*/

#define PARAPORT_ERROR                           ( 0x20000000 )
#define PARAPORT_ERROR_INTERNAL_1                ( PARAPORT_ERROR | 1 )
#define PARAPORT_ERROR_INTERNAL_2                ( PARAPORT_ERROR | 2 )
#define PARAPORT_ERROR_INTERNAL_3                ( PARAPORT_ERROR | 3 )
#define PARAPORT_ERROR_INVALID_HANDLE            ( PARAPORT_ERROR | 4 )
#define PARAPORT_ERROR_INVALID_PORTNAME          ( PARAPORT_ERROR | 5 )
#define PARAPORT_ERROR_LIBRARY_NOT_IMPLEMENTED   ( PARAPORT_ERROR | 6 )
#define PARAPORT_ERROR_LIBRARY_NOT_LOADED        ( PARAPORT_ERROR | 7 )

#pragma pack( )

#endif /* ParaPort_h */

/*
///////////////////////////////////////////////////////////////////////////////
*/