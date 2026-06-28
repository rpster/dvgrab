/*
* ieee1394io.cc -- asynchronously grabbing DV data
* Copyright (C) 2000 Arne Schirmacher <arne@schirmacher.de>
* Copyright (C) 2003-2009 Dan Dennedy <dan@dennedy.org>
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

/**
    \page The IEEE 1394 Reader Class
 
    This text explains how the IEEE 1394 Reader class works.
 
    The IEEE1394Reader object maintains a connection to a DV
    camcorder. It reads DV frames from the camcorder and stores them
    in a queue. The frames can then be retrieved from the buffer and
    displayed, stored, or processed in other ways.
 
    The IEEE1394Reader class supports asynchronous operation: it
    starts a separate thread, which reads as fast as possible from the
    ieee1394 interface card to make sure that no frames are
    lost. Since the buffer can be configured to hold many frames, no
    frames will be lost even if the disk access is temporarily slow.
 
    There are two queues available in an IEEE1394Reader object. One
    queue holds empty frames, the other holds frames filled with DV
    content just read from the interface. During operation the reader
    thread takes unused frames from the inFrames queue, fills them and
    places them in the outFrame queue. The program can then take
    frames from the outFrames queue, process them and finally put
    them back in the inFrames queue.
 
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <deque>
#include <iostream>
#include <typeinfo>

using std::endl;

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>

#include <libavc1394/avc1394.h>
#include <libavc1394/avc1394_vcr.h>
#include <libavc1394/rom1394.h>

#include "ieee1394io.h"
#include "dvframe.h"
#include "hdvframe.h"
#include "error.h"

/// Set asynchronously by the main.cc signal handler on Ctrl-C / shutdown.
/// Used here so the HDV channel-detection wait can be interrupted while it
/// is blocked waiting for the tape to start streaming.
extern volatile sig_atomic_t g_done;

/** Initializes the IEEE1394Reader object.
 
    The object is initialized with port and channel number. These
    parameters define the interface card and the iso channel on which
    the camcorder sends its data.
 
    The object contains a list of empty frames, which are allocated
    here. 50 frames (2 seconds) should be enough in most cases.
 
    \param c the iso channel number to use
    \param bufSize the number of frames to allocate for the frames buffer
 */


IEEE1394Reader::IEEE1394Reader( int c, int bufSize, bool hdv ) :
	droppedFrames( 0 ),
	badFrames( 0 ),
	currentFrame( NULL ),
	channel( c ),
	isRunning( false ),
	isHDV( hdv )
{
	Frame * frame;

	/* Create empty frames and put them in our inFrames queue */
	for ( int i = 0; i < bufSize; ++i )
	{
		if ( isHDV )
			frame = new HDVFrame( &hdvStreamParams );
		else
			frame = new DVFrame();

		inFrames.push_back( frame );
	}

	/* Initialize mutexes */
	pthread_mutex_init( &mutex, NULL );

	/* Initialise mutex and condition for action triggerring */
	pthread_mutex_init( &condition_mutex, NULL );
	pthread_cond_init( &condition, NULL );

}


/** Destroys the IEEE1394Reader object.
 
    In particular, it deletes all frames in the inFrames and outFrames
    queues, as well as the one currently in use.  Note that one or
    more frames may have been taken out of the queues by a user of the
    IEEE1394Reader class.
 
*/

IEEE1394Reader::~IEEE1394Reader()
{
	Frame * frame;

	for ( int i = inFrames.size(); i > 0; --i )
	{
		frame = inFrames[ 0 ];
		inFrames.pop_front();
		delete frame;
	}
	for ( int i = outFrames.size(); i > 0; --i )
	{
		frame = outFrames[ 0 ];
		outFrames.pop_front();
		delete frame;
	}
	if ( currentFrame != NULL )
	{
		delete currentFrame;
		currentFrame = NULL;
	}
	pthread_mutex_destroy( &condition_mutex );
	pthread_cond_destroy( &condition );
}


/** Fetches the next frame from the output queue
 
    The outFrames contains a list of frames to be processed (saved,
    displayed) by the user of this class.  Copy the first frame
    (actually only a pointer to it) and remove it from the queue.
 
    \note If this returns NULL, wait some time (1/25 sec.) before
    calling it again.
 
    \return a pointer to the current frame, or NULL if no frames are
    in the queue
 
 */

Frame* IEEE1394Reader::GetFrame()
{
	Frame * frame = NULL;

	pthread_mutex_lock( &mutex );

	if ( outFrames.size() > 0 )
	{
		frame = outFrames[ 0 ];
		outFrames.pop_front();
	}
	pthread_mutex_unlock( &mutex );

	return frame;
}


/** Put back a frame to the queue of available frames
*/

void IEEE1394Reader::DoneWithFrame( Frame* frame )
{
	pthread_mutex_lock( &mutex );
	inFrames.push_back( frame );
	pthread_mutex_unlock( &mutex );
}


/** Return the number of dropped frames since last call
*/

int IEEE1394Reader::GetDroppedFrames( void )
{
	pthread_mutex_lock( &mutex );
	int n = droppedFrames;
	droppedFrames = 0;
	pthread_mutex_unlock( &mutex );
	return n;
}


/** Return the number of incomplete frames since last call
*/

int IEEE1394Reader::GetBadFrames( void )
{
	pthread_mutex_lock( &mutex );
	int n = badFrames;
	badFrames = 0;
	pthread_mutex_unlock( &mutex );
	return n;
}


/** Throw away all currently available frames.
 
    All frames in the outFrames queue are put back to the inFrames
    queue.  Also the currentFrame is put back too.  */

void IEEE1394Reader::Flush()
{
	Frame * frame = NULL;

	for ( int i = outFrames.size(); i > 0; --i )
	{
		frame = outFrames[ 0 ];
		outFrames.pop_front();
		inFrames.push_back( frame );
	}
	if ( currentFrame != NULL )
	{
		inFrames.push_back( currentFrame );
		currentFrame = NULL;
	}
}

bool IEEE1394Reader::WaitForAction( int seconds )
{
	pthread_mutex_lock( &mutex );
	int size = outFrames.size( );
	pthread_mutex_unlock( &mutex );

	if ( size == 0 )
	{
		pthread_mutex_lock( &condition_mutex );
		if ( seconds == 0 )
		{
			pthread_cond_wait( &condition, &condition_mutex );
			pthread_mutex_unlock( &condition_mutex );
			pthread_mutex_lock( &mutex );
			size = outFrames.size( );
		}
		else
		{
			struct timeval tp;
			struct timespec ts;
			int result;

			gettimeofday( &tp, NULL );
			ts.tv_sec = tp.tv_sec + seconds;
			ts.tv_nsec = tp.tv_usec * 1000;

			result = pthread_cond_timedwait( &condition, &condition_mutex, &ts );
			pthread_mutex_unlock( &condition_mutex );
			pthread_mutex_lock( &mutex );

			if ( result == ETIMEDOUT )
				size = 0;
			else
				size = outFrames.size();
		}
		pthread_mutex_unlock( &mutex );
	}

	return size != 0;
}

void IEEE1394Reader::TriggerAction( )
{
	pthread_mutex_lock( &condition_mutex );
	pthread_cond_signal( &condition );
	pthread_mutex_unlock( &condition_mutex );
}


/** Initializes the raw1394Reader object.
 
    The object is initialized with port and channel number. These
    parameters define the interface card and the iso channel on which
    the camcorder sends its data.
 
    \param p the number of the interface card to use
    \param c the iso channel number to use
    \param bufSize the number of frames to allocate for the frames buffer
 */


