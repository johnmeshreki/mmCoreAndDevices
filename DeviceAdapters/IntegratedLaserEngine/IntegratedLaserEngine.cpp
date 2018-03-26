///////////////////////////////////////////////////////////////////////////////
// FILE:          IntegratedLaserEngine.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   IntegratedLaserEngine controller adapter
//
// Based off the AndorLaserCombiner adapter from Karl Hoover, UCSF
//

#include "ALC_REV.h"
#include "IntegratedLaserEngine.h"
#include "SingleILE.h"
#include "DualILE.h"
#include "ILEWrapper/ILEWrapper.h"
#include "Lasers.h"


// Properties
const char* const g_DeviceListProperty = "Device";
const char* const g_ResetDeviceProperty = "Reset device connection";

// Property values
const char* const g_Undefined = "Undefined";
const char* const g_PropertyOn = "On";
const char* const g_PropertyOff = "Off";


///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////

MODULE_API void InitializeModuleData()
{
  RegisterDevice( CSingleILE::g_DeviceName, MM::ShutterDevice, CSingleILE::g_DeviceDescription );
  RegisterDevice( CDualILE::g_DualDeviceName, MM::ShutterDevice, CDualILE::g_DualDeviceDescription );
  RegisterDevice( CDualILE::g_Dual700DeviceName, MM::ShutterDevice, CDualILE::g_Dual700DeviceDescription );
}

MODULE_API MM::Device* CreateDevice(const char* DeviceName)
{
  if ( DeviceName == 0 )
  {
    return 0;
  }

  if ( ( strcmp( DeviceName, CSingleILE::g_DeviceName ) == 0 ) )
  {
    // Single ILE
    CSingleILE* vIntegratedLaserEngine = new CSingleILE();
    return vIntegratedLaserEngine;
  }

  if ( ( strcmp( DeviceName, CDualILE::g_DualDeviceName ) == 0 ) )
  {
    // Dual ILE
    CDualILE* vIntegratedLaserEngine = new CDualILE( false );
    return vIntegratedLaserEngine;
  }

  if ( ( strcmp( DeviceName, CDualILE::g_Dual700DeviceName ) == 0 ) )
  {
    // Dual ILE 700
    CDualILE* vIntegratedLaserEngine = new CDualILE( true );
    return vIntegratedLaserEngine;
  }

  return 0;
}

MODULE_API void DeleteDevice(MM::Device* Device)
{
  delete Device;
}

///////////////////////////////////////////////////////////////////////////////
// Controller implementation
///////////////////////////////////////////////////////////////////////////////

CIntegratedLaserEngine::CIntegratedLaserEngine( const std::string& Description, int NbDevices ) :
  Initialized_( false ),
  ChangedTime_( 0.0 ),
  ILEWrapper_( nullptr ),
  ILEDevice_( nullptr ),
  Lasers_( nullptr ),
  ResetDeviceProperty_( nullptr )
{
  if ( NbDevices <= 0 )
  {
    throw std::logic_error( "Number of requested ILE devices null or negative" );
  }

  // Load the library
  ILEWrapper_ = LoadILEWrapper( this );

  InitializeDefaultErrorMessages();

  SetErrorText( ERR_PORTS_INIT, "Ports initialisation failed" );
  SetErrorText( ERR_ACTIVEBLANKING_INIT, "Active Blanking initialisation failed" );
  SetErrorText( ERR_LOWPOWERMODE_INIT, "Low Power mode initialisation failed" );
  SetErrorText( ERR_LASERS_INIT, "Lasers initialisation failed" );
  SetErrorText( ERR_INTERLOCK, "Interlock triggered" );
  SetErrorText( ERR_CLASSIV_INTERLOCK, "Class IV interlock triggered" );
  SetErrorText( ERR_DEVICE_NOT_CONNECTED, "Device reconnecting. Please wait." );
  SetErrorText( ERR_ACTIVEBLANKING_SET, "Setting active blanking failed" );
  SetErrorText( ERR_DEVICE_INDEXINVALID, "Device index invalid" );
  SetErrorText( ERR_DEVICE_CONNECTIONFAILED, "Connection to the device failed" );
  SetErrorText( ERR_LOWPOWERMODE_SET, "Setting low power mode failed" );
  SetErrorText( ERR_PORTS_SET, "Setting port failed" );

  // Create pre-initialization properties:
  // -------------------------------------

  // Description
  CreateStringProperty( MM::g_Keyword_Description, Description.c_str(), true );
 
  // Devices
  ILEWrapper_->GetListOfDevices( DeviceList_ );
  DevicesNames_.assign( NbDevices, g_Undefined );
  if ( NbDevices == 1 )
  {
    CreateDeviceSelectionProperty( 0, 0 );
  }
  else
  {
    for ( int vDeviceIndex = 0; vDeviceIndex < NbDevices; ++vDeviceIndex )
    {
      CreateDeviceSelectionProperty( vDeviceIndex + 1, vDeviceIndex );
    }
  }
  
  EnableDelay(); // Signals that the delay setting will be used
  UpdateStatus();
}

