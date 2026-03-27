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

static enum raw1394_iso_disposition
rawIsoHandler( raw1394handle_t handle, unsigned char *data,
	unsigned int len, unsigned char channel, unsigned char tag,
	unsigned char sy, unsigned int cycle, unsigned int dropped )
{
	int *counter = static_cast< int* >( raw1394_get_userdata( handle ) );
	( *counter )++;
	if ( *counter == 1 )
	{
		fprintf( stderr, "  Raw iso packet: channel=%d len=%u tag=%d "
			"cycle=%u\n", channel, len, tag, cycle );
		// Dump CIP header (first 8 bytes) if present
		if ( len >= 8 && tag == 1 )
		{
			unsigned char fmt = data[4] & 0x3F;
			unsigned char fdf = data[5];
			int dbs = data[1];
			int fn  = ( data[2] >> 6 ) & 0x3;
			int sph = ( data[2] >> 2 ) & 1;
			fprintf( stderr, "  CIP: DBS=%d FN=%d SPH=%d FMT=0x%02x "
				"FDF=0x%02x\n", dbs, fn, sph, fmt, fdf );
			// Dump first 16 bytes hex
			fprintf( stderr, "  Hex:" );
			for ( unsigned int b = 0; b < 16 && b < len; b++ )
				fprintf( stderr, " %02x", data[b] );
			fprintf( stderr, "\n" );
		}
	}
	return RAW1394_ISO_OK;
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
	if ( rawCallCount == 1 )
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

	// Phase 1: Find DIF frame alignment by scanning for the
	// FSC=1→FSC=0 transition at a header boundary (SCT=0).
	// Also auto-detect the true frame size from FSC consistency.
	if ( self->m_rawIsoAlignOffset < 0 )
	{
		// Need at least one full frame to scan
		if ( self->m_rawIsoFrameOffset < self->m_rawIsoFrameSize )
			return RAW1394_ISO_OK;

		// Scan for header blocks and find FSC transitions
		int prevFsc = -1;
		int prevFscOff = -1;
		int alignOff = -1;
		int detectedFrameSize = -1;
		for ( int off = 0; off + 80 <= self->m_rawIsoFrameOffset;
			off += 80 )
		{
			int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
			if ( sct == 0 )
			{
				int fsc = ( self->m_rawIsoFrameBuf[off+1] >> 3 ) & 1;
				if ( prevFsc >= 0 && fsc != prevFsc )
				{
					if ( alignOff < 0 )
					{
						// First FSC transition — this is a
						// frame boundary.  Record size from
						// this transition to the next one.
						if ( fsc == 0 )
						{
							alignOff = off;
							detectedFrameSize = off - prevFscOff;
						}
						else
						{
							// FSC 0→1 — note offset, keep
							// scanning for 1→0.
							prevFscOff = off;
						}
					}
					else if ( detectedFrameSize < 0 )
					{
						// Second FSC transition — now we know
						// the true frame size.
						detectedFrameSize = off - alignOff;
					}
					else
						break;
				}
				if ( prevFsc < 0 )
					prevFscOff = off;
				prevFsc = fsc;
			}
		}

		if ( alignOff >= 0 )
		{
			self->m_rawIsoAlignOffset = alignOff;

			// Update frame size if FSC detection found a
			// different size than the CIP-based estimate.
			if ( detectedFrameSize > 0 &&
				detectedFrameSize != self->m_rawIsoFrameSize )
			{
				fprintf( stderr, "DIF frame size auto-detected: "
					"%d bytes (was %d)\n",
					detectedFrameSize,
					self->m_rawIsoFrameSize );
				self->m_rawIsoFrameSize = detectedFrameSize;
			}

			fprintf( stderr, "DIF frame alignment: offset=%d "
				"bytes (%d DIF blocks), frame_size=%d\n",
				alignOff, alignOff / 80,
				self->m_rawIsoFrameSize );

			// Shift data so the frame starts at alignOff
			int remaining = self->m_rawIsoFrameOffset - alignOff;
			if ( remaining > 0 )
				memmove( self->m_rawIsoFrameBuf,
					self->m_rawIsoFrameBuf + alignOff, remaining );
			self->m_rawIsoFrameOffset = remaining;
		}
		else
		{
			// No FSC transition found — fall back to first header
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
			if ( alignOff >= 0 )
			{
				self->m_rawIsoAlignOffset = alignOff;
				fprintf( stderr, "DIF frame alignment (header): "
					"offset=%d bytes\n", alignOff );
				int remaining = self->m_rawIsoFrameOffset - alignOff;
				if ( remaining > 0 )
					memmove( self->m_rawIsoFrameBuf,
						self->m_rawIsoFrameBuf + alignOff,
						remaining );
				self->m_rawIsoFrameOffset = remaining;
			}
			else
			{
				// No headers at all — use fixed-size (no alignment)
				self->m_rawIsoAlignOffset = 0;
				fprintf( stderr, "DIF frame alignment: no headers "
					"found, using unaligned\n" );
			}
		}
	}

	// Phase 2: Deliver aligned frames
	while ( self->m_rawIsoFrameOffset >= self->m_rawIsoFrameSize )
	{
		// Dump DIF header info for first frame
		if ( !self->m_rawIsoSynced )
		{
			self->m_rawIsoSynced = true;
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
		}

		// Fix APT field in all DIF header blocks if needed
		if ( self->m_rawIsoFixApt >= 0 )
		{
			for ( int off = 0; off + 80 <= self->m_rawIsoFrameSize;
				off += 80 )
			{
				int sct = ( self->m_rawIsoFrameBuf[off] >> 5 ) & 7;
				if ( sct == 0 )
				{
					// Byte 4 of DIF block: bits 2-0 = APT
					self->m_rawIsoFrameBuf[off + 4] =
						( self->m_rawIsoFrameBuf[off + 4] & 0xF8 ) |
						( self->m_rawIsoFixApt & 0x07 );
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

bool iec61883Reader::StartReceive()
{
	bool success;

	// Probe: try raw iso receive to check for isochronous data and
	// detect stream parameters.
	fprintf( stderr, "Probing for isochronous data on channel %d...\n",
		channel );
	int probeFn = -1;
	int probeDbs = -1;
	int probeFdf = -1;
	int probePackets = 0;
	int probeMaxLen = 0;
	{
		raw1394handle_t probe = raw1394_new_handle_on_port( m_port );
		if ( probe )
		{
			int counter = 0;
			raw1394_set_userdata( probe, &counter );

			if ( raw1394_iso_recv_init( probe, rawIsoHandler, 64, 2048,
				channel, RAW1394_DMA_DEFAULT, -1 ) == 0 )
			{
				if ( raw1394_iso_recv_start( probe, -1, -1, 0 ) == 0 )
				{
					int probe_fd = raw1394_get_fd( probe );
					struct pollfd pfd = { probe_fd, POLLIN, 0 };
					for ( int t = 0; t < 10; t++ )
					{
						int r = poll( &pfd, 1, 50 );
						if ( r > 0 )
							raw1394_loop_iterate( probe );
					}
					raw1394_iso_stop( probe );
					raw1394_iso_shutdown( probe );
					probePackets = counter;
					fprintf( stderr, "  Probe: %d packets in 500ms\n",
						counter );
				}
			}
			raw1394_destroy_handle( probe );
		}

		// Second probe pass to capture CIP details
		if ( probePackets > 0 )
		{
			struct CipProbeData {
				int fn, dbs, fdf, maxLen, count;
			} cipData = { -1, -1, -1, 0, 0 };

			// Use a lambda-like static function
			struct CipProbe {
				static enum raw1394_iso_disposition handler(
					raw1394handle_t h, unsigned char *d, unsigned int l,
					unsigned char ch, unsigned char tg, unsigned char s,
					unsigned int cy, unsigned int dr )
				{
					CipProbeData *cd = static_cast< CipProbeData* >(
						raw1394_get_userdata( h ) );
					if ( (int)l > cd->maxLen )
						cd->maxLen = l;
					if ( tg == 1 && l >= 8 && cd->fn < 0 )
					{
						cd->dbs = d[1];
						cd->fn  = ( d[2] >> 6 ) & 0x3;
						cd->fdf = d[5];
					}
					cd->count++;
					return RAW1394_ISO_OK;
				}
			};

			raw1394handle_t probe2 = raw1394_new_handle_on_port( m_port );
			if ( probe2 )
			{
				raw1394_set_userdata( probe2, &cipData );
				if ( raw1394_iso_recv_init( probe2, CipProbe::handler,
					64, 2048, channel, RAW1394_DMA_DEFAULT, -1 ) == 0 )
				{
					if ( raw1394_iso_recv_start( probe2, -1, -1, 0 ) == 0 )
					{
						int fd2 = raw1394_get_fd( probe2 );
						struct pollfd pfd2 = { fd2, POLLIN, 0 };
						for ( int t = 0; t < 4; t++ )
						{
							int r = poll( &pfd2, 1, 50 );
							if ( r > 0 )
								raw1394_loop_iterate( probe2 );
						}
						raw1394_iso_stop( probe2 );
						raw1394_iso_shutdown( probe2 );
					}
				}
				raw1394_destroy_handle( probe2 );
				probeFn = cipData.fn;
				probeDbs = cipData.dbs;
				probeFdf = cipData.fdf;
				probeMaxLen = cipData.maxLen;
				fprintf( stderr, "  CIP: DBS=%d FN=%d FDF=0x%02x "
					"max_pkt=%d\n", probeDbs, probeFn, probeFdf,
					probeMaxLen );
			}
		}
	}

	/* Starting iso receive */
	fprintf( stderr, "Starting isochronous receive on channel %d\n", channel );

	// Use raw iso mode for DVCPRO50+ (FN>0 means packets larger than
	// DV25, which libiec61883 may not handle correctly)
	if ( !isHDV && probePackets > 0 && probeFn > 0 )
	{
		// Determine format from FN:
		//   FN=1: DVCPRO50 (50 Mbps) - 960 byte packets
		//   FN=2: DVCPRO HD (100 Mbps) - 1920 byte packets
		const char *fmtName = probeFn >= 2 ? "DVCPRO-HD" : "DVCPRO50";
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
			int maxPkt = probeMaxLen > 0 ? probeMaxLen : 2048;
			if ( raw1394_iso_recv_init( m_rawIsoHandle, RawDvIsoHandler,
				400, maxPkt, channel, RAW1394_DMA_DEFAULT, -1 ) == 0 )
			{
				if ( raw1394_iso_recv_start( m_rawIsoHandle,
					-1, -1, 0 ) == 0 )
				{
					success = true;
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
	if ( handlerCalls == 0 )
		fprintf( stderr, "First DV packet received: %d bytes\n", length );
	handlerCalls++;

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

	while ( isRunning )
	{
		int result = poll( &raw1394_poll, 1, 20 );

		if ( result < 0 && errno != EAGAIN && errno != EINTR )
		{
			perror( "error: raw1394 poll" );
			break;
		}

		raw1394_loop_iterate( iterHandle );
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