iec61883Reader::iec61883Reader( int p, int c, int bufSize,
	BusResetHandler resetHandler, BusResetHandlerData data, bool hdv ) :
		IEEE1394Reader( c, bufSize, hdv ), m_port( p ), m_resetHandler( resetHandler),
		m_resetHandlerData( data )
{
	m_handle = NULL;
	m_iec61883_mpeg2 = NULL;
	m_iec61883_dv = NULL;
	m_framesSeen = 0;
	m_rawIsoMode = false;
	m_rawIsoHandle = NULL;
	m_rawIsoFrameBuf = NULL;
	m_rawIsoBufCapacity = 0;
	m_rawIsoFrameSize = 0;
	m_rawIsoFrameOffset = 0;
	m_rawIsoAlignOffset = -1;
	m_rawIsoFixApt = -1;
	m_rawIsoSynced = false;
}


iec61883Reader::~iec61883Reader()
{
	Close();
}


/** Start receiving DV frames
 
    The ieee1394 subsystem is initialized with the parameters provided
    to the constructor (port and channel).  The received frames can be
    retrieved from the outFrames queue.
 
*/

bool iec61883Reader::StartThread()
{
	if ( isRunning )
		return true;
	pthread_mutex_lock( &mutex );
	currentFrame = NULL;
	if ( Open() && StartReceive() )
	{
		isRunning = true;
		pthread_create( &thread, NULL, ThreadProxy, this );
		pthread_mutex_unlock( &mutex );
		return true;
	}
	else
	{
		Close();
		pthread_mutex_unlock( &mutex );
		return false;
	}
}


/** Stop the receiver thread.
 
    The receiver thread is being canceled. It will finish the next
    time it calls the pthread_testcancel() function.  After it is
    canceled, we turn off iso receive and close the ieee1394
    subsystem.  We also remove all frames in the outFrames queue that
    have not been processed until now.
 
*/

void iec61883Reader::StopThread()
{
	if ( isRunning )
	{
		isRunning = false;
		pthread_join( thread, NULL );
		StopReceive();
		Close();
		Flush();
		TriggerAction( );
	}
}


void iec61883Reader::ResetHandler( void )
{
	if ( m_resetHandler )
		m_resetHandler( const_cast< void* >( m_resetHandlerData ) );
}

int iec61883Reader::ResetHandlerProxy( raw1394handle_t handle, unsigned int generation )
{
	iec61883Reader *self = NULL;
	void *userdata = raw1394_get_userdata( handle );

	if ( typeid( iec61883_mpeg2_t ) == typeid( userdata ) )
	{
		iec61883_mpeg2_t mpeg2 = static_cast< iec61883_mpeg2_t >( userdata );
		self = static_cast< iec61883Reader* >( iec61883_mpeg2_get_callback_data( mpeg2 ) );
	}
	else if ( typeid( iec61883_dv_t ) == typeid( userdata ) )
	{
		iec61883_dv_t dv = static_cast< iec61883_dv_t >( userdata );
		iec61883_dv_fb_t dvfb = static_cast< iec61883_dv_fb_t >( iec61883_dv_get_callback_data( dv ) );
		self = static_cast< iec61883Reader* >( iec61883_dv_fb_get_callback_data( dvfb ) );
	}
	if ( self )
		self->ResetHandler();

	return 0;
}


/** Open the raw1394 interface
 
    \return success/failure
*/

bool iec61883Reader::Open()
{
	bool success;

	assert( m_handle == 0 );

	try
	{
		m_handle = raw1394_new_handle_on_port( m_port );
		if ( m_handle == NULL )
			return false;
		raw1394_set_bus_reset_handler( m_handle, this->ResetHandlerProxy );

		if ( isHDV )
		{
			m_iec61883_mpeg2 = iec61883_mpeg2_recv_init( m_handle, Mpeg2HandlerProxy, this );
			success = ( m_iec61883_mpeg2 != NULL );
		}
		else
		{
			m_iec61883_dv = iec61883_dv_fb_init( m_handle, DvHandlerProxy, this );
			success = ( m_iec61883_dv != NULL );
		}
	}
	catch ( string exc )
	{
		Close();
		sendEvent( exc.c_str() );
		success = false;
	}
	return success;
}


/** Close the raw1394 interface
 
*/

void iec61883Reader::Close()
{
	if ( m_rawIsoHandle != NULL )
	{
		raw1394_destroy_handle( m_rawIsoHandle );
		m_rawIsoHandle = NULL;
	}
	if ( m_rawIsoFrameBuf != NULL )
	{
		delete[] m_rawIsoFrameBuf;
		m_rawIsoFrameBuf = NULL;
	}
	if ( m_iec61883_dv != NULL )
	{
		iec61883_dv_fb_close( m_iec61883_dv );
		m_iec61883_dv = NULL;
	}
	else if ( m_iec61883_mpeg2 != NULL )
	{
		iec61883_mpeg2_close( m_iec61883_mpeg2 );
		m_iec61883_mpeg2 = NULL;
	}
	if ( m_handle )
	{
		raw1394_destroy_handle( m_handle );
		m_handle = NULL;
	}
}

