/*
 * Android audio driver, on top of AAudio
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
#include <dlfcn.h>
#include <pthread.h>

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

WINE_DEFAULT_DEBUG_CHANNEL(androidaudio);

/* AAudio, as declared by <aaudio/AAudio.h>. The driver dlopens libaaudio.so
 * rather than linking against it, the same way it loads the rest of Android. */

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_stream_state_t;

#define AAUDIO_OK                        0
#define AAUDIO_UNSPECIFIED               0

#define AAUDIO_FORMAT_PCM_I16            1
#define AAUDIO_FORMAT_PCM_FLOAT          2

#define AAUDIO_DIRECTION_OUTPUT          0
#define AAUDIO_DIRECTION_INPUT           1

#define AAUDIO_SHARING_MODE_EXCLUSIVE    0
#define AAUDIO_SHARING_MODE_SHARED       1

#define AAUDIO_PERFORMANCE_MODE_NONE     10

#define AAUDIO_STREAM_STATE_STARTED      4
#define AAUDIO_STREAM_STATE_PAUSED       6
#define AAUDIO_STREAM_STATE_FLUSHED      8
#define AAUDIO_STREAM_STATE_STOPPED      10
#define AAUDIO_STREAM_STATE_DISCONNECTED 13

typedef aaudio_result_t (*pfn_AAudio_createStreamBuilder)( AAudioStreamBuilder ** );
typedef const char *(*pfn_AAudio_convertResultToText)( aaudio_result_t );
typedef void (*pfn_AAudioStreamBuilder_setInt)( AAudioStreamBuilder *, int32_t );
typedef aaudio_result_t (*pfn_AAudioStreamBuilder_openStream)( AAudioStreamBuilder *, AAudioStream ** );
typedef aaudio_result_t (*pfn_AAudioStreamBuilder_delete)( AAudioStreamBuilder * );
typedef aaudio_result_t (*pfn_AAudioStream_request)( AAudioStream * );
typedef aaudio_result_t (*pfn_AAudioStream_write)( AAudioStream *, const void *, int32_t, int64_t );
typedef aaudio_result_t (*pfn_AAudioStream_read)( AAudioStream *, void *, int32_t, int64_t );
typedef int32_t (*pfn_AAudioStream_getInt)( AAudioStream * );
typedef int64_t (*pfn_AAudioStream_getFrames)( AAudioStream * );

static pfn_AAudio_createStreamBuilder p_AAudio_createStreamBuilder;
static pfn_AAudio_convertResultToText p_AAudio_convertResultToText;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setSampleRate;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setChannelCount;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setFormat;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setDirection;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setSharingMode;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setPerformanceMode;
static pfn_AAudioStreamBuilder_setInt p_AAudioStreamBuilder_setBufferCapacityInFrames;
static pfn_AAudioStreamBuilder_openStream p_AAudioStreamBuilder_openStream;
static pfn_AAudioStreamBuilder_delete p_AAudioStreamBuilder_delete;
static pfn_AAudioStream_request p_AAudioStream_close;
static pfn_AAudioStream_request p_AAudioStream_requestStart;
static pfn_AAudioStream_request p_AAudioStream_requestPause;
static pfn_AAudioStream_request p_AAudioStream_requestStop;
static pfn_AAudioStream_request p_AAudioStream_requestFlush;
static pfn_AAudioStream_write p_AAudioStream_write;
static pfn_AAudioStream_read p_AAudioStream_read;
static pfn_AAudioStream_getInt p_AAudioStream_getSampleRate;
static pfn_AAudioStream_getInt p_AAudioStream_getChannelCount;
static pfn_AAudioStream_getInt p_AAudioStream_getBufferSizeInFrames;
static pfn_AAudioStream_getInt p_AAudioStream_getBufferCapacityInFrames;
static pfn_AAudioStream_getInt p_AAudioStream_getFramesPerBurst;
static pfn_AAudioStream_getFrames p_AAudioStream_getFramesWritten;
static pfn_AAudioStream_getFrames p_AAudioStream_getFramesRead;

struct android_stream
{
    WAVEFORMATEX *fmt;
    EDataFlow flow;
    UINT flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    HANDLE timer_thread;

    AAudioStream *aaudio;

    BOOL playing, mute, please_quit;
    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, held_frames, tmp_buffer_frames;
    UINT32 lcl_offs_frames;   /* offset into local_buffer where valid data starts */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last;        /* <0 when using tmp_buffer */

    pthread_mutex_t lock;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 30000;

/* AAudio hands the hardware whole bursts, so a period shorter than one burst
 * is never consumed and the stream stalls with the data still in it. The
 * device's own burst is the shortest period this driver can serve. */
static REFERENCE_TIME hw_period;

/* the one device this driver exposes, since AAudio picks the route itself */
static const char default_device[] = "aaudio";

static ULONG_PTR zero_bits = 0;

static void *libaaudio;

static NTSTATUS android_not_implemented( void *args )
{
    return STATUS_SUCCESS;
}

static pthread_once_t aaudio_once = PTHREAD_ONCE_INIT;

static void load_aaudio_once(void)
{
    if (!(libaaudio = dlopen( "libaaudio.so", RTLD_NOW )))
    {
        WARN( "failed to load libaaudio.so: %s\n", dlerror() );
        return;
    }

#define LOAD_FUNCPTR( f )                                               \
    if (!(p_##f = (typeof(p_##f))dlsym( libaaudio, #f )))               \
    {                                                                   \
        ERR( "failed to find %s in libaaudio.so\n", #f );               \
        dlclose( libaaudio );                                           \
        libaaudio = NULL;                                               \
        return;                                                         \
    }
    LOAD_FUNCPTR( AAudio_createStreamBuilder );
    LOAD_FUNCPTR( AAudio_convertResultToText );
    LOAD_FUNCPTR( AAudioStreamBuilder_setSampleRate );
    LOAD_FUNCPTR( AAudioStreamBuilder_setChannelCount );
    LOAD_FUNCPTR( AAudioStreamBuilder_setFormat );
    LOAD_FUNCPTR( AAudioStreamBuilder_setDirection );
    LOAD_FUNCPTR( AAudioStreamBuilder_setSharingMode );
    LOAD_FUNCPTR( AAudioStreamBuilder_setPerformanceMode );
    LOAD_FUNCPTR( AAudioStreamBuilder_setBufferCapacityInFrames );
    LOAD_FUNCPTR( AAudioStreamBuilder_openStream );
    LOAD_FUNCPTR( AAudioStreamBuilder_delete );
    LOAD_FUNCPTR( AAudioStream_close );
    LOAD_FUNCPTR( AAudioStream_requestStart );
    LOAD_FUNCPTR( AAudioStream_requestPause );
    LOAD_FUNCPTR( AAudioStream_requestStop );
    LOAD_FUNCPTR( AAudioStream_requestFlush );
    LOAD_FUNCPTR( AAudioStream_write );
    LOAD_FUNCPTR( AAudioStream_read );
    LOAD_FUNCPTR( AAudioStream_getSampleRate );
    LOAD_FUNCPTR( AAudioStream_getChannelCount );
    LOAD_FUNCPTR( AAudioStream_getBufferSizeInFrames );
    LOAD_FUNCPTR( AAudioStream_getBufferCapacityInFrames );
    LOAD_FUNCPTR( AAudioStream_getFramesPerBurst );
    LOAD_FUNCPTR( AAudioStream_getFramesWritten );
    LOAD_FUNCPTR( AAudioStream_getFramesRead );
#undef LOAD_FUNCPTR

    TRACE( "loaded libaaudio.so\n" );
}

static void query_hw_period(void)
{
    AAudioStreamBuilder *builder;
    AAudioStream *stream;
    int32_t burst, rate;

    if (p_AAudio_createStreamBuilder( &builder ) != AAUDIO_OK) return;
    p_AAudioStreamBuilder_setDirection( builder, AAUDIO_DIRECTION_OUTPUT );
    p_AAudioStreamBuilder_setPerformanceMode( builder, AAUDIO_PERFORMANCE_MODE_NONE );
    if (p_AAudioStreamBuilder_openStream( builder, &stream ) == AAUDIO_OK)
    {
        burst = p_AAudioStream_getFramesPerBurst( stream );
        rate = p_AAudioStream_getSampleRate( stream );
        if (burst > 0 && rate > 0) hw_period = (REFERENCE_TIME)burst * 10000000 / rate;
        TRACE( "the device plays %d frame bursts at %d Hz, %lld00ns per burst\n",
               burst, rate, (long long)(hw_period / 100) );
        p_AAudioStream_close( stream );
    }
    p_AAudioStreamBuilder_delete( builder );
}

static BOOL load_aaudio(void)
{
    pthread_once( &aaudio_once, load_aaudio_once );
    if (libaaudio && !hw_period) query_hw_period();
    return libaaudio != NULL;
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

static void stream_lock( struct android_stream *stream )
{
    pthread_mutex_lock( &stream->lock );
}

static void stream_unlock( struct android_stream *stream )
{
    pthread_mutex_unlock( &stream->lock );
}

static NTSTATUS stream_unlock_result( struct android_stream *stream, HRESULT *result, HRESULT value )
{
    *result = value;
    stream_unlock( stream );
    return STATUS_SUCCESS;
}

static struct android_stream *handle_get_stream( stream_handle h )
{
    return (struct android_stream *)(UINT_PTR)h;
}

static NTSTATUS android_test_connect( void *args )
{
    struct test_connect_params *params = args;

    params->priority = load_aaudio() ? Priority_Preferred : Priority_Unavailable;
    return STATUS_SUCCESS;
}

static NTSTATUS android_get_endpoint_ids( void *args )
{
    static const WCHAR outW[] = { 'A','n','d','r','o','i','d',' ','O','u','t','p','u','t',0 };
    static const WCHAR inW[] = { 'A','n','d','r','o','i','d',' ','I','n','p','u','t',0 };
    struct get_endpoint_ids_params *params = args;
    unsigned int name_len, device_len, needed, offset;
    const WCHAR *name;

    if (!load_aaudio())
    {
        params->result = AUDCLNT_E_SERVICE_NOT_RUNNING;
        return STATUS_SUCCESS;
    }

    /* AAudio routes to whichever device Android has selected, and offers no
     * enumeration of its own, so present that as the one endpoint. */
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
    case 3: return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 5: return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 7: return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    FIXME( "Unknown speaker configuration: %u\n", channels );
    return 0;
}

/* AAudio takes 16 bit integer or 32 bit float samples, and nothing else */
static aaudio_format_t get_aaudio_format( const WAVEFORMATEX *fmt )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->wBitsPerSample != fmtex->Samples.wValidBitsPerSample)
        return AAUDIO_UNSPECIFIED;

    if (fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM )))
    {
        if (fmt->wBitsPerSample == 16) return AAUDIO_FORMAT_PCM_I16;
        return AAUDIO_UNSPECIFIED;
    }

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT )))
    {
        if (fmt->wBitsPerSample == 32) return AAUDIO_FORMAT_PCM_FLOAT;
        return AAUDIO_UNSPECIFIED;
    }

    return AAUDIO_UNSPECIFIED;
}

static HRESULT check_format( const WAVEFORMATEX *fmt )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if (!get_aaudio_format( fmt )) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        (fmtex->Format.nAvgBytesPerSec == 0 || fmtex->Format.nBlockAlign == 0 ||
         fmtex->Samples.wValidBitsPerSample > fmtex->Format.wBitsPerSample))
        return E_INVALIDARG;

    if (fmt->nChannels == 0 || fmt->nChannels > 8) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 192000) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (fmt->nBlockAlign != fmt->nChannels * fmt->wBitsPerSample / 8 ||
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

static HRESULT open_aaudio_stream( struct android_stream *stream, const WAVEFORMATEX *fmt,
                                   UINT32 buffer_frames )
{
    AAudioStreamBuilder *builder;
    aaudio_result_t res;
    int32_t rate;

    if ((res = p_AAudio_createStreamBuilder( &builder )) != AAUDIO_OK)
    {
        ERR( "failed to create a stream builder: %s\n", p_AAudio_convertResultToText( res ) );
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    p_AAudioStreamBuilder_setDirection( builder, stream->flow == eRender ? AAUDIO_DIRECTION_OUTPUT :
                                        AAUDIO_DIRECTION_INPUT );
    p_AAudioStreamBuilder_setSampleRate( builder, fmt->nSamplesPerSec );
    p_AAudioStreamBuilder_setChannelCount( builder, fmt->nChannels );
    p_AAudioStreamBuilder_setFormat( builder, get_aaudio_format( fmt ) );
    p_AAudioStreamBuilder_setSharingMode( builder, AAUDIO_SHARING_MODE_SHARED );
    /* the driver hands over whole periods from a timer, so the low latency
     * path - which wants to be fed from its own callback - buys nothing */
    p_AAudioStreamBuilder_setPerformanceMode( builder, AAUDIO_PERFORMANCE_MODE_NONE );
    if (buffer_frames) p_AAudioStreamBuilder_setBufferCapacityInFrames( builder, buffer_frames );

    res = p_AAudioStreamBuilder_openStream( builder, &stream->aaudio );
    p_AAudioStreamBuilder_delete( builder );

    if (res != AAUDIO_OK)
    {
        WARN( "failed to open the stream: %s\n", p_AAudio_convertResultToText( res ) );
        stream->aaudio = NULL;
        return AUDCLNT_E_DEVICE_IN_USE;
    }

    /* AAudio is free to hand back a stream that does not match the request */
    rate = p_AAudioStream_getSampleRate( stream->aaudio );
    if (rate != fmt->nSamplesPerSec || p_AAudioStream_getChannelCount( stream->aaudio ) != fmt->nChannels)
    {
        WARN( "opened %d Hz %d channels for a %u Hz %u channel stream\n", rate,
              p_AAudioStream_getChannelCount( stream->aaudio ), (unsigned int)fmt->nSamplesPerSec,
              fmt->nChannels );
        p_AAudioStream_close( stream->aaudio );
        stream->aaudio = NULL;
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    TRACE( "opened %d Hz %d channels, %d frames per burst, capacity %d frames\n", rate,
           p_AAudioStream_getChannelCount( stream->aaudio ),
           p_AAudioStream_getFramesPerBurst( stream->aaudio ),
           p_AAudioStream_getBufferCapacityInFrames( stream->aaudio ) );
    return S_OK;
}

static NTSTATUS android_create_stream( void *args )
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct android_stream *stream;
    SIZE_T size;

    params->result = S_OK;

    if (!load_aaudio())
    {
        params->result = AUDCLNT_E_SERVICE_NOT_RUNNING;
        return STATUS_SUCCESS;
    }

    if (!(stream = calloc( 1, sizeof(*stream) )))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->flow = params->flow;
    pthread_mutex_init( &stream->lock, NULL );

    params->result = check_format( params->fmt );
    if (FAILED(params->result)) goto exit;

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

    params->result = open_aaudio_stream( stream, params->fmt, stream->bufsize_frames );
    if (FAILED(params->result)) goto exit;

    if (!(fmtex = clone_format( params->fmt )))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;

    size = stream->bufsize_frames * params->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                 &size, MEM_COMMIT, PAGE_READWRITE ))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;

exit:
    if (FAILED(params->result))
    {
        if (stream->aaudio) p_AAudioStream_close( stream->aaudio );
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

static NTSTATUS android_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
    SIZE_T size;

    if (stream->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject( stream->timer_thread, FALSE, NULL );
        NtClose( stream->timer_thread );
    }

    if (stream->aaudio)
    {
        p_AAudioStream_requestStop( stream->aaudio );
        p_AAudioStream_close( stream->aaudio );
    }
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

static void silence_buffer( struct android_stream *stream, BYTE *buffer, UINT32 frames )
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

static void android_write_data( struct android_stream *stream )
{
    UINT32 to_write, offs;
    aaudio_result_t written;
    BYTE *buf;

    if (!stream->held_frames) return;

    offs = stream->lcl_offs_frames;
    to_write = min( stream->held_frames, stream->bufsize_frames - offs );
    buf = stream->local_buffer + offs * stream->fmt->nBlockAlign;
    if (stream->mute) silence_buffer( stream, buf, to_write );

    /* AAudio paces the writer itself: the write returns as the device takes
     * the data, and takes ownership of what it accepts. Nothing else here can
     * pace the stream - the frame counters do not advance on this path. */
    written = p_AAudioStream_write( stream->aaudio, buf, to_write, stream->period * 100 );
    if (written < 0)
    {
        WARN( "write failed: %s\n", p_AAudio_convertResultToText( written ) );
        return;
    }

    stream->lcl_offs_frames = (offs + written) % stream->bufsize_frames;
    stream->held_frames -= written;
}

static void android_read_data( struct android_stream *stream )
{
    UINT32 pos, readable;
    aaudio_result_t nread;

    pos = (stream->held_frames + stream->lcl_offs_frames) % stream->bufsize_frames;
    readable = stream->bufsize_frames - pos;

    nread = p_AAudioStream_read( stream->aaudio, stream->local_buffer + pos * stream->fmt->nBlockAlign,
                                 readable, 0 );
    if (nread < 0)
    {
        WARN( "read failed: %s\n", p_AAudio_convertResultToText( nread ) );
        return;
    }

    stream->held_frames += nread;

    if (stream->held_frames > stream->bufsize_frames)
    {
        WARN( "Overflow of unread data\n" );
        stream->lcl_offs_frames += stream->held_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames = stream->bufsize_frames;
    }
}

static void android_timer_loop( void *args )
{
    struct android_stream *stream = args;
    LARGE_INTEGER delay, now, next;
    int adjust;

    stream_lock( stream );

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter( &now, NULL );
    next.QuadPart = now.QuadPart + stream->period;

    while (!stream->please_quit)
    {
        if (stream->playing)
        {
            if (stream->flow == eRender && stream->held_frames) android_write_data( stream );
            else if (stream->flow == eCapture) android_read_data( stream );
        }
        if (stream->event) NtSetEvent( stream->event, NULL );
        stream_unlock( stream );

        NtDelayExecution( FALSE, &delay );

        stream_lock( stream );
        NtQueryPerformanceCounter( &now, NULL );
        adjust = next.QuadPart - now.QuadPart;
        if (adjust > stream->period / 2) adjust = stream->period / 2;
        else if (adjust < -stream->period / 2) adjust = -stream->period / 2;
        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }

    stream_unlock( stream );
}

static NTSTATUS android_start( void *args )
{
    static const WCHAR name[] = { 'a','u','d','i','o','_','c','l','i','e','n','t','_','t','i','m','e','r',0 };
    struct start_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
    aaudio_result_t res;

    stream_lock( stream );

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET );

    if (stream->playing)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    if ((res = p_AAudioStream_requestStart( stream->aaudio )) != AAUDIO_OK)
    {
        WARN( "failed to start the stream: %s\n", p_AAudio_convertResultToText( res ) );
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_DEVICE_INVALIDATED );
    }

    stream->playing = TRUE;
    if (!stream->timer_thread)
    {
        NTSTATUS status = create_unix_thread( &stream->timer_thread, name, android_timer_loop, stream );
        if (status) ERR( "failed to create the timer thread: %#x\n", (unsigned int)status );
    }
    TRACE( "started, buffer size %d of %d frames\n",
           p_AAudioStream_getBufferSizeInFrames( stream->aaudio ),
           p_AAudioStream_getBufferCapacityInFrames( stream->aaudio ) );

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_stop( void *args )
{
    struct stop_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (!stream->playing)
        return stream_unlock_result( stream, &params->result, S_FALSE );

    p_AAudioStream_requestPause( stream->aaudio );
    stream->playing = FALSE;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_reset( void *args )
{
    struct reset_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (stream->playing)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    if (stream->getbuf_last)
        return stream_unlock_result( stream, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING );

    /* a paused stream still holds what was written to it */
    p_AAudioStream_requestFlush( stream->aaudio );

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

static NTSTATUS android_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static void wrap_buffer( struct android_stream *stream, BYTE *buffer, UINT32 written_frames )
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

static NTSTATUS android_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static NTSTATUS android_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static NTSTATUS android_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static NTSTATUS android_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;

    params->result = check_format( params->fmt_in );
    return STATUS_SUCCESS;
}

static NTSTATUS android_get_mix_format( void *args )
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

static NTSTATUS android_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    load_aaudio();
    if (params->def_period) *params->def_period = max( hw_period, def_period );
    if (params->min_period) *params->min_period = hw_period ? hw_period : min_period;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS android_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->frames = stream->bufsize_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->latency = stream->period + 6666;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->padding = stream->held_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    *params->frames = stream->held_frames < stream->period_frames ? 0 : stream->period_frames;
    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    return stream_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS android_get_position( void *args )
{
    struct get_position_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static NTSTATUS android_set_volumes( void *args )
{
    struct set_volumes_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );
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

static NTSTATUS android_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

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

static NTSTATUS android_is_started( void *args )
{
    struct is_started_params *params = args;
    struct android_stream *stream = handle_get_stream( params->stream );

    stream_lock( stream );
    return stream_unlock_result( stream, &params->result, stream->playing ? S_OK : S_FALSE );
}

static NTSTATUS android_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

/* There is no MIDI here: Android has no synthesizer to hand a stream of MIDI
 * events to. These have to answer properly all the same - mmdevapi defaults
 * the error out to success, and starts a notification thread and calls back
 * through an uninitialised context if it is left that way. */

static NTSTATUS android_midi_init( void *args )
{
    struct midi_init_params *params = args;

    *params->err = MMSYSERR_NOTSUPPORTED;
    return STATUS_SUCCESS;
}

static NTSTATUS android_midi_out_message( void *args )
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

static NTSTATUS android_midi_in_message( void *args )
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

static NTSTATUS android_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    *params->quit = TRUE;
    params->notify->send_notify = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS android_aux_message( void *args )
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
    android_not_implemented,
    android_not_implemented,
    android_not_implemented,
    android_not_implemented,
    android_get_endpoint_ids,
    android_create_stream,
    android_release_stream,
    android_start,
    android_stop,
    android_reset,
    android_get_render_buffer,
    android_release_render_buffer,
    android_get_capture_buffer,
    android_release_capture_buffer,
    android_is_format_supported,
    android_not_implemented,
    android_get_mix_format,
    android_get_device_period,
    android_get_buffer_size,
    android_get_latency,
    android_get_current_padding,
    android_get_next_packet_size,
    android_get_frequency,
    android_get_position,
    android_set_volumes,
    android_set_event_handle,
    android_not_implemented,
    android_test_connect,
    android_is_started,
    android_get_prop_value,
    android_not_implemented,  /* midi_get_driver: this driver handles it */
    android_midi_init,
    android_not_implemented,  /* midi_release */
    android_midi_out_message,
    android_midi_in_message,
    android_midi_notify_wait,
    android_aux_message,
    android_init,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS android_wow64_process_attach( void *args )
{
    SYSTEM_BASIC_INFORMATION info;

    NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL );
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_test_connect( void *args )
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr( params32->name ),
    };

    android_test_connect( &params );
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_endpoint_ids( void *args )
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
        .endpoints = ULongToPtr( params32->endpoints ),
        .size = params32->size
    };

    android_get_endpoint_ids( &params );
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_create_stream( void *args )
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        UINT flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr( params32->name ),
        .device = ULongToPtr( params32->device ),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr( params32->fmt ),
        .channel_count = ULongToPtr( params32->channel_count ),
        .stream = ULongToPtr( params32->stream )
    };

    android_create_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_release_stream( void *args )
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

    android_release_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_render_buffer( void *args )
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

    android_get_render_buffer( &params );
    params32->result = params.result;
    *(unsigned int *)ULongToPtr( params32->data ) = PtrToUlong( data );
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_capture_buffer( void *args )
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
        .frames = ULongToPtr( params32->frames ),
        .flags = ULongToPtr( params32->flags ),
        .devpos = ULongToPtr( params32->devpos ),
        .qpcpos = ULongToPtr( params32->qpcpos )
    };

    android_get_capture_buffer( &params );
    params32->result = params.result;
    *(unsigned int *)ULongToPtr( params32->data ) = PtrToUlong( data );
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_is_format_supported( void *args )
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
        .device = ULongToPtr( params32->device ),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr( params32->fmt_in ),
    };

    android_is_format_supported( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_mix_format( void *args )
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
        .device = ULongToPtr( params32->device ),
        .flow = params32->flow,
        .fmt = ULongToPtr( params32->fmt )
    };

    android_get_mix_format( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_device_period( void *args )
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
        .device = ULongToPtr( params32->device ),
        .flow = params32->flow,
        .def_period = ULongToPtr( params32->def_period ),
        .min_period = ULongToPtr( params32->min_period ),
    };

    android_get_device_period( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_buffer_size( void *args )
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
        .frames = ULongToPtr( params32->frames )
    };

    android_get_buffer_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_latency( void *args )
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
        .latency = ULongToPtr( params32->latency )
    };

    android_get_latency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_current_padding( void *args )
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
        .padding = ULongToPtr( params32->padding )
    };

    android_get_current_padding( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_next_packet_size( void *args )
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
        .frames = ULongToPtr( params32->frames )
    };

    android_get_next_packet_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_frequency( void *args )
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
        .freq = ULongToPtr( params32->freq )
    };

    android_get_frequency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_position( void *args )
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
        .pos = ULongToPtr( params32->pos ),
        .qpctime = ULongToPtr( params32->qpctime )
    };

    android_get_position( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_set_volumes( void *args )
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
        .volumes = ULongToPtr( params32->volumes ),
        .session_volumes = ULongToPtr( params32->session_volumes ),
    };

    return android_set_volumes( &params );
}

static NTSTATUS android_wow64_set_event_handle( void *args )
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
        .event = ULongToHandle( params32->event )
    };

    android_set_event_handle( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_get_prop_value( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer;
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr( params32->device ),
        .flow = params32->flow,
        .guid = ULongToPtr( params32->guid ),
        .prop = ULongToPtr( params32->prop ),
        .value = &value,
        .buffer = ULongToPtr( params32->buffer ),
        .buffer_size = ULongToPtr( params32->buffer_size )
    };

    android_get_prop_value( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_midi_init( void *args )
{
    struct
    {
        PTR32 err;
    } *params32 = args;
    struct midi_init_params params =
    {
        .err = ULongToPtr( params32->err )
    };

    return android_midi_init( &params );
}

struct notify_context32
{
    BOOL send_notify;
    WORD dev_id;
    WORD msg;
    UINT param_1;
    UINT param_2;
    UINT callback;
    UINT flags;
    PTR32 device;
    UINT instance;
};

static void notify_to_notify32( struct notify_context32 *notify32, const struct notify_context *notify )
{
    notify32->send_notify = notify->send_notify;
    notify32->dev_id = notify->dev_id;
    notify32->msg = notify->msg;
    notify32->param_1 = notify->param_1;
    notify32->param_2 = notify->param_2;
    notify32->callback = notify->callback;
    notify32->flags = notify->flags;
    notify32->device = PtrToUlong( notify->device );
    notify32->instance = notify->instance;
}

static NTSTATUS android_wow64_midi_out_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;
    struct notify_context notify;
    struct midi_out_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr( params32->err ),
        .notify = &notify
    };

    android_midi_out_message( &params );
    notify_to_notify32( ULongToPtr( params32->notify ), &notify );
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_midi_in_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;
    struct notify_context notify;
    struct midi_in_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr( params32->err ),
        .notify = &notify
    };

    android_midi_in_message( &params );
    notify_to_notify32( ULongToPtr( params32->notify ), &notify );
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_midi_notify_wait( void *args )
{
    struct
    {
        PTR32 quit;
        PTR32 notify;
    } *params32 = args;
    struct notify_context notify;
    struct midi_notify_wait_params params =
    {
        .quit = ULongToPtr( params32->quit ),
        .notify = &notify
    };

    android_midi_notify_wait( &params );
    notify_to_notify32( ULongToPtr( params32->notify ), &notify );
    return STATUS_SUCCESS;
}

static NTSTATUS android_wow64_aux_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
    } *params32 = args;
    struct aux_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr( params32->err ),
    };

    return android_aux_message( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    android_wow64_process_attach,
    android_not_implemented,
    android_not_implemented,
    android_not_implemented,
    android_wow64_get_endpoint_ids,
    android_wow64_create_stream,
    android_wow64_release_stream,
    android_start,
    android_stop,
    android_reset,
    android_wow64_get_render_buffer,
    android_release_render_buffer,
    android_wow64_get_capture_buffer,
    android_release_capture_buffer,
    android_wow64_is_format_supported,
    android_not_implemented,
    android_wow64_get_mix_format,
    android_wow64_get_device_period,
    android_wow64_get_buffer_size,
    android_wow64_get_latency,
    android_wow64_get_current_padding,
    android_wow64_get_next_packet_size,
    android_wow64_get_frequency,
    android_wow64_get_position,
    android_wow64_set_volumes,
    android_wow64_set_event_handle,
    android_not_implemented,
    android_wow64_test_connect,
    android_is_started,
    android_wow64_get_prop_value,
    android_not_implemented,  /* midi_get_driver: this driver handles it */
    android_wow64_midi_init,
    android_not_implemented,  /* midi_release */
    android_wow64_midi_out_message,
    android_wow64_midi_in_message,
    android_wow64_midi_notify_wait,
    android_wow64_aux_message,
    android_init,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
