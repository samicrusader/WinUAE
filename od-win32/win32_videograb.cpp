
/* UAE Win32 Video frame grabber support
 * Toni Wilen 2016
 */

#include "sysconfig.h"

#include "sysdeps.h"
#include "options.h"

#include <windows.h>
#include <dshow.h>
#include <atlcomcli.h>

#include "videograb.h"

#pragma comment(lib,"Strmiids.lib") 

// following have been removed from newer SDKs

static const IID IID_ISampleGrabber = { 0x6B652FFF, 0x11FE, 0x4fce, { 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };
static const IID IID_ISampleGrabberCB = { 0x0579154A, 0x2B53, 0x4994, { 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85 } };
static const CLSID CLSID_SampleGrabber = { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
static const CLSID CLSID_NullRenderer = { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

interface ISampleGrabberCB : public IUnknown
{
	virtual STDMETHODIMP SampleCB(double SampleTime, IMediaSample *pSample) = 0;
	virtual STDMETHODIMP BufferCB(double SampleTime, BYTE *pBuffer, long BufferLen) = 0;
};
interface ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize, long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *pCallback, long WhichMethodToCallback) = 0;
};

// based on code from: http://forum.devmaster.net/t/generating-textures-from-video-frames/8259

static CComPtr<ICaptureGraphBuilder2> graphBuilder;
static CComPtr<IFilterGraph2> filterGraph;
static CComPtr<ISampleGrabber> sampleGrabber;
static CComPtr<IMediaControl> mediaControl;
static CComPtr<IMediaSeeking> mediaSeeking;
static CComPtr<IMediaEvent> mediaEvent;
static CComPtr<IBasicAudio> audio;
static bool videoInitialized;
static int videoPaused;
static long *frameBuffer;
static long bufferSize;
static long frameBufferBytes;
static int videoWidth, videoHeight;
static int audio_chflags, audio_volume;
/* The selected disc track, copied over the other so the chosen programme
 * reaches both outputs. audio_chflags is written from the emulation thread
 * and read on the streaming thread; a torn int here is at worst one wrong
 * buffer, so it is deliberately unsynchronised. */
static CComPtr<ISampleGrabber> audioGrabber;
static bool audio_select_ok;
static int audio_bits, audio_channels;

class AudioChannelCB : public ISampleGrabberCB
{
public:
	LONG refs;
	AudioChannelCB() : refs(1) {}
	virtual ~AudioChannelCB() {}
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
			*ppv = static_cast<ISampleGrabberCB*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef(void) { return InterlockedIncrement(&refs); }
	STDMETHODIMP_(ULONG) Release(void) { return InterlockedDecrement(&refs); }
	STDMETHODIMP SampleCB(double t, IMediaSample *samp)
	{
		BYTE *p = NULL;
		if (!samp || FAILED(samp->GetPointer(&p)) || !p)
			return S_OK;
		apply(p, samp->GetActualDataLength());
		return S_OK;
	}
	STDMETHODIMP BufferCB(double t, BYTE *p, long len)
	{
		apply(p, len);
		return S_OK;
	}
	void apply(BYTE *p, long len)
	{
		int ch = audio_chflags;
		if (!p || len <= 0 || audio_channels != 2)
			return;
		if (ch != 1 && ch != 2)
			return;		/* 3 = both tracks as-is, 0 = muted by volume */
		int src = (ch == 1) ? 0 : 1;
		int dst = 1 - src;
		int bytes = audio_bits / 8;
		if (bytes != 2 && bytes != 3 && bytes != 4)
			return;
		long frame = bytes * 2;
		long n = len / frame;
		/* raw byte copy, so this is right for 16/24/32-bit PCM and float32 */
		for (long i = 0; i < n; i++) {
			BYTE *b = p + i * frame;
			memcpy(b + dst * bytes, b + src * bytes, bytes);
		}
	}
};
static AudioChannelCB audioCB;

/* Find the filter that renders audio -- a connected MEDIATYPE_Audio input
 * and no outputs -- and splice a Sample Grabber in front of it. */
static bool insert_audio_grabber(void)
{
	CComPtr<IEnumFilters> en;
	if (FAILED(filterGraph->EnumFilters(&en)))
		return false;
	IBaseFilter *f = NULL;
	ULONG got = 0;
	CComPtr<IPin> rendIn;
	while (!rendIn && en->Next(1, &f, &got) == S_OK && f) {
		CComPtr<IEnumPins> ep;
		if (SUCCEEDED(f->EnumPins(&ep))) {
			IPin *pin = NULL;
			bool hasOut = false;
			CComPtr<IPin> candidate;
			while (ep->Next(1, &pin, NULL) == S_OK && pin) {
				PIN_DIRECTION dir;
				if (SUCCEEDED(pin->QueryDirection(&dir))) {
					if (dir == PINDIR_OUTPUT) {
						hasOut = true;
					} else {
						AM_MEDIA_TYPE mt;
						memset(&mt, 0, sizeof mt);
						if (SUCCEEDED(pin->ConnectionMediaType(&mt))) {
							if (mt.majortype == MEDIATYPE_Audio)
								candidate = pin;
							if (mt.pbFormat)
								CoTaskMemFree(mt.pbFormat);
						}
					}
				}
				pin->Release();
				pin = NULL;
			}
			if (!hasOut && candidate)
				rendIn = candidate;
		}
		f->Release();
		f = NULL;
	}
	if (!rendIn) {
		write_log(_T("videograb: no audio renderer found\n"));
		return false;
	}
	CComPtr<IPin> upstream;
	if (FAILED(rendIn->ConnectedTo(&upstream)) || !upstream)
		return false;

	CComPtr<IBaseFilter> agFilter;
	if (FAILED(agFilter.CoCreateInstance(CLSID_SampleGrabber)))
		return false;
	if (FAILED(agFilter->QueryInterface(IID_ISampleGrabber, (void**)&audioGrabber)))
		return false;
	AM_MEDIA_TYPE want;
	memset(&want, 0, sizeof want);
	want.majortype = MEDIATYPE_Audio;	/* any PCM flavour the decoder gives */
	audioGrabber->SetMediaType(&want);
	audioGrabber->SetBufferSamples(FALSE);
	if (FAILED(filterGraph->AddFilter(agFilter, L"Audio Channel Select")))
		return false;
	if (FAILED(upstream->Disconnect()) || FAILED(rendIn->Disconnect()))
		return false;

	CComPtr<IPin> agIn, agOut;
	if (FAILED(agFilter->FindPin(L"In", &agIn)) ||
		FAILED(agFilter->FindPin(L"Out", &agOut)))
		return false;
	if (FAILED(filterGraph->ConnectDirect(upstream, agIn, NULL))) {
		if (FAILED(filterGraph->Connect(upstream, agIn)))
			return false;
	}
	if (FAILED(filterGraph->Connect(agOut, rendIn)))
		return false;

	AM_MEDIA_TYPE got2;
	memset(&got2, 0, sizeof got2);
	if (SUCCEEDED(audioGrabber->GetConnectedMediaType(&got2))) {
		if (got2.formattype == FORMAT_WaveFormatEx && got2.pbFormat) {
			WAVEFORMATEX *wf = (WAVEFORMATEX*)got2.pbFormat;
			audio_bits = wf->wBitsPerSample;
			audio_channels = wf->nChannels;
		}
		if (got2.pbFormat)
			CoTaskMemFree(got2.pbFormat);
	}
	if (FAILED(audioGrabber->SetCallback(&audioCB, 0)))
		return false;
	write_log(_T("videograb: audio channel select active, %d-bit %d channels\n"),
		audio_bits, audio_channels);
	return true;
}

static long *grabBuffer;
static long grabBufferBytes;
static int grabWidth, grabHeight;
static bool videoFrozen;

void uninitvideograb(void)
{
	write_log(_T("uninitvideograb\n"));

	videoInitialized = false;
	videoPaused = -1;
	audio_chflags = 0;
	audio_volume = 0;
	audioGrabber.Release();
	audio_select_ok = false;
	audio_bits = 0;
	audio_channels = 0;

	sampleGrabber.Release();
	mediaSeeking.Release();
	mediaEvent.Release();
	audio.Release();
	if (mediaControl) {
		mediaControl->Stop();
	}
	mediaControl.Release();
	filterGraph.Release();
	graphBuilder.Release();

	delete[] frameBuffer;
	frameBuffer = NULL;
	frameBufferBytes = 0;
}

static void FindPin(IBaseFilter* baseFilter, PIN_DIRECTION direction, int pinNumber, IPin** destPin)
{
	CComPtr<IEnumPins> enumPins;

	*destPin = NULL;

	if (SUCCEEDED(baseFilter->EnumPins(&enumPins))) {
		ULONG numFound;
		IPin* tmpPin;

		while (SUCCEEDED(enumPins->Next(1, &tmpPin, &numFound))) {
			PIN_DIRECTION pinDirection;

			tmpPin->QueryDirection(&pinDirection);
			if (pinDirection == direction) {
				if (pinNumber == 0) {
					// Return the pin's interface
					*destPin = tmpPin;
					break;
				}
				pinNumber--;
			}
			tmpPin->Release();
		}
	}
}

static bool ConnectPins(IBaseFilter* outputFilter, unsigned int outputNum, IBaseFilter* inputFilter, unsigned int inputNum)
{
	CComPtr<IPin> inputPin;
	CComPtr<IPin> outputPin;

	if (!outputFilter || !inputFilter) {
		write_log(_T("ConnectPins OUT=%d IN=%d\n"), outputFilter != 0, inputFilter != 0);
		return false;
	}

	FindPin(outputFilter, PINDIR_OUTPUT, outputNum, &outputPin);
	FindPin(inputFilter, PINDIR_INPUT, inputNum, &inputPin);

	if (inputPin && outputPin) {
		HRESULT hr = filterGraph->Connect(outputPin, inputPin);
		if (SUCCEEDED(hr))
			return true;
		write_log(_T("ConnectPins Connect %08x\n"), hr);
	} else {
		write_log(_T("ConnectPins OUTPIN=%d INPIN=%d\n"), outputPin != 0, inputPin != 0);

	}
	return false;
}

bool initvideograb(const TCHAR *filename)
{
	HRESULT hr;

	uninitvideograb();

	write_log(_T("initvideograb '%s'\n"), filename ? filename : _T("<null>"));

	graphBuilder.CoCreateInstance(CLSID_CaptureGraphBuilder2);
	filterGraph.CoCreateInstance(CLSID_FilterGraph);
	graphBuilder->SetFiltergraph(filterGraph);
	CComPtr<IBaseFilter> sourceFilter;

	if (filename == NULL || !filename[0]) {
		filename = NULL;
		// capture device mode
		IMoniker *pMoniker;
		CComPtr<ICreateDevEnum> pCreateDevEnum;
		hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC, IID_ICreateDevEnum, (void**)&pCreateDevEnum);
		if (FAILED(hr)) {
			write_log(_T("CLSID_SystemDeviceEnum IID_ICreateDevEnum failed %08x\n"), hr);
			uninitvideograb();
			return false;
		}
		CComPtr<IEnumMoniker> pEmum;
		hr = pCreateDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEmum, 0);
		if (FAILED(hr)) {
			write_log(_T("CreateClassEnumerator CLSID_VideoInputDeviceCategory failed %08x\n"), hr);
			uninitvideograb();
			return false;
		}
		if (hr == S_FALSE) {
			write_log(_T("initvideograb CreateDevEnum: didn't find any capture devices.\n"));
			uninitvideograb();
			return false;
		}
		pEmum->Reset();
		ULONG cFetched = 0;
		//Take the first capture device found
		hr = pEmum->Next(1, &pMoniker, &cFetched);
		if (FAILED(hr)) {
			write_log(_T("initvideograb Next: didn't find any capture devices.\n"));
			uninitvideograb();
			return false;
		}
		pMoniker->BindToObject(NULL, NULL, IID_IBaseFilter, (void**)&sourceFilter);
		pMoniker->Release();
		filterGraph->AddFilter(sourceFilter, L"Video Capture");
	}

	// Create the Sample Grabber which we will use
	// To take each frame for texture generation
	CComPtr<IBaseFilter> grabberFilter;
	grabberFilter.CoCreateInstance(CLSID_SampleGrabber);
	grabberFilter->QueryInterface(IID_ISampleGrabber, reinterpret_cast<void**>(&sampleGrabber));

	hr = filterGraph->AddFilter(grabberFilter, L"Sample Grabber");

	// We have to set the 24-bit RGB desire here
	// So that the proper conversion filters
	// Are added automatically.
	AM_MEDIA_TYPE desiredType;
	memset(&desiredType, 0, sizeof(desiredType));
	desiredType.majortype = MEDIATYPE_Video;
	desiredType.subtype = MEDIASUBTYPE_RGB24;
	desiredType.formattype = FORMAT_VideoInfo;

	hr = sampleGrabber->SetMediaType(&desiredType);
	hr = sampleGrabber->SetBufferSamples(TRUE);

	if (filename) {
		hr = filterGraph->RenderFile(filename, NULL);
		if (FAILED(hr)) {
			uninitvideograb();
			return false;
		}
	}

	if (!filename) {
		// Use pin connection methods instead of 
		// ICaptureGraphBuilder::RenderStream because of
		// the SampleGrabber setting we're using.
		if (!ConnectPins(sourceFilter, 0, grabberFilter, 0)) {
			uninitvideograb();
			return false;
		}
	}

	// A Null Renderer does not display the video
	// But it allows the Sample Grabber to run
	// And it will keep proper playback timing
	// Unless specified otherwise.
	CComPtr<IBaseFilter> nullRenderer;
	nullRenderer.CoCreateInstance(CLSID_NullRenderer);

	hr = filterGraph->AddFilter(nullRenderer, L"Null Renderer");
	if (FAILED(hr)) {
		uninitvideograb();
		return false;
	}

	if (!filename) {
		if (!ConnectPins(grabberFilter, 0, nullRenderer, 0)) {
			uninitvideograb();
			return false;
		}
	}

	// Just a little trick so that we don't have to know
	// The video resolution when calling this method.
	bool mediaConnected = false;
	AM_MEDIA_TYPE connectedType;
	hr = sampleGrabber->GetConnectedMediaType(&connectedType);
	if (SUCCEEDED(hr)) {
		if (connectedType.formattype == FORMAT_VideoInfo) {
			VIDEOINFOHEADER* infoHeader = (VIDEOINFOHEADER*)connectedType.pbFormat;
			videoWidth = infoHeader->bmiHeader.biWidth;
			videoHeight = infoHeader->bmiHeader.biHeight;
			mediaConnected = true;
		}
		CoTaskMemFree(connectedType.pbFormat);
	}

	if (!mediaConnected) {
		uninitvideograb();
		return false;
	}

	if (filename) {
		// based on ofDirectShowPlayer
		IPin* pinIn = NULL;
		IPin* pinOut = NULL;
		IBaseFilter * m_pVideoRenderer;
		hr = filterGraph->FindFilterByName(_T("Video Renderer"), &m_pVideoRenderer);
		if (FAILED(hr)) {
			write_log(_T("FindFilterByName failed: %08x\n"), hr);
			uninitvideograb();
			return false;
		}
		hr = grabberFilter->FindPin(_T("Out"), &pinOut);
		if (FAILED(hr)) {
			write_log(_T("FindPin Out failed: %08x\n"), hr);
			m_pVideoRenderer->Release();
			uninitvideograb();
			return false;
		}
		hr = pinOut->Disconnect();
		if (FAILED(hr)) {
			write_log(_T("Disconnect failed: %08x\n"), hr);
			m_pVideoRenderer->Release();
			uninitvideograb();
			return false;
		}
		hr = filterGraph->RemoveFilter(m_pVideoRenderer);
		if (FAILED(hr)) {
			write_log(_T("RemoveFilter failed: %08x\n"), hr);
			m_pVideoRenderer->Release();
			uninitvideograb();
			return false;
		}
		m_pVideoRenderer->Release();
		hr = nullRenderer->FindPin(_T("In"), &pinIn);
		if (FAILED(hr)) {
			write_log(_T("FindPin In failed: %08x\n"), hr);
			uninitvideograb();
			return false;
		}
		hr = pinOut->Connect(pinIn, NULL);
		if (FAILED(hr)) {
			write_log(_T("Connect In failed: %08x\n"), hr);
			uninitvideograb();
			return false;
		}
	}

	hr = filterGraph->QueryInterface(IID_IMediaSeeking, (void**)&mediaSeeking);

	hr = filterGraph->QueryInterface(IID_IMediaEvent, (void**)&mediaEvent);

	/* What did RenderFile actually build? A missing audio renderer is the
	 * difference between "muted" and "there was never any sound". */
	{
		CComPtr<IEnumFilters> en;
		if (SUCCEEDED(filterGraph->EnumFilters(&en))) {
			IBaseFilter *f = NULL;
			ULONG got = 0;
			while (en->Next(1, &f, &got) == S_OK && f) {
				FILTER_INFO fi;
				memset(&fi, 0, sizeof fi);
				if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
					write_log(_T("videograb filter: '%s'\n"), fi.achName);
					if (fi.pGraph)
						fi.pGraph->Release();
				}
				f->Release();
				f = NULL;
			}
		}
	}

	audio_select_ok = insert_audio_grabber();

	hr = filterGraph->QueryInterface(IID_IBasicAudio, (void**)&audio);
	write_log(_T("videograb IBasicAudio %08x, iface %p, genlock_audio_mute %d, sound_volume_genlock %d\n"),
		hr, (void*)audio, currprefs.genlock_audio_mute ? 1 : 0,
		currprefs.sound_volume_genlock);
	setvolumevideograb(100 - currprefs.sound_volume_genlock);
	setchflagsvideograb(0, false);

	hr = filterGraph->QueryInterface(IID_IMediaControl, (void**)&mediaControl);
	if (FAILED(hr)) {
		uninitvideograb();
		return false;
	}
	if (mediaSeeking) {
		hr = mediaSeeking->SetTimeFormat(&TIME_FORMAT_FRAME);
		if (FAILED(hr))
			write_log(_T("SetTimeFormat format %08x\n"), hr);
	}
	if (SUCCEEDED(mediaControl->Run())) {
		videoInitialized = true;
		write_log(_T("Playing '%s'\n"), filename ? filename : _T("<capture>"));
		return true;
	} else {
		uninitvideograb();
		return false;
	}
}

