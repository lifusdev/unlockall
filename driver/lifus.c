#include "Driver.h"
#pragma warning(disable : 4996)

NTSTATUS read_write_memory(p_rw_request req) {
    PEPROCESS target_process = NULL;
    PEPROCESS client_process = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T bytes_copied = 0;
    if (!req || req->size == 0 || req->size > max_transfer_size)
        return STATUS_INVALID_PARAMETER;
    if (req->address == 0 || req->buffer == 0)
        return STATUS_INVALID_PARAMETER;
    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req->process_id, &target_process);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    client_process = PsGetCurrentProcess();
    if (req->write) {
        status = MmCopyVirtualMemory(client_process, (PVOID)(ULONG_PTR)req->buffer, target_process,
            (PVOID)(ULONG_PTR)req->address, req->size, KernelMode, &bytes_copied);
    } else {
        status = MmCopyVirtualMemory(target_process, (PVOID)(ULONG_PTR)req->address, client_process,
            (PVOID)(ULONG_PTR)req->buffer, req->size, KernelMode, &bytes_copied);
    }
    if (target_process) {
        ObDereferenceObject(target_process);
    }
    return status;
}

NTSTATUS handle_bulk(p_bulk_request bulk_req) {
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    if (!bulk_req || bulk_req->count == 0 || bulk_req->count > max_bulk_operations) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < bulk_req->count; i++) {
        bulk_req->operations[i].status = read_write_memory(&bulk_req->operations[i]);
        if (!NT_SUCCESS(bulk_req->operations[i].status) && NT_SUCCESS(status)) {
            status = bulk_req->operations[i].status;
        }
    }
    return status;
}

NTSTATUS handle_process_list(p_proc_list_request req, size_t buffer_length) {
    ULONG capacity;
    ULONG system_info_length;
    PVOID system_info;
    ULONG needed;
    NTSTATUS status;
    p_system_process_info current;
    ULONG returned;
    if (!req || req->count == 0 || req->count > max_process_list_capacity)
        return STATUS_INVALID_PARAMETER;
    capacity = req->count;
    if (sizeof(proc_list_request) + (capacity - 1) * sizeof(proc_entry) > buffer_length)
        return STATUS_BUFFER_TOO_SMALL;
    system_info_length = 256 * 1024;
    system_info = NULL;
    needed = 0;
    status = STATUS_INFO_LENGTH_MISMATCH;
    while (system_info_length <= 8 * 1024 * 1024) {
        system_info = ExAllocatePoolWithTag(NonPagedPool, system_info_length, 'nBlA');
        if (!system_info)
            return STATUS_INSUFFICIENT_RESOURCES;
        status = ZwQuerySystemInformation(SystemProcessInformation, system_info, system_info_length, &needed);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            ExFreePoolWithTag(system_info, 'nBlA');
            system_info = NULL;
            system_info_length = needed > system_info_length ? needed : system_info_length * 2;
            continue;
        }
        break;
    }
    if (!NT_SUCCESS(status)) {
        if (system_info)
            ExFreePoolWithTag(system_info, 'nBlA');
        return status;
    }
    returned = 0;
    current = (p_system_process_info)system_info;
    for (;;) {
        if (returned < capacity && current->image_name.Buffer && current->image_name.Length > 0) {
            p_proc_entry entry = &req->processes[returned];
            ULONG copy_length = current->image_name.Length;
            if (copy_length > (max_process_name_length - 1) * sizeof(WCHAR))
                copy_length = (max_process_name_length - 1) * sizeof(WCHAR);
            entry->process_id = (ULONG)(ULONG_PTR)current->unique_process_id;
            RtlCopyMemory(entry->name, current->image_name.Buffer, copy_length);
            entry->name[copy_length / sizeof(WCHAR)] = L'\0';
            returned++;
        }
        if (current->next_entry_offset == 0)
            break;
        current = (p_system_process_info)((PUCHAR)current + current->next_entry_offset);
    }
    ExFreePoolWithTag(system_info, 'nBlA');
    req->count = returned;
    return STATUS_SUCCESS;
}

