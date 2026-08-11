/*
 * iOS audio driver, on top of AudioQueue
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *           2022 Huw Davies
 *           2026 Alon Amir
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <AudioToolbox/AudioToolbox.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "initguid.h"
#include "audioclient.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(iosaudio);

/* AudioQueue hands buffers back as it plays them, so the driver keeps a few in
 * flight and fills each one from the local buffer as it comes back. */
#define QUEUE_BUFFER_COUNT 4

struct ios_stream
{
    WAVEFORMATEX *fmt;
    EDataFlow flow;
    UINT flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    HANDLE timer_thread;

    AudioQueueRef queue;
    AudioQueueBufferRef buffers[QUEUE_BUFFER_COUNT];
    UINT32 queued_frames;

    BOOL playing, mute, please_quit;
    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, held_frames, tmp_buffer_frames;
    UINT32 lcl_offs_frames;   /* offset into local_buffer where valid data starts */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    BYTE silence;
    INT32 getbuf_last;        /* <0 when using tmp_buffer */

    pthread_mutex_t lock;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;

/* AudioQueue picks the route itself, so there is one endpoint */
static const char default_device[] = "audioqueue";

static ULONG_PTR zero_bits = 0;

static NTSTATUS ios_not_implemented( void *args )
{
    return STATUS_SUCCESS;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static void stream_lock( struct ios_stream *stream )
{
    pthread_mutex_lock( &stream->lock );
}

static void stream_unlock( struct ios_stream *stream )
{
    pthread_mutex_unlock( &stream->lock );
}

static NTSTATUS stream_unlock_result( struct ios_stream *stream, HRESULT *result, HRESULT value )
{
    *result = value;
    stream_unlock( stream );
    return STATUS_SUCCESS;
}

static struct ios_stream *handle_get_stream( stream_handle h )
{
    return (struct ios_stream *)(UINT_PTR)h;
}

static NTSTATUS ios_test_connect( void *args )
{
    struct test_connect_params *params = args;

    /* every iOS device has audio output */
    params->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_endpoint_ids( void *args )
{
    static const WCHAR outW[] = { 'i','O','S',' ','O','u','t','p','u','t',0 };
    static const WCHAR inW[] = { 'i','O','S',' ','I','n','p','u','t',0 };
    struct get_endpoint_ids_params *params = args;
    unsigned int name_len, device_len, needed, offset;
    const WCHAR *name;

    if (params->flow != eRender)   /* output-only: don't advertise a capture device */
    {
        params->num = 0;
        params->default_idx = 0;
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    name = params->flow == eRender ? outW : inW;
    name_len = wcslen( name ) + 1;
    device_len = strlen( default_device ) + 1;

    offset = needed = sizeof(*params->endpoints);
    needed += name_len * sizeof(WCHAR) + ((device_len + 1) & ~1);

    if (needed <= params->size)
    {
        params->endpoints->name = offset;
        memcpy( (char *)params->endpoints + offset, name, name_len * sizeof(WCHAR) );
        offset += name_len * sizeof(WCHAR);
        params->endpoints->device = offset;
        memcpy( (char *)params->endpoints + offset, default_device, device_len );
    }

    params->num = 1;
    params->default_idx = 0;

    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    }
    else params->result = S_OK;

    return STATUS_SUCCESS;
}

static UINT get_channel_mask( unsigned int channels )
{
    switch (channels)
    {
    case 0: return 0;
    case 1: return KSAUDIO_SPEAKER_MONO;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    FIXME( "Unknown speaker configuration: %u\n", channels );
    return 0;
}

/* AudioQueue takes linear PCM in whatever width the description says */
static HRESULT fill_stream_description( const WAVEFORMATEX *fmt, AudioStreamBasicDescription *desc )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
    BOOL is_float = FALSE;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT )))
        is_float = TRUE;
    else if (fmt->wFormatTag != WAVE_FORMAT_PCM &&
             !(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM )))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (is_float && fmt->wBitsPerSample != 32) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (!is_float && fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
        fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (!fmt->nChannels || fmt->nChannels > 8) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 192000) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    memset( desc, 0, sizeof(*desc) );
    desc->mFormatID = kAudioFormatLinearPCM;
    desc->mSampleRate = fmt->nSamplesPerSec;
    desc->mChannelsPerFrame = fmt->nChannels;
    desc->mBitsPerChannel = fmt->wBitsPerSample;
    desc->mFramesPerPacket = 1;
    desc->mBytesPerFrame = fmt->nChannels * fmt->wBitsPerSample / 8;
    desc->mBytesPerPacket = desc->mBytesPerFrame;
    desc->mFormatFlags = kLinearPCMFormatFlagIsPacked;
    if (is_float) desc->mFormatFlags |= kLinearPCMFormatFlagIsFloat;
    else if (fmt->wBitsPerSample > 8) desc->mFormatFlags |= kLinearPCMFormatFlagIsSignedInteger;

    if (fmt->nBlockAlign != desc->mBytesPerFrame ||
        fmt->nAvgBytesPerSec != fmt->nBlockAlign * fmt->nSamplesPerSec)
        return S_FALSE;

    return S_OK;
}

