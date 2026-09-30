/*
///////////////////////////////////////////////////////////////////////////////
// 
// class ParaPortDll
//
// copyright (c) 2002, 2003 by Paul R. ADAM
// all rights reserved
// read the "http://www.ParaPort.net/TermsOfUse.html"
//
// more information on "http://www.ParaPort.net"
//
///////////////////////////////////////////////////////////////////////////////
*/

#ifndef ParaPortDll_h
#define ParaPortDll_h

#include <windows.h>

#include "ParaPort.h"

#pragma pack( 1 )

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

 BOOL                        closePort( HANDLE Handle );

 BOOL                        executeCycle( HANDLE Handle, PARAPORT_CYCLE* ParaPortCycle, int Count );

 BOOL                        getPortInfo( HANDLE Handle, PARAPORT_INFO* Info );

 HANDLE                      openPort( const char* PortName );

#ifdef __cplusplus
}
#endif  /* __cplusplus */


 typedef BOOL                ( *ParaPort_closePort     ) ( HANDLE Handle );

 typedef BOOL                ( *ParaPort_executeCycle  ) ( HANDLE Handle, PARAPORT_CYCLE* ParaPortCycle, int Count );
 
 typedef BOOL                ( *ParaPort_getPortInfo   ) ( HANDLE Handle, PARAPORT_INFO* Info );

 typedef HANDLE              ( *ParaPort_openPort      ) ( const char* PortName );

#ifdef __cplusplus

///////////////////////////////////////////////////////////////////////////////
class ParaPortDll
{

///////////////////////////////////////////////////////////////////////////////
// public interface:
public:

 BOOL                        closePort( HANDLE Handle )
                                                 {
                                                  if ( !_closePort )
                                                  {
                                                   ::SetLastError( PARAPORT_ERROR_LIBRARY_NOT_LOADED );
                                                   return FALSE;
                                                  }
                                                  return _closePort( Handle );
                                                 }

 static void                 deleteSingleton( )  { initSingleton( true ); }

 BOOL                        executeCycle( HANDLE Handle, PARAPORT_CYCLE* Cycle, int Count = 1 )
                                                 {
                                                  if ( !_executeCycle )
                                                  {
                                                   ::SetLastError( PARAPORT_ERROR_LIBRARY_NOT_LOADED );
                                                   return FALSE;
                                                  }
                                                  return _executeCycle( Handle, Cycle, Count );
                                                 }

 BOOL                        getPortInfo( HANDLE Handle, PARAPORT_INFO* Info )
                                                 {
                                                  if ( !_getPortInfo )
                                                  {
                                                   ::SetLastError( PARAPORT_ERROR_LIBRARY_NOT_LOADED );
                                                   return FALSE;
                                                  }
                                                  return _getPortInfo( Handle, Info );
                                                 }

 static ParaPortDll*         getSingleton( )     { return initSingleton( false ); }

 BOOL                        loadLibrary( const char* FileName )
                                                 {
                                                  _Instance = ::LoadLibrary( FileName );
                                                  if ( !_Instance )
                                                   return FALSE;
                                                  _closePort     = ( ParaPort_closePort     ) ::GetProcAddress( _Instance, "closePort" );
                                                  _executeCycle  = ( ParaPort_executeCycle  ) ::GetProcAddress( _Instance, "executeCycle" );
                                                  _getPortInfo   = ( ParaPort_getPortInfo   ) ::GetProcAddress( _Instance, "getPortInfo" );
                                                  _openPort      = ( ParaPort_openPort      ) ::GetProcAddress( _Instance, "openPort" );
                                                  return TRUE;
                                                 }

 HANDLE                      openPort( const char* PortName )
                                                 {
                                                  if ( !_openPort )
                                                  {
                                                   ::SetLastError( PARAPORT_ERROR_LIBRARY_NOT_LOADED );
                                                   return INVALID_HANDLE_VALUE;
                                                  }
                                                  return _openPort( PortName );
                                                 }

///////////////////////////////////////////////////////////////////////////////
// implementation:
protected:

// constructor
                             ParaPortDll( )      : _Instance( NULL ) 
                                                 , _closePort( NULL )
                                                 , _executeCycle( NULL )
                                                 , _getPortInfo( NULL )
                                                 , _openPort( NULL )
                                                 { }
                            ~ParaPortDll( )      {
                                                  if ( _Instance )
                                                   ::FreeLibrary( _Instance );
                                                 }

 static ParaPortDll*         initSingleton( bool Delete )
                                                 {
                                                  static ParaPortDll* Singleton = NULL;
                                                  if ( Delete )
                                                  {
                                                   if ( Singleton )
                                                   {
                                                    delete Singleton;
                                                    Singleton = NULL;
                                                   }
                                                  }
                                                  else
                                                  {
                                                   if ( !Singleton )
                                                    Singleton = new ParaPortDll;
                                                  }
                                                  return Singleton;
                                                 }

 HMODULE                     _Instance;
 ParaPort_closePort          _closePort;
 ParaPort_executeCycle       _executeCycle;
 ParaPort_getPortInfo        _getPortInfo;
 ParaPort_openPort           _openPort;

};

#endif /* __cplusplus */

#pragma pack( )

#endif /* ParaPortDll_h */

/*
///////////////////////////////////////////////////////////////////////////////
*/