CIntegratedLaserEngine::~CIntegratedLaserEngine()
{
  // Unload the library
  UnloadILEWrapper();
}

void CIntegratedLaserEngine::CreateDeviceSelectionProperty( int DeviceID, int DeviceIndex )
{
  std::string vInitialDevice = g_Undefined;
  if ( !DeviceList_.empty() )
  {
    vInitialDevice = *( DeviceList_.begin() );
  }
  std::string vPropertyName = g_DeviceListProperty;
  if ( DeviceID > 0 )
  {
    vPropertyName += std::to_string( static_cast<long long>( DeviceID ) );
  }
  CPropertyActionEx* pAct = new CPropertyActionEx( this, &CIntegratedLaserEngine::OnDeviceChange, DeviceIndex );
  CreateStringProperty( vPropertyName.c_str(), vInitialDevice.c_str(), false, pAct, true );
  std::vector<std::string> vDevices;
  CILEWrapper::TDeviceList::const_iterator vDeviceIt = DeviceList_.begin();
  while ( vDeviceIt != DeviceList_.end() )
  {
    vDevices.push_back( *vDeviceIt );
    ++vDeviceIt;
  }
  SetAllowedValues( vPropertyName.c_str(), vDevices );
  DevicesNames_[DeviceIndex] = vInitialDevice;
}

bool CIntegratedLaserEngine::Busy()
{
  MM::MMTime vInterval = GetCurrentMMTime() - ChangedTime_;
  MM::MMTime vDelay( GetDelayMs()*1000.0 );
  if ( vInterval < vDelay )
  {
    return true;
  }
  else
  {
    return false;
  }
}

void CIntegratedLaserEngine::GetName(char* Name) const
{
  CDeviceUtils::CopyLimitedString( Name, GetDeviceName().c_str() );
}

int CIntegratedLaserEngine::Initialize()
{
  if ( Initialized_ )
  {
    return DEVICE_OK;
  }
  
  // Connecting to the device
  try
  {
    if ( !CreateILE() )
    {
      LogMessage( "CreateILE failed" );
      return DEVICE_NOT_CONNECTED;
    }
  }
  catch ( std::string& exs )
  {
    LogMessage( exs.c_str() );
    SetErrorText( DEVICE_LOCALLY_DEFINED_ERROR, exs.c_str() );
    //CodeUtility::DebugOutput(exs.c_str());
    return DEVICE_LOCALLY_DEFINED_ERROR;
  }

  // Reset device property
  CPropertyAction* vAct = new CPropertyAction( this, &CIntegratedLaserEngine::OnResetDevice );
  CreateStringProperty( g_ResetDeviceProperty, g_PropertyOff, true, vAct );
  std::vector<std::string> vEnabledValues;
  vEnabledValues.push_back( g_PropertyOn );
  vEnabledValues.push_back( g_PropertyOff );
  SetAllowedValues( g_ResetDeviceProperty, vEnabledValues );

  // Lasers
  IALC_REV_ILEPowerManagement* vLowPowerMode = ILEWrapper_->GetILEPowerManagementInterface( ILEDevice_ );
  IALC_REV_Laser2* vLaserInterface = ILEDevice_->GetLaserInterface2();
  IALC_REV_ILE* vILE = ILEDevice_->GetILEInterface();
  if ( vLaserInterface != nullptr )
  {
    try
    {
      Lasers_ = new CLasers( vLaserInterface, vLowPowerMode, vILE, this );
    }
    catch ( std::exception& vException )
    {
      std::string vMessage( "Error loading the Lasers. Caught Exception with message: " );
      vMessage += vException.what();
      LogMessage( vMessage );
      return ERR_LASERS_INIT;
    }
  }
  else
  {
    LogMessage( "Laser interface pointer invalid" );
  }
  
  // Ports
  int vRet = InitializePorts();
  if ( vRet != DEVICE_OK )
  {
    return vRet;
  }

  // Active Blanking
  vRet = InitializeActiveBlanking();
  if ( vRet != DEVICE_OK )
  {
    return vRet;
  }

  // Low Power Mode
  vRet = InitializeLowPowerMode();
  if ( vRet != DEVICE_OK )
  {
    return vRet;
  }

  Initialized_ = true;
  return DEVICE_OK;
}

