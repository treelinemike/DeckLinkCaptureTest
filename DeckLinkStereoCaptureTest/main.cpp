// single-channel video capture test
#include <iostream>
#include <opencv2/opencv.hpp>
#include "platform.h"
#include "DeckLinkAPI_h.h"
#include "Uyvy8VideoFrame.h"
#include "Xle10VideoFrame.h"
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <atlstr.h>

#define kDeviceCount 2

using namespace std;
using namespace cv;

// Video mode parameters
const BMDDisplayMode      kDisplayMode = bmdModeHD1080i5994;


/*
	enum _BMDVideoInputFlags
	{
		bmdVideoInputFlagDefault	= 0,
		bmdVideoInputEnableFormatDetection	= ( 1 << 0 ) ,
		bmdVideoInputDualStream3D	= ( 1 << 1 ) ,
		bmdVideoInputSynchronizeToCaptureGroup	= ( 1 << 2 )
	} ;
	*/
const BMDVideoInputFlags  kInputFlag = bmdVideoInputDualStream3D;
const BMDPixelFormat      kPixelFormat = bmdFormat10BitYUV;

// Frame parameters
const INT32_UNSIGNED kFrameDuration = 1000;
const INT32_UNSIGNED kTimeScale = 25000;
const INT32_UNSIGNED kSynchronizedCaptureGroup = 2;

static const BMDTimeScale kMicroSecondsTimeScale = 1000000;

class DeckLinkDevice;

class InputCallback : public IDeckLinkInputCallback
{
public:
	InputCallback(DeckLinkDevice* deckLinkDevice) :
		m_deckLinkDevice(deckLinkDevice),
		m_refCount(1)
	{
	}

	HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode, BMDDetectedVideoInputFormatFlags detectedSignalFlags) override;

	HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket) override;

	// IUnknown needs only a dummy implementation
	HRESULT	STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
	{
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++m_refCount;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		INT32_UNSIGNED newRefValue = --m_refCount;

		if (newRefValue == 0)
			delete this;

		return newRefValue;
	}

private:
	DeckLinkDevice* m_deckLinkDevice;
	std::atomic<INT32_SIGNED> m_refCount;
};

class NotificationCallback : public IDeckLinkNotificationCallback
{
public:
	NotificationCallback(DeckLinkDevice* deckLinkDevice) :
		m_deckLinkDevice(deckLinkDevice),
		m_refCount(1)
	{
	}

	HRESULT STDMETHODCALLTYPE Notify(BMDNotifications topic, uint64_t param1, uint64_t param2) override;

	// IUnknown needs only a dummy implementation
	HRESULT	STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
	{
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return AtomicIncrement(&m_refCount);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		INT32_UNSIGNED newRefValue = AtomicDecrement(&m_refCount);

		if (newRefValue == 0)
			delete this;

		return newRefValue;
	}
private:
	DeckLinkDevice* m_deckLinkDevice;
	INT32_SIGNED m_refCount;
};

class DeckLinkDevice
{
public:
	DeckLinkDevice() :
		m_index(0),
		m_deckLink(nullptr),
		m_deckLinkConfig(nullptr),
		m_deckLinkStatus(nullptr),
		m_deckLinkNotification(nullptr),
		m_notificationCallback(nullptr),
		m_deckLinkInput(nullptr),
		m_inputCallback(nullptr)
	{
	}

