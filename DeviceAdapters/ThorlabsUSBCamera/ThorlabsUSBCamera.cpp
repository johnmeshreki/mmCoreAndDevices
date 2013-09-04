///////////////////////////////////////////////////////////////////////////////
// FILE:          ThorlabsUSB.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Device adapter for Thorlabs USB cameras DCU223M, DCU223C, 
//				      DCU224M, DCU224C, DCC1545M, DCC1645C, DCC1240M, DCC1240C.
//				      Has been developed and tested with the DCC1545M, based on the 
//				      source code of the DemoCamera device adapter
//                
// AUTHOR:        Christophe Dupre, christophe.dupre@gmail.com, 09/25/2012
//				      Updated to support DC3240C features, Nenad Amodaj, 09/2013
//
// COPYRIGHT:     University of California, San Francisco, 2006
// LICENSE:       This file is distributed under the BSD license.
//                License text is included with the source distribution.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//                IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//                CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//                INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.

#include "ThorlabsUSBCamera.h"
#include "ModuleInterface.h"
#include "uc480.h"
#include <cstdio>
#include <string>
#include <math.h>
#include <sstream>
#include <algorithm>
#include <iostream>

#include <fstream>
#include <boost/progress.hpp>
using boost::timer;

using namespace std;

// External names used used by the rest of the system
// to load particular device from the "ThorlabsUSBCamera.dll" library
const char* g_CameraDeviceName = "ThorCam";

// constants for naming pixel types (allowed values of the "PixelType" property)
const char* g_PixelType_8bit = "8bit";
const char* g_PixelType_16bit = "16bit";

// TODO: linux entry code

// windows DLL entry code
#ifdef WIN32
BOOL APIENTRY DllMain( HANDLE /*hModule*/, 
                      DWORD  ul_reason_for_call, 
                      LPVOID /*lpReserved*/
                      )
{
   switch (ul_reason_for_call)
   {
   case DLL_PROCESS_ATTACH:
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
   case DLL_PROCESS_DETACH:
      break;
   }
   return TRUE;
}
#endif





///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////

/**
 * List all suppoerted hardware devices here
 * Do not discover devices at runtime.  To avoid warnings about missing DLLs, Micro-Manager
 * maintains a list of supported device (MMDeviceList.txt).  This list is generated using 
 * information supplied by this function, so runtime discovery will create problems.
 */
MODULE_API void InitializeModuleData()
{
   AddAvailableDeviceName(g_CameraDeviceName, "Thorlabs DCx USB Camera");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == 0)
      return 0;

   // decide which device class to create based on the deviceName parameter
   if (strcmp(deviceName, g_CameraDeviceName) == 0)
   {
      // create camera
      return new ThorlabsUSBCam();
   }

   // ...supplied name not recognized
   return 0;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}

///////////////////////////////////////////////////////////////////////////////
// ThorlabsUSBCam implementation
// ~~~~~~~~~~~~~~~~~~~~~~~~~~

/**
* ThorlabsUSBCam constructor.
* Setup default all variables and create device properties required to exist
* before intialization. In this case, no such properties were required. All
* properties will be created in the Initialize() method.
*
* As a general guideline Micro-Manager devices do not access hardware in the
* the constructor. We should do as little as possible in the constructor and
* perform most of the initialization in the Initialize() method.
*/
ThorlabsUSBCam::ThorlabsUSBCam() :
   CCameraBase<ThorlabsUSBCam> (),
   initialized_(false),
   bitDepth_(8),
   roiX_(0),
   roiY_(0),
   sequenceStartTime_(0),
	binSize_(1),
   nComponents_(1),
   cameraBuf(0),
   cameraBufId(0),
   hEvent(0),
   fps(0.0)
{

   // call the base class method to set-up default error codes/messages
   InitializeDefaultErrorMessages();
   readoutStartTime_ = GetCurrentMMTime();
   thd_ = new MySequenceThread(this);

}

