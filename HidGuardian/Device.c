/*
MIT License

Copyright (c) 2016 Benjamin "Nefarius" H�glinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "driver.h"
#include "device.tmh"
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, HidGuardianCreateDevice)
#pragma alloc_text (PAGE, AmIAffected)
#endif


NTSTATUS
HidGuardianCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PDEVICE_CONTEXT         deviceContext;
    WDFDEVICE               device;
    NTSTATUS                status;
    WDF_FILEOBJECT_CONFIG   deviceConfig;
    WDFMEMORY               memory;

    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_FILEOBJECT_CONFIG_INIT(&deviceConfig, EvtDeviceFileCreate, NULL, NULL);

    WdfDeviceInitSetFileObjectConfig(
        DeviceInit,
        &deviceConfig,
        &deviceAttributes
    );

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        deviceContext = DeviceGetContext(device);

        //
        // Create a device interface so that applications can find and talk
        // to us.
        //
        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_HIDGUARDIAN,
            NULL // ReferenceString
        );

        if (!NT_SUCCESS(status)) {
            KdPrint((DRIVERNAME "WdfDeviceCreateDeviceInterface failed with status 0x%X", status));
            return status;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
        deviceAttributes.ParentObject = device;

        //
        // Query for current device's Hardware ID
        // 
        status = WdfDeviceAllocAndQueryProperty(device,
            DevicePropertyHardwareID,
            NonPagedPool,
            &deviceAttributes,
            &memory
        );

        if (!NT_SUCCESS(status)) {
            KdPrint((DRIVERNAME "WdfDeviceAllocAndQueryProperty failed with status 0x%X", status));
            return status;
        }

        //
        // Get Hardware ID string
        // 
        deviceContext->HardwareIDMemory = memory;
        deviceContext->HardwareID = WdfMemoryGetBuffer(memory, NULL);

        //
        // Initialize the I/O Package and any Queues
        //
        status = HidGuardianQueueInitialize(device);

        if (!NT_SUCCESS(status)) {
            KdPrint((DRIVERNAME "HidGuardianQueueInitialize failed with status 0x%X", status));
            return status;
        }

        //
        // Check if this device should get intercepted
        // 
        status = AmIAffected(deviceContext);

        KdPrint(("AmIAffected status 0x%X\n", status));
    }

    return status;
}

//
// Catches CreateFile(...) calls.
// 
VOID EvtDeviceFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(FileObject);

    KdPrint(("CreateFile(...) blocked\n"));

    //
    // We are loaded within a targeted device, fail the request
    // 
    WdfRequestComplete(Request, STATUS_ACCESS_DENIED);    
}

//
// Checks if the current device should be intercepted or not.
// 
NTSTATUS AmIAffected(PDEVICE_CONTEXT DeviceContext)
{
    WDF_OBJECT_ATTRIBUTES   stringAttributes;
    WDFCOLLECTION           col;
    NTSTATUS                status;
    ULONG                   count;
    ULONG                   i;
    WDFKEY                  keyParams;
    BOOLEAN                 affected = FALSE;
    DECLARE_CONST_UNICODE_STRING(valueMultiSz, L"AffectedDevices");
    DECLARE_UNICODE_STRING_SIZE(currentHardwareID, MAX_HARDWARE_ID_SIZE);
    DECLARE_UNICODE_STRING_SIZE(myHardwareID, MAX_HARDWARE_ID_SIZE);

    PAGED_CODE();

    //
    // Convert wide into Unicode string
    // 
    status = RtlUnicodeStringInit(&myHardwareID, DeviceContext->HardwareID);
    if (!NT_SUCCESS(status)) {
        KdPrint(("RtlUnicodeStringInit failed: 0x%x\n", status));
        return status;
    }

    //
    // Create collection holding the Hardware IDs
    // 
    status = WdfCollectionCreate(
        NULL,
        &col
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfCollectionCreate failed: 0x%x\n", status));
        return status;
    }

    //
    // Get the filter drivers Parameter key
    // 
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), STANDARD_RIGHTS_ALL, WDF_NO_OBJECT_ATTRIBUTES, &keyParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDriverOpenParametersRegistryKey failed: 0x%x\n", status));
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&stringAttributes);
    stringAttributes.ParentObject = col;

    //
    // Get the multi-string value
    // 
    status = WdfRegistryQueryMultiString(
        keyParams,
        &valueMultiSz,
        &stringAttributes,
        col
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfRegistryQueryMultiString failed: 0x%x\n", status));
        return status;
    }

    count = WdfCollectionGetCount(col);

    // 
    // Loop through registry multi-string values
    // 
    for (i = 0; i < count; i++)
    {
        WdfStringGetUnicodeString(WdfCollectionGetItem(col, i), &currentHardwareID);

        KdPrint(("My ID %wZ vs current ID %wZ\n", &myHardwareID, &currentHardwareID));

        affected = RtlEqualUnicodeString(&myHardwareID, &currentHardwareID, TRUE);
        KdPrint(("Are we affected: %d\n", affected));

        if (affected) break;
    }

    WdfRegistryClose(keyParams);

    //
    // If Hardware ID wasn't found, report failure so the filter gets unloaded
    // 
    return (affected) ? STATUS_SUCCESS : STATUS_DEVICE_FEATURE_NOT_SUPPORTED;
}