static WAVEFORMATEXTENSIBLE *clone_format( const WAVEFORMATEX *fmt )
{
    WAVEFORMATEXTENSIBLE *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) size = sizeof(WAVEFORMATEXTENSIBLE);
    else size = sizeof(WAVEFORMATEX);

    if (!(ret = malloc( size ))) return NULL;
    memcpy( ret, fmt, size );
    ret->Format.cbSize = size - sizeof(WAVEFORMATEX);
    return ret;
}

static void silence_buffer( struct ios_stream *stream, BYTE *buffer, UINT32 frames )
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE *)stream->fmt;

    if ((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
         (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM ))) &&
        stream->fmt->wBitsPerSample == 8)
        memset( buffer, 128, frames * stream->fmt->nBlockAlign );
    else
        memset( buffer, 0, frames * stream->fmt->nBlockAlign );
}

/* iOS only runs the audio hardware for a process with an active audio session,
 * and there is no C API for it - go through the runtime rather than make the
 * driver an Objective C file for three calls. */
/* declared here rather than through <objc/runtime.h>, whose BOOL clashes with
 * the Windows one */
typedef void *objc_class_t;
typedef void *objc_id_t;
typedef void *objc_sel_t;
extern objc_class_t objc_getClass( const char * );
extern objc_sel_t sel_registerName( const char * );
extern void objc_msgSend( void );

static void activate_audio_session(void)
{
    objc_id_t (*send_id)( objc_id_t, objc_sel_t ) = (objc_id_t (*)( objc_id_t, objc_sel_t ))objc_msgSend;
    char (*send_active)( objc_id_t, objc_sel_t, char, void * ) =
        (char (*)( objc_id_t, objc_sel_t, char, void * ))objc_msgSend;
    char (*send_category)( objc_id_t, objc_sel_t, objc_id_t, void * ) =
        (char (*)( objc_id_t, objc_sel_t, objc_id_t, void * ))objc_msgSend;
    objc_id_t (*send_string)( objc_id_t, objc_sel_t, const char * ) =
        (objc_id_t (*)( objc_id_t, objc_sel_t, const char * ))objc_msgSend;
    static BOOL done;
    objc_class_t session_class, string_class;
    objc_id_t session, category;

    if (done) return;
    done = TRUE;

    if (!(session_class = objc_getClass( "AVAudioSession" )))
    {
        WARN( "no AVAudioSession, the device may stay silent\n" );
        return;
    }
    if (!(session = send_id( session_class, sel_registerName( "sharedInstance" ) ))) return;

    if ((string_class = objc_getClass( "NSString" )))
    {
        category = send_string( string_class, sel_registerName( "stringWithUTF8String:" ),
                                "AVAudioSessionCategoryPlayback" );
        send_category( session, sel_registerName( "setCategory:error:" ), category, NULL );
    }
    send_active( session, sel_registerName( "setActive:error:" ), 1, NULL );
    TRACE( "activated the audio session\n" );
}

