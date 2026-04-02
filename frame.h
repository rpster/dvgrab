/*
* frame.h -- utilities for process digital video frames
* Copyright (C) 2000 Arne Schirmacher <arne@schirmacher.de>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software Foundation,
* Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifndef _FRAME_H
#define _FRAME_H 1

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

// C++ includes

#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <deque>

using std::string;
using std::ostringstream;
using std::setw;
using std::setfill;
using std::deque;
using std::cout;
using std::endl;

// C includes

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define TIMECODE_TO_SEC( tc ) (((((tc).hour * 60) + (tc).min) * 60) + (tc).sec)

typedef struct TimeCode
{
	int hour;
	int min;
	int sec;
	int frame;
}
TimeCode;


#define DATA_BUFFER_LEN (1024*1024)

// DV25 (DV/DVCAM/DVCPRO) frames: 120000 (NTSC) or 144000 (PAL)
#define DV_NTSC_FRAME_SIZE    120000
#define DV_PAL_FRAME_SIZE     144000

// DVCPRO50 frames: double the DIF sequences
#define DVCPRO50_NTSC_FRAME_SIZE  240000
#define DVCPRO50_PAL_FRAME_SIZE   288000

// DVCPRO HD (100 Mbps) frames: quadruple the DIF sequences
#define DVCPROHD_NTSC_FRAME_SIZE  480000
#define DVCPROHD_PAL_FRAME_SIZE   576000

// Buffer must accommodate the largest possible DV frame
#define DV_FRAME_BUFFER_LEN DVCPROHD_PAL_FRAME_SIZE

// Maximum allowed value for -buffers flag
#define MAX_BUFFERS 2000

class Frame
{
public:
	unsigned char *data;
private:
	int dataLen;
	int dataCapacity;

public:
	Frame( int bufferSize = DATA_BUFFER_LEN );
	virtual ~Frame();

	// Prevent copying (buffer is owned)
	Frame( const Frame& ) = delete;
	Frame& operator=( const Frame& ) = delete;

	virtual int GetDataLen( void );
	virtual int GetDataCapacity( void );
	virtual void SetDataLen( int len );
	virtual void AddDataLen( int len );
	virtual void Clear( void );

	// Meta-data
	virtual bool GetTimeCode( TimeCode &timeCode ) { return false; }
	virtual bool GetRecordingDate( struct tm &recDate ) { return false; }
	virtual bool IsNewRecording( void ) { return false; }
	virtual bool IsComplete( void ) { return false; }

	// Video info
	virtual int GetWidth() { return -1; }
	virtual int GetHeight() { return -1; }
	virtual float GetFrameRate() { return -1; }

	// HDV vs DV
	virtual bool IsHDV() { return false; }
	virtual bool CouldBeJVCP25() { return false; }

	// For HDV only GOP packets can start a new stream/file
	// For DV we can start a new stream/file on any packet
	virtual bool CanStartNewStream() { return true; }
};


#endif
