/*
///////////////////////////////////////////////////////////////////////////////
// 
// class ParaPortCycle
//
// copyright (c) 2002, 2003 by Paul R. ADAM
// all rights reserved
// read the "http://www.ParaPort.net/TermsOfUse.html"
//
// more information on "http://www.ParaPort.net"
//
///////////////////////////////////////////////////////////////////////////////
*/

#ifndef ParaPortCycle_h
#define ParaPortCycle_h

#include <windows.h>

#include "ParaPort.h"

#pragma pack( 1 )

#ifdef __cplusplus

///////////////////////////////////////////////////////////////////////////////
class ParaPortCycle : public PARAPORT_CYCLE
{

///////////////////////////////////////////////////////////////////////////////
// public interface:
public:

// constructor
                             ParaPortCycle( )    { clear( ); }

 void                        clear( )            {
                                                  Control     = Data     = Status     = 0x00;
                                                  MaskControl = MaskData = MaskStatus = 0x00;
                                                  RepeatFactor = 0x00;
                                                 }

///////////////////////////////////////////////////////////////////////////////
// access to data byte

 UCHAR                       getData( ) const    { return Data; }
 void                        setData( UCHAR Byte )
                                                 {
                                                  Data = Byte;
                                                  MaskData = 0xFF;
                                                 }
 void                        setData( UCHAR Byte, UCHAR Mask )
                                                 {
                                                  Data = Byte;
                                                  MaskData = Mask;
                                                 }

 bool                        isDataInput( )      { return Control & PARAPORT_MASK_CONTROL_DIRECTION ? true : false; }
 bool                        isDataOutput( )     { return !isDataInput( ); }
 
 void                        setDataInput( )     { Control |= PARAPORT_MASK_CONTROL_DIRECTION; }
 void                        setDataOutput( )    { Control &= ~PARAPORT_MASK_CONTROL_DIRECTION; }
 
///////////////////////////////////////////////////////////////////////////////
// access to control byte

 void                        clearControl( )     {
                                                  clearInit( );
                                                  clearLineFeed( );
                                                  clearSelectIn( );
                                                  clearStrobe( );
                                                 }

 void                        setControl( )       {
                                                  setInit( );
                                                  setLineFeed( );
                                                  setSelectIn( );
                                                  setStrobe( );
                                                 }

 void                        setControl( UCHAR Byte )
                                                 {
                                                  Control = Byte & PARAPORT_MASK_CONTROL;
                                                  MaskControl = PARAPORT_MASK_CONTROL;
                                                 }

 void                        clearInit( )        {
                                                  Control &= ~PARAPORT_MASK_CONTROL_INIT;
                                                  MaskControl |= PARAPORT_MASK_CONTROL_INIT;
                                                 }
 void                        setInit( )          {
                                                  Control |= PARAPORT_MASK_CONTROL_INIT;
                                                  MaskControl |= PARAPORT_MASK_CONTROL_INIT;
                                                 }

 void                        clearLineFeed( )    {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_LINEFEED;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_LINEFEED;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_LINEFEED;
                                                 }
 void                        setLineFeed( )      {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_LINEFEED;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_LINEFEED;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_LINEFEED;
                                                 }

 void                        clearSelectIn( )    {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_SELECTIN;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_SELECTIN;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_SELECTIN;
                                                 }
 void                        setSelectIn( )      {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_SELECTIN;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_SELECTIN;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_SELECTIN;
                                                 }

 void                        clearStrobe( )      {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_STROBE;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_STROBE;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_STROBE;
                                                 }
 void                        setStrobe( )        {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  Control |= PARAPORT_MASK_CONTROL_STROBE;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  Control &= ~PARAPORT_MASK_CONTROL_STROBE;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                  MaskControl |= PARAPORT_MASK_CONTROL_STROBE;
                                                 }


///////////////////////////////////////////////////////////////////////////////
// access to status byte

 UCHAR                       getStatus( ) const  { return Status & PARAPORT_MASK_STATUS; }

 bool                        isStatusAcknoledge( )
                                                 {
                                                  return Status & PARAPORT_MASK_STATUS_ACKNOWLEDGE ? true : false;
                                                 }
 bool                        isStatusBusy( )     {
#ifdef PARAPORT_NO_HARDWARE_INVERSION
                                                  return Status & PARAPORT_MASK_STATUS_BUSY ? true : false;
#else // PARAPORT_NO_HARDWARE_INVERSION
                                                  return Status & PARAPORT_MASK_STATUS_BUSY ? false : true;
#endif // PARAPORT_NO_HARDWARE_INVERSION
                                                 }
 bool                        isStatusError( )    { return Status & PARAPORT_MASK_STATUS_ERROR ? true : false; }
 bool                        isStatusPaperEnd( ) { return Status & PARAPORT_MASK_STATUS_PAPEREND ? true : false; }
 bool                        isStatusSelect( )   { return Status & PARAPORT_MASK_STATUS_SELECT ? true : false; }

///////////////////////////////////////////////////////////////////////////////
// modify masks
 void                        addMaskControl( UCHAR Mask )
                                                 { MaskControl |= ( Mask & PARAPORT_MASK_CONTROL ); }
 void                        removeMaskControl( UCHAR Mask )
                                                 { MaskControl &= ~( Mask & PARAPORT_MASK_CONTROL ); }
 void                        setMaskControl( UCHAR Mask )
                                                 { MaskControl = ( Mask & PARAPORT_MASK_CONTROL ); }

 void                        addMaskData( UCHAR Mask )
                                                 { MaskData |= ( Mask & PARAPORT_MASK_DATA ); }
 void                        removeMaskData( UCHAR Mask )
                                                 { MaskData &= ~( Mask & PARAPORT_MASK_DATA ); }
 void                        setMaskData( UCHAR Mask )
                                                 { MaskData = ( Mask & PARAPORT_MASK_DATA ); }

 void                        addMaskStatus( UCHAR Mask )
                                                 { MaskStatus |= ( Mask & PARAPORT_MASK_STATUS ); }
 void                        removeMaskStatus( UCHAR Mask )
                                                 { MaskStatus &= ~( Mask & PARAPORT_MASK_STATUS ); }
 void                        setMaskStatus( UCHAR Mask )
                                                 { MaskStatus = ( Mask & PARAPORT_MASK_STATUS ); }

///////////////////////////////////////////////////////////////////////////////
// access to repeat factor

 UCHAR                       getRepeatFactor( ) const
                                                 { return RepeatFactor; }
 void                        setRepeatFactor( UCHAR Byte )
                                                 { RepeatFactor = Byte; }

};

#endif /* __cplusplus */

#pragma pack( )

#endif /* ParaPortCycle_h */

/*
///////////////////////////////////////////////////////////////////////////////
*/