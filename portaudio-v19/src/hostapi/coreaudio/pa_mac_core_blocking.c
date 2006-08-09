/* This file contains the implementation
 * required for blocking I/O. It is separated from pa_mac_core.c simply to ease
 * development. */

#include "pa_mac_core_blocking.h"
#include "pa_mac_core_internal.h"
#include <assert.h>
#ifdef MOSX_USE_NON_ATOMIC_FLAG_BITS
# define OSAtomicOr32( a, b ) ( (*(b)) |= (a) )
# define OSAtomicAnd32( a, b ) ( (*(b)) &= (a) )
#else
# include <libkern/OSAtomic.h>
#endif

/*
 * This fnuction determines the size of a particular sample format.
 * if the format is not recognized, this returns zero.
 */
static size_t computeSampleSizeFromFormat( PaSampleFormat format )
{
   switch( format ) {
   case paFloat32: return 4;
   case paInt32: return 4;
   case paInt24: return 3;
   case paInt16: return 2;
   case paInt8: case paUInt8: return 1;
   default: return 0;
   }
}


/*
 * Functions for initializing, resetting, and destroying BLIO structures.
 *
 */

/* This should be called with the relevant info when initializing a stream for
   callback. */
PaError initializeBlioRingBuffers(
                                       PaMacBlio *blio,
                                       PaSampleFormat inputSampleFormat,
                                       PaSampleFormat outputSampleFormat,
                                       size_t framesPerBuffer,
                                       long ringBufferSize,
                                       int inChan,
                                       int outChan )
{
   void *data;
   int result;

   /* zeroify things */
   bzero( blio, sizeof( PaMacBlio ) );
   /* this is redundant, but the buffers are used to check
      if the bufffers have been initialized, so we do it explicitly. */
   blio->inputRingBuffer.buffer = NULL;
   blio->outputRingBuffer.buffer = NULL;

   /* initialize simple data */
   blio->inputSampleFormat = inputSampleFormat;
   blio->inputSampleSize = computeSampleSizeFromFormat(inputSampleFormat);
   blio->outputSampleFormat = outputSampleFormat;
   blio->outputSampleSize = computeSampleSizeFromFormat(outputSampleFormat);
   blio->framesPerBuffer = framesPerBuffer;
   blio->inChan = inChan;
   blio->outChan = outChan;
   blio->statusFlags = 0;
   blio->errors = paNoError;
#ifdef PA_MAC_BLIO_MUTEX
   blio->isInputEmpty = false;
   blio->isOutputFull = false;
#endif

   /* setup ring buffers */
#ifdef PA_MAC_BLIO_MUTEX
   result = PaMacCore_SetUnixError( pthread_mutex_init(&(blio->inputMutex),NULL), 0 );
   if( result )
      goto error;
   result = UNIX_ERR( pthread_cond_init( &(blio->inputCond), NULL ) );
   if( result )
      goto error;
   result = UNIX_ERR( pthread_mutex_init(&(blio->outputMutex),NULL) );
   if( result )
      goto error;
   result = UNIX_ERR( pthread_cond_init( &(blio->outputCond), NULL ) );
#endif
   if( inChan ) {
      data = calloc( ringBufferSize, blio->inputSampleSize );
      if( !data )
      {
         result = paInsufficientMemory;
         goto error;
      }

      assert( 0 == RingBuffer_Init(
            &blio->inputRingBuffer,
            ringBufferSize*blio->inputSampleSize,
            data ) );
   }
   if( outChan ) {
      data = calloc( ringBufferSize, blio->outputSampleSize );
      if( !data )
      {
         result = paInsufficientMemory;
         goto error;
      }

      assert( 0 == RingBuffer_Init(
            &blio->outputRingBuffer,
            ringBufferSize*blio->outputSampleSize,
            data ) );
   }

   result = resetBlioRingBuffers( blio );
   if( result )
      goto error;

   return 0;

 error:
   destroyBlioRingBuffers( blio );
   return result;
}