uae_s64 getdurationvideograb(void)
{
	LONGLONG dura;
	if (!mediaSeeking) {
		return 0;
	}
	HRESULT hr = mediaSeeking->GetDuration(&dura);
	if (FAILED(hr)) {
		return 0;
	}
	return dura;
}

uae_s64 getsetpositionvideograb(uae_s64 framepos)
{
	if (!videoInitialized || !mediaSeeking)
		return 0;
	LONGLONG pos;
	HRESULT hr;
	if (framepos < 0) {
		LONGLONG stoppos;
		hr = mediaSeeking->GetPositions(&pos, &stoppos);
		if (FAILED(hr)) {
			write_log(_T("GetPositions failed %08x\n"), hr);
			pos = 0;
		}
		return pos;
	} else {
		LONGLONG pos = framepos;
		hr = mediaSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
		if (FAILED(hr)) {
			write_log(_T("SetPositions %lld failed %08x\n"), framepos, hr);
			return 0;
		}
		return pos;
	}
}

void setchflagsvideograb(int chflags, bool mute)
{
	if (!audio)
		return;
	audio_chflags = chflags;
	/* With the channel-select grabber in place the chosen track is already
	 * on both outputs, so the balance stays centred. Panning is only the
	 * fallback for when the splice failed -- it isolates the right
	 * programme, just on one speaker. */
	long bal = 0;
	if (!audio_select_ok) {
		if (chflags == 1)
			bal = -10000;
		else if (chflags == 2)
			bal = 10000;
	}
	if (!currprefs.win32_videograb_balance) {
		audio->put_Balance(bal);
	}
	if (chflags && !mute && !currprefs.genlock_audio_mute) {
		setvolumevideograb(audio_volume);
	} else {
		// IBasicAudio is an attenuation: 0 is FULL volume and -10000 is
		// silence. Putting 0 here muted nothing - it did the opposite, and
		// since initvideograb() calls this with chflags 0 the clip played at
		// full blast the moment the graph was built.
		audio->put_Volume(-10000);
	}
}