enum raw1394_iso_disposition
iec61883Reader::RawDvIsoHandler( raw1394handle_t handle, unsigned char *data,
	unsigned int len, unsigned char channel, unsigned char tag,
	unsigned char sy, unsigned int cycle, unsigned int dropped )
{
	iec61883Reader *self = static_cast< iec61883Reader* >(
		raw1394_get_userdata( handle ) );

	static int rawCallCount = 0;
	rawCallCount++;
	if ( d_all && rawCallCount == 1 )
		fprintf( stderr, "RawDvIsoHandler: receiving data, len=%u "
			"channel=%d\n", len, channel );

	// Need CIP header (tag=1) with payload
	if ( tag != 1 || len <= 8 )
		return RAW1394_ISO_OK;

	// Check FMT = 0x00 (DV)
	unsigned char fmt = data[4] & 0x3F;
	if ( fmt != 0x00 )
		return RAW1394_ISO_OK;

	int payloadLen = len - 8;
	if ( payloadLen == 0 )
		return RAW1394_ISO_OK;

	unsigned char *payload = data + 8;

	// Append payload to accumulation buffer
	if ( self->m_rawIsoFrameOffset + payloadLen > self->m_rawIsoBufCapacity )
	{
		// Buffer overflow — should not happen, reset
		self->m_rawIsoFrameOffset = 0;
		return RAW1394_ISO_OK;
	}
	memcpy( self->m_rawIsoFrameBuf + self->m_rawIsoFrameOffset,
		payload, payloadLen );
	self->m_rawIsoFrameOffset += payloadLen;

	// Phase 1: Find DIF frame alignment.
	// For DVCPRO HD (4 channels), FSC toggles per channel so we
	// cannot use FSC transitions for frame boundaries.  Instead,
	// align to the first DIF header block (SCT=0, DSN=0).
	// For DVCPRO50 (2 channels), use FSC=1→FSC=0 transitions.
	if ( self->m_rawIsoAlignOffset < 0 )
	{
		// Need at least one full frame to scan
		if ( self->m_rawIsoFrameOffset < self->m_rawIsoFrameSize )
			return RAW1394_ISO_OK;

		int alignOff = -1;

		if ( self->m_rawIsoFrameSize <= DVCPRO50_PAL_FRAME_SIZE )
		{
			// DVCPRO50: use FSC transitions for alignment
			int prevFsc = -1;
			for ( int off = 0; off + 80 <= self->m_rawIsoFrameOffset;
				off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
				if ( sct == 0 )
				{
					int fsc = ( self->m_rawIsoFrameBuf[off+1]
						>> 3 ) & 1;
					if ( prevFsc == 1 && fsc == 0 )
					{
						alignOff = off;
						break;
					}
					prevFsc = fsc;
				}
			}
		}
		else
		{
			// DVCPRO HD: FSC toggles per channel, so scan for
			// the pattern DSN=9 followed by DSN=0 which marks
			// a channel boundary.  Count 4 such boundaries to
			// find a frame start (or use the first DSN=0 found
			// after a DSN=9).
			int prevDsn = -1;
			for ( int off = 0; off + 80 <= self->m_rawIsoFrameOffset;
				off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
				if ( sct == 0 )
				{
					int dsn = ( self->m_rawIsoFrameBuf[off+1]
						>> 4 ) & 0x0F;
					if ( prevDsn == 9 && dsn == 0 )
					{
						alignOff = off;
						break;
					}
					prevDsn = dsn;
				}
			}
		}

		if ( alignOff < 0 )
		{
			// Fall back to first header block
			for ( int off = 0; off + 80 <= self->m_rawIsoFrameOffset;
				off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
				if ( sct == 0 )
				{
					alignOff = off;
					break;
				}
			}
		}

		if ( alignOff >= 0 )
		{
			self->m_rawIsoAlignOffset = alignOff;
			if ( d_all )
				fprintf( stderr, "DIF frame alignment: offset=%d "
					"bytes (%d DIF blocks), frame_size=%d\n",
					alignOff, alignOff / 80,
					self->m_rawIsoFrameSize );
			int remaining = self->m_rawIsoFrameOffset - alignOff;
			if ( remaining > 0 )
				memmove( self->m_rawIsoFrameBuf,
					self->m_rawIsoFrameBuf + alignOff, remaining );
			self->m_rawIsoFrameOffset = remaining;
		}
		else
		{
			self->m_rawIsoAlignOffset = 0;
			if ( d_all )
				fprintf( stderr, "DIF frame alignment: no headers "
					"found, using unaligned\n" );
		}
	}

	// Phase 2: Deliver aligned frames
	while ( self->m_rawIsoFrameOffset >= self->m_rawIsoFrameSize )
	{
		// Dump DIF header info for first frame
		if ( !self->m_rawIsoSynced )
		{
			self->m_rawIsoSynced = true;
			if ( d_all )
			{
				fprintf( stderr, "=== First frame DIF structure "
					"(frame_size=%d) ===\n",
					self->m_rawIsoFrameSize );
				int seqNum = 0;
				for ( int off = 0; off + 80 <= self->m_rawIsoFrameSize;
					off += 80 )
				{
					int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
					if ( sct == 0 )
					{
						int dsn = ( self->m_rawIsoFrameBuf[off + 1] >> 4 ) & 0x0F;
						int fsc = ( self->m_rawIsoFrameBuf[off + 1] >> 3 ) & 1;
						int apt = self->m_rawIsoFrameBuf[off + 4] & 0x07;
						int tf1a = ( self->m_rawIsoFrameBuf[off + 4] >> 5 ) & 1;
						fprintf( stderr, "  Seq %2d @ offset %6d: "
							"SCT=0 DSN=%d FSC=%d APT=%d TF1a=%d "
							"bytes[0..7]: %02x %02x %02x %02x %02x "
							"%02x %02x %02x\n",
							seqNum, off, dsn, fsc, apt, tf1a,
							self->m_rawIsoFrameBuf[off],
							self->m_rawIsoFrameBuf[off+1],
							self->m_rawIsoFrameBuf[off+2],
							self->m_rawIsoFrameBuf[off+3],
							self->m_rawIsoFrameBuf[off+4],
							self->m_rawIsoFrameBuf[off+5],
							self->m_rawIsoFrameBuf[off+6],
							self->m_rawIsoFrameBuf[off+7] );
						seqNum++;
					}
				}
				fprintf( stderr, "=== Total DIF sequences: %d ===\n",
					seqNum );

				// Dump VAUX packs from first DIF sequence to
				// identify video format (VS=0x60, VSC=0x61).
				// VAUX blocks are at offsets 240, 320, 400
				// (after 1 header + 2 subcode blocks).
				fprintf( stderr, "=== VAUX packs (first "
					"sequence) ===\n" );
				for ( int vb = 0; vb < 3; vb++ )
				{
					int voff = ( 3 + vb ) * 80 + 3;
					if ( voff + 77 > self->m_rawIsoFrameSize )
						break;
					// Each VAUX block has 15 packs × 5 bytes
					for ( int p = 0; p < 15; p++ )
					{
						int poff = voff + p * 5;
						unsigned char hdr =
							self->m_rawIsoFrameBuf[poff];
						if ( hdr == 0x60 || hdr == 0x61 )
						{
							fprintf( stderr,
								"  Pack 0x%02x @ %d:"
								" %02x %02x %02x %02x"
								" %02x\n",
								hdr, poff,
								self->m_rawIsoFrameBuf[poff],
								self->m_rawIsoFrameBuf[poff+1],
								self->m_rawIsoFrameBuf[poff+2],
								self->m_rawIsoFrameBuf[poff+3],
								self->m_rawIsoFrameBuf[poff+4]
							);
						}
					}
				}
			}
		}

		// Reorder and normalize DVCPRO HD channels.
		//
		// The camera sends 4 channels per DIF frame (480000 bytes).
		// ffmpeg's 720p decoder reads 240000 bytes per frame
		// (n_difchan=2), so it naturally splits our 480000 bytes
		// into Frame A (ch0+1) and Frame B (ch2+3).
		//
		// Channel identity from camera byte1 lower nibble:
		//   0x07: FSC=0, FSP=1  →  channel 0
		//   0x0F: FSC=1, FSP=1  →  channel 1
		//   0x03: FSC=0, FSP=0  →  channel 2
		//   0x0B: FSC=1, FSP=0  →  channel 3
		//
		// FSP (bit 2) is critical for 720p: ffmpeg's decoder
		// checks (buf[1] & 0x0C) to apply macroblock Y-coordinate
		// displacement for Frame B.  Channels 2,3 must have FSP=0.
		//
		// Order by FSP (bit2): same mapping for 1080i and 720p.
		if ( self->m_rawIsoFixApt >= 0 &&
			self->m_rawIsoFrameSize >= 480000 )
		{
			int channelSize = self->m_rawIsoFrameSize / 4;

			// Map byte1 signature to channel index
			// (group by FSP/bit2, same for all formats)
			int srcPos[4] = { -1, -1, -1, -1 };
			for ( int ch = 0; ch < 4; ch++ )
			{
				int sig = self->m_rawIsoFrameBuf[
					ch * channelSize + 1 ] & 0x0F;
				int slot;
				switch ( sig )
				{
					case 0x07: slot = 0; break;
					case 0x0F: slot = 1; break;
					case 0x03: slot = 2; break;
					case 0x0B: slot = 3; break;
					default:   slot = ch; break;
				}
				srcPos[slot] = ch;
			}

			// Reorder if needed
			bool needReorder = false;
			for ( int i = 0; i < 4; i++ )
			{
				if ( srcPos[i] < 0 ) srcPos[i] = i;
				if ( srcPos[i] != i ) needReorder = true;
			}
			if ( needReorder )
			{
				unsigned char *temp = self->m_rawIsoFrameBuf
					+ 2 * self->m_rawIsoFrameSize;
				memcpy( temp, self->m_rawIsoFrameBuf,
					self->m_rawIsoFrameSize );
				for ( int i = 0; i < 4; i++ )
					memcpy( self->m_rawIsoFrameBuf
						+ i * channelSize,
						temp + srcPos[i] * channelSize,
						channelSize );
			}

			// Normalize DIF headers: set correct FSC, FSP, R,
			// and APT per channel index.
			//
			// Per-channel byte1 lower nibble (from ffmpeg dvenc.c):
			//   Ch0: FSC=0 FSP=1 R=11 → 0x07
			//   Ch1: FSC=1 FSP=1 R=11 → 0x0F
			//   Ch2: FSC=0 FSP=0 R=11 → 0x03
			//   Ch3: FSC=1 FSP=0 R=11 → 0x0B
			static const unsigned char chLower[4] =
				{ 0x07, 0x0F, 0x03, 0x0B };

			for ( int off = 0; off + 80 <=
				self->m_rawIsoFrameSize; off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off]
					>> 5 ) & 7;
				if ( sct == 0 )
				{
					int chIdx = off / channelSize;
					if ( chIdx > 3 ) chIdx = 3;
					self->m_rawIsoFrameBuf[off + 1] =
						( self->m_rawIsoFrameBuf[off + 1]
						& 0xF0 )
						| chLower[chIdx];
					self->m_rawIsoFrameBuf[off + 4] =
						( self->m_rawIsoFrameBuf[off + 4]
						& 0xF8 )
						| ( self->m_rawIsoFixApt & 0x07 );
				}
			}
		}
		else if ( self->m_rawIsoFixApt >= 0 )
		{
			// Non-HD: just fix APT
			for ( int off = 0; off + 80 <=
				self->m_rawIsoFrameSize; off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off]
					>> 5 ) & 7;
				if ( sct == 0 )
				{
					self->m_rawIsoFrameBuf[off + 4] =
						( self->m_rawIsoFrameBuf[off + 4]
						& 0xF8 )
						| ( self->m_rawIsoFixApt & 0x07 );
				}
			}
		}

		self->Handler( self->m_rawIsoFrameBuf,
			self->m_rawIsoFrameSize, 0 );

		int remaining = self->m_rawIsoFrameOffset - self->m_rawIsoFrameSize;
		if ( remaining > 0 )
			memmove( self->m_rawIsoFrameBuf,
				self->m_rawIsoFrameBuf + self->m_rawIsoFrameSize,
				remaining );
		self->m_rawIsoFrameOffset = remaining;
	}

	return RAW1394_ISO_OK;
}