#ifdef PA_MAC_BLIO_MUTEX
PaError blioSetIsInputEmpty( PaMacBlio *blio, bool isEmpty )
{
   PaError result = paNoError;
   if( isEmpty == blio->isInputEmpty )
      goto done;

   /* we need to update the value. Here's what we do:
    * - Lock the mutex, so noone else can write.
    * - update the value.
    * - unlock.
    * - broadcast to all listeners.
    */
   result = UNIX_ERR( pthread_mutex_lock( &blio->inputMutex ) );
   if( result )
      goto done;
   blio->isInputEmpty = isEmpty;
   result = UNIX_ERR( pthread_mutex_unlock( &blio->inputMutex ) );
   if( result )
      goto done;
   result = UNIX_ERR( pthread_cond_broadcast( &blio->inputCond ) );
   if( result )
      goto done;

 done:
   return result;
}
PaError blioSetIsOutputFull( PaMacBlio *blio, bool isFull )
{
   PaError result = paNoError;
   if( isFull == blio->isOutputFull )
      goto done;

   /* we need to update the value. Here's what we do:
    * - Lock the mutex, so noone else can write.
    * - update the value.
    * - unlock.
    * - broadcast to all listeners.
    */
   result = UNIX_ERR( pthread_mutex_lock( &blio->outputMutex ) );
   if( result )
      goto done;
   blio->isOutputFull = isFull;
   result = UNIX_ERR( pthread_mutex_unlock( &blio->outputMutex ) );
   if( result )
      goto done;
   result = UNIX_ERR( pthread_cond_broadcast( &blio->outputCond ) );
   if( result )
      goto done;

 done:
   return result;
}
#endif

/* This should be called after stopping or aborting the stream, so that on next
   start, the buffers will be ready. */
PaError resetBlioRingBuffers( PaMacBlio *blio )
{
#ifdef PA_MAC__BLIO_MUTEX
   int result;
#endif
   blio->statusFlags = 0;
   if( blio->outputRingBuffer.buffer ) {
      RingBuffer_Flush( &blio->outputRingBuffer );
      bzero( blio->outputRingBuffer.buffer,
             blio->outputRingBuffer.bufferSize );
      /* Advance buffer */
      RingBuffer_AdvanceWriteIndex( &blio->outputRingBuffer, blio->outputRingBuffer.bufferSize );

      /* Update isOutputFull. */
#ifdef PA_MAC__BLIO_MUTEX
      result = blioSetIsOutputFull( blio, toAdvance == blio->outputRingBuffer.bufferSize );
      if( result )
         goto error;
#endif
/*
      printf( "------%d\n" ,  blio->framesPerBuffer );
      printf( "------%d\n" ,  blio->outChan );
      printf( "------%d\n" ,  blio->outputSampleSize );
      printf( "------%d\n" ,  blio->framesPerBuffer*blio->outChan*blio->outputSampleSize );
*/
   }
   if( blio->inputRingBuffer.buffer ) {
      RingBuffer_Flush( &blio->inputRingBuffer );
      bzero( blio->inputRingBuffer.buffer,
             blio->inputRingBuffer.bufferSize );
      /* Update isInputEmpty. */
#ifdef PA_MAC__BLIO_MUTEX
      result = blioSetIsInputEmpty( blio, true );
      if( result )
         goto error;
#endif
   }
   return paNoError;
#ifdef PA_MAC__BLIO_MUTEX
 error:
   return result;
#endif
}

/*This should be called when you are done with the blio. It can safely be called
  multiple times if there are no exceptions. */
PaError destroyBlioRingBuffers( PaMacBlio *blio )
{
   PaError result = paNoError;
   if( blio->inputRingBuffer.buffer ) {
      free( blio->inputRingBuffer.buffer );
#ifdef PA_MAC__BLIO_MUTEX
      result = UNIX_ERR( pthread_mutex_destroy( & blio->inputMutex ) );
      if( result ) return result;
      result = UNIX_ERR( pthread_cond_destroy( & blio->inputCond ) );
      if( result ) return result;
#endif
   }
   blio->inputRingBuffer.buffer = NULL;
   if( blio->outputRingBuffer.buffer ) {
      free( blio->outputRingBuffer.buffer );
#ifdef PA_MAC__BLIO_MUTEX
      result = UNIX_ERR( pthread_mutex_destroy( & blio->outputMutex ) );
      if( result ) return result;
      result = UNIX_ERR( pthread_cond_destroy( & blio->outputCond ) );
      if( result ) return result;
#endif
   }
   blio->outputRingBuffer.buffer = NULL;

   return result;
}