NTSTATUS handle_region_query(p_region_request req, size_t buffer_length) {
    PEPROCESS target_process;
    HANDLE h_process;
    NTSTATUS status;
    ULONG64 address;
    ULONG returned;
    if (!req || req->count == 0 || req->count > max_region_list_capacity)
        return STATUS_INVALID_PARAMETER;
    if (sizeof(region_request) + (req->count - 1) * sizeof(region_info) > buffer_length)
        return STATUS_BUFFER_TOO_SMALL;
    target_process = NULL;
    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)req->process_id, &target_process);
    if (!NT_SUCCESS(status))
        return status;
    h_process = NULL;
    status = ObOpenObjectByPointer(target_process, OBJ_KERNEL_HANDLE, NULL, PROCESS_QUERY_INFORMATION,
        *PsProcessType, KernelMode, &h_process);
    ObDereferenceObject(target_process);
    if (!NT_SUCCESS(status))
        return status;
    returned = 0;
    address = req->start_address ? req->start_address : 0x10000;
    while (returned < req->count) {
        MEMORY_BASIC_INFORMATION info;
        SIZE_T returned_length;
        ULONG64 next;
        RtlZeroMemory(&info, sizeof(info));
        returned_length = 0;
        status = ZwQueryVirtualMemory(
            h_process, (PVOID)address, MemoryBasicInformation, &info, sizeof(info), &returned_length);
        if (!NT_SUCCESS(status))
            break;
        if (info.RegionSize == 0)
            break;
        req->regions[returned].base_address = (ULONG64)(ULONG_PTR)info.BaseAddress;
        req->regions[returned].region_size = info.RegionSize;
        req->regions[returned].state = info.State;
        req->regions[returned].protect = info.Protect;
        req->regions[returned].type = info.Type;
        returned++;
        next = (ULONG64)(ULONG_PTR)info.BaseAddress + info.RegionSize;
        if (next <= address)
            break;
        address = next;
    }
    ZwClose(h_process);
    req->count = returned;
    return returned > 0 ? STATUS_SUCCESS : status;
}

NTSTATUS handle_thread_list(p_thread_list_request req, size_t buffer_length) {
    ULONG system_info_length;
    PVOID system_info;
    ULONG needed;
    NTSTATUS status;
    p_system_process_info current;
    ULONG returned;
    if (!req || req->count == 0 || req->count > max_thread_list_capacity)
        return STATUS_INVALID_PARAMETER;
    if (sizeof(thread_list_request) + (req->count - 1) * sizeof(thread_info) > buffer_length)
        return STATUS_BUFFER_TOO_SMALL;
    system_info_length = 256 * 1024;
    system_info = NULL;
    needed = 0;
    status = STATUS_INFO_LENGTH_MISMATCH;
    while (system_info_length <= 8 * 1024 * 1024) {
        system_info = ExAllocatePoolWithTag(NonPagedPool, system_info_length, 'nBlA');
        if (!system_info)
            return STATUS_INSUFFICIENT_RESOURCES;
        status = ZwQuerySystemInformation(SystemProcessInformation, system_info, system_info_length, &needed);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            ExFreePoolWithTag(system_info, 'nBlA');
            system_info = NULL;
            system_info_length = needed > system_info_length ? needed : system_info_length * 2;
            continue;
        }
        break;
    }
    if (!NT_SUCCESS(status)) {
        if (system_info)
            ExFreePoolWithTag(system_info, 'nBlA');
        return status;
    }
    returned = 0;
    current = (p_system_process_info)system_info;
    for (;;) {
        if ((ULONG)(ULONG_PTR)current->unique_process_id == req->process_id && returned < req->count) {
            p_system_thread_info thread =
                (p_system_thread_info)((PUCHAR)current + sizeof(system_process_info));
            ULONG i;
            for (i = 0; i < current->number_of_threads && returned < req->count; i++) {
                HANDLE thread_id = thread[i].client_id.UniqueThread;
                PETHREAD p_thread;
                HANDLE h_thread;
                thread_basic_info basic;
                if (!thread_id)
                    continue;
                p_thread = NULL;
                if (!NT_SUCCESS(PsLookupThreadByThreadId(thread_id, &p_thread)))
                    continue;
                h_thread = NULL;
                status = ObOpenObjectByPointer(p_thread, OBJ_KERNEL_HANDLE, NULL, THREAD_QUERY_INFORMATION,
                    *PsThreadType, KernelMode, &h_thread);
                ObDereferenceObject(p_thread);
                if (!NT_SUCCESS(status))
                    continue;
                RtlZeroMemory(&basic, sizeof(basic));
                status =
                    ZwQueryInformationThread(h_thread, ThreadBasicInformation, &basic, sizeof(basic), NULL);
                ZwClose(h_thread);
                if (!NT_SUCCESS(status) || !basic.teb_base_address)
                    continue;
                req->threads[returned].thread_id = (ULONG)(ULONG_PTR)thread_id;
                req->threads[returned].teb_base_address = (ULONG64)(ULONG_PTR)basic.teb_base_address;
                returned++;
            }
        }
        if (current->next_entry_offset == 0)
            break;
        current = (p_system_process_info)((PUCHAR)current + current->next_entry_offset);
    }
    ExFreePoolWithTag(system_info, 'nBlA');
    req->count = returned;
    return STATUS_SUCCESS;
}