/* AudioQueue asks for the next buffer as it finishes the previous one */
/* This runs on one of AudioQueue's own threads, which wine knows nothing
 * about: it must touch nothing but the lock and the buffer, no wine call and
 * no tracing. The timer thread does everything that needs a wine thread. */
static void queue_callback( void *user, AudioQueueRef queue, AudioQueueBufferRef buffer )
{
    struct ios_stream *stream = user;
    UINT32 frames, capacity, copied = 0;
    BYTE *dst = buffer->mAudioData;

    pthread_mutex_lock( &stream->lock );

    capacity = buffer->mAudioDataBytesCapacity / stream->fmt->nBlockAlign;
    frames = min( stream->held_frames, capacity );

    while (copied < frames)
    {
        UINT32 chunk = min( frames - copied, stream->bufsize_frames - stream->lcl_offs_frames );

        memcpy( dst + copied * stream->fmt->nBlockAlign,
                stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign,
                chunk * stream->fmt->nBlockAlign );
        stream->lcl_offs_frames = (stream->lcl_offs_frames + chunk) % stream->bufsize_frames;
        copied += chunk;
    }
    stream->held_frames -= copied;

    /* the device wants a full buffer either way, so pad the rest with silence */
    if (copied < capacity)
        memset( dst + copied * stream->fmt->nBlockAlign, stream->silence,
                (capacity - copied) * stream->fmt->nBlockAlign );
    if (stream->mute) memset( dst, stream->silence, capacity * stream->fmt->nBlockAlign );

    buffer->mAudioDataByteSize = capacity * stream->fmt->nBlockAlign;

    pthread_mutex_unlock( &stream->lock );

    AudioQueueEnqueueBuffer( queue, buffer, 0, NULL );
}

static HRESULT open_queue( struct ios_stream *stream, const WAVEFORMATEX *fmt )
{
    AudioStreamBasicDescription desc;
    UINT32 bytes;
    OSStatus status;
    HRESULT hr;
    int i;

    if (FAILED(hr = fill_stream_description( fmt, &desc ))) return hr;

    activate_audio_session();

    status = AudioQueueNewOutput( &desc, queue_callback, stream, NULL, NULL, 0, &stream->queue );
    if (status)
    {
        WARN( "AudioQueueNewOutput failed: %d\n", (int)status );
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }

    bytes = stream->period_frames * fmt->nBlockAlign;
    for (i = 0; i < QUEUE_BUFFER_COUNT; i++)
    {
        if ((status = AudioQueueAllocateBuffer( stream->queue, bytes, &stream->buffers[i] )))
        {
            WARN( "AudioQueueAllocateBuffer failed: %d\n", (int)status );
            AudioQueueDispose( stream->queue, true );
            stream->queue = NULL;
            return E_OUTOFMEMORY;
        }
    }

    TRACE( "opened %u Hz %u channels, %u frame buffers\n", (unsigned int)fmt->nSamplesPerSec,
           fmt->nChannels, stream->period_frames );
    return S_OK;
}

static NTSTATUS ios_create_stream( void *args )
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct ios_stream *stream;
    SIZE_T size;

    params->result = S_OK;

    if (!(stream = calloc( 1, sizeof(*stream) )))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->flow = params->flow;
    pthread_mutex_init( &stream->lock, NULL );

    stream->period = params->period;
    stream->period_frames = muldiv( params->fmt->nSamplesPerSec, params->period, 10000000 );
    if (!stream->period_frames)
    {
        params->result = E_INVALIDARG;
        goto exit;
    }

    stream->bufsize_frames = muldiv( params->duration, params->fmt->nSamplesPerSec, 10000000 );
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;
    if (!stream->bufsize_frames)
    {
        params->result = E_INVALIDARG;
        goto exit;
    }

    if (!(fmtex = clone_format( params->fmt )))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;

    if (stream->flow == eRender && FAILED(params->result = open_queue( stream, params->fmt ))) goto exit;

    size = stream->bufsize_frames * params->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                 &size, MEM_COMMIT, PAGE_READWRITE ))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;
    stream->silence = (params->fmt->wBitsPerSample == 8 &&
                       params->fmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) ? 128 : 0;