/**
* ThorlabsUSBCam destructor.
* If this device used as intended within the Micro-Manager system,
* Shutdown() will be always called before the destructor. But in any case
* we need to make sure that all resources are properly released even if
* Shutdown() was not called.
*/
ThorlabsUSBCam::~ThorlabsUSBCam()
{
   StopSequenceAcquisition();
   delete thd_;
}

/**
* Obtains device name.
* Required by the MM::Device API.
*/
void ThorlabsUSBCam::GetName(char* name) const
{
   // Return the name used to refer to this device adapter
   CDeviceUtils::CopyLimitedString(name, g_CameraDeviceName);
}

/**
* Initializes the hardware.
* Required by the MM::Device API.
* Typically we access and initialize hardware at this point.
* Device properties are typically created here as well, except
* the ones we need to use for defining initialization parameters.
* Such pre-initialization properties are created in the constructor.
* (This device does not have any pre-initialization properties)
*/
int ThorlabsUSBCam::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   // set property list
   // -----------------

   // CameraName
   int nRet = CreateProperty(MM::g_Keyword_CameraName, "Thorlabs DCx Camera", MM::String, true);
   assert(nRet == DEVICE_OK);

   // initialize Camera
   camHandle_ = (HCAM) 0; // open next camera
   nRet = is_InitCamera(&camHandle_, NULL); // init camera - no window handle for live required
   if (nRet != IS_SUCCESS)
      return nRet;

   CAMINFO camInfo;
   nRet = is_GetCameraInfo(camHandle_, &camInfo);
   if (nRet != IS_SUCCESS)
      return nRet;

   nRet = is_GetSensorInfo(camHandle_, &sensorInfo);
   if (nRet != IS_SUCCESS)
      return nRet;

   // set display mode
   nRet = is_SetDisplayMode(camHandle_, IS_SET_DM_DIB);
   if (nRet != IS_SUCCESS)
      return nRet;

   // set color mode
   nRet = is_SetColorMode(camHandle_, IS_CM_SENSOR_RAW8);
   if (nRet != IS_SUCCESS)
      return nRet;
   bitDepth_ = 8;

   // binning
   CPropertyAction *pAct = new CPropertyAction (this, &ThorlabsUSBCam::OnBinning);
   nRet = CreateProperty(MM::g_Keyword_Binning, "1", MM::Integer, true, pAct);
   assert(nRet == DEVICE_OK);

   nRet = SetAllowedBinning();
   if (nRet != DEVICE_OK)
      return nRet;

   // pixel type
   // start in the default 8-bit mode
   pAct = new CPropertyAction (this, &ThorlabsUSBCam::OnPixelType);
   nRet = CreateProperty(MM::g_Keyword_PixelType, g_PixelType_8bit, MM::String, false, pAct);
   assert(nRet == DEVICE_OK);

   vector<string> pixelTypeValues;
   pixelTypeValues.push_back(g_PixelType_8bit);
   pixelTypeValues.push_back(g_PixelType_16bit); 

   nRet = SetAllowedValues(MM::g_Keyword_PixelType, pixelTypeValues);
   if (nRet != DEVICE_OK)
      return nRet;

   // Exposure
   pAct = new CPropertyAction (this, &ThorlabsUSBCam::OnExposure);
   CreateProperty(MM::g_Keyword_Exposure, "15", MM::Integer, false, pAct);
   SetPropertyLimits(MM::g_Keyword_Exposure, 1, 35);

   // camera gain
   pAct = new CPropertyAction (this, &ThorlabsUSBCam::OnHardwareGain);
   CreateProperty("HardwareGain", "1", MM::Integer, false, pAct);
   SetPropertyLimits("HardwareGain", 1, 100);

   // PixelClock
   UINT pixClockRange[3];
   ZeroMemory(pixClockRange, sizeof(pixClockRange));
   nRet = is_PixelClock(camHandle_, IS_PIXELCLOCK_CMD_GET_RANGE, (void*)pixClockRange, sizeof(pixClockRange));
   if (nRet != IS_SUCCESS)
      return nRet;

   int minClock = pixClockRange[0];
   int maxClock = pixClockRange[1];
   
   UINT curPixClock(0);
   nRet = is_PixelClock(camHandle_, IS_PIXELCLOCK_CMD_GET, (void*)&curPixClock, sizeof(curPixClock));

   ostringstream osClock;
   osClock << curPixClock;
   pAct = new CPropertyAction(this, &ThorlabsUSBCam::OnPixelClock);
   CreateProperty("PixelClockMHz", osClock.str().c_str(), MM::Integer, false, pAct);
   SetPropertyLimits("PixelClockMHz", minClock, maxClock);

   pAct = new CPropertyAction(this, &ThorlabsUSBCam::OnFPS);
   CreateProperty("FPS", "0.0", MM::Float, true, pAct);

   // synchronize all properties
   // --------------------------
   nRet = UpdateStatus();
   if (nRet != DEVICE_OK)
      return nRet;

   // setup the buffer
   // ----------------
   nRet = ResizeImageBuffer();
   if (nRet != DEVICE_OK)
      return nRet;

   initialized_ = true;
   return DEVICE_OK;
}