void setvolumevideograb(int volume)
{
	if (!audio)
		return;
	audio_volume = volume;
	if (!audio_chflags || currprefs.genlock_audio_mute) {
		volume = 0;
	}
	// log10(0) is -inf and the cast to long is undefined, so silence has to
	// be the explicit -10000 rather than something the FPU happens to hand us.
	long vol = volume > 0 ? (long)(log10((float)volume / 100.0) * 4000.0) : -10000;
	if (vol < -10000)
		vol = -10000;
	else if (vol > 0)
		vol = 0;
	audio->put_Volume(vol);
}

bool getpausevideograb(void)
{
	return videoPaused > 0;
}

void pausevideograb(int pause)
{
	HRESULT hr;
	if (!videoInitialized)
		return;
	if (videoPaused == pause)
		return;
	if (pause < 0) {
		pause = videoPaused ? 0 : 1;
	}
	if (pause > 0) {
		hr = mediaControl->Pause();
		if (SUCCEEDED(hr))
			videoPaused = 1;
	} else if (pause == 0) {
		hr = mediaControl->Run();
		if (SUCCEEDED(hr))
			videoPaused = 0;
	}
}

bool getfreezevideograb(void)
{
	return videoFrozen;
}

/* Snapshot the frame on screen now and keep handing it out until the
 * freeze is lifted. The graph is left alone, so sound carries on from
 * wherever the player was sent. */
