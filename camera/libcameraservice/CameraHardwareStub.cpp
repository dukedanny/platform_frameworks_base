/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CameraHardwareStub"
#include <utils/Log.h>

#include "CameraHardwareStub.h"
#include <utils/threads.h>
#include <fcntl.h>
#include <sys/mman.h>


extern "C" {
#include "jpeglib.h"
}
//#include "CannedJpeg.h"

namespace android {

CameraHardwareStub::CameraHardwareStub()
                  : mParameters(),
                    mHeap(0),
                    mFakeCamera(0),
                    mPreviewFrameSize(0),
                    mRawPictureCallback(0),
                    mJpegPictureCallback(0),
                    mPictureCallbackCookie(0),
                    mPreviewCallback(0),
                    mPreviewCallbackCookie(0),
                    mAutoFocusCallback(0),
                    mAutoFocusCallbackCookie(0),
                    mCurrentPreviewFrame(0)
{
    initDefaultParameters();
}

void CameraHardwareStub::initDefaultParameters()
{
    CameraParameters p;

    p.setPreviewSize(320, 240);
    p.setPreviewFrameRate(15);
    p.setPreviewFormat("rgb565");

    p.setPictureSize(1600,1200);
    p.setPictureFormat("jpeg");

    if (setParameters(p) != NO_ERROR) {
        LOGE("Failed to set default parameters?!");
    } 
}

void CameraHardwareStub::initHeapLocked()
{
    int width, height;
    mParameters.getPreviewSize(&width, &height);

    LOGD("initHeapLocked: preview size=%dx%d", width, height);

    // Note that we enforce yuv422 in setParameters().
    int how_big = width * height * 2;

    // If we are being reinitialized to the same size as before, no
    // work needs to be done.
    if (how_big == mPreviewFrameSize)
        return;

    mPreviewFrameSize = how_big;

    // Make a new mmap'ed heap that can be shared across processes. 
    // use code below to test with pmem
    mHeap = new MemoryHeapBase(mPreviewFrameSize * kBufferCount);
    // Make an IMemory for each frame so that we can reuse them in callbacks.
    for (int i = 0; i < kBufferCount; i++) {
        mBuffers[i] = new MemoryBase(mHeap, i * mPreviewFrameSize, mPreviewFrameSize);
    }

    // Recreate the fake camera to reflect the current size.
    delete mFakeCamera;
    mFakeCamera = new FakeCamera(width, height);
}

CameraHardwareStub::~CameraHardwareStub()
{
    delete mFakeCamera;
    mFakeCamera = 0; // paranoia
    singleton.clear();
}

sp<IMemoryHeap> CameraHardwareStub::getPreviewHeap() const
{
    return mHeap;
}

// ---------------------------------------------------------------------------

int CameraHardwareStub::previewThread()
{
    mLock.lock();
        // the attributes below can change under our feet...

        int previewFrameRate = mParameters.getPreviewFrameRate();

        // Find the offset within the heap of the current buffer.
        ssize_t offset = mCurrentPreviewFrame * mPreviewFrameSize;

        sp<MemoryHeapBase> heap = mHeap;
    
        // this assumes the internal state of fake camera doesn't change
        // (or is thread safe)
        FakeCamera* fakeCamera = mFakeCamera;
	
        
        sp<MemoryBase> buffer = mBuffers[mCurrentPreviewFrame];
        
    mLock.unlock();

    // TODO: here check all the conditions that could go wrong
    if (buffer != 0) {
        // Calculate how long to wait between frames.
        int delay = (int)(1000000.0f / float(previewFrameRate));
    
        // This is always valid, even if the client died -- the memory
        // is still mapped in our process.
        void *base = heap->base();
    
        // Fill the current frame with the fake camera.
        uint8_t *frame = ((uint8_t *)base) + offset;
        fakeCamera->getNextFrameAsYuv422(frame);
    
        //LOGV("previewThread: generated frame to buffer %d", mCurrentPreviewFrame);
        
        // Notify the client of a new frame.
        mPreviewCallback(buffer, mPreviewCallbackCookie);
    
        // Advance the buffer pointer.
        mCurrentPreviewFrame = (mCurrentPreviewFrame + 1) % kBufferCount;

        // Wait for it...
        usleep(delay);
    }

    return NO_ERROR;
}

status_t CameraHardwareStub::startPreview(preview_callback cb, void* user)
{
    Mutex::Autolock lock(mLock);
    if (mPreviewThread != 0) {
        // already running
        return INVALID_OPERATION;
    }
	  mFakeCamera->start();
    mPreviewCallback = cb;
    mPreviewCallbackCookie = user;
    mPreviewThread = new PreviewThread(this);
    return NO_ERROR;
}

void CameraHardwareStub::stopPreview()
{
    sp<PreviewThread> previewThread;
    
    { // scope for the lock
        Mutex::Autolock lock(mLock);
        previewThread = mPreviewThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (previewThread != 0) {
        previewThread->requestExitAndWait();
    }
		mFakeCamera->stop();
    Mutex::Autolock lock(mLock);
    mPreviewThread.clear();
}

bool CameraHardwareStub::previewEnabled() {
    return mPreviewThread != 0;
}

// ---------------------------------------------------------------------------

int CameraHardwareStub::beginAutoFocusThread(void *cookie)
{
    CameraHardwareStub *c = (CameraHardwareStub *)cookie;
    return c->autoFocusThread();
}

int CameraHardwareStub::autoFocusThread()
{
    if (mAutoFocusCallback != NULL) {
        mAutoFocusCallback(true, mAutoFocusCallbackCookie);
        mAutoFocusCallback = NULL;
        return NO_ERROR;
    }
    return UNKNOWN_ERROR;
}

status_t CameraHardwareStub::autoFocus(autofocus_callback af_cb,
                                       void *user)
{
    Mutex::Autolock lock(mLock);

    if (mAutoFocusCallback != NULL) {
        return mAutoFocusCallback == af_cb ? NO_ERROR : INVALID_OPERATION;
    }

    mAutoFocusCallback = af_cb;
    mAutoFocusCallbackCookie = user;
    if (createThread(beginAutoFocusThread, this) == false)
        return UNKNOWN_ERROR;
    return NO_ERROR;
}

/*static*/ int CameraHardwareStub::beginPictureThread(void *cookie)
{
    CameraHardwareStub *c = (CameraHardwareStub *)cookie;
    return c->pictureThread();
}
static const int  k0 = 298;
static const int  k1 = 208;
static const int  k2 = 100;
static const int  k3 = 409;
static const int  k4 = 516;
#define clamp(x) ((unsigned int) x <= 255 ? x : (x < 0 ? 0: 255))

int CameraHardwareStub::pictureThread()
{
	FILE *picfile;
    if (mShutterCallback)
        mShutterCallback(mPictureCallbackCookie);

	
    if (mRawPictureCallback) {
        //FIXME: use a canned YUV image!
        // In the meantime just make another fake camera picture.
        int w, h;
	    //mParameters.getPictureSize(&w, &h);
	    //  sp<MemoryHeapBase> heap = new MemoryHeapBase(w * 2 * h);
	    //  sp<MemoryBase> mem = new MemoryBase(heap, 0, w * 2 * h);
	    //  FakeCamera cam(w, h);
	    //	    	cam.start();
	    //  cam.getNextFrameAsYuv422((uint8_t *)heap->base());
	    //	cam.stop();
	    
        if (mRawPictureCallback)
	             mRawPictureCallback(mBuffers[mCurrentPreviewFrame], mPictureCallbackCookie);
    }
	
	
	picfile=fopen("/sdroot/picture.jpg","wb");
	if(!picfile) 
		LOGE("Can't open temporary picture file");
	else {
		struct jpeg_compress_struct cinfo;
		struct jpeg_error_mgr jerr;
		JSAMPROW row_pointer[1];
		int cam_dev;
		char *image_buffer;
		int imagesize=1600*1200+1600*1200/2;
		
		int row_stride;
		int w, h,i;
		int filesize;
		char line[1600*3];
		int ready;
		char *frame;//(char *)mHeap->base()+mCurrentPreviewFrame * mPreviewFrameSize;
		char *uvframe;
		short pixel;
      LOGI("Opening Camera device"); 
		cam_dev=open("/dev/camera",O_RDONLY);
		LOGI("Mmaping fb"); 
		image_buffer=(char *)mmap(NULL,imagesize,PROT_READ,MAP_SHARED,cam_dev,0);
		frame=image_buffer;
		LOGI("Reading frame"); 
		read(cam_dev,&ready,4);
		LOGI("Sending small snapshot");
		if (mRawPictureCallback) {
			int w=320, h=240;
			sp<MemoryHeapBase> heap = new MemoryHeapBase(w * 2 * h);
			sp<MemoryBase> mem = new MemoryBase(heap, 0, w * 2 * h);
			mFakeCamera->getRawSnapshot((uint8_t *)heap->base());
			if (mRawPictureCallback)
							mRawPictureCallback(mem, mPictureCallbackCookie);
      }
		mParameters.getPictureSize(&w, &h);
		uvframe=frame+w*h;
		LOGI("Writing %d x %d JPEG",w,h); 
		cinfo.err = jpeg_std_error(&jerr);
		jpeg_create_compress(&cinfo);
		jpeg_stdio_dest(&cinfo, picfile);
		cinfo.image_width = w;
		cinfo.image_height = h;
		cinfo.input_components = 3;
		cinfo.in_color_space = JCS_RGB;
		jpeg_set_defaults(&cinfo);
		jpeg_set_quality(&cinfo, 85, TRUE); // quality of 80
		jpeg_start_compress(&cinfo, TRUE);
		//		row_stride = w;
		while (cinfo.next_scanline < cinfo.image_height) {
			//			LOGI("line %d\n",cinfo.next_scanline);
			for(i=0;i<w;i++) {
				int y,u,v,r,g,b,gp,rp,bp;
				/* for yuv
				line[i*3]=*(frame+cinfo.next_scanline*w+i);
				line[i*3+1]=*(uvframe+(cinfo.next_scanline/2)*w+(i/2)*2);
				line[i*3+2]=*(uvframe+(cinfo.next_scanline/2)*w+(i/2)*2+1);
				*/
				// for rgb_565
				/*
				pixel=*((short *)frame+cinfo.next_scanline*w+i);
				line[i*3]=(pixel&0xf800)>>8;
				line[i*3+1]=(pixel&0x07e0)>>3;
				line[i*3+2]=(pixel&0x01f)<<3;
				*/
				// convert yuv to rgb for jpeg compression
				y=(*(frame+cinfo.next_scanline*w+i)-16)*k0;
				u=*(uvframe+(cinfo.next_scanline/2)*w+(i/2)*2)-128;
				v=*(uvframe+(cinfo.next_scanline/2)*w+(i/2)*2+1)-128;
				gp=(k1*v)+(k2*u);
				bp=(k3*v);
				rp=(k4*u);
				r=(y+rp)>>8;
				g=(y-gp)>>8;
				b=(y+bp)>>8;
				line[i*3]=clamp(r);
				line[i*3+1]=clamp(g);
				line[i*3+2]=clamp(b);
			}
			row_pointer[0] = (JSAMPLE *)line;//(frame+(cinfo.next_scanline * row_stride));
    	(void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
			//			LOGI("write_scnlines done");
  	}
		LOGI("JPEG Written"); 
		jpeg_finish_compress(&cinfo);
		filesize=ftell(picfile);
		fclose(picfile);
		close(cam_dev);
		jpeg_destroy_compress(&cinfo);
    if (mJpegPictureCallback) {
  		sp<MemoryHeapBase> heap = new MemoryHeapBase(filesize);
  		sp<MemoryBase> mem = new MemoryBase(heap, 0, filesize);
  		picfile=fopen("/sdroot/picture.jpg","rb");
	    if(!picfile)
					LOGE("Can't open temporary picture file for reading");
	    else {
		    fread((uint8_t *)heap->base(),1,filesize,picfile);
		    fclose(picfile); 
        if (mJpegPictureCallback)
            mJpegPictureCallback(mem, mPictureCallbackCookie);
	    }
    }
	}
    return NO_ERROR;
}

status_t CameraHardwareStub::takePicture(shutter_callback shutter_cb,
                                         raw_callback raw_cb,
                                         jpeg_callback jpeg_cb,
                                         void* user)
{
    stopPreview();
    mShutterCallback = shutter_cb;
    mRawPictureCallback = raw_cb;
    mJpegPictureCallback = jpeg_cb;
    mPictureCallbackCookie = user;
    if (createThread(beginPictureThread, this) == false)
        return -1;
    return NO_ERROR;
}

status_t CameraHardwareStub::cancelPicture(bool cancel_shutter,
                                           bool cancel_raw,
                                           bool cancel_jpeg)
{
    if (cancel_shutter) mShutterCallback = NULL;
    if (cancel_raw) mRawPictureCallback = NULL;
    if (cancel_jpeg) mJpegPictureCallback = NULL;
    return NO_ERROR;
}

status_t CameraHardwareStub::dump(int fd, const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    AutoMutex lock(&mLock);
    if (mFakeCamera != 0) {
        mFakeCamera->dump(fd, args);
        mParameters.dump(fd, args);
        snprintf(buffer, 255, " preview frame(%d), size (%d), running(%s)\n", mCurrentPreviewFrame, mPreviewFrameSize, mPreviewRunning?"true": "false");
        result.append(buffer);
    } else {
        result.append("No camera client yet.\n");
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t CameraHardwareStub::setParameters(const CameraParameters& params)
{
    Mutex::Autolock lock(mLock);
    // XXX verify params

//    if (strcmp(params.getPreviewFormat(), "yuv422sp") != 0) {
//        LOGE("Only yuv422sp preview is supported");
//        return -1;
//    }

//    if (strcmp(params.getPictureFormat(), "jpeg") != 0) {
//        LOGE("Only jpeg still pictures are supported");
//        return -1;
//    }

    int w, h;
    params.getPictureSize(&w, &h);
	/*
    if (w != kCannedJpegWidth && h != kCannedJpegHeight) {
        LOGE("Still picture size must be size of canned JPEG (%dx%d)",
             kCannedJpegWidth, kCannedJpegHeight);
        return -1;
    }
*/
    mParameters = params;

    initHeapLocked();

    return NO_ERROR;
}

CameraParameters CameraHardwareStub::getParameters() const
{
    Mutex::Autolock lock(mLock);
    return mParameters;
}

void CameraHardwareStub::release()
{
}

wp<CameraHardwareInterface> CameraHardwareStub::singleton;

sp<CameraHardwareInterface> CameraHardwareStub::createInstance()
{
    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            return hardware;
        }
    }
    sp<CameraHardwareInterface> hardware(new CameraHardwareStub());
    singleton = hardware;
    return hardware;
}

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    return CameraHardwareStub::createInstance();
}

}; // namespace android