	HRESULT setup(IDeckLink* deckLink, unsigned index)
	{

		m_index = index;
		m_deckLink = deckLink;  // THIS IS THE ISSUE!

		// initialize our converter
		if (GetDeckLinkVideoConversion(&m_frameConverter) != S_OK) {
			printf("Frame converter initialization failed...\n");
		} else {
			printf("Converter initialized!\n");
		}



		//BSTR deckLinkDisplayName;
		//deckLink->GetDisplayName(&deckLinkDisplayName);
		//fprintf(stdout, "Trying to setup: %S\n", CString(deckLinkDisplayName));

		// Obtain the configuration interface for the DeckLink device
		HRESULT result = m_deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&m_deckLinkConfig);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n", result);
			goto bail;
		}

		// Set 3D format to capture separate streams
		// Equivalent of checking "Capture two independent streams as 3D" in Blackmagic Media Express
		// TODO: don't fully understand how this works, but it does appear to allow capture from da Vinci S imaging system
		// as long as the LEFT CCU Ext Ref output is piped into the RIGHT CCU Sync input
		result = m_deckLinkConfig->SetFlag(bmdDeckLinkConfigSDIInput3DPayloadOverride, true);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not set 3D override flag\n");
		}
		else {
			printf("3D override flag set successfully!\n");
		}

		// Set the synchronized capture group number. This can be any 32-bit number
		// All devices enabled for synchronized capture with the same group number are started together
		// TODO: shouldn't need this
		result = m_deckLinkConfig->SetInt(bmdDeckLinkConfigCaptureGroup, kSynchronizedCaptureGroup);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not set capture group - result = %08x\n", result);
			goto bail;
		}


		// Obtain the status interface for the DeckLink device
		result = m_deckLink->QueryInterface(IID_IDeckLinkStatus, (void**)&m_deckLinkStatus);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not obtain the IDeckLinkStatus interface - result = %08x\n", result);
			goto bail;
		}

		// Obtain the notification interface for the DeckLink device
		result = m_deckLink->QueryInterface(IID_IDeckLinkNotification, (void**)&m_deckLinkNotification);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not obtain the IDeckLinkNotification interface - result = %08x\n", result);
			goto bail;
		}

		m_notificationCallback = new NotificationCallback(this);
		if (m_notificationCallback == nullptr)
		{
			fprintf(stderr, "Could not create notification callback object\n");
			goto bail;
		}

		// Set the callback object to the DeckLink device's notification interface
		result = m_deckLinkNotification->Subscribe(bmdStatusChanged, m_notificationCallback);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not set notification callback - result = %08x\n", result);
			goto bail;
		}

		// Obtain the input interface for the DeckLink device
		result = m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&m_deckLinkInput);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not obtain the IDeckLinkInput interface - result = %08x\n", result);
			goto bail;
		}

		// Create an instance of output callback
		m_inputCallback = new InputCallback(this);
		if (m_inputCallback == nullptr)
		{
			fprintf(stderr, "Could not create input callback object\n");
			goto bail;
		}

		// Set the callback object to the DeckLink device's input interface
		result = m_deckLinkInput->SetCallback(m_inputCallback);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not set input callback - result = %08x\n", result);
			goto bail;
		}

	bail:
		return result;

	}

	HRESULT waitForSignalLock()
	{
		// When performing synchronized capture, all participating devices need to have their signal locked
		std::unique_lock<std::mutex> guard(m_mutex);

		HRESULT result = S_OK;

		m_signalCondition.wait(guard, [this, &result]()
			{
				INT64_SIGNED displayMode;
				result = m_deckLinkStatus->GetInt(bmdDeckLinkStatusDetectedVideoInputMode, &displayMode);
				if (result != S_OK && result != S_FALSE)
				{
					fprintf(stderr, "Could not query input status - result = %08x\n", result);
					return true;
				}

				return (BMDDisplayMode)displayMode == kDisplayMode;
			});

		return result;
	}

	void notifyVideoInputChanged()
	{
		m_signalCondition.notify_all();
	}

	HRESULT prepareForCapture()
	{
		// Enable video output
		HRESULT result = m_deckLinkInput->EnableVideoInput(kDisplayMode, kPixelFormat, kInputFlag);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not enable video input - result = %08x\n", result);
			goto bail;
		}

	bail:
		return result;
	}

	HRESULT startCapture()
	{
		HRESULT result = m_deckLinkInput->StartStreams();
		if (result != S_OK)
		{
			fprintf(stderr, "Could not start - result = %08x\n", result);
			goto bail;
		}

	bail:
		return result;
	}

	HRESULT stopCapture()
	{
		HRESULT result = m_deckLinkInput->StopStreams();
		if (result != S_OK)
		{
			fprintf(stderr, "Could not stop - result = %08x\n", result);
			goto bail;
		}

	bail:
		return result;
	}

	HRESULT cleanUpFromCapture()
	{
		HRESULT result = m_deckLinkInput->DisableVideoInput();
		if (result != S_OK)
		{
			fprintf(stderr, "Could not disable - result = %08x\n", result);
			goto bail;
		}

	bail:
		return result;
	}

	// convert video frame into something we can deal with using OpenCV