void setfreezevideograb(int freeze)
{
	if (!videoInitialized || freeze <= 0) {
		videoFrozen = false;
		return;
	}
	if (videoFrozen)
		return;
	long *b = NULL;
	int w = 0, h = 0;
	if (!getvideograb(&b, &w, &h) || !b)
		return;
	long stride = (w * 3 + 3) & ~3;
	long need = stride * (h < 0 ? -h : h);
	if (need <= 0)
		return;
	if (!grabBuffer || need > grabBufferBytes) {
		delete[] grabBuffer;
		grabBuffer = new long[(need + 3) / 4];
		grabBufferBytes = need;
	}
	memcpy(grabBuffer, b, need);
	grabWidth = w;
	grabHeight = h;
	videoFrozen = true;
}

bool getvideograb(long **buffer, int *width, int *height)
{
	HRESULT hr;

	if (!videoInitialized)
		return false;

	if (videoFrozen && grabBuffer) {
		*buffer = grabBuffer;
		*width = grabWidth;
		*height = grabHeight;
		return true;
	}

	// The caller reads a whole RGB24 frame (rows padded to 4 bytes) out of
	// the buffer, so never hand out one that holds less than that, and grow
	// the buffer if the sample size grows. The old code sized the buffer once,
	// from whatever the first query returned after the graph (re)started --
	// which happens on every emulated reset -- and handed it out whatever
	// the copy returned. An Amiga reboot crashed in do_genlock() reading
	// past the end of a small heap block, which fits a short first sample.
	long size = 0;
	hr = sampleGrabber->GetCurrentBuffer(&size, NULL);
	if (FAILED(hr)) {
		return false;
	}
	long stride = (videoWidth * 3 + 3) & ~3;
	long need = stride * (videoHeight < 0 ? -videoHeight : videoHeight);
	if (size != need) {
		// the decoder may have renegotiated the frame size since init
		AM_MEDIA_TYPE mt;
		if (SUCCEEDED(sampleGrabber->GetConnectedMediaType(&mt))) {
			if (mt.formattype == FORMAT_VideoInfo && mt.pbFormat) {
				VIDEOINFOHEADER *vih = (VIDEOINFOHEADER*)mt.pbFormat;
				if (vih->bmiHeader.biWidth != videoWidth || vih->bmiHeader.biHeight != videoHeight) {
					write_log(_T("getvideograb: frame size %dx%d -> %dx%d\n"),
						videoWidth, videoHeight, vih->bmiHeader.biWidth, vih->bmiHeader.biHeight);
					videoWidth = vih->bmiHeader.biWidth;
					videoHeight = vih->bmiHeader.biHeight;
				}
			}
			CoTaskMemFree(mt.pbFormat);
		}
		stride = (videoWidth * 3 + 3) & ~3;
		need = stride * (videoHeight < 0 ? -videoHeight : videoHeight);
	}
	if (size < need || need <= 0) {
		return false;
	}
	if (!frameBuffer || size > frameBufferBytes) {
		delete[] frameBuffer;
		frameBuffer = new long[(size + 3) / 4];
		frameBufferBytes = size;
	}

	bufferSize = frameBufferBytes;
	hr = sampleGrabber->GetCurrentBuffer(&bufferSize, (long*)frameBuffer);
	if (SUCCEEDED(hr) && bufferSize >= need) {
		*buffer = frameBuffer;
		*width = videoWidth;
		*height = videoHeight;
		return true;
	}
	write_log(_T("getvideograb get buffer %08x\n"), hr);
	return false;
}

bool isvideograb(void)
{
	return videoInitialized != 0;
}

void isvideograb_status(void)
{
	if (!videoInitialized)
		return;
	if (currprefs.genlock_audio_mute != changed_prefs.genlock_audio_mute) {
		currprefs.genlock_audio_mute = changed_prefs.genlock_audio_mute;
		setchflagsvideograb(audio_chflags, currprefs.genlock_audio_mute);
	}
	if (currprefs.sound_volume_genlock != changed_prefs.sound_volume_genlock) {
		currprefs.sound_volume_genlock = changed_prefs.sound_volume_genlock;
		setvolumevideograb(100 - currprefs.sound_volume_genlock);
	}
	if (mediaEvent == NULL)
		return;
	long EventCode;
	LONG_PTR lParam1, lParam2;
	for (;;) {
		HRESULT hr = mediaEvent->GetEvent(&EventCode, &lParam1, &lParam2, 0);
		if (FAILED(hr))
			break;
		mediaEvent->FreeEventParams(EventCode, lParam1, lParam2);
		write_log(_T("VIDEOGRAB EVENT %08X %08X %08X\n"), EventCode, lParam1, lParam2);
		if (EventCode == EC_COMPLETE) {
			getsetpositionvideograb(0);
		}
	}
}
