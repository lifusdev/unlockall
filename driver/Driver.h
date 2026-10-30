#pragma once
#include <ntifs.h>
#include <wdmsec.h>

NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(PEPROCESS source_process, PVOID source_address, PEPROCESS target_process,
    PVOID target_address, SIZE_T buffer_size, KPROCESSOR_MODE previous_mode, PSIZE_T return_size);

NTKERNELAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(ULONG system_information_class, PVOID system_information,
    ULONG system_information_length, PULONG return_length);

NTKERNELAPI
NTSTATUS
NTAPI
ZwQueryInformationThread(HANDLE thread_handle, THREADINFOCLASS thread_information_class,
    PVOID thread_information, ULONG thread_information_length, PULONG return_length);

#ifndef SystemProcessInformation
#define SystemProcessInformation 5
#endif

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (0x0400)
#endif

#ifndef THREAD_QUERY_INFORMATION
#define THREAD_QUERY_INFORMATION (0x0040)
#endif

#ifndef DECLARE_CONST_UNICODE_STRING
#define DECLARE_CONST_UNICODE_STRING(_variablename, _string)                                                 \
    const WCHAR _variablename##_buffer[] = _string;                                                          \
    const UNICODE_STRING _variablename = {                                                                   \
        (USHORT)(sizeof(_string) - sizeof(WCHAR)), (USHORT)(sizeof(_string)), (PWCH)_variablename##_buffer}
#endif

#define device_path L"\\Device\\lifus"
#define symbolic_name L"\\DosDevices\\lifus"

#define ioctl_read CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ioctl_write CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ioctl_bulk CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ioctl_processes CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ioctl_regions CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ioctl_threads CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define max_transfer_size (PAGE_SIZE * 256)
#define max_bulk_operations 64
#define max_process_name_length 260
#define max_process_list_capacity 2048
#define max_region_list_capacity 8192
#define max_thread_list_capacity 2048

typedef struct rw_request_tag {
    ULONG process_id;
    ULONG64 address;
    ULONG64 buffer;
    SIZE_T size;
    BOOLEAN write;
    NTSTATUS status;
} rw_request, *p_rw_request;

typedef struct bulk_request_tag {
    ULONG count;
    rw_request operations[max_bulk_operations];
} bulk_request, *p_bulk_request;

typedef struct proc_entry_tag {
    ULONG process_id;
    WCHAR name[max_process_name_length];
} proc_entry, *p_proc_entry;

typedef struct proc_list_request_tag {
    ULONG count;
    proc_entry processes[1];
} proc_list_request, *p_proc_list_request;

typedef struct region_info_tag {
    ULONG64 base_address;
    SIZE_T region_size;
    ULONG state;
    ULONG protect;
    ULONG type;
} region_info, *p_region_info;

typedef struct region_request_tag {
    ULONG process_id;
    ULONG64 start_address;
    ULONG count;
    region_info regions[1];
} region_request, *p_region_request;

typedef struct thread_info_tag {
    ULONG thread_id;
    ULONG64 teb_base_address;
} thread_info, *p_thread_info;

typedef struct thread_list_request_tag {
    ULONG process_id;
    ULONG count;
    thread_info threads[1];
} thread_list_request, *p_thread_list_request;

typedef struct system_thread_info_tag {
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER create_time;
    ULONG wait_time;
    PVOID start_address;
    CLIENT_ID client_id;
    KPRIORITY priority;
    LONG base_priority;
    ULONG context_switches;
    ULONG thread_state;
    ULONG wait_reason;
} system_thread_info, *p_system_thread_info;

typedef struct system_process_info_tag {
    ULONG next_entry_offset;
    ULONG number_of_threads;
    UCHAR reserved1[48];
    UNICODE_STRING image_name;
    KPRIORITY base_priority;
    HANDLE unique_process_id;
    PVOID reserved2;
    ULONG handle_count;
    ULONG session_id;
    PVOID reserved3;
    SIZE_T peak_virtual_size;
    SIZE_T virtual_size;
    ULONG reserved4;
    SIZE_T peak_working_set_size;
    SIZE_T working_set_size;
    PVOID reserved5;
    SIZE_T quota_paged_pool_usage;
    PVOID reserved6;
    SIZE_T quota_non_paged_pool_usage;
    SIZE_T pagefile_usage;
    SIZE_T peak_pagefile_usage;
    SIZE_T private_page_count;
    LARGE_INTEGER reserved7[6];
} system_process_info, *p_system_process_info;

typedef struct thread_basic_info_tag {
    NTSTATUS exit_status;
    PVOID teb_base_address;
    CLIENT_ID client_id;
    ULONG_PTR affinity_mask;
    KPRIORITY priority;
    LONG base_priority;
} thread_basic_info, *p_thread_basic_info;