int CIntegratedLaserEngine::Shutdown()
{
  delete Lasers_;
  Lasers_ = nullptr;
  DeleteILE();
  
  Initialized_ = false;
  return DEVICE_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Action interface
///////////////////////////////////////////////////////////////////////////////

int CIntegratedLaserEngine::OnDeviceChange( MM::PropertyBase* Prop, MM::ActionType Act, long DeviceIndex )
{
  if ( DeviceIndex >= DevicesNames_.size() )
  {
    return ERR_DEVICE_INDEXINVALID;
  }
  if ( Act == MM::BeforeGet )
  {
    Prop->Set( DevicesNames_[DeviceIndex].c_str() );    
  }
  else if ( Act == MM::AfterSet )
  {
    Prop->Get( DevicesNames_[DeviceIndex] );
  }
  return DEVICE_OK;
}

int CIntegratedLaserEngine::OnResetDevice( MM::PropertyBase* Prop, MM::ActionType Act )
{
  if ( ResetDeviceProperty_ == nullptr )
  {
    ResetDeviceProperty_ = Prop;
  }
  int vRet = DEVICE_OK;
  if ( Act == MM::AfterSet )
  {
    std::string vValue;
    Prop->Get( vValue );
    if ( vValue == g_PropertyOn )
    {
      // Disconnect from the ILE interface
      DisconnectILEInterfaces();
      Lasers_->UpdateILEInterface( nullptr, nullptr, nullptr );

      // Disconnect the device
      DeleteILE();

      // Reconnect the device
      try
      {
        if ( CreateILE() )
        {
          LogMessage( "CreateILE failed" );
          return ERR_DEVICE_CONNECTIONFAILED;
        }
      }
      catch ( std::string& exs )
      {
        vRet = DEVICE_LOCALLY_DEFINED_ERROR;
        LogMessage( exs.c_str() );
        SetErrorText( DEVICE_LOCALLY_DEFINED_ERROR, exs.c_str() );
        //CodeUtility::DebugOutput(exs.c_str());
        return vRet;
      }

      // Reconnect to ILE interface
      IALC_REV_ILEPowerManagement* vLowPowerMode = ILEWrapper_->GetILEPowerManagementInterface( ILEDevice_ );
      IALC_REV_Laser2* vLaserInterface = ILEDevice_->GetLaserInterface2();
      IALC_REV_ILE* vILE = ILEDevice_->GetILEInterface();
      Lasers_->UpdateILEInterface( vLaserInterface, vLowPowerMode, vILE );
      ReconnectILEInterfaces();

      Prop->Set( g_PropertyOff );
      MM::Property* pChildProperty = ( MM::Property* )Prop;
      pChildProperty->SetReadOnly( true );
    }
  }
  return vRet;
}

///////////////////////////////////////////////////////////////////////////////
// Shutter API
///////////////////////////////////////////////////////////////////////////////

int CIntegratedLaserEngine::SetOpen(bool Open)
{
  if ( Lasers_ != nullptr )
  {
    return Lasers_->SetOpen( Open );
  }
  return DEVICE_OK;
}

int CIntegratedLaserEngine::GetOpen(bool& Open)
{
  Open = false;
  if ( Lasers_ != nullptr )
  {
    Lasers_->GetOpen( Open );
  }
  return DEVICE_OK;
}

/**
 * ON for DeltaT milliseconds.  Other implementations of Shutter don't
 * implement this.  Is this perhaps because this blocking call is not
 * appropriate?
 */
int CIntegratedLaserEngine::Fire(double DeltaT)
{
  SetOpen( true );
  CDeviceUtils::SleepMs( (long)( DeltaT + .5 ) );
  SetOpen( false );
  return DEVICE_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Public helper functions
///////////////////////////////////////////////////////////////////////////////

void CIntegratedLaserEngine::LogMMMessage( std::string Message, bool DebugOnly )
{
  LogMessage( Message, DebugOnly );
}

MM::MMTime CIntegratedLaserEngine::GetCurrentTime()
{
  return GetCurrentMMTime();
}

void CIntegratedLaserEngine::CheckAndUpdateLasers()
{
  if ( Lasers_ != nullptr )
  {
    Lasers_->CheckAndUpdateLasers();
  }
}

void CIntegratedLaserEngine::ActiveClassIVInterlock()
{
  if ( ResetDeviceProperty_ != nullptr )
  {
    MM::Property* pChildProperty = ( MM::Property* )ResetDeviceProperty_;
    pChildProperty->SetReadOnly( false );
  }
}

void CIntegratedLaserEngine::UpdatePropertyUI( const char* PropertyName, const char* PropertyValue )
{
  GetCoreCallback()->OnPropertyChanged( this, PropertyName, PropertyValue );
}
