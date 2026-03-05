/*
* frame.cc -- utilities for processing digital video frames
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

#include "frame.h"

#include <stdlib.h>
#include <string.h>

Frame::Frame( int bufferSize ) : data( NULL ), dataLen( 0 ), dataCapacity( bufferSize )
{
	data = ( unsigned char* ) malloc( dataCapacity );
	if ( !data )
	{
		fprintf( stderr, "ERROR: Failed to allocate %d bytes for frame buffer\n", dataCapacity );
		exit( 1 );
	}
}

Frame::~Frame()
{
	free( data );
	data = NULL;
}

int Frame::GetDataLen()
{
	return dataLen;
}

int Frame::GetDataCapacity()
{
	return dataCapacity;
}

void Frame::SetDataLen( int len )
{
	dataLen = len;
}

void Frame::AddDataLen( int len )
{
	SetDataLen( GetDataLen() + len );
}

void Frame::Clear()
{
	dataLen = 0;
}