/**
* Shuts down (unloads) the device.
* Required by the MM::Device API.
* Ideally this method will completely unload the device and release all resources.
* Shutdown() may be called multiple times in a row.
* After Shutdown() we should be allowed to call Initialize() again to load the device
* without causing problems.
*/
int ThorlabsUSBCam::Shutdown()
{
   if (cameraBuf != 0)
   {
      int ret = is_FreeImageMem(camHandle_, cameraBuf, cameraBufId);
      if (ret != IS_SUCCESS)
         return ret;

      cameraBuf = 0;
      cameraBufId = 0;
   }

	is_ExitCamera(camHandle_);

   initialized_ = false;
   return DEVICE_OK;
}

/**
* Performs exposure and grabs a single image.
* This function should block during the actual exposure and return immediately afterwards 
* (i.e., before readout).  This behavior is needed for proper synchronization with the shutter.
* Required by the MM::Camera API.
*/
int ThorlabsUSBCam::SnapImage()
{
	int ret = is_FreezeVideo(camHandle_, IS_WAIT);
   if (ret != IS_SUCCESS)
      return ret;

   memcpy(img_.GetPixelsRW(),
          cameraBuf,
          img_.Width()*img_.Height()*img_.Depth());

   return DEVICE_OK;
}


/**
* Returns pixel data.
* Required by the MM::Camera API.
* The calling program will assume the size of the buffer based on the values
* obtained from GetImageBufferSize(), which in turn should be consistent with
* values returned by GetImageWidth(), GetImageHight() and GetImageBytesPerPixel().
* The calling program allso assumes that camera never changes the size of
* the pixel buffer on its own. In other words, the buffer can change only if
* appropriate properties are set (such as binning, pixel type, etc.)
*/
const unsigned char* ThorlabsUSBCam::GetImageBuffer()
{

   MMThreadGuard g(imgPixelsLock_);
   unsigned char *pB = (unsigned char*)(img_.GetPixels());
   return pB;
}

/**
* Returns image buffer X-size in pixels.
* Required by the MM::Camera API.
*/
unsigned ThorlabsUSBCam::GetImageWidth() const
{
   return img_.Width();
}

/**
* Returns image buffer Y-size in pixels.
* Required by the MM::Camera API.
*/
unsigned ThorlabsUSBCam::GetImageHeight() const
{

   return img_.Height();
}

/**
* Returns image buffer pixel depth in bytes.
* Required by the MM::Camera API.
*/
unsigned ThorlabsUSBCam::GetImageBytesPerPixel() const
{

   return img_.Depth();
} 

/**
* Returns the bit depth (dynamic range) of the pixel.
* This does not affect the buffer size, it just gives the client application
* a guideline on how to interpret pixel values.
* Required by the MM::Camera API.
*/
unsigned ThorlabsUSBCam::GetBitDepth() const
{

   return bitDepth_;
}

