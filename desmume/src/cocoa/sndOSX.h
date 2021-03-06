/*
	Copyright (C) 2007 Jeff Bland
	Copyright (C) 2007-2014 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _OSXSOUNDINTERFACE_
#define _OSXSOUNDINTERFACE_

#include <pthread.h>
#include "../SPU.h"
#undef BOOL

#define SNDCORE_OSX 58325 //hopefully this is unique number

extern SoundInterface_struct SNDOSX; // Sound interface to the SPU
extern pthread_rwlock_t *rwlockAudioEmulateCore; // RWlock for the emulation core - used when mixing audio in Dual Synch/Asynch mode in post-process

// Core Audio functions for the sound interface
int		SNDOSXInit(int buffer_size);
void	SNDOSXDeInit();
int		SNDOSXReset();
void	SNDOSXUpdateAudio(s16 *buffer, u32 num_samples);
u32		SNDOSXGetAudioSpace();
void	SNDOSXMuteAudio();
void	SNDOSXUnMuteAudio();
void	SNDOSXPauseAudio();
void	SNDOSXUnpauseAudio();
void	SNDOSXSetVolume(int volume);
void	SNDOSXClearBuffer();
void	SNDOSXFetchSamples(s16 *sampleBuffer, size_t sampleCount, ESynchMode synchMode, ISynchronizingAudioBuffer *theSynchronizer);
size_t	SNDOSXPostProcessSamples(s16 *postProcessBuffer, size_t requestedSampleCount, ESynchMode synchMode, ISynchronizingAudioBuffer *theSynchronizer);

#endif // _OSXSOUNDINTERFACE_