exit:
    if (FAILED(params->result))
    {
        if (stream->queue) AudioQueueDispose( stream->queue, true );
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
        }
        pthread_mutex_destroy( &stream->lock );
        free( stream->fmt );
        free( stream );
    }
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ios_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    SIZE_T size;

    if (stream->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject( stream->timer_thread, FALSE, NULL );
        NtClose( stream->timer_thread );
    }

    if (stream->queue) AudioQueueDispose( stream->queue, true );
    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
    }
    free( stream->fmt );
    pthread_mutex_destroy( &stream->lock );
    free( stream );

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static void ios_timer_loop( void *args )
{
    struct ios_stream *stream = args;
    LARGE_INTEGER delay;

    delay.QuadPart = -stream->period;

    while (!stream->please_quit)
    {
        stream_lock( stream );
        if (stream->playing && stream->event) NtSetEvent( stream->event, NULL );
        stream_unlock( stream );
        NtDelayExecution( FALSE, &delay );
    }
}

static NTSTATUS ios_start( void *args )
{
    static const WCHAR timer_name[] = {'a','u','d','i','o','_','c','l','i','e','n','t','_','t','i','m','e','r',0};
    struct start_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    OSStatus status;
    int i;

    stream_lock( stream );

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET );

    if (stream->playing)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    stream->playing = TRUE;

    if (!stream->queue) return stream_unlock_result( stream, &params->result, S_OK );

    /* prime the queue: the callback only runs for buffers already in it. The
     * lock has to be dropped first - the queue calls back from its own thread
     * while it starts, and would deadlock against it. */
    for (i = 0; i < QUEUE_BUFFER_COUNT; i++)
    {
        AudioQueueBufferRef buffer = stream->buffers[i];

        silence_buffer( stream, buffer->mAudioData,
                        buffer->mAudioDataBytesCapacity / stream->fmt->nBlockAlign );
        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    }
    stream_unlock( stream );

    for (i = 0; i < QUEUE_BUFFER_COUNT; i++)
        AudioQueueEnqueueBuffer( stream->queue, stream->buffers[i], 0, NULL );

    TRACE( "starting the queue\n" );
    if ((status = AudioQueueStart( stream->queue, NULL )))
    {
        WARN( "AudioQueueStart failed: %d\n", (int)status );
        stream_lock( stream );
        stream->playing = FALSE;
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_DEVICE_INVALIDATED );
    }

    stream_lock( stream );
    if (!stream->timer_thread)
        create_unix_thread( &stream->timer_thread, timer_name, ios_timer_loop, stream );
    stream_unlock( stream );

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_stop( void *args )
{
    struct stop_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (!stream->playing)
        return stream_unlock_result( stream, &params->result, S_FALSE );

    if (stream->queue) AudioQueuePause( stream->queue );
    stream->playing = FALSE;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_reset( void *args )
{
    struct reset_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (stream->playing)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    if (stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING );

    if (stream->queue) AudioQueueReset( stream->queue );

    if (stream->flow == eRender)
    {
        stream->written_frames = 0;
        stream->last_pos_frames = 0;
    }
    else stream->written_frames += stream->held_frames;

    stream->held_frames = 0;
    stream->lcl_offs_frames = 0;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT32 write_pos, frames = params->frames;
    BYTE **data = params->data;
    SIZE_T size;

    stream_lock( stream );

    if (stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (!frames)
        return stream_unlock_result( stream, &params->result, S_OK );

    if (stream->held_frames + frames > stream->bufsize_frames)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE );

    write_pos = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    if (write_pos + frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
                stream->tmp_buffer = NULL;
            }
            size = frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                         &size, MEM_COMMIT, PAGE_READWRITE ))
            {
                stream->tmp_buffer_frames = 0;
                return stream_unlock_result( stream, &params->result, E_OUTOFMEMORY );
            }
            stream->tmp_buffer_frames = frames;
        }
        *data = stream->tmp_buffer;
        stream->getbuf_last = -frames;
    }
    else
    {
        *data = stream->local_buffer + write_pos * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }

    silence_buffer( stream, *data, frames );

    return stream_unlock_result( stream, &params->result, S_OK );
}