// Lightweight single-channel isochronous probe, used both to detect the
// channel/format at StartReceive() time and to re-detect the live HDV channel
// from the receive thread (see iec61883Reader::findActiveHdvChannel).  Lifted
// to file scope so both callers can share it.
namespace {

struct IsoProbeData {
	int fn, dbs, fdf, maxLen, count;
};

enum raw1394_iso_disposition isoProbeHandler(
	raw1394handle_t h, unsigned char *d, unsigned int l,
	unsigned char ch, unsigned char tg, unsigned char s,
	unsigned int cy, unsigned int dr )
{
	IsoProbeData *pd = static_cast< IsoProbeData* >(
		raw1394_get_userdata( h ) );
	// Only count packets with actual payload, not empty CIP headers.
	if ( l > 8 )
	{
		pd->count++;
		if ( (int)l > pd->maxLen )
			pd->maxLen = l;
		if ( tg == 1 && pd->fn < 0 )
		{
			pd->dbs = d[1];
			pd->fn  = ( d[2] >> 6 ) & 0x3;
			pd->fdf = d[5];
		}
	}
	return RAW1394_ISO_OK;
}

// Probe a single channel for roughly durationMs; returns the number of
// payload-bearing packets seen and fills pd.
int isoProbeChannel( int port, int ch, int durationMs, IsoProbeData *pd )
{
	pd->fn = pd->dbs = pd->fdf = -1;
	pd->maxLen = 0;
	pd->count = 0;
	raw1394handle_t h = raw1394_new_handle_on_port( port );
	if ( !h )
		return 0;
	raw1394_set_userdata( h, pd );
	if ( raw1394_iso_recv_init( h, isoProbeHandler, 64, 2048, ch,
		RAW1394_DMA_DEFAULT, -1 ) == 0 )
	{
		if ( raw1394_iso_recv_start( h, -1, -1, 0 ) == 0 )
		{
			int fd = raw1394_get_fd( h );
			struct pollfd pfd = { fd, POLLIN, 0 };
			// Poll in fine slices and bail out the instant a payload packet
			// arrives, so probing the channel the device is actually
			// streaming on returns immediately rather than dwelling for the
			// full duration.  Silent channels still cost up to durationMs
			// (one slice of poll timeout per iteration).
			const int pollMs = 20;
			int iters = durationMs / pollMs;
			if ( iters < 1 )
				iters = 1;
			for ( int t = 0; t < iters && pd->count == 0; t++ )
			{
				if ( poll( &pfd, 1, pollMs ) > 0 )
					raw1394_loop_iterate( h );
			}
			raw1394_iso_stop( h );
			raw1394_iso_shutdown( h );
		}
	}
	raw1394_destroy_handle( h );
	return pd->count;
}

} // namespace

/** Find the iso channel an HDV device is actually streaming on.

    Probes the preferred (CMP-negotiated) channel first, then sweeps the rest.
    Returns the channel carrying live payload data, or -1 if none is streaming
    yet.  Aborts early on g_done, and (when checkRunning is set, i.e. when
    called from the receive thread) on isRunning going false, so StopThread
    isn't blocked for a full sweep.
*/
int iec61883Reader::findActiveHdvChannel( int preferred, bool checkRunning )
{
	IsoProbeData pd;
	if ( g_done || ( checkRunning && !isRunning ) )
		return -1;
	// Favour the negotiated channel (longer dwell): on OHCI it is normally
	// correct, so an already-rolling tape there is caught immediately.
	if ( isoProbeChannel( m_port, preferred, 200, &pd ) > 0 )
		return preferred;
	for ( int ch = 0; ch < 64; ch++ )
	{
		if ( g_done || ( checkRunning && !isRunning ) )
			break;
		if ( ch == preferred )
			continue;
		if ( isoProbeChannel( m_port, ch, 20, &pd ) > 0 )
			return ch;
	}
	return -1;
}