// p_outputFrame should be a pointer to something like:  cv::Mat cvFrameBGR8(frameHeight, frameWidth, CV_8UC3);
	HRESULT extractCVMat8(IDeckLinkVideoFrame* videoFrame, cv::Mat* p_outputMatrix) {

		// get height, width, bytes per row, and pixel format of raw frame
		// captured from DeckLink
		const auto frameHeight = (int32_t)videoFrame->GetHeight();
		const auto frameWidth = (int32_t)videoFrame->GetWidth();
		const auto rowBytes = videoFrame->GetRowBytes();
		printf("Width: %d; Height: %d; total bytes per row: %d\n", frameWidth, frameHeight, rowBytes);
		printf("Raw pixel format: 0x%0X\n", videoFrame->GetPixelFormat());

		// create a new frame in 8 bit YUV (4:2:2 UYVY format)
		// and use BMD tools to convert raw frame into this intermediate format
		// which can be accepted (with conversion) into OpenCV
		// TODO: we lose bit depth here! can we push 10-bit 4:2:2 into 16-bit for openCV?
		// TODO: check name of frame class, is it really 16??
		Uyvy8VideoFrame* uyuv8Frame = NULL;
		uyuv8Frame = new Uyvy8VideoFrame(videoFrame->GetWidth(), videoFrame->GetHeight(), videoFrame->GetFlags());
		m_frameConverter->ConvertFrame((IDeckLinkVideoFrame*)videoFrame, uyuv8Frame);
		printf("Pixel format after BMD frame conversion: 0x%0X\n", uyuv8Frame->GetPixelFormat());  // this check is really a bit silly, we directly set this value in our own frame class...

		// assign pointer to raw pixel bytes in the new frame
		uyuv8Frame->GetBytes((void**)&m_deckLinkBuffer);

		// create OpenCV frames for the YUV and BGR frames
		cv::Mat cvFrameYUV8(frameHeight, frameWidth, CV_8UC2);

		// encode intermediate BMD frame directly in the pixels of the OpenCV MAT object
		uint8_t* frameData = cvFrameYUV8.data;
		unsigned long writeByteIdx = 0;
		unsigned long readByteIdx = 0;
		for (writeByteIdx = 0; writeByteIdx < 2 * (frameHeight * frameWidth); ++writeByteIdx) {

			*frameData = (uint8_t) * ((CHAR*)m_deckLinkBuffer);
			++frameData;
			++m_deckLinkBuffer;
		}

		// release the new frame object
		uyuv8Frame->Release();

		// convert YUV 4:2:2 to BGR in OpenCV
		cv::cvtColor(cvFrameYUV8, *p_outputMatrix, cv::COLOR_YUV2BGR_UYVY);

		// done
		return S_OK;
	}

	HRESULT frameArrived(IDeckLinkVideoInputFrame* videoFrame)
	{
		BMDTimeValue time;
		HRESULT result = videoFrame->GetStreamTime(&time, nullptr, kTimeScale);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not get stream time from frame - result = %08x\n", result);
			return S_OK;
		}


		unsigned frames = (unsigned)((time % kTimeScale) / kFrameDuration);
		unsigned seconds = (unsigned)((time / kTimeScale) % 60);
		unsigned minutes = (unsigned)((time / kTimeScale / 60) % 60);
		unsigned hours = (unsigned)(time / kTimeScale / 60 / 60);

		BMDTimeValue hwTime;
		result = videoFrame->GetHardwareReferenceTimestamp(kMicroSecondsTimeScale, &hwTime, NULL);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not get hardware reference time from frame - result = %08x\n", result);
			return S_OK;
		}


		printf("[%llu.%06llu] Device #%u: Frame %02u:%02u:%02u:%03u arrived\n", hwTime / kMicroSecondsTimeScale, hwTime % kMicroSecondsTimeScale, m_index, hours, minutes, seconds, frames);

		// get height, width, bytes per row, and pixel format of raw frame
		// captured from DeckLink
		const auto frameHeight = (int32_t)videoFrame->GetHeight();
		const auto frameWidth = (int32_t)videoFrame->GetWidth();
		const auto rowBytes = videoFrame->GetRowBytes();
		printf("Width: %d; Height: %d; total bytes per row: %d\n", frameWidth, frameHeight, rowBytes);
		printf("Raw pixel format: 0x%0X\n", videoFrame->GetPixelFormat());

		// Save the LEFT frame (8 bit)
		cv::Mat cvFrameBGR8_L(frameHeight, frameWidth, CV_8UC3);
		extractCVMat8((IDeckLinkVideoFrame*)videoFrame, &cvFrameBGR8_L);
		cv::imwrite("C:\\Users\\f002r5k\\Desktop\\test8_L.tif", cvFrameBGR8_L);
		
		// and for our next trick,
		// extract the RIGHT frame
		IDeckLinkVideoFrame3DExtensions* videoFrameExtensions = NULL;
		IDeckLinkVideoFrame* videoFrameRight = NULL;
		result = videoFrame->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void**)&videoFrameExtensions);
		if (result != S_OK) {
			fprintf(stderr, "Could not retrieve 3D extensions object...\n");
		}
		result = videoFrameExtensions->GetFrameForRightEye(&videoFrameRight);
		if (result != S_OK) {
			fprintf(stderr, "Could not retrieve right eye frame...\n");
		}
		
		// Save the RIGHT frame (8 bit)
		cv::Mat cvFrameBGR8_R(frameHeight, frameWidth, CV_8UC3);
		extractCVMat8((IDeckLinkVideoFrame*)videoFrameRight, &cvFrameBGR8_R);
		cv::imwrite("C:\\Users\\f002r5k\\Desktop\\test8_R.tif", cvFrameBGR8_R);

		// put away right eye frame objects
		videoFrameRight->Release();
		videoFrameExtensions->Release();


		// try 10 bit
		Xle10VideoFrame* m_newFrameXLE = NULL;
		m_newFrameXLE = new Xle10VideoFrame(videoFrame->GetWidth(), videoFrame->GetHeight(), videoFrame->GetFlags());
		m_frameConverter->ConvertFrame((IDeckLinkVideoFrame*)videoFrame, m_newFrameXLE);
		printf("Pixel format after BMD frame conversion: 0x%0X\n", m_newFrameXLE->GetPixelFormat());  // this check is really a bit silly, we directly set this value in our own frame class...

		// assign pointer to raw pixel bytes in the new frame
		m_newFrameXLE->GetBytes((void**)&m_deckLinkBuffer);

		// NOW... let's try to get the 10-bit frame values into a 16-bit frame
		cv::Mat cvFrameBGR16(frameHeight, frameWidth, CV_16UC3);
		uint16_t* p_cvFrameBGR16 = (uint16_t*)cvFrameBGR16.data;
		uint32_t localPixelData = 0;
		CHAR* p_localPixelData;
		for (unsigned int pixelIdx = 0; pixelIdx < (frameWidth * frameHeight); pixelIdx++)
		{

			// read next four bytes from deckLinkBuffer into a 32-bit word
			CHAR* p_localPixelData = (CHAR*)&localPixelData;
			for (unsigned int byteIdx = 0; byteIdx < 4; byteIdx++)
			{
				*p_localPixelData = (CHAR) * ((CHAR*)m_deckLinkBuffer);
				++m_deckLinkBuffer;
				++p_localPixelData;
			}

			//printf("0x%08X\n", localPixelData);

			// extract three 10-bit components from 32-bit word,
			// convert each to 16-bit representations,
			// and write out to the 16-bit OpenCV image
			uint32_t bitMask = 0;
			uint32_t componentValue10Bit = 0;
			for (unsigned int componentIdx = 0; componentIdx < 3; componentIdx++)
			{
				bitMask = 0xFFC << (10 * componentIdx);
				componentValue10Bit = (localPixelData & bitMask) >> (10 * componentIdx + 2);
				*p_cvFrameBGR16 = (uint16_t)(((double)componentValue10Bit) * (65535.0 / 1023.0));
				++p_cvFrameBGR16;
			}
		}

		// add something to the frame
		//char mystr[255];
		//sprintf_s(mystr, "Frame #%03d", 001);
		//putText(cvFrameBGR16, (string)mystr, Point(50, cvFrameBGR16.rows / 2), FONT_HERSHEY_SIMPLEX, 5.0, CV_RGB(65535, 65535, 0), 10);

		// save the frame to file
		cv::imwrite("C:\\Users\\f002r5k\\Desktop\\test16.tif", cvFrameBGR16);



		// and do something with it...



		// hang here, for now...
		// TODO: stream to video file, and make sure we release everything properly in the destructor
		while (1) {};

		return S_OK;
	}

	~DeckLinkDevice()
	{
		if (m_inputCallback)
		{
			m_deckLinkInput->SetCallback(nullptr);
			m_inputCallback->Release();
		}

		if (m_notificationCallback)
		{
			m_deckLinkNotification->Unsubscribe(bmdStatusChanged, m_notificationCallback);
			m_notificationCallback->Release();
		}

		if (m_deckLink)
			m_deckLink->Release();

		if (m_deckLinkConfig)
			m_deckLinkConfig->Release();

		if (m_deckLinkStatus)
			m_deckLinkStatus->Release();

		if (m_deckLinkInput)
			m_deckLinkInput->Release();

		if (m_deckLinkNotification)
			m_deckLinkNotification->Release();

		if(m_frameConverter)
			m_frameConverter->Release();
	}