/**
* Returns the size in bytes of the image buffer.
* Required by the MM::Camera API.
*/
long ThorlabsUSBCam::GetImageBufferSize() const
{
   return img_.Width() * img_.Height() * GetImageBytesPerPixel();
}

/**
* Sets the camera Region Of Interest.
* Required by the MM::Camera API.
* This command will change the dimensions of the image.
* Depending on the hardware capabilities the camera may not be able to configure the
* exact dimensions requested - but should try do as close as possible.
* If the hardware does not have this capability the software should simulate the ROI by
* appropriately cropping each frame.
* This demo implementation ignores the position coordinates and just crops the buffer.
* @param x - top-left corner coordinate
* @param y - top-left corner coordinate
* @param xSize - width
* @param ySize - height
*/
int ThorlabsUSBCam::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{

   if (xSize == 0 && ySize == 0)
   {
      // effectively clear ROI
      ResizeImageBuffer();
      roiX_ = 0;
      roiY_ = 0;
   }
   else
   {
      // apply ROI
      img_.Resize(xSize, ySize);
      roiX_ = x;
      roiY_ = y;
   }
   return DEVICE_OK;
}

/**
* Returns the actual dimensions of the current ROI.
* Required by the MM::Camera API.
*/
int ThorlabsUSBCam::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{

   x = roiX_;
   y = roiY_;

   xSize = img_.Width();
   ySize = img_.Height();

   return DEVICE_OK;
}

/**
* Resets the Region of Interest to full frame.
* Required by the MM::Camera API.
*/
int ThorlabsUSBCam::ClearROI()
{
   ResizeImageBuffer();
   roiX_ = 0;
   roiY_ = 0;
      
   return DEVICE_OK;
}

/**
* Returns the current exposure setting in milliseconds.
* Required by the MM::Camera API.
*/
double ThorlabsUSBCam::GetExposure() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Exposure, buf);
   if (ret != DEVICE_OK)
      return 0.0;
   return atof(buf);
}

/**
* Sets exposure in milliseconds.
* Required by the MM::Camera API.
*/
void ThorlabsUSBCam::SetExposure(double exp)
{
   SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exp));
}

/**
* Returns the current binning factor.
* Required by the MM::Camera API.
*/
int ThorlabsUSBCam::GetBinning() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Binning, buf);
   if (ret != DEVICE_OK)
      return 1;
   return atoi(buf);
}

/**
* Sets binning factor.
* Required by the MM::Camera API.
*/
int ThorlabsUSBCam::SetBinning(int binF)
{
   return SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));
}

int ThorlabsUSBCam::SetAllowedBinning() 
{
   vector<string> binValues;
   binValues.push_back("1");
   return SetAllowedValues(MM::g_Keyword_Binning, binValues);
}


/**
 * Required by the MM::Camera API
 * Please implement this yourself and do not rely on the base class implementation
 * The Base class implementation is deprecated and will be removed shortly
 */
int ThorlabsUSBCam::StartSequenceAcquisition(double interval) {
   return StartSequenceAcquisition(LONG_MAX, interval, false);            
}

/**                                                                       
* Stop and wait for the Sequence thread finished                                   
*/                                                                        
int ThorlabsUSBCam::StopSequenceAcquisition()                                     
{
   int ret = is_StopLiveVideo(camHandle_, IS_DONT_WAIT);
   if (ret != IS_SUCCESS)
      LogMessage("Camera failed to stop live video.");

   if (!thd_->IsStopped()) {
      thd_->Stop();                                                       
      thd_->wait();                                                       
   }                                                                      
                                                                          
   return DEVICE_OK;                                                      
} 