bool iec61883Reader::StartReceive()
{
	bool success;

	int probeFn = -1;
	int probeDbs = -1;
	int probeFdf = -1;
	int probePackets = 0;
	int probeMaxLen = 0;

	IsoProbeData pd;
	pd.count = 0;

	if ( isHDV )
	{
		// HDV: do NOT block here probing.  The tape may not be rolling yet
		// (StartReceive runs before the camera streams, and is also called
		// from the constructor before AVC Play is even sent), and on the
		// juju backend the camera often streams on a channel other than the
		// negotiated one.  A one-shot probe here would either stall startup
		// or lock onto the wrong channel and capture zero frames.
		//
		// Instead bind the negotiated channel now and let the receive thread
		// (Thread()) re-probe and rebind the moment live data appears — see
		// the re-detection block there.  This keeps startup instant and makes
		// capture work whether the tape is already playing or only starts
		// after dvgrab is armed.
		if ( d_all )
			fprintf( stderr, "HDV: binding negotiated channel %d; the receive "
				"thread will lock onto the live stream when data arrives\n",
				channel );
	}
	else
	{
		if ( d_all )
			fprintf( stderr, "Probing for isochronous data (negotiated channel "
				"%d)...\n", channel );

		// DV / DVCPRO: probe once.  These formats are captured the moment
		// the device streams and the negotiated channel is normally
		// correct, so the original single-pass detection is retained to
		// avoid changing proven behaviour.
		//
		// Phase 1: retry the negotiated channel a few times, since the
		// device may need a moment to start isochronous output.
		for ( int attempt = 0; attempt < 3 && probePackets == 0; attempt++ )
		{
			if ( attempt > 0 )
			{
				if ( d_all )
					fprintf( stderr, "  Retrying negotiated channel (attempt "
						"%d)...\n", attempt + 1 );
				timespec t = {0, 500000000L};
				nanosleep( &t, NULL );
			}
			if ( isoProbeChannel( m_port, channel, 500, &pd ) > 0 )
			{
				probePackets = pd.count;
				probeFn = pd.fn;
				probeDbs = pd.dbs;
				probeFdf = pd.fdf;
				probeMaxLen = pd.maxLen;
				if ( d_all )
					fprintf( stderr, "  Channel %d: %d packets (negotiated)\n",
						channel, pd.count );
			}
		}

		// Phase 2: negotiated channel silent — sweep every channel once and
		// lock onto wherever isochronous data is actually arriving.
		if ( probePackets == 0 )
		{
			if ( d_all )
				fprintf( stderr, "  Channel %d silent; sweeping all channels...\n",
					channel );
			for ( int ch = 0; ch < 64; ch++ )
			{
				if ( ch == channel )
					continue;
				if ( isoProbeChannel( m_port, ch, 60, &pd ) > 0 )
				{
					if ( d_all )
						fprintf( stderr, "  Found isochronous data on channel "
							"%d (%d packets); switching from negotiated "
							"channel %d\n", ch, pd.count, channel );
					channel = ch;
					probePackets = pd.count;
					probeFn = pd.fn;
					probeDbs = pd.dbs;
					probeFdf = pd.fdf;
					probeMaxLen = pd.maxLen;
					break;
				}
			}
		}
	}

	if ( d_all )
		fprintf( stderr, "  Probe result: channel=%d packets=%d DBS=%d "
			"FN=%d FDF=0x%02x max_pkt=%d\n", channel, probePackets,
			probeDbs, probeFn, probeFdf, probeMaxLen );

	/* Starting iso receive */
	if ( d_all )
		fprintf( stderr, "Starting isochronous receive on channel %d\n", channel );

	// Use raw iso mode for DVCPRO50+ (FN>0 means packets larger than
	// DV25, which libiec61883 may not handle correctly)
	if ( !isHDV && probePackets > 0 && probeFn > 0 )
	{
		// Determine format from FN:
		//   FN=1: DVCPRO50 (50 Mbps) - 960 byte packets
		//   FN=2: DVCPRO HD (100 Mbps) - 1920 byte packets
		const char *fmtName = probeFn >= 2 ? "DVCPRO-HD" : "DVCPRO50";
		if ( d_all )
			fprintf( stderr, "Using raw iso receive mode for %s "
				"(FN=%d, %d-byte packets)\n", fmtName, probeFn,
				probeMaxLen );

		// Determine frame size from FN and FDF 50/60 flag.
		bool pal = ( probeFdf & 0x80 ) != 0;
		if ( probeFn >= 2 )
			m_rawIsoFrameSize = pal ? DVCPROHD_PAL_FRAME_SIZE
				: DVCPROHD_NTSC_FRAME_SIZE;
		else
			m_rawIsoFrameSize = pal ? DVCPRO50_PAL_FRAME_SIZE
				: DVCPRO50_NTSC_FRAME_SIZE;
		// Need 3× capacity for alignment detection across
		// frame boundaries (must see two FSC transitions).
		m_rawIsoBufCapacity = m_rawIsoFrameSize * 3;
		m_rawIsoFrameBuf = new unsigned char[ m_rawIsoBufCapacity ];
		m_rawIsoFrameOffset = 0;
		m_rawIsoAlignOffset = -1;
		// DVCPRO HD cameras may report APT=1 in DIF headers;
		// fix to APT=4 so decoders use correct shuffle tables.
		// Also normalizes FSC and reserved bits per channel.
		m_rawIsoFixApt = ( probeFn >= 2 ) ? 4 : -1;
		m_rawIsoSynced = false;
		m_rawIsoMode = true;

		// Use a dedicated handle for raw iso — iec61883_dv_fb_init
		// on m_handle sets internal iso state that conflicts.
		m_rawIsoHandle = raw1394_new_handle_on_port( m_port );
		if ( !m_rawIsoHandle )
		{
			fprintf( stderr, "Failed to open raw1394 handle for raw iso: "
				"%s\n", strerror( errno ) );
			m_rawIsoMode = false;
			success = false;
		}
		else
		{
			raw1394_set_userdata( m_rawIsoHandle, this );
			// Use 2048 as minimum — the probe may only see empty CIP
			// headers (8 bytes) if the device just started streaming.
			int maxPkt = probeMaxLen > 2048 ? probeMaxLen : 2048;
			if ( raw1394_iso_recv_init( m_rawIsoHandle, RawDvIsoHandler,
				400, maxPkt, channel, RAW1394_DMA_DEFAULT, -1 ) == 0 )
			{
				if ( raw1394_iso_recv_start( m_rawIsoHandle,
					-1, -1, 0 ) == 0 )
				{
					success = true;
					if ( d_all )
						fprintf( stderr, "Raw iso receive started "
							"(frame_size=%d)\n", m_rawIsoFrameSize );
				}
				else
				{
					fprintf( stderr, "raw1394_iso_recv_start failed: "
						"%s\n", strerror( errno ) );
					raw1394_iso_shutdown( m_rawIsoHandle );
					raw1394_destroy_handle( m_rawIsoHandle );
					m_rawIsoHandle = NULL;
					m_rawIsoMode = false;
					success = false;
				}
			}
			else
			{
				fprintf( stderr, "raw1394_iso_recv_init failed: %s\n",
					strerror( errno ) );
				raw1394_destroy_handle( m_rawIsoHandle );
				m_rawIsoHandle = NULL;
				m_rawIsoMode = false;
				success = false;
			}
		}
	}
	else
	{
		try
		{
			if ( isHDV )
				fail_neg( iec61883_mpeg2_recv_start( m_iec61883_mpeg2, channel ) );
			else
				fail_neg( iec61883_dv_fb_start( m_iec61883_dv, channel ) );
			success = true;
			if ( d_all )
				fprintf( stderr, "Isochronous receive started successfully\n" );
		}
		catch ( string exc )
		{
			sendEvent( exc.c_str() );
			success = false;
		}
	}
	return success;
}


void iec61883Reader::StopReceive()
{
	if ( m_rawIsoMode && m_rawIsoHandle )
	{
		raw1394_iso_stop( m_rawIsoHandle );
		raw1394_iso_shutdown( m_rawIsoHandle );
		m_rawIsoMode = false;
	}
	else if ( m_iec61883_dv != NULL )
	{
		iec61883_dv_fb_stop( m_iec61883_dv );
	}
	else if ( m_iec61883_mpeg2 != NULL )
	{
		iec61883_mpeg2_recv_stop( m_iec61883_mpeg2 );
	}
}

int iec61883Reader::Mpeg2HandlerProxy( unsigned char *data, int length,
	unsigned int dropped, void *callback_data )
{
	iec61883Reader *self = static_cast< iec61883Reader* >( callback_data );
	return self->Handler( data, length, dropped );
}

int iec61883Reader::DvHandlerProxy( unsigned char *data, int length,
	int complete, void *callback_data )
{
	iec61883Reader *self = static_cast< iec61883Reader* >( callback_data );
	return self->Handler( data, length, !complete );
}

