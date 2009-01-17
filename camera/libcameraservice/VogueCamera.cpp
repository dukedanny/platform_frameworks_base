#define LOG_TAG "FakeCamera"
#include <utils/Log.h>

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include "VogueCamera.h"

namespace android {


const int FakeCamera::kRed;
const int FakeCamera::kGreen;
const int FakeCamera::kBlue;

const int buffersize=320*240+320*240/2;

FakeCamera::FakeCamera(int width, int height)
	//      : mTmpRgb16Buffer(0)
{
  setSize(width, height);
}



FakeCamera::~FakeCamera()
{
	stop();
	//   delete[] mTmpRgb16Buffer;
}

void FakeCamera::start(void) {
	vfe_dev=open("/dev/vfe",O_RDONLY);
	if(vfe_dev<0) return;
	frame_buffer=(char *)mmap(NULL,buffersize*4,PROT_READ,MAP_SHARED,vfe_dev,0);
	LOGI("frame buffer=%x\n",(unsigned)frame_buffer);
	if(frame_buffer==MAP_FAILED)
		frame_buffer=0;
}

void FakeCamera::stop(void) {
	close(vfe_dev);
}
void FakeCamera::setSize(int width, int height)
{
    mWidth = width;
    mHeight = height;
    mCounter = 0;
    mCheckX = 0;
    mCheckY = 0;

    // This will cause it to be reallocated on the next call
    // to getNextFrameAsYuv422().
	//   delete[] mTmpRgb16Buffer;
	//    mTmpRgb16Buffer = 0;
}
/*
void FakeCamera::getNextFrameAsRgb565(uint16_t *buffer)
{
    int size = mWidth / 10;

    drawCheckerboard(buffer, size);

    int x = ((mCounter*3)&255);
    if(x>128) x = 255 - x;
    int y = ((mCounter*5)&255);
    if(y>128) y = 255 - y;

    drawSquare(buffer, x*size/32, y*size/32, (size*5)>>1, (mCounter&0x100)?kRed:kGreen, kBlue);

    mCounter++;
}
*/
/* the equation used by the video code to translate YUV to RGB looks like this
 *
 *    Y  = (Y0 - 16)*k0
 *    Cb = Cb0 - 128
 *    Cr = Cr0 - 128
 *
 *    G = ( Y - k1*Cr - k2*Cb )
 *    R = ( Y + k3*Cr )
 *    B = ( Y + k4*Cb )
 *
 */
static const int  k0 = 298;
static const int  k1 = 208;
static const int  k2 = 100;
static const int  k3 = 409;
static const int  k4 = 516;

/*
inline static int clamp(int  x) {
    if (x > 255) return 255;
    if (x < 0)   return 0;
    return x;
}
*/

#define clamp(x) ((unsigned int) x <= 255 ? x : (x < 0 ? 0: 255))


void yuv420_to_rgb565(short *buffer_out,char *buffer_in) {
	char *uv_in;
	char *y_in;
	int i,j;
	int y,u,v;
	int r,g,b;
	int gp,rp,bp;
	for(i=0;i<240;i++) {
		uv_in=buffer_in+320*240+(i/2)*320;
		y_in=buffer_in+i*320;
		for(j=0;j<160;j++) {
			y=((*y_in++-16)*k0);
			u=*uv_in++ - 128;
			v=*uv_in++ - 128;
			gp=(k1*v)+(k2*u);
			bp=(k3*v);
			rp=(k4*u);
			r=(y+rp)>>8;
			g=(y-gp)>>8;
			b=(y+bp)>>8;
			*buffer_out++=(clamp(r)>>3)<<11 | (clamp(g)>>2)<<5 | (clamp(b)>>3);
			y=((*y_in++-16)*k0);
			r=(y+rp)>>8;
			g=(y-gp)>>8;
			b=(y+bp)>>8;
			*buffer_out++=(clamp(r)>>3)<<11 | (clamp(g)>>2)<<5 | (clamp(b)>>3);
		}
	}
}

void FakeCamera::getRawSnapshot(uint8_t *buffer) {
	yuv420_to_rgb565((short *)buffer,frame_buffer);
}

void FakeCamera::getNextFrameAsYuv422(uint8_t *buffer)
{
	unsigned offset;
	int i,j;
	char *linestart,*ystart,*uvstart;

	read(vfe_dev,&offset,4);

	LOGI("get next frame: %x=%x\n",(unsigned)offset,*(frame_buffer+offset));

	yuv420_to_rgb565((short *)buffer,frame_buffer+offset);

	//memcpy(buffer,frame_buffer+offset,mWidth*mHeight+mWidth*mHeight/2); //+mWidth*mHeight/2);
	/*

	for(int i=0;i<mHeight;i++) {
		linestart=(char *)buffer+mWidth*mHeight+i*mWidth;
		uvstart=frame_buffer+offset+320*240+(i/2)*320;
		for(int j=0;j<mWidth;j++) {
			//			*linestart++=*ystart++;
			*linestart++=*uvstart++;
		}
	}
	*/

	//    getNextFrameAsRgb565(mTmpRgb16Buffer);
	//  convert_rgb16_to_yuv422((uint8_t*)mTmpRgb16Buffer, buffer, mWidth, mHeight);
}


status_t FakeCamera::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, 255, " width x height (%d x %d), counter (%d), check x-y coordinate(%d, %d)\n", mWidth, mHeight, mCounter, mCheckX, mCheckY);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}


}; // namespace android