/**
* Simple implementation of Sequence Acquisition
* A sequence acquisition should run on its own thread and transport new images
* coming of the camera into the MMCore circular buffer.
*/
int ThorlabsUSBCam::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
   if (IsCapturing())
      return DEVICE_CAMERA_BUSY_ACQUIRING;

   int ret = GetCoreCallback()->PrepareForAcq(this);
   if (ret != DEVICE_OK)
      return ret;
   sequenceStartTime_ = GetCurrentMMTime();
   imageCounter_ = 0;

   hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
   is_InitEvent(camHandle_, hEvent, IS_SET_EVENT_FRAME);
   is_EnableEvent(camHandle_, IS_SET_EVENT_FRAME);

   ret = is_CaptureVideo(camHandle_, IS_WAIT);
   if (ret != IS_SUCCESS)
      return ret;

   thd_->Start(numImages,interval_ms);
   stopOnOverFlow_ = stopOnOverflow;
   return DEVICE_OK;
}

/*
 * Inserts Image and MetaData into MMCore circular Buffer
 */
int ThorlabsUSBCam::InsertImage()
{
   MM::MMTime timeStamp = this->GetCurrentMMTime();
   char label[MM::MaxStrLength];
   this->GetLabel(label);
 
   // Important:  metadata about the image are generated here:
   Metadata md;
   md.put("Camera", label);
   md.put(MM::g_Keyword_Metadata_StartTime, CDeviceUtils::ConvertToString(sequenceStartTime_.getMsec()));
   md.put(MM::g_Keyword_Elapsed_Time_ms, CDeviceUtils::ConvertToString((timeStamp - sequenceStartTime_).getMsec()));
   md.put(MM::g_Keyword_Metadata_ROI_X, CDeviceUtils::ConvertToString( (long) roiX_)); 
   md.put(MM::g_Keyword_Metadata_ROI_Y, CDeviceUtils::ConvertToString( (long) roiY_)); 

   imageCounter_++;

   MMThreadGuard g(imgPixelsLock_);

   const unsigned char* pI = GetImageBuffer();
   unsigned int w = GetImageWidth();
   unsigned int h = GetImageHeight();
   unsigned int b = GetImageBytesPerPixel();

   int ret = GetCoreCallback()->InsertImage(this, pI, w, h, b, md.Serialize().c_str());
   if (!stopOnOverFlow_ && ret == DEVICE_BUFFER_OVERFLOW)
   {
      // do not stop on overflow - just reset the buffer
      GetCoreCallback()->ClearImageBuffer(this);
      // don't process this same image again...
      return GetCoreCallback()->InsertImage(this, pI, w, h, b, md.Serialize().c_str(), false);
   } else
      return ret;
}

/*
 * Do actual capturing
 * Called from inside the thread  
 */
int ThorlabsUSBCam::ThreadRun (void)
{
   DWORD dwRet = WaitForSingleObject(hEvent, 2000);
   if (dwRet == WAIT_TIMEOUT)
   {
      return ERR_THORCAM_LIVE_TIMEOUT;
   }
   else if (dwRet == WAIT_OBJECT_0)
   {
      memcpy(img_.GetPixelsRW(),
             cameraBuf,
             img_.Width()*img_.Height()*img_.Depth());

      return InsertImage();
   }
   else
   {
      ostringstream os;
      os << "Unknown event status " << dwRet;
      LogMessage(os.str());
      return ERR_THORCAM_LIVE_UNKNOWN_EVENT;
   }
};

bool ThorlabsUSBCam::IsCapturing() {
   return !thd_->IsStopped();
}

/*
 * called from the thread function before exit 
 */
void ThorlabsUSBCam::OnThreadExiting() throw()
{
   is_DisableEvent(camHandle_, IS_SET_EVENT_FRAME);
   is_ExitEvent(camHandle_, IS_SET_EVENT_FRAME);
   CloseHandle(hEvent);
   hEvent = 0;
   try
   {
      LogMessage(g_Msg_SEQUENCE_ACQUISITION_THREAD_EXITING);
      GetCoreCallback()?GetCoreCallback()->AcqFinished(this,0):DEVICE_OK;
   }
   catch(...)
   {
      LogMessage(g_Msg_EXCEPTION_IN_ON_THREAD_EXITING, false);
   }
}