private:
	unsigned						                m_index;
	IDeckLink* m_deckLink;
	IDeckLinkConfiguration* m_deckLinkConfig;
	IDeckLinkStatus* m_deckLinkStatus;
	IDeckLinkNotification* m_deckLinkNotification;
	NotificationCallback* m_notificationCallback;
	IDeckLinkInput* m_deckLinkInput;
	InputCallback* m_inputCallback;
	std::mutex										m_mutex;
	std::condition_variable							m_signalCondition;
	IDeckLinkVideoConversion* m_frameConverter = NULL;
	//Uyvy8VideoFrame* m_newFrame = NULL;
	//Xle10VideoFrame* m_newFrameXLE = NULL;
	CHAR* m_deckLinkBuffer = NULL;

};

HRESULT InputCallback::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode, BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{
	return S_OK;
}

HRESULT InputCallback::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket)
{
	if (!videoFrame || (videoFrame->GetFlags() & bmdFrameHasNoInputSource))
	{
		printf("No valid frame\n");
		return S_OK;
	}

	return m_deckLinkDevice->frameArrived(videoFrame);
}

HRESULT NotificationCallback::Notify(BMDNotifications topic, uint64_t param1, uint64_t param2)
{
	if (topic != bmdStatusChanged)
		return S_OK;

	if ((BMDDeckLinkStatusID)param1 != bmdDeckLinkStatusDetectedVideoInputMode)
		return S_OK;

	m_deckLinkDevice->notifyVideoInputChanged();
	return S_OK;
}