static void wrap_buffer( struct ios_stream *stream, BYTE *buffer, UINT32 written_frames )
{
    UINT32 write_offs_frames = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    UINT32 write_offs_bytes = write_offs_frames * stream->fmt->nBlockAlign;
    UINT32 chunk_frames = stream->bufsize_frames - write_offs_frames;
    UINT32 chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
    UINT32 written_bytes = written_frames * stream->fmt->nBlockAlign;

    if (written_bytes <= chunk_bytes)
        memcpy( stream->local_buffer + write_offs_bytes, buffer, written_bytes );
    else
    {
        memcpy( stream->local_buffer + write_offs_bytes, buffer, chunk_bytes );
        memcpy( stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes );
    }
}

static NTSTATUS ios_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT32 written_frames = params->written_frames;
    UINT flags = params->flags;
    BYTE *buffer;

    stream_lock( stream );

    if (!written_frames)
    {
        stream->getbuf_last = 0;
        return stream_unlock_result( stream, &params->result, S_OK );
    }

    if (!stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_INVALID_SIZE );

    if (stream->getbuf_last >= 0)
        buffer = stream->local_buffer + stream->fmt->nBlockAlign *
                 ((stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silence_buffer( stream, buffer, written_frames );

    if (stream->getbuf_last < 0) wrap_buffer( stream, buffer, written_frames );

    stream->held_frames += written_frames;
    stream->written_frames += written_frames;
    stream->getbuf_last = 0;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT64 *devpos = params->devpos, *qpcpos = params->qpcpos;
    UINT32 *frames = params->frames;
    UINT *flags = params->flags;
    BYTE **data = params->data;
    SIZE_T size;

    stream_lock( stream );

    if (stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (stream->held_frames < stream->period_frames)
    {
        *frames = 0;
        return stream_unlock_result( stream, &params->result, AUDCLNT_S_BUFFER_EMPTY );
    }

    *flags = 0;
    *frames = stream->period_frames;

    if (stream->lcl_offs_frames + *frames > stream->bufsize_frames)
    {
        UINT32 chunk_bytes, offs_bytes, frames_bytes;

        if (stream->tmp_buffer_frames < *frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
                stream->tmp_buffer = NULL;
            }
            size = *frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                         &size, MEM_COMMIT, PAGE_READWRITE ))
            {
                stream->tmp_buffer_frames = 0;
                return stream_unlock_result( stream, &params->result, E_OUTOFMEMORY );
            }
            stream->tmp_buffer_frames = *frames;
        }

        *data = stream->tmp_buffer;
        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;
        offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        frames_bytes = *frames * stream->fmt->nBlockAlign;
        memcpy( stream->tmp_buffer, stream->local_buffer + offs_bytes, chunk_bytes );
        memcpy( stream->tmp_buffer + chunk_bytes, stream->local_buffer, frames_bytes - chunk_bytes );
    }
    else *data = stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign;

    stream->getbuf_last = *frames;

    if (devpos) *devpos = stream->written_frames;
    if (qpcpos)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter( &stamp, &freq );
        *qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return stream_unlock_result( stream, &params->result, *frames ? S_OK : AUDCLNT_S_BUFFER_EMPTY );
}

