// single-channel video capture test
#include <iostream>
#include <opencv2/opencv.hpp>
#include "platform.h"
#include "DeckLinkAPI_h.h"
#include "Uyvy16VideoFrame.h"
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <atlstr.h>

#define kDeviceCount 2

using namespace std;

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
const BMDVideoInputFlags  kInputFlag = bmdVideoInputFlagDefault;
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

		// now we actually extract the frame and do something with it
		// probably save to video using OpenCV0




		void* p_frameBytes;
		videoFrame->GetBytes(&p_frameBytes);

		const auto frameHeight = (int32_t)videoFrame->GetHeight();
		const auto frameWidth = (int32_t)videoFrame->GetWidth();
		const auto rowBytes = videoFrame->GetRowBytes();
		printf("Width: %d; Height: %d; total bytes per row: %d\n", frameWidth, frameHeight, rowBytes);

		printf("pixel format: 0x%0X\n", videoFrame->GetPixelFormat());


		// create a instance of the BMD frame converter
		// using literal black magic?
		// or maybe just COM programming
		//IDeckLinkVideoConversion* deckLinkFrameConverter = NULL;
		//CComPtr<IDeckLinkVideoConversion>	frameConverter;
		//if (frameConverter->CoCreateInstance(CLSID_CDeckLinkVideoConversion, nullptr, CLSCTX_ALL) != S_OK) {
		//	printf("Failed creating frame converter...\n");
		//}

		/*
		// now we need to create the video frame
		//CComPtr<IDeckLinkOutput> dummyDeckLinkOutput;
		//CComPtr<IDeckLinkMutableVideoFrame>	newFrame;
		IDeckLinkVideoConversion* frameConverter = NULL;
		IDeckLinkOutput* dummyDeckLinkOutput = NULL;
		Uyvy16VideoFrame* newFrame = NULL;
		*/

		m_newFrame = new Uyvy16VideoFrame(videoFrame->GetWidth(), videoFrame->GetHeight(), videoFrame->GetFlags());

		m_frameConverter->ConvertFrame((IDeckLinkVideoFrame*) videoFrame, m_newFrame);
		m_newFrame->GetBytes((void**)&m_deckLinkBuffer);
		


		/**/

		cv::Mat frameCPU(frameHeight, frameWidth, CV_8UC1);
		//cv::UMat image(frameHeight, frameWidth, CV_8UC3);
		//cv::cvtColor(frameCPU, image, cv::COLOR_YUV2BGR_Y422);
		//cv::imshow("Preview", image);
		//cv::imwrite("C:\\Users\\f002r5k\\Desktop\\test.tif", image);
		//while (1) {};
		// could call videoFrame->GetBytes(byteBuffer) to get pointer to pixel bytes 

		//uint32_t word0 = 0;
		//uint32_t word1 = 0;
		//uint32_t word2 = 0;
		//uint32_t word3 = 0;

		// encode grayscale info directly in the pixls of the OpenCV matrix
		uint8_t* frameData = frameCPU.data;
		
		
		unsigned long writeByteIdx = 0;
		unsigned long readByteIdx = 0;
		CHAR byte0 = 0;
		CHAR byte1 = 0;
		CHAR byte2 = 0;
		CHAR byte3 = 0;
		
		m_deckLinkBuffer += 1;
		for (writeByteIdx = 0; writeByteIdx < frameHeight * frameWidth; ++writeByteIdx) {

			*frameData = (uint8_t) * ((CHAR*)m_deckLinkBuffer);
			++frameData;
			m_deckLinkBuffer += 2;
		}

		cv::imwrite("C:\\Users\\f002r5k\\Desktop\\test.tif", frameCPU);
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
	Uyvy16VideoFrame* m_newFrame = NULL;
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