NTSTATUS create_close(PDEVICE_OBJECT device_object, PIRP irp) {
    UNREFERENCED_PARAMETER(device_object);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS device_control(PDEVICE_OBJECT device_object, PIRP irp) {
    UNREFERENCED_PARAMETER(device_object);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG ioctl = stack->Parameters.DeviceIoControl.IoControlCode;
    size_t input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    PVOID buffer = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytes_returned = 0;
    if (input_length < sizeof(rw_request)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto complete;
    }
    switch (ioctl) {
    case ioctl_read:
    case ioctl_write: {
        p_rw_request req = (p_rw_request)buffer;
        req->write = (ioctl == ioctl_write);
        req->status = read_write_memory(req);
        bytes_returned = sizeof(rw_request);
        break;
    }
    case ioctl_bulk: {
        p_bulk_request bulk_req = (p_bulk_request)buffer;
        if (input_length < sizeof(bulk_request)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = handle_bulk(bulk_req);
        bytes_returned = input_length;
        break;
    }
    case ioctl_processes: {
        p_proc_list_request process_req = (p_proc_list_request)buffer;
        if (input_length < sizeof(proc_list_request)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = handle_process_list(process_req, input_length);
        bytes_returned = input_length;
        break;
    }
    case ioctl_regions: {
        p_region_request region_req = (p_region_request)buffer;
        if (input_length < sizeof(region_request)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = handle_region_query(region_req, input_length);
        bytes_returned = input_length;
        break;
    }
    case ioctl_threads: {
        p_thread_list_request thread_req = (p_thread_list_request)buffer;
        if (input_length < sizeof(thread_list_request)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = handle_thread_list(thread_req, input_length);
        bytes_returned = input_length;
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
complete:
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes_returned;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

VOID driver_unload(PDRIVER_OBJECT driver_object) {
    UNICODE_STRING symbolic_link;
    RtlInitUnicodeString(&symbolic_link, symbolic_name);
    IoDeleteSymbolicLink(&symbolic_link);
    if (driver_object->DeviceObject) {
        IoDeleteDevice(driver_object->DeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    NTSTATUS status;
    PDEVICE_OBJECT device_object = NULL;
    UNICODE_STRING device_name;
    UNICODE_STRING symbolic_link;
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    RtlInitUnicodeString(&device_name, device_path);
    status = IoCreateDeviceSecure(
        driver_object, 0, &device_name, FILE_DEVICE_UNKNOWN, 0, FALSE, &sddl, NULL, &device_object);
    if (!NT_SUCCESS(status))
        return status;
    RtlInitUnicodeString(&symbolic_link, symbolic_name);
    status = IoCreateSymbolicLink(&symbolic_link, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device_object);
        return status;
    }
    driver_object->MajorFunction[IRP_MJ_CREATE] = create_close;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = create_close;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = device_control;
    driver_object->DriverUnload = driver_unload;
    return STATUS_SUCCESS;
}