MySequenceThread::MySequenceThread(ThorlabsUSBCam* pCam)
   :intervalMs_(default_intervalMS)
   ,numImages_(default_numImages)
   ,imageCounter_(0)
   ,stop_(true)
   ,suspend_(false)
   ,camera_(pCam)
   ,startTime_(0)
   ,actualDuration_(0)
   ,lastFrameTime_(0)
{};

MySequenceThread::~MySequenceThread() {};

void MySequenceThread::Stop() {
   MMThreadGuard(this->stopLock_);
   stop_=true;
}

void MySequenceThread::Start(long numImages, double intervalMs)
{
   MMThreadGuard(this->stopLock_);
   MMThreadGuard(this->suspendLock_);
   numImages_=numImages;
   intervalMs_=intervalMs;
   imageCounter_=0;
   stop_ = false;
   suspend_=false;
   activate();
   actualDuration_ = 0;
   startTime_= camera_->GetCurrentMMTime();
   lastFrameTime_ = 0;
}

bool MySequenceThread::IsStopped(){
   MMThreadGuard(this->stopLock_);
   return stop_;
}

void MySequenceThread::Suspend() {
   MMThreadGuard(this->suspendLock_);
   suspend_ = true;
}

bool MySequenceThread::IsSuspended() {
   MMThreadGuard(this->suspendLock_);
   return suspend_;
}

void MySequenceThread::Resume() {
   MMThreadGuard(this->suspendLock_);
   suspend_ = false;
}

int MySequenceThread::svc(void) throw()
{
   int ret = DEVICE_ERR;
   try 
   {
      do
      {  
         MM::MMTime startTime = camera_->GetCurrentMMTime();
         ret = camera_->ThreadRun();
         MM::MMTime frameTime;
         frameTime = camera_->GetCurrentMMTime() - startTime;
         double secInterval = frameTime.getMsec() / 1000.0;
         if (secInterval > 0.0)
            camera_->fps = 1.0 / secInterval;
         else
            camera_->fps = 0.0;
      } while (DEVICE_OK == ret && !IsStopped() && imageCounter_++ < numImages_-1);

      if (IsStopped())
         camera_->LogMessage("SeqAcquisition interrupted by the user\n");

   } catch(...){
      camera_->LogMessage(g_Msg_EXCEPTION_IN_THREAD, false);
   }
   stop_=true;
   actualDuration_ = camera_->GetCurrentMMTime() - startTime_;
   camera_->OnThreadExiting();
   return ret;
}


///////////////////////////////////////////////////////////////////////////////
// ThorlabsUSBCam Action handlers
///////////////////////////////////////////////////////////////////////////////
/**
* Handles "Binning" property.
*/
int ThorlabsUSBCam::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{

   int ret = DEVICE_ERR;
   switch(eAct)
   {
   case MM::AfterSet:
      {
         if(IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

         // the user just set the new value for the property, so we have to
         // apply this value to the 'hardware'.
         long binFactor;
         pProp->Get(binFactor);
			if(binFactor > 0 && binFactor < 10)
			{
				img_.Resize(sensorInfo.nMaxWidth/binFactor, sensorInfo.nMaxHeight/binFactor);
				binSize_ = binFactor;
            std::ostringstream os;
            os << binSize_;
            OnPropertyChanged("Binning", os.str().c_str());
				ret=DEVICE_OK;
			}
      }break;
   case MM::BeforeGet:
      {
         ret=DEVICE_OK;
			pProp->Set(binSize_);
      }break;
   }
   return ret; 
}

/**
* Handles "PixelType" property.
*/
int ThorlabsUSBCam::OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      if(IsCapturing())
         return DEVICE_CAMERA_BUSY_ACQUIRING;

      string pixelType;
      pProp->Get(pixelType);

      if (pixelType.compare(g_PixelType_8bit) == 0)
      {
         int nRet = is_SetColorMode(camHandle_, IS_CM_SENSOR_RAW8);
         if (nRet != IS_SUCCESS)
            return nRet;
         bitDepth_ = 8;
         nComponents_ = 1;
         return ResizeImageBuffer();
      }
      else if (pixelType.compare(g_PixelType_16bit) == 0)
      {
         int nRet = is_SetColorMode(camHandle_, IS_CM_SENSOR_RAW16);
         if (nRet != IS_SUCCESS)
            return nRet;
          
         bitDepth_ = 16;
         nComponents_ = 1;
         return ResizeImageBuffer();
      }
      else
      {
         // on error switch to default pixel type
         nComponents_ = 1;
         bitDepth_ = 8;
         return ERR_THORCAM_UNKNOWN_PIXEL_TYPE;
      }
   }
   else if (eAct == MM::BeforeGet)
   {
      if (bitDepth_ == 8)
         pProp->Set(g_PixelType_8bit);
      else
         pProp->Set(g_PixelType_16bit);
   }
   return DEVICE_OK; 
}