int iec61883Reader::Handler( unsigned char *data, int length, int dropped )
{
	static int handlerCalls = 0;
	if ( d_all && handlerCalls == 0 )
		fprintf( stderr, "First DV packet received: %d bytes\n", length );
	handlerCalls++;
	// Signals the receive thread that the stream is live on the bound
	// channel, so it can stop re-probing for the HDV channel.
	m_framesSeen++;

	badFrames += dropped;

	if ( currentFrame == NULL )
	{
		if ( inFrames.size() > 0 )
		{
			pthread_mutex_lock( &mutex );
			currentFrame = inFrames.front();
			currentFrame->Clear();
			inFrames.pop_front();
			pthread_mutex_unlock( &mutex );
		}
		else
		{
			droppedFrames++;
			return 0;
		}
	}

    //  BOUNDS CHECKING FOR DV FRAMES ONLY
	if (!currentFrame->IsHDV())
	{
		int currentLen = currentFrame->GetDataLen();
		int maxLen = currentFrame->GetDataCapacity();

		if (currentLen + length > maxLen)
		{
			// Buffer would overflow - drop this packet and reset frame
			fprintf(stderr, "ERROR: Frame buffer overflow prevented! "
					"current=%d, incoming=%d, max=%d\n",
					currentLen, length, maxLen);

			// Reset the frame to prevent corruption
			currentFrame->Clear();
			badFrames++;
			droppedFrames++;

			// Don't attempt the memcpy
			return 0;
		}
	}
	
	memcpy( &currentFrame->data[currentFrame->GetDataLen()], data, length );
	currentFrame->AddDataLen( length );

	// IsComplete() uses GetFrameSize() which relies on parsing the DIF
	// header APT field.  For raw iso assembled DVCPRO50 frames this
	// detection may fail (returns DV25 size), so also accept the frame
	// when it reaches the raw iso assembler's known frame size.
	bool complete = currentFrame->IsComplete();
	if ( !complete && m_rawIsoMode &&
		currentFrame->GetDataLen() >= m_rawIsoFrameSize )
		complete = true;

	if ( complete )
	{
		pthread_mutex_lock( &mutex );
		outFrames.push_back( currentFrame );
		currentFrame = NULL;
		TriggerAction( );
		pthread_mutex_unlock( &mutex );
	}

	return 0;
}


/** The thread responsible for polling the raw1394 interface.
 
    Though this is an infinite loop, it can be canceled by StopThread,
    but only in the pthread_testcancel() function.
 
*/
void* iec61883Reader::ThreadProxy( void* arg )
{
	iec61883Reader* self = static_cast< iec61883Reader* >( arg );
	return self->Thread();
}

void* iec61883Reader::Thread()
{
	// In raw iso mode, iterate on the dedicated iso handle;
	// otherwise use the main iec61883 handle.
	raw1394handle_t iterHandle = m_rawIsoMode ? m_rawIsoHandle : m_handle;
	int fd = raw1394_get_fd( iterHandle );

	// Set non-blocking so raw1394_loop_iterate doesn't block forever
	int flags = fcntl( fd, F_GETFL );
	if ( flags >= 0 )
		fcntl( fd, F_SETFL, flags | O_NONBLOCK );

	struct pollfd raw1394_poll;
	raw1394_poll.fd = fd;
	raw1394_poll.events = POLLIN | POLLERR | POLLHUP | POLLPRI;

	// HDV channel re-detection.  StartReceive() binds the negotiated channel
	// without probing, because the tape may not be rolling yet and the juju
	// backend often streams on a different channel.  If no frames arrive on
	// the bound channel, periodically re-probe and rebind libiec61883's MPEG2
	// receiver to wherever the live stream actually is.  This is what makes
	// HDV capture work when the tape starts *after* dvgrab is armed, and it
	// locks on within a fraction of a second of playback beginning instead of
	// requiring the tape to already be playing.  Only applies to the
	// libiec61883 MPEG2 path (not raw-iso DVCPRO).
	const bool hdvRedetect = isHDV && !m_rawIsoMode && m_iec61883_mpeg2;
	struct timespec lastDetect;
	clock_gettime( CLOCK_MONOTONIC, &lastDetect );

	while ( isRunning )
	{
		int result = poll( &raw1394_poll, 1, 20 );

		if ( result < 0 && errno != EAGAIN && errno != EINTR )
		{
			perror( "error: raw1394 poll" );
			break;
		}

		raw1394_loop_iterate( iterHandle );

		if ( hdvRedetect && m_framesSeen == 0 )
		{
			struct timespec now;
			clock_gettime( CLOCK_MONOTONIC, &now );
			long elapsedMs = ( now.tv_sec - lastDetect.tv_sec ) * 1000
				+ ( now.tv_nsec - lastDetect.tv_nsec ) / 1000000;
			if ( elapsedMs >= 500 )
			{
				// Nothing on the bound channel yet.  Stop the MPEG2 receiver
				// (it frees its iso context so the probe can use one), sweep
				// for the live channel, and rebind there.  If still nothing is
				// streaming, rebind the same channel and try again next pass.
				// The stop/start are kept strictly paired so StopReceive()'s
				// later stop stays balanced.
				iec61883_mpeg2_recv_stop( m_iec61883_mpeg2 );
				int found = findActiveHdvChannel( channel, true );
				if ( found >= 0 && found != channel )
				{
					if ( d_all )
						fprintf( stderr, "HDV stream found on channel %d; "
							"rebinding from %d\n", found, channel );
					channel = found;
				}
				iec61883_mpeg2_recv_start( m_iec61883_mpeg2, channel );
				// recv_start may re-init the iso context and reset fd flags;
				// re-assert non-blocking so loop_iterate never blocks.
				if ( flags >= 0 )
					fcntl( fd, F_SETFL, flags | O_NONBLOCK );
				clock_gettime( CLOCK_MONOTONIC, &lastDetect );
			}
		}
	}

	// Restore blocking mode
	if ( flags >= 0 )
		fcntl( fd, F_SETFL, flags );

	return NULL;
}


iec61883Connection::iec61883Connection( int port, int node ) :
	m_node( node | 0xffc0 ), m_channel( -1 ), m_bandwidth( 0 ),
	m_outputPort( -1 ), m_inputPort( -1 ), m_cmpConnected( false ),
	m_forcedoPCR( false ), m_originaloPCR( 0 )
{
	m_handle = raw1394_new_handle_on_port( port );
	if ( m_handle )
	{
		m_channel = iec61883_cmp_connect( m_handle, m_node, &m_outputPort,
			raw1394_get_local_id( m_handle ), &m_inputPort, &m_bandwidth );
		if ( m_channel >= 0 && m_bandwidth > 0 )
		{
			// CMP fully succeeded (channel allocated and bandwidth reserved).
			m_cmpConnected = true;
		}
		else if ( m_channel >= 0 )
		{
			// CMP returned a channel but failed to allocate bandwidth
			// (IRM unavailable).  The oPCR may not have been modified.
			// Force a known-good connection.
			if ( d_all )
				fprintf( stderr, "CMP returned channel %d but no bandwidth "
					"allocated, forcing oPCR\n", m_channel );
			m_channel = ForceConnection();
		}
		else
		{
			// CMP connect failed (typically "Failed to get channels
			// available" when IRM channel allocation doesn't work).
			// Attempt a direct oPCR write to force the device to stream.
			m_channel = ForceConnection();
		}
	}
}