static NTSTATUS ios_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT32 done = params->done;

    stream_lock( stream );

    if (!done)
    {
        stream->getbuf_last = 0;
        return stream_unlock_result( stream, &params->result, S_OK );
    }

    if (!stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (stream->getbuf_last != done)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_INVALID_SIZE );

    stream->written_frames += done;
    stream->held_frames -= done;
    stream->lcl_offs_frames += done;
    stream->lcl_offs_frames %= stream->bufsize_frames;
    stream->getbuf_last = 0;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;
    AudioStreamBasicDescription desc;

    params->result = fill_stream_description( params->fmt_in, &desc );
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_mix_format( void *args )
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = E_UNEXPECTED;
        return STATUS_SUCCESS;
    }

    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.wBitsPerSample = 32;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fmt->Format.nChannels = params->flow == eRender ? 2 : 1;
    fmt->Format.nSamplesPerSec = 48000;
    fmt->dwChannelMask = get_channel_mask( fmt->Format.nChannels );
    fmt->Format.nBlockAlign = fmt->Format.wBitsPerSample * fmt->Format.nChannels / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = def_period;
    if (params->min_period) *params->min_period = min_period;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->frames = stream->bufsize_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->latency = stream->period + 6666;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->padding = stream->held_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->frames = stream->held_frames < stream->period_frames ? 0 : stream->period_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_get_position( void *args )
{
    struct get_position_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT64 *pos = params->pos, *qpctime = params->qpctime;

    if (params->device)
    {
        FIXME( "Device position reporting not implemented\n" );
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    stream_lock( stream );

    if (stream->flow == eRender)
    {
        *pos = stream->written_frames - stream->held_frames;
        if (*pos < stream->last_pos_frames) *pos = stream->last_pos_frames;
    }
    else *pos = stream->written_frames + stream->held_frames;

    stream->last_pos_frames = *pos;

    if (stream->share == AUDCLNT_SHAREMODE_SHARED) *pos *= stream->fmt->nBlockAlign;

    if (qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter( &stamp, &freq );
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_set_volumes( void *args )
{
    struct set_volumes_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );
    UINT16 i;

    if (params->master_volume)
    {
        for (i = 0; i < stream->fmt->nChannels; ++i)
        {
            if (params->master_volume * params->volumes[i] * params->session_volumes[i] != 1.0f)
            {
                FIXME( "Volume control is not implemented\n" );
                break;
            }
        }
    }

    stream_lock( stream );
    stream->mute = !params->master_volume;
    stream_unlock( stream );

    return STATUS_SUCCESS;
}

static NTSTATUS ios_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED );

    if (stream->event)
    {
        FIXME( "called twice\n" );
        return stream_unlock_result( stream, &params->result, HRESULT_FROM_WIN32( ERROR_INVALID_NAME ) );
    }

    stream->event = params->event;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ios_is_started( void *args )
{
    struct is_started_params *params = args;
    struct ios_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    return stream_unlock_result( stream, &params->result, stream->playing ? S_OK : S_FALSE );
}

static NTSTATUS ios_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

/* There is no MIDI here: Android has no synthesizer to hand a stream of MIDI
 * events to. These have to answer properly all the same - mmdevapi defaults
 * the error out to success, and starts a notification thread and calls back
 * through an uninitialised context if it is left that way. */

static NTSTATUS ios_midi_init( void *args )
{
    struct midi_init_params *params = args;

    *params->err = MMSYSERR_NOTSUPPORTED;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_midi_out_message( void *args )
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;
    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MODM_GETNUMDEVS:
        *params->err = 0;  /* the device count is the return value */
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ios_midi_in_message( void *args )
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;
    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MIDM_GETNUMDEVS:
        *params->err = 0;
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ios_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    *params->quit = TRUE;
    params->notify->send_notify = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_aux_message( void *args )
{
    struct aux_message_params *params = args;

    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case AUXDM_GETNUMDEVS:
        *params->err = 0;
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ios_not_implemented,
    ios_not_implemented,
    ios_not_implemented,
    ios_not_implemented,
    ios_get_endpoint_ids,
    ios_create_stream,
    ios_release_stream,
    ios_start,
    ios_stop,
    ios_reset,
    ios_get_render_buffer,
    ios_release_render_buffer,
    ios_get_capture_buffer,
    ios_release_capture_buffer,
    ios_is_format_supported,
    ios_not_implemented,
    ios_get_mix_format,
    ios_get_device_period,
    ios_get_buffer_size,
    ios_get_latency,
    ios_get_current_padding,
    ios_get_next_packet_size,
    ios_get_frequency,
    ios_get_position,
    ios_set_volumes,
    ios_set_event_handle,
    ios_not_implemented,
    ios_test_connect,
    ios_is_started,
    ios_get_prop_value,
    ios_not_implemented,  /* midi_get_driver: this driver handles it */
    ios_midi_init,
    ios_not_implemented,  /* midi_release */
    ios_midi_out_message,
    ios_midi_in_message,
    ios_midi_notify_wait,
    ios_aux_message,
    ios_init,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* 32-bit (WoW64) callers reach the unix lib through this table. ntdll faults if a
 * wow64-capable module lacks it, which blocked the *graphics* driver from loading in
 * 32-bit processes: wineios.drv is both the audio and the display driver, and loading
 * it as the display driver still runs DllMain -> WINE_UNIX_CALL(unix_init). ios_init
 * (unix_init) takes no pointer arguments, so it is called directly. The audio entries
 * take 32-bit-vs-64-bit-divergent structs; until their wow64 thunks are ported from
 * winecoreaudio.drv they return STATUS_NOT_IMPLEMENTED, so a 32-bit app's audio init
 * fails gracefully instead of faulting (it was already unavailable here). */

typedef UINT PTR32;

/* wineios.so is a *native* unix lib, so ULongToPtr here is a bare zero-extend.
 * But the 32-bit guest is relocated into [guest_base, guest_base+4GB): a 32-bit
 * guest pointer P lives at guest_base + P on the host. Rebase the pointer fields
 * these wow64 thunks marshal (mmdevapi hands them over as raw 32-bit addresses),
 * exactly as the wow64/wow64win PE thunks do. */
static ULONG_PTR ios_guest_base_value(void)
{
    static ULONG_PTR gb;
    static int done;
    if (!done)
    {
        const char *e = getenv( "WINE_WOW64_GUEST_BASE" );
        if (e) gb = strtoull( e, NULL, 0 );
        done = 1;
    }
    return gb;
}
#undef ULongToPtr
#define ULongToPtr(u) ((void *)((UINT)(u) ? ios_guest_base_value() + (ULONG_PTR)(UINT)(u) : 0))

static NTSTATUS ios_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    ios_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    ios_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    ios_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    ios_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    ios_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS ios_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    ios_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    ios_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    ios_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ios_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    ios_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    ios_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ios_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    ios_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    ios_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return ios_set_volumes(&params);
}

static NTSTATUS ios_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    ios_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    ios_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    ios_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}



static NTSTATUS ios_wow64_process_attach( void *args )
{
    SYSTEM_BASIC_INFORMATION info;

    /* Constrain the audio buffer allocations (NtAllocateVirtualMemory with
     * zero_bits) to the 32-bit guest's address space so they land inside the
     * guest window and stay reachable from the relocated 32-bit guest. */
    NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL );
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_midi_init( void *args )
{
    struct { PTR32 err; } *p32 = args;
    UINT err = 0;
    struct midi_init_params params = { .err = &err };
    ios_midi_init( &params );
    if (p32->err) *(UINT *)ULongToPtr( p32->err ) = err;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_midi_out_message( void *args )
{
    struct { UINT dev_id; UINT msg; PTR32 user; PTR32 param_1; PTR32 param_2; PTR32 err; PTR32 notify; } *p32 = args;
    UINT err = 0;
    struct notify_context notify = {0};
    struct midi_out_message_params params =
    {
        .dev_id = p32->dev_id, .msg = p32->msg,
        .user = (UINT_PTR)p32->user, .param_1 = (UINT_PTR)p32->param_1, .param_2 = (UINT_PTR)p32->param_2,
        .err = &err, .notify = &notify,
    };
    ios_midi_out_message( &params );
    if (p32->err) *(UINT *)ULongToPtr( p32->err ) = err;
    if (p32->notify) *(BOOL *)ULongToPtr( p32->notify ) = notify.send_notify;   /* always FALSE here */
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_midi_in_message( void *args )
{
    struct { UINT dev_id; UINT msg; PTR32 user; PTR32 param_1; PTR32 param_2; PTR32 err; PTR32 notify; } *p32 = args;
    UINT err = 0;
    struct notify_context notify = {0};
    struct midi_in_message_params params =
    {
        .dev_id = p32->dev_id, .msg = p32->msg,
        .user = (UINT_PTR)p32->user, .param_1 = (UINT_PTR)p32->param_1, .param_2 = (UINT_PTR)p32->param_2,
        .err = &err, .notify = &notify,
    };
    ios_midi_in_message( &params );
    if (p32->err) *(UINT *)ULongToPtr( p32->err ) = err;
    if (p32->notify) *(BOOL *)ULongToPtr( p32->notify ) = notify.send_notify;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_midi_notify_wait( void *args )
{
    struct { PTR32 quit; PTR32 notify; } *p32 = args;
    BOOL quit = FALSE;
    struct notify_context notify = {0};
    struct midi_notify_wait_params params = { .quit = &quit, .notify = &notify };
    ios_midi_notify_wait( &params );
    if (p32->quit) *(BOOL *)ULongToPtr( p32->quit ) = quit;   /* ios sets TRUE -> notify thread exits */
    if (p32->notify) *(BOOL *)ULongToPtr( p32->notify ) = notify.send_notify;
    return STATUS_SUCCESS;
}

static NTSTATUS ios_wow64_aux_message( void *args )
{
    struct { UINT dev_id; UINT msg; PTR32 user; PTR32 param_1; PTR32 param_2; PTR32 err; } *p32 = args;
    UINT err = 0;
    struct aux_message_params params =
    {
        .dev_id = p32->dev_id, .msg = p32->msg,
        .user = (UINT_PTR)p32->user, .param_1 = (UINT_PTR)p32->param_1, .param_2 = (UINT_PTR)p32->param_2,
        .err = &err,
    };
    ios_aux_message( &params );
    if (p32->err) *(UINT *)ULongToPtr( p32->err ) = err;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    ios_wow64_process_attach,  /* process_attach: sets zero_bits so buffers land in the guest window */
    ios_not_implemented,  /* process_detach */
    ios_not_implemented,  /* main_loop */
    ios_not_implemented,  /* get_device_name_from_guid */
    ios_wow64_get_endpoint_ids,  /* get_endpoint_ids: report 0 devices, don't leave it uninit */
    ios_wow64_create_stream,  /* create_stream */
    ios_wow64_release_stream,  /* release_stream */
    ios_start,  /* start */
    ios_stop,  /* stop */
    ios_reset,  /* reset */
    ios_wow64_get_render_buffer,  /* get_render_buffer */
    ios_release_render_buffer,  /* release_render_buffer */
    ios_wow64_get_capture_buffer,  /* get_capture_buffer */
    ios_release_capture_buffer,  /* release_capture_buffer */
    ios_wow64_is_format_supported,  /* is_format_supported */
    ios_not_implemented,  /* get_loopback_capture_device */
    ios_wow64_get_mix_format,  /* get_mix_format */
    ios_wow64_get_device_period,  /* get_device_period */
    ios_wow64_get_buffer_size,  /* get_buffer_size */
    ios_wow64_get_latency,  /* get_latency */
    ios_wow64_get_current_padding,  /* get_current_padding */
    ios_wow64_get_next_packet_size,  /* get_next_packet_size */
    ios_wow64_get_frequency,  /* get_frequency */
    ios_wow64_get_position,  /* get_position */
    ios_wow64_set_volumes,  /* set_volumes */
    ios_wow64_set_event_handle,  /* set_event_handle */
    ios_not_implemented,  /* set_sample_rate */
    ios_wow64_test_connect,  /* test_connect */
    ios_is_started,  /* is_started */
    ios_wow64_get_prop_value,  /* get_prop_value */
    ios_not_implemented,  /* midi_get_driver */
    ios_wow64_midi_init,  /* midi_init */
    ios_not_implemented,  /* midi_release */
    ios_wow64_midi_out_message,  /* midi_out_message */
    ios_wow64_midi_in_message,  /* midi_in_message */
    ios_wow64_midi_notify_wait,  /* midi_notify_wait */
    ios_wow64_aux_message,  /* aux_message */
    ios_init,             /* unix_init: display-driver registration, no pointer args */
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */

