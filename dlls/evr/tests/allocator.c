/*
 * EVR sample allocator buffer tests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "mfapi.h"
#include "mfidl.h"
#include "evr.h"
#include "wine/test.h"

static void test_buffer_operations(void)
{
    IMFMediaBuffer *buffer, *contiguous, *copy;
    IMFVideoSampleAllocator *allocator;
    IMFMediaType *type;
    DWORD length, total_length;
    IMFSample *sample;
    HRESULT hr;
    BYTE *data;

    hr = MFCreateVideoSampleAllocator(&IID_IMFVideoSampleAllocator, (void **)&allocator);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    hr = MFCreateMediaType(&type);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, (UINT64)16 << 32 | 16);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFVideoSampleAllocator_InitializeSampleAllocator(allocator, 1, type);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    IMFMediaType_Release(type);
    if (FAILED(hr))
    {
        IMFVideoSampleAllocator_Release(allocator);
        return;
    }

    hr = IMFVideoSampleAllocator_AllocateSample(allocator, &sample);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    /* Software transforms request contiguous buffers from allocator samples. */
    hr = IMFSample_ConvertToContiguousBuffer(sample, &contiguous);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(contiguous == buffer, "Unexpected contiguous buffer.\n");
        IMFMediaBuffer_Release(contiguous);
    }

    hr = IMFMediaBuffer_Lock(buffer, &data, &length, NULL);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    ok(length == 16 * 16 * 4, "Unexpected buffer length %lu.\n", length);
    memset(data, 0x5a, length);
    hr = IMFMediaBuffer_Unlock(buffer);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFMediaBuffer_SetCurrentLength(buffer, length);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFSample_GetTotalLength(sample, &total_length);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    ok(total_length == length, "Unexpected total length %lu, expected %lu.\n", total_length, length);

    hr = MFCreateMemoryBuffer(length, &copy);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFSample_CopyToBuffer(sample, copy);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IMFMediaBuffer_Lock(copy, &data, NULL, &total_length);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        ok(total_length == length, "Unexpected copied length %lu.\n", total_length);
        ok(data[0] == 0x5a && data[length - 1] == 0x5a, "Unexpected copied data.\n");
        hr = IMFMediaBuffer_Unlock(copy);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    }
    IMFMediaBuffer_Release(copy);
    IMFMediaBuffer_Release(buffer);
    IMFSample_Release(sample);
    IMFVideoSampleAllocator_Release(allocator);
}

START_TEST(allocator)
{
    CoInitialize(NULL);
    test_buffer_operations();
    CoUninitialize();
}