int iec61883Connection::ForceConnection( void )
{
	// Bypass IRM channel allocation by directly manipulating the
	// device's output Plug Control Register (oPCR[0]).  This is
	// needed when the firewire stack cannot act as IRM or allocate
	// isochronous resources (common on Raspberry Pi / firewire-core).
	//
	// oPCR layout (IEC 61883-1):
	//   bit  31       : online
	//   bit  30       : broadcast connection counter
	//   bits 29-24    : point-to-point connection counter (6 bits)
	//   bits 23-16    : reserved
	//   bits 15-10    : channel number (6 bits)
	//   bits  9-8     : data rate
	//   bits  7-2     : overhead_id
	//   bits  1-0     : payload

	// Read the output Master Plug Register (oMPR) at offset 0x900
	// to find how many output plugs exist.
	//
	// oMPR layout:
	//   bits 31-30 : data rate capability
	//   bits 29-24 : broadcast channel base
	//   bits  4-0  : number of output plugs
	static const nodeaddr_t OMPR_ADDR  = CSR_REGISTER_BASE + 0x900;
	static const nodeaddr_t OPCR0_ADDR = CSR_REGISTER_BASE + 0x904;

	quadlet_t oMPR = 0;
	int numPlugs = 1;
	int bcastBase = 63;
	if ( raw1394_read( m_handle, m_node, OMPR_ADDR,
		sizeof( oMPR ), &oMPR ) == 0 )
	{
		quadlet_t mprVal = ntohl( oMPR );
		numPlugs = mprVal & 0x1F;
		int rateCap = ( mprVal >> 30 ) & 0x3;
		bcastBase = ( mprVal >> 24 ) & 0x3F;
		if ( d_all )
			fprintf( stderr, "oMPR = 0x%08x (rate_cap=%d, bcast_base=%d, "
				"num_plugs=%d)\n", mprVal, rateCap, bcastBase, numPlugs );
	}

	// Read all output Plug Control Registers
	int bestChannel = 63;
	bool foundActive = false;

	for ( int plug = 0; plug < numPlugs && plug < 4; plug++ )
	{
		quadlet_t oPCR = 0;
		nodeaddr_t addr = OPCR0_ADDR + plug * 4;

		if ( raw1394_read( m_handle, m_node, addr,
			sizeof( oPCR ), &oPCR ) != 0 )
			continue;

		quadlet_t value = ntohl( oPCR );
		int online    = ( value >> 31 ) & 1;
		int bcastConn = ( value >> 30 ) & 1;
		int p2pCount  = ( value >> 24 ) & 0x3F;
		int oPCRchan  = ( value >> 10 ) & 0x3F;
		int dataRate  = ( value >> 8 )  & 0x3;

		if ( d_all )
			fprintf( stderr, "oPCR[%d] = 0x%08x (online=%d, bcast=%d, p2p=%d, "
				"channel=%d, rate=%d)\n", plug, value,
				online, bcastConn, p2pCount, oPCRchan, dataRate );

		if ( plug == 0 )
			m_originaloPCR = oPCR;

		// When the broadcast bit is set, the device uses the oMPR
		// bcast_base channel for output, not the oPCR channel field.
		if ( online && !foundActive )
		{
			if ( bcastConn )
				bestChannel = bcastBase;
			else
				bestChannel = oPCRchan;
			foundActive = true;
		}
	}

	if ( d_all )
		fprintf( stderr, "Using %s channel %d\n",
			foundActive ? "broadcast" : "fallback", bestChannel );

	return bestChannel;
}


iec61883Connection::~iec61883Connection( )
{
	if ( m_handle )
	{
		if ( m_cmpConnected )
		{
			iec61883_cmp_disconnect( m_handle, m_node, m_outputPort,
				raw1394_get_local_id (m_handle), m_inputPort,
				m_channel, m_bandwidth );
		}
		else if ( m_forcedoPCR )
		{
			// Restore the original oPCR value to tear down our
			// forced connection.
			static const nodeaddr_t OPCR0_ADDR = CSR_REGISTER_BASE + 0x904;
			raw1394_write( m_handle, m_node, OPCR0_ADDR,
				sizeof( m_originaloPCR ), &m_originaloPCR );
		}
		raw1394_destroy_handle( m_handle );
	}
}

void iec61883Connection::CheckConsistency( int port, int node )
{
	raw1394handle_t handle = raw1394_new_handle_on_port( port );
	if ( handle )
	{
		iec61883_cmp_normalize_output( handle, 0xffc0 | node );
		raw1394_destroy_handle( handle );
	}
}


int iec61883Connection::Reconnect( void )
{
	return iec61883_cmp_reconnect( m_handle, m_node, &m_outputPort,
		raw1394_get_local_id( m_handle ), &m_inputPort,
		&m_bandwidth, m_channel );
}


/** Initializes the AVC object.
 
    \param p the number of the interface card to use (port)
 */


AVC::AVC( int p ) : port( p )
{
	pthread_mutex_init( &avc_mutex, NULL );
	avc_handle = NULL;
	int numcards;
	struct raw1394_portinfo pinf[ 16 ];

	try
	{
		avc_handle = raw1394_new_handle();
		if ( avc_handle == 0 )
			return ;
		fail_neg( numcards = raw1394_get_port_info( avc_handle, pinf, 16 ) );
		fail_neg( raw1394_set_port( avc_handle, port ) );

	}
	catch ( string exc )
	{
		if ( avc_handle != NULL )
			raw1394_destroy_handle( avc_handle );
		avc_handle = NULL;
		sendEvent( exc.c_str() );
	}
	return ;
}


/** Destroys the AVC object.
 
*/

AVC::~AVC()
{
	if ( avc_handle != NULL )
	{
		pthread_mutex_lock( &avc_mutex );
		raw1394_destroy_handle( avc_handle );
		avc_handle = NULL;
		pthread_mutex_unlock( &avc_mutex );
	}
}


/** See if a node_id is still valid and pointing to an AV/C Recorder.
 
	If the node_id is not valid, then look for the first AV/C device on
	the bus;
	
	\param phyID The node_id to check.
	\return The same node_id if valid, a new node_id if not valid and a
	        another AV/C recorder exists, or -1 if not valid and no
			AV/C recorders exist.
   
*/
int AVC::isPhyIDValid( int phyID )
{
	int value = -1;
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		int currentNode, nodeCount;
		rom1394_directory rom1394_dir;

		nodeCount = raw1394_get_nodecount( avc_handle );

		if ( phyID >= 0 && phyID < nodeCount )
		{
			rom1394_get_directory( avc_handle, phyID, &rom1394_dir );
			if ( rom1394_get_node_type( &rom1394_dir ) == ROM1394_NODE_TYPE_AVC )
			{
				if ( avc1394_check_subunit_type( avc_handle, phyID, AVC1394_SUBUNIT_TYPE_VCR ) )
					value = phyID;
			}
			rom1394_free_directory( &rom1394_dir );
		}

		// look for a new AVC recorder
		for ( currentNode = 0; value == -1 && currentNode < nodeCount; currentNode++ )
		{
			rom1394_get_directory( avc_handle, currentNode, &rom1394_dir );
			if ( rom1394_get_node_type( &rom1394_dir ) == ROM1394_NODE_TYPE_AVC )
			{
				if ( avc1394_check_subunit_type( avc_handle, currentNode, AVC1394_SUBUNIT_TYPE_VCR ) )
				{
					// set Preferences to the newly found AVC node and return
					//octlet_t guid = rom1394_get_guid( avc_handle, currentNode );
					//snprintf( Preferences::getInstance().avcGUID, 64, "%08x%08x", (quadlet_t) (guid>>32),
					//(quadlet_t) (guid & 0xffffffff) );
					value = currentNode;
				}
			}
			rom1394_free_directory( &rom1394_dir );
		}
	}
	pthread_mutex_unlock( &avc_mutex );
	return value;
}


/** Do not do anything but let raw1394 make necessary
    callbacks (bus reset)
*/
void AVC::Noop( void )
{
	struct pollfd raw1394_poll;
	raw1394_poll.fd = raw1394_get_fd( avc_handle );
	raw1394_poll.events = POLLIN | POLLPRI;
	raw1394_poll.revents = 0;
	if ( poll( &raw1394_poll, 1, 100 ) > 0 )
	{
		if ( ( raw1394_poll.revents & POLLIN )
		        || ( raw1394_poll.revents & POLLPRI ) )
			raw1394_loop_iterate( avc_handle );
	}
}