/*
 * this is the BlioCallback function. It expects to recieve a PaMacBlio Object
 * pointer as userData.
 *
 */
int BlioCallback( const void *input, void *output, unsigned long frameCount,
	const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void *userData )
{
   PaMacBlio *blio = (PaMacBlio*)userData;
   long avail;
   long toRead;
   long toWrite;

   /* set flags returned by OS: */
   OSAtomicOr32( statusFlags, &blio->statusFlags ) ;

   /* --- Handle Input Buffer --- */
   if( blio->inChan ) {
      avail = RingBuffer_GetWriteAvailable( &blio->inputRingBuffer );

      /* check for underflow */
      if( avail < frameCount * blio->inputSampleSize * blio->inChan )
         OSAtomicOr32( paInputOverflow, &blio->statusFlags );

      toRead = MIN( avail, frameCount * blio->inputSampleSize * blio->inChan );

      /* copy the data */
      /*printf( "reading %d\n", toRead );*/
      assert( toRead == RingBuffer_Write( &blio->inputRingBuffer, input, toRead ) );
#ifdef PA_MAC__BLIO_MUTEX
      /* Priority inversion. See notes below. */
      blioSetIsInputEmpty( blio, false );
#endif
   }


   /* --- Handle Output Buffer --- */
   if( blio->outChan ) {
      avail = RingBuffer_GetReadAvailable( &blio->outputRingBuffer );

      /* check for underflow */
      if( avail < frameCount * blio->outputSampleSize * blio->outChan )
         OSAtomicOr32( paOutputUnderflow, &blio->statusFlags );

      toWrite = MIN( avail, frameCount * blio->outputSampleSize * blio->outChan );

      if( toWrite != frameCount * blio->outputSampleSize * blio->outChan )
         bzero( ((char *)output)+toWrite,
                frameCount * blio->outputSampleSize * blio->outChan - toWrite );
      /* copy the data */
      /*printf( "writing %d\n", toWrite );*/
      assert( toWrite == RingBuffer_Read( &blio->outputRingBuffer, output, toWrite ) );
#ifdef PA_MAC__BLIO_MUTEX
      /* We have a priority inversion here. However, we will only have to
         wait if this was true and is now false, which means we've got
         some room in the buffer.
         Hopefully problems will be minimized. */
      blioSetIsOutputFull( blio, false );
#endif
   }

   return paContinue;
}

PaError ReadStream( PaStream* stream,
                           void *buffer,
                           unsigned long frames )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    char *cbuf = (char *) buffer;
    PaError ret = paNoError;
    VVDBUG(("ReadStream()\n"));

    while( frames > 0 ) {
       long avail;
       long toRead;
       do {
          avail = RingBuffer_GetReadAvailable( &blio->inputRingBuffer );
/*
          printf( "Read Buffer is %%%g full: %ld of %ld.\n",
                  100 * (float)avail / (float) blio->inputRingBuffer.bufferSize,
                  avail, blio->inputRingBuffer.bufferSize );
*/
          if( avail == 0 ) {
#ifdef PA_MAC_BLIO_MUTEX
             /**block when empty*/
             ret = UNIX_ERR( pthread_mutex_lock( &blio->inputMutex ) );
             if( ret )
                return ret;
             while( blio->isInputEmpty ) {
                ret = UNIX_ERR( pthread_cond_wait( &blio->inputCond, &blio->inputMutex ) );
                if( ret )
                   return ret;
             }
             ret = UNIX_ERR( pthread_mutex_unlock( &blio->inputMutex ) );
             if( ret )
                return ret;
#else
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
#endif
          }
       } while( avail == 0 );
       toRead = MIN( avail, frames * blio->inputSampleSize * blio->inChan );
       toRead -= toRead % blio->inputSampleSize * blio->inChan ;
       RingBuffer_Read( &blio->inputRingBuffer, (void *)cbuf, toRead );
       cbuf += toRead;
       frames -= toRead / ( blio->inputSampleSize * blio->inChan );

       if( toRead == avail ) {
#ifdef PA_MAC_BLIO_MUTEX
          /* we just emptied the buffer, so we need to mark it as empty. */
          ret = blioSetIsInputEmpty( blio, true );
          if( ret )
             return ret;
          /* of course, in the meantime, the callback may have put some sats
             in, so
             so check for that, too, to avoid a race condition. */
          if( RingBuffer_GetReadAvailable( &blio->inputRingBuffer ) ) {
             blioSetIsInputEmpty( blio, false );
             if( ret )
                return ret;
          }
#endif
       }
    }

    /*   Report either paNoError or paInputOverflowed. */
    /*   may also want to report other errors, but this is non-standard. */
    ret = blio->statusFlags & paInputOverflow;

    /* report underflow only once: */
    if( ret ) {
       OSAtomicAnd32( ~paInputOverflow, &blio->statusFlags );
       ret = paInputOverflowed;
    }

    return ret;
}


PaError WriteStream( PaStream* stream,
                            const void *buffer,
                            unsigned long frames )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    char *cbuf = (char *) buffer;
    PaError ret = paNoError;
    VVDBUG(("WriteStream()\n"));

    while( frames > 0 ) {
       long avail = 0;
       long toWrite;

       do {
          avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
/*
          printf( "Write Buffer is %%%g full: %ld of %ld.\n",
                  100 - 100 * (float)avail / (float) blio->outputRingBuffer.bufferSize,
                  avail, blio->outputRingBuffer.bufferSize );
*/
          if( avail == 0 ) {
#ifdef PA_MAC_BLIO_MUTEX
             /*block while full*/
             ret = UNIX_ERR( pthread_mutex_lock( &blio->outputMutex ) );
             if( ret )
                return ret;
             while( blio->isOutputFull ) {
                ret = UNIX_ERR( pthread_cond_wait( &blio->outputCond, &blio->outputMutex ) );
                if( ret )
                   return ret;
             }
             ret = UNIX_ERR( pthread_mutex_unlock( &blio->outputMutex ) );
             if( ret )
                return ret;
#else
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
#endif
          }
       } while( avail == 0 );

       toWrite = MIN( avail, frames * blio->outputSampleSize * blio->outChan );
       toWrite -= toWrite % blio->outputSampleSize * blio->outChan ;
       RingBuffer_Write( &blio->outputRingBuffer, (void *)cbuf, toWrite );
       cbuf += toWrite;
       frames -= toWrite / ( blio->outputSampleSize * blio->outChan );

#ifdef PA_MAC_BLIO_MUTEX
       if( toWrite == avail ) {
          /* we just filled up the buffer, so we need to mark it as filled. */
          ret = blioSetIsOutputFull( blio, true );
          if( ret )
             return ret;
          /* of course, in the meantime, we may have emptied the buffer, so
             so check for that, too, to avoid a race condition. */
          if( RingBuffer_GetWriteAvailable( &blio->outputRingBuffer ) ) {
             blioSetIsOutputFull( blio, false );
             if( ret )
                return ret;
          }
       }
#endif
    }

    /*   Report either paNoError or paOutputUnderflowed. */
    /*   may also want to report other errors, but this is non-standard. */
    ret = blio->statusFlags & paOutputUnderflow;

    /* report underflow only once: */
    if( ret ) {
      OSAtomicAnd32( ~paOutputUnderflow, &blio->statusFlags );
      ret = paOutputUnderflowed;
    }

    return ret;
}

/*
 *
 */
void waitUntilBlioWriteBufferIsFlushed( PaMacBlio *blio )
{
    if( blio->outputRingBuffer.buffer ) {
       long avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
       while( avail != blio->outputRingBuffer.bufferSize ) {
          if( avail == 0 )
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
          avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
       }
    }
}


signed long GetStreamReadAvailable( PaStream* stream )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    VVDBUG(("GetStreamReadAvailable()\n"));

    return RingBuffer_GetReadAvailable( &blio->inputRingBuffer )
                         / ( blio->outputSampleSize * blio->outChan );
}


signed long GetStreamWriteAvailable( PaStream* stream )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    VVDBUG(("GetStreamWriteAvailable()\n"));

    return RingBuffer_GetWriteAvailable( &blio->outputRingBuffer )
                         / ( blio->outputSampleSize * blio->outChan );
}