int ThorlabsUSBCam::OnExposure(MM::PropertyBase* pProp , MM::ActionType eAct)
{
 
   double expfoo;
   double *newEXP;
   newEXP = &expfoo;

   if (eAct == MM::BeforeGet)
   {
		pProp->Set(Exposure_);
   }
   else if (eAct == MM::AfterSet)
   {
      long value;
      pProp->Get(value);
		if( value != Exposure_)
		{
			Exposure_ = value;
			is_SetExposureTime(camHandle_, Exposure_, newEXP);
		}
   }
	return DEVICE_OK;
}

int ThorlabsUSBCam::OnHardwareGain(MM::PropertyBase* pProp , MM::ActionType eAct)
{
   
   if (eAct == MM::BeforeGet)
   {
		pProp->Set(HardwareGain_);
   }
   else if (eAct == MM::AfterSet)
   {
      long value;
      pProp->Get(value);
		if( value != HardwareGain_)
		{
			HardwareGain_ = value;
			is_SetHardwareGain(camHandle_, (int) HardwareGain_, IS_IGNORE_PARAMETER,IS_IGNORE_PARAMETER,IS_IGNORE_PARAMETER);
		}
   }
	return DEVICE_OK;
}

int ThorlabsUSBCam::OnPixelClock(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      UINT curPixClock(0);
      int ret = is_PixelClock(camHandle_, IS_PIXELCLOCK_CMD_GET, (void*)&curPixClock, sizeof(curPixClock));
      if (ret != IS_SUCCESS)
         return ret;

		pProp->Set((long)curPixClock);
   }
   else if (eAct == MM::AfterSet)
   {
      long value;
      pProp->Get(value);
      UINT pixClock = (unsigned) value;
      int ret = is_PixelClock(camHandle_, IS_PIXELCLOCK_CMD_SET, (void*)&pixClock, sizeof(pixClock));
      if (ret != IS_SUCCESS)
         return ret;
   }
	return DEVICE_OK;
}

int ThorlabsUSBCam::OnFPS(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
		pProp->Set(fps);

	return DEVICE_OK;
}


///////////////////////////////////////////////////////////////////////////////
// Private ThorlabsUSBCam methods
///////////////////////////////////////////////////////////////////////////////

/**
* Sync internal image buffer size to the chosen property values.
*/
int ThorlabsUSBCam::ResizeImageBuffer()
{
   if (cameraBuf != 0)
   {
      int ret = is_FreeImageMem(camHandle_, cameraBuf, cameraBufId);
      if (ret != IS_SUCCESS)
         return ret;

      cameraBuf = 0;
      cameraBufId = 0;
   }

   int byteDepth = bitDepth_ == 8 ? 1 : 2;
   int ret = is_AllocImageMem(   camHandle_,
                                 sensorInfo.nMaxWidth/binSize_,
                                 sensorInfo.nMaxHeight/binSize_,
						               byteDepth * 8,
						               &cameraBuf,
						               &cameraBufId);
   if (ret != IS_SUCCESS)
      return ret;

	is_SetImageMem(camHandle_, cameraBuf, cameraBufId);	// set memory active

   img_.Resize(sensorInfo.nMaxWidth/binSize_, sensorInfo.nMaxHeight/binSize_, byteDepth);

   return DEVICE_OK;
}