int AVC::Play( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
		{
			if ( !avc1394_vcr_is_recording( avc_handle, phyID ) &&
				avc1394_vcr_is_playing( avc_handle, phyID ) != AVC1394_VCR_OPERAND_PLAY_FORWARD )
					avc1394_vcr_play( avc_handle, phyID );
		}
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}


int AVC::Pause( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
		{
			if ( !avc1394_vcr_is_recording( avc_handle, phyID ) &&
				( avc1394_vcr_is_playing( avc_handle, phyID ) != AVC1394_VCR_OPERAND_PLAY_FORWARD_PAUSE ) )
					avc1394_vcr_pause( avc_handle, phyID );
		}
	}
	struct timespec t =
	    {
		    0, 250000000L
	    };
	nanosleep( &t, NULL );
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}


int AVC::Stop( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_stop( avc_handle, phyID );
	}
	struct timespec t =
	    {
		    0, 250000000L
	    };
	nanosleep( &t, NULL );
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}


int AVC::Rewind( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_rewind( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}


int AVC::FastForward( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_forward( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::Forward( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_next( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::Back( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_previous( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::NextScene( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_next_index( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::PreviousScene( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_previous_index( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::Record( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_record( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

int AVC::Shuttle( int phyID, int speed )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_trick_play( avc_handle, phyID, speed );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

unsigned int AVC::TransportStatus( int phyID )
{
	quadlet_t val = 0;
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			val = avc1394_vcr_status( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return val;
}

bool AVC::Timecode( int phyID, char* timecode )
{
	bool result = false;
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
		{
			quadlet_t request[ 2 ];
			quadlet_t *response;

			request[ 0 ] = AVC1394_CTYPE_STATUS | AVC1394_SUBUNIT_TYPE_TAPE_RECORDER | AVC1394_SUBUNIT_ID_0 |
			               AVC1394_VCR_COMMAND_TIME_CODE | AVC1394_VCR_OPERAND_TIME_CODE_STATUS;
			request[ 1 ] = 0xFFFFFFFF;
			response = avc1394_transaction_block( avc_handle, phyID, request, 2, 1 );
			if ( response && response[1] != 0xffffffff )
			{
				sprintf( timecode, "%2.2x:%2.2x:%2.2x:%2.2x",
			         response[ 1 ] & 0x000000ff,
			         ( response[ 1 ] >> 8 ) & 0x000000ff,
			         ( response[ 1 ] >> 16 ) & 0x000000ff,
			         ( response[ 1 ] >> 24 ) & 0x000000ff );
				result = true;
			}
		}

	}
	pthread_mutex_unlock( &avc_mutex );
	return result;
}

int AVC::getNodeId( const char *guid )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		for ( int currentNode = 0; currentNode < raw1394_get_nodecount( avc_handle ); currentNode++ )
		{
			octlet_t currentGUID = rom1394_get_guid( avc_handle, currentNode );
			char currentGUIDStr[ 65 ];
			snprintf( currentGUIDStr, 64, "%08x%08x", ( quadlet_t ) ( currentGUID >> 32 ),
			          ( quadlet_t ) ( currentGUID & 0xffffffff ) );
			if ( strncmp( currentGUIDStr, guid, 64 ) == 0 )
			{
				pthread_mutex_unlock( &avc_mutex );
				return currentNode;
			}
		}
		pthread_mutex_unlock( &avc_mutex );
	}
	return -1;
}

int AVC::Reverse( int phyID )
{
	pthread_mutex_lock( &avc_mutex );
	if ( avc_handle != NULL )
	{
		if ( phyID >= 0 )
			avc1394_vcr_reverse( avc_handle, phyID );
	}
	pthread_mutex_unlock( &avc_mutex );
	return 0;
}

bool AVC::isHDV( int phyID ) const
{
	int retry = 2;
	quadlet_t response = avc1394_transaction( avc_handle, phyID,
		AVC1394_CTYPE_STATUS | AVC1394_SUBUNIT_TYPE_TAPE_RECORDER | AVC1394_SUBUNIT_ID_0
		| AVC1394_VCR_COMMAND_OUTPUT_SIGNAL_MODE | 0xFF, retry );
	response = AVC1394_GET_OPERAND0( response );
// 	fprintf(stderr, "%s: 0x%x\n", __PRETTY_FUNCTION__, response);
	return ( response == 0x10 || response == 0x90 || response == 0x1A || response == 0x9A );
}

/** Start receiving DV frames
 
    The received frames can be retrieved from the outFrames queue.
 
*/
bool pipeReader::StartThread()
{
	pthread_mutex_lock( &mutex );
	currentFrame = NULL;
	pthread_create( &thread, NULL, ThreadProxy, this );
	pthread_mutex_unlock( &mutex );
	return true;
}


/** Stop the receiver thread.
 
    The receiver thread is being canceled. It will finish the next
    time it calls the pthread_testcancel() function. We also remove all frames 
    in the outFrames queue that have not been processed until now.
 
*/
void pipeReader::StopThread()
{
	pthread_cancel( thread );
	pthread_join( thread, NULL );
	Flush();
}


bool pipeReader::Handler()
{
	bool ret = true;

	pthread_mutex_lock( &mutex );
	if ( currentFrame == NULL && inFrames.size() > 0 )
	{
		currentFrame = inFrames.front();
		currentFrame->Clear();
		inFrames.pop_front();
		//printf("reader < buf: buffer %d, output %d\n", inFrames.size(), outFrames.size());
		//fflush(stdout);
	}
	pthread_mutex_unlock( &mutex );
	if ( currentFrame != NULL )
	{
		if ( isHDV )
		{
			void *buf = &currentFrame->data[currentFrame->GetDataLen()];
			if ( ret = ( fread( buf, IEC61883_MPEG2_TSP_SIZE, 1, file ) == 1 ) )
				currentFrame->AddDataLen( IEC61883_MPEG2_TSP_SIZE );
			else
				((HDVFrame*)currentFrame)->SetComplete();
		}
		else
		{
			if ( ret = ( fread( currentFrame->data, 120000, 1, file ) == 1 ) )
			{
				currentFrame->SetDataLen( 120000 );

				if ( currentFrame->data[ 3 ] & 0x80 )
					if ( ret = ( fread( currentFrame->data + 120000, 24000, 1, file ) == 1 ) )
						currentFrame->AddDataLen( 24000 );
			}
		}

		if ( ( ret && currentFrame->IsComplete() ) || ( !ret && currentFrame->GetDataLen() > 0 ) )
		{
			pthread_mutex_lock( &mutex );
			outFrames.push_back( currentFrame );
			currentFrame = NULL;
			TriggerAction( );
			pthread_mutex_unlock( &mutex );
		}
	}
	return ret;
}


/** The thread responsible for polling the rawdv interface.
 
    Though this is an infinite loop, it can be canceled by StopThread,
    but only in the pthread_testcancel() function.
 
*/
void* pipeReader::ThreadProxy( void* arg )
{
	pipeReader* self = static_cast< pipeReader* >( arg );
	return self->Thread();
}

void* pipeReader::Thread()
{
	if ( strcmp( input_file, "-" ) == 0 )
		file = stdin;
	else
		file = fopen( input_file, "rb" );

	if ( ! file )
	{
		sendEvent( "No input file" );
		return NULL;
	}

	while ( true )
	{
		if ( ! Handler() )
			break;
		pthread_testcancel();
	}

	if ( strcmp( input_file, "-" ) != 0 )
		fclose( file );

	sendEvent( "End of pipe" );
	pthread_mutex_lock( &mutex );
	if ( currentFrame ) outFrames.push_back( currentFrame );
	currentFrame = NULL;
	outFrames.push_back( currentFrame );
	TriggerAction( );
	pthread_mutex_unlock( &mutex );
	return NULL;
}