int main(void) {


	IDeckLinkIterator* deckLinkIterator = nullptr;
	IDeckLink* deckLink = nullptr;
	IDeckLink* deckLinkDiscard = nullptr;
	DeckLinkDevice           device;
	HRESULT                 result;
	unsigned				index = 0;
	unsigned int deckLinkCount = 0;

	Initialize();

	cout << "Hello, world!" << endl;



	// Create an IDeckLinkIterator object to enumerate all DeckLink cards in the system
	result = GetDeckLinkIterator(&deckLinkIterator);
	if (result != S_OK)
	{
		fprintf(stderr, "A DeckLink iterator could not be created.  The DeckLink drivers may not be installed.\n");
		goto bail;
	}

	// Get the first DeckLink device
	result = deckLinkIterator->Next(&deckLink);
	if (result != S_OK)
	{
		fprintf(stderr, "No DeckLink device found - result = %08x\n", result);
		goto bail;
	}


	BSTR deckLinkDisplayName;
	deckLink->GetDisplayName(&deckLinkDisplayName);
	fprintf(stdout, "Got a DeckLink device: %S\n", CString(deckLinkDisplayName));


	// Make sure we don't have another
	result = deckLinkIterator->Next(&deckLinkDiscard);
	if (result == S_OK)
	{
		fprintf(stderr, "More than one DeckLink device found! We can't handle that yet...\n");
		goto bail;
	}
	else {
		fprintf(stdout, "This is the only DeckLink device... good!\n");
	}


	result = device.setup(deckLink, 0);
	if (result != S_OK)
		goto bail;

	deckLink = nullptr;  // THIS TURNS OUT TO BE SUPER IMPORTANT, CAN'T DESTRUCT device PROPERLY WITHOUT IT!

	result = device.prepareForCapture();
	if (result != S_OK)
		goto bail;


	// Wait for devices to lock to the signal
	//fprintf(stdout,"Waiting for signal lock...\n");
	//result = device.waitForSignalLock();
	//if (result != S_OK)
	//	goto bail;


	

	// Start capture - This only needs to be performed on one device in the group
	fprintf(stdout, "Starting capture...\n");
	result = device.startCapture();
	if (result != S_OK)
		goto bail;

	// Wait until user presses Enter
	printf("Capturing... Press <RETURN> to exit\n");
	getchar();


	// Stop capture - This only needs to be performed on one device in the group
	printf("Exiting.\n");
	result = device.stopCapture();

	// Disable the video input interface
	result = device.cleanUpFromCapture();

	// Release resources
bail:

	// Release the Decklink objects
	if (deckLinkDiscard != nullptr)
		deckLinkDiscard->Release();
	if (deckLink != nullptr)
		deckLink->Release();

	// Release the DeckLink iterator
	if (deckLinkIterator != nullptr)
		deckLinkIterator->Release();

	return (result == S_OK) ? 0 : 1;
}
