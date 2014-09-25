#include "DriverControl.h"
#include "../Misc/Utils.h"
#include "../Misc/Trace.hpp"
#include "../Misc/DynImport.h"
#include "../Include/Macro.h"

#include "VersionHelpers.h"

namespace blackbone
{

#define DRIVER_SVC_NAME L"BlackBone"

DriverControl::DriverControl()
{
    DynImport::load( "NtLoadDriver", GetModuleHandleW( L"ntdll.dll" ) );
    DynImport::load( "NtUnloadDriver", GetModuleHandleW( L"ntdll.dll" ) );
    DynImport::load( "RtlDosPathNameToNtPathName_U", GetModuleHandleW( L"ntdll.dll" ) );   
}


DriverControl::~DriverControl()
{
    Unload();
}

DriverControl& DriverControl::Instance()
{
    static DriverControl instance;
    return instance;
}

/// <summary>
/// Try to load driver if it isn't loaded
/// </summary>
/// <param name="path">Path to the driver file</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::EnsureLoaded( const std::wstring& path /*= L"" */ )
{
    // Already open
    if (_hDriver != INVALID_HANDLE_VALUE)
        return STATUS_SUCCESS;

    // Try to open handle to existing driver
    _hDriver = CreateFile( BLACKBONE_DEVICE_FILE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL );

    if (_hDriver != INVALID_HANDLE_VALUE)
        return STATUS_SUCCESS;

    // Start new instance
    return Reload( path );
}

/// <summary>
/// Reload driver
/// </summary>
/// <param name="path">Path to the driver file</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::Reload( std::wstring path /*= L"" */ )
{
    NTSTATUS status = STATUS_SUCCESS;

    Unload();

    // Use default path
    if (path.empty())
    {
        const wchar_t* filename = nullptr;
        if (IsWindows8Point1OrGreater())
            filename = L"BlackBoneDrv81.sys";
        else if (IsWindows8OrGreater())
            filename = L"BlackBoneDrv8.sys";
        else if (IsWindows7OrGreater())
            filename = L"BlackBoneDrv7.sys";
        else
            filename = L"BlackBoneDrv.sys";

        path = Utils::GetExeDirectory() + L"\\" + filename;
    }

    status = LoadDriver( DRIVER_SVC_NAME, path );
    if (!NT_SUCCESS( status ))
    {
        BLACBONE_TRACE( L"Failed to load driver %ls. Status 0x%X", path.c_str(), status );
        return LastNtStatus( status );
    }

    _hDriver = CreateFile( BLACKBONE_DEVICE_FILE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL );

    if (_hDriver == INVALID_HANDLE_VALUE)
    {
        BLACBONE_TRACE( L"Failed to open driver handle. Status 0x%X", status );
        return LastNtStatus();
    }

    return STATUS_SUCCESS;
}

/// <summary>
/// Unload driver
/// </summary>
/// <returns>Status code</returns>
NTSTATUS DriverControl::Unload()
{
    if (_hDriver != INVALID_HANDLE_VALUE)
    {
        CloseHandle( _hDriver );
        _hDriver = INVALID_HANDLE_VALUE;
    }

    return UnloadDriver( DRIVER_SVC_NAME );
}


/// <summary>
/// Maps target process memory into current process
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="pipeName">Pipe name to use for hook data transfer</param>
/// <param name="mapSections">The map sections.</param>
/// <param name="result">Results</param>
/// <returns>Status code </returns>
NTSTATUS DriverControl::MapMemory( DWORD pid, const std::wstring& pipeName, bool mapSections, MapMemoryResult& result )
{
    MAP_MEMORY data = { 0 };
    DWORD bytes = 0;
    ULONG sizeRequired = 0;
    data.pid = pid;
    data.mapSections = mapSections;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    wcscpy_s( data.pipeName, pipeName.c_str() );

    BOOL res = DeviceIoControl( _hDriver, IOCTL_BLACKBONE_MAP_MEMORY, &data, sizeof( data ), &sizeRequired, sizeof( sizeRequired ), &bytes, NULL );
    if (res != FALSE && bytes == 4)
    {
        MAP_MEMORY_RESULT* pResult = (MAP_MEMORY_RESULT*)malloc( sizeRequired );

        if (DeviceIoControl( _hDriver, IOCTL_BLACKBONE_MAP_MEMORY, &data, sizeof( data ), pResult, sizeRequired, &bytes, NULL ))
        {
            for (ULONG i = 0; i < pResult->count; i++)
                result.regions.emplace( std::make_pair( std::make_pair( pResult->entries[i].originalPtr, pResult->entries[i].size ),
                                        pResult->entries[i].newPtr ) );

            result.hostSharedPage = pResult->hostPage;
            result.targetSharedPage = pResult->targetPage;
            result.targetPipe = (HANDLE)pResult->pipeHandle;

            free( pResult );
            return STATUS_SUCCESS;
        }
    }

    return LastNtStatus();
}

/// <summary>
/// Maps single memory region into current process
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="base">Region base address</param>
/// <param name="size">Region size</param>
/// <param name="result">Mapped region info</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::MapMemoryRegion( DWORD pid, ptr_t base, uint32_t size, MapMemoryRegionResult& result )
{
    MAP_MEMORY_REGION data = { 0 };
    MAP_MEMORY_REGION_RESULT mapResult = { 0 };
    DWORD bytes = 0;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    data.pid = pid;
    data.base = base;
    data.size = size;

    if (DeviceIoControl( _hDriver, IOCTL_BLACKBONE_MAP_REGION, &data, sizeof( data ), &mapResult, sizeof( mapResult ), &bytes, NULL ))
    {
        result.newPtr = mapResult.newPtr;
        result.originalPtr = mapResult.originalPtr;
        result.removedPtr = mapResult.removedPtr;
        result.removedSize = mapResult.removedSize;
        result.size = mapResult.size;

        return STATUS_SUCCESS;
    }

    return LastNtStatus();
}

/// <summary>
/// Unmap memory of the target process from current
/// </summary>
/// <param name="pid">Target PID</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::UnmapMemory( DWORD pid )
{
    UNMAP_MEMORY data = { pid };
    DWORD bytes = 0;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (DeviceIoControl( _hDriver, IOCTL_BLACKBONE_UNMAP_MEMORY, &data, sizeof( data ), NULL, 0, &bytes, NULL ))
        return STATUS_SUCCESS;

    return LastNtStatus();
}

/// <summary>
/// Unmap single memory region
/// If unmapped region size is smaller than the size specified during map, function will return info about
/// 2 regions that emerged after unmap
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="base">Region base</param>
/// <param name="size">Region size</param>
/// <param name="result">Unampped region info</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::UnmapMemoryRegion( DWORD pid, ptr_t base, uint32_t size )
{
    UNMAP_MEMORY_REGION data = { 0 };
    DWORD bytes = 0;

    data.pid = pid;
    data.base = base;
    data.size = size;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_UNMAP_REGION, &data, sizeof( data ), NULL, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}


/// <summary>
/// Disable DEP for process
/// Has no effect on native x64 processes
/// </summary>
/// <param name="pid">Target PID</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::DisableDEP( DWORD pid )
{
    DWORD bytes = 0;
    DISABLE_DEP disableDep = { pid };

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_DISABLE_DEP, &disableDep, sizeof( disableDep ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Change process protection flag
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="enable">true to enable protection, false to disable</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::ProtectProcess( DWORD pid, bool enable )
{
    DWORD bytes = 0;
    SET_PROC_PROTECTION setProt = { pid, enable };

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_SET_PROTECTION, &setProt, sizeof( setProt ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Change handle access rights
/// </summary>
/// <param name="pid">Target PID.</param>
/// <param name="handle">Handle</param>
/// <param name="access">New access</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::PromoteHandle( DWORD pid, HANDLE handle, DWORD access )
{
    DWORD bytes = 0;
    HANDLE_GRANT_ACCESS grantAccess = { 0 };

    grantAccess.pid = pid;
    grantAccess.handle = (ULONGLONG)handle;
    grantAccess.access = access;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_GRANT_ACCESS, &grantAccess, sizeof( grantAccess ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Allocate virtual memory
/// </summary>
/// <param name="pid">Tarhet PID</param>
/// <param name="base">Desired base. If 0 address is chosed by the system</param>
/// <param name="size">Region size</param>
/// <param name="type">Allocation type - MEM_RESERVE/MEM_COMMIT</param>
/// <param name="protection">Memory protection</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::AllocateMem( DWORD pid, ptr_t& base, ptr_t& size, DWORD type, DWORD protection, bool physical /*= false*/ )
{
    DWORD bytes = 0;
    ALLOCATE_FREE_MEMORY allocMem = { 0 };
    ALLOCATE_FREE_MEMORY_RESULT result = { 0 };

    allocMem.pid = pid;
    allocMem.base = base;
    allocMem.size = size;
    allocMem.type = type;
    allocMem.protection = protection;
    allocMem.allocate = TRUE;
    allocMem.physical = physical;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_ALLOCATE_FREE_MEMORY, &allocMem, sizeof( allocMem ), &result, sizeof( result ), &bytes, NULL ))
    {
        size = base = 0;
        return LastNtStatus();
    }

    base = result.address;
    size = result.size;

    return STATUS_SUCCESS;
}

/// <summary>
/// Free virtual memory
/// </summary>
/// <param name="pid">Tarhet PID</param>
/// <param name="base">Desired base. If 0 address is chosed by the system</param>
/// <param name="size">Region size</param>
/// <param name="type">Free type - MEM_RELEASE/MEM_DECOMMIT</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::FreeMem( DWORD pid, ptr_t base, ptr_t size, DWORD type )
{
    DWORD bytes = 0;
    ALLOCATE_FREE_MEMORY freeMem = { 0 };
    ALLOCATE_FREE_MEMORY_RESULT result = { 0 };

    freeMem.pid = pid;
    freeMem.base = base;
    freeMem.size = size;
    freeMem.type = type;
    freeMem.allocate = FALSE;
    freeMem.physical = FALSE;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_ALLOCATE_FREE_MEMORY, &freeMem, sizeof( freeMem ), &result, sizeof( result ), &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Read process memory
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="base">Target base</param>
/// <param name="size">Data size</param>
/// <param name="buffer">Buffer address</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::ReadMem( DWORD pid, ptr_t base, ptr_t size, PVOID buffer )
{
    DWORD bytes = 0;
    COPY_MEMORY copyMem = { 0 };

    copyMem.pid = pid;
    copyMem.targetPtr = base;
    copyMem.localbuf = (ULONGLONG)buffer;
    copyMem.size = size;
    copyMem.write = FALSE;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_COPY_MEMORY, &copyMem, sizeof( copyMem ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Write process memory
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="base">Target base</param>
/// <param name="size">Data size</param>
/// <param name="buffer">Buffer address</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::WriteMem( DWORD pid, ptr_t base, ptr_t size, PVOID buffer )
{
    DWORD bytes = 0;
    COPY_MEMORY copyMem = { 0 };

    copyMem.pid = pid;
    copyMem.targetPtr = base;
    copyMem.localbuf = (ULONGLONG)buffer;
    copyMem.size = size;
    copyMem.write = TRUE; 

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_COPY_MEMORY, &copyMem, sizeof( copyMem ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Change memory protection
/// </summary>
/// <param name="pid">Target PID.</param>
/// <param name="base">Regiod base address</param>
/// <param name="size">Region size</param>
/// <param name="protection">New protection</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::ProtectMem( DWORD pid, ptr_t base, ptr_t size, DWORD protection )
{
    DWORD bytes = 0;
    PROTECT_MEMORY protectMem = { 0 };

    protectMem.pid = pid;
    protectMem.base = base;
    protectMem.size = size;
    protectMem.newProtection = protection;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_PROTECT_MEMORY, &protectMem, sizeof( protectMem ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Inject DLL into arbitrary process
/// </summary>
/// <param name="pid">Target PID.</param>
/// <param name="path">Full qualified dll path.</param>
/// <param name="itype">Injection type</param>
/// <param name="initRVA">Init routine RVA</param>
/// <param name="initArg">Init routine argument</param>
/// <param name="unlink">Unlink module after injection</param>
/// <param name="wait">Wait for injection</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::InjectDll(
    DWORD pid,
    const std::wstring& path,
    InjectType itype,
    uint32_t initRVA /*= 0*/,
    const std::wstring& initArg /*= L""*/,
    bool unlink /*= false*/,
    bool wait /*= true*/
    )
{
    DWORD bytes = 0;
    INJECT_DLL data = { 0 };

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    wcscpy_s( data.FullDllPath, path.c_str() );
    wcscpy_s( data.initArg, initArg.c_str() );
    data.pid = pid;
    data.initRVA = initRVA;
    data.wait = wait;
    data.unlink = unlink;
    data.type = itype;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_INJECT_DLL, &data, sizeof( data ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Manually map another system driver into system space
/// </summary>
/// <param name="path">Fully quialified path to the drver</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::MMapDriver( const std::wstring& path )
{
    DWORD bytes = 0;
    MMAP_DRIVER data = { 0 };
    UNICODE_STRING ustr = { 0 };

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    // Convert path to native format
    GET_IMPORT( RtlDosPathNameToNtPathName_U )(path.c_str(), &ustr, NULL, NULL);

    wcscpy_s( data.FullPath, ustr.Buffer );

    GET_IMPORT( RtlFreeUnicodeString )(&ustr);

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_MAP_DRIVER, &data, sizeof( data ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}


/// <summary>
/// Make VAD region appear as PAGE_NO_ACESS to NtQueryVirtualMemory
/// </summary>
/// <param name="pid">Target process ID</param>
/// <param name="base">Region base</param>
/// <param name="size">Region size</param>
/// <returns>Status code</returns>
NTSTATUS DriverControl::ConcealVAD( DWORD pid, ptr_t base, uint32_t size )
{
    DWORD bytes = 0;
    HIDE_VAD hideVAD = { 0 };

    hideVAD.base = base;
    hideVAD.size = size;
    hideVAD.pid = pid;

    // Not loaded
    if (_hDriver == INVALID_HANDLE_VALUE)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    if (!DeviceIoControl( _hDriver, IOCTL_BLACKBONE_HIDE_VAD, &hideVAD, sizeof( hideVAD ), nullptr, 0, &bytes, NULL ))
        return LastNtStatus();

    return STATUS_SUCCESS;
}

/// <summary>
/// Load arbitrary driver
/// </summary>
/// <param name="svcName">Driver service name</param>
/// <param name="path">Driver file path</param>
/// <returns>Status</returns>
NTSTATUS DriverControl::LoadDriver( const std::wstring& svcName, const std::wstring& path )
{
    UNICODE_STRING Ustr;

    // If no file provided, try to start existing service
    if (!path.empty() && PrepareDriverRegEntry( svcName, path ) != 0)
        return LastNtStatus();

    std::wstring regPath = L"\\registry\\machine\\SYSTEM\\CurrentControlSet\\Services\\" + svcName;
    GET_IMPORT( RtlInitUnicodeString )(&Ustr, regPath.c_str());

    return GET_IMPORT( NtLoadDriver )(&Ustr);
}


/// <summary>
/// Unload arbitrary driver
/// </summary>
/// <param name="svcName">Driver service name</param>
/// <returns>Status</returns>
NTSTATUS DriverControl::UnloadDriver( const std::wstring& svcName )
{
    UNICODE_STRING Ustr;

    std::wstring regPath = L"\\registry\\machine\\SYSTEM\\CurrentControlSet\\Services\\" + svcName;
    GET_IMPORT( RtlInitUnicodeString )(&Ustr, regPath.c_str());

    // Remove previously loaded instance, if any
    return GET_IMPORT( NtUnloadDriver )(&Ustr);
}

/// <summary>
/// Fill minimal required driver registry entry
/// </summary>
/// <param name="svcName">Driver service name</param>
/// <param name="path">Driver path</param>
/// <returns>Status code</returns>
LSTATUS DriverControl::PrepareDriverRegEntry( const std::wstring& svcName, const std::wstring& path )
{
    HKEY key1, key2;
    BYTE dwType = 1;
    LSTATUS status = 0;
    WCHAR wszLocalPath[MAX_PATH] = { 0 };

    swprintf_s( wszLocalPath, ARRAYSIZE( wszLocalPath ), L"\\??\\%s", path.c_str() );

    status = RegOpenKeyW( HKEY_LOCAL_MACHINE, L"system\\CurrentControlSet\\Services", &key1 );
    if (status)
        return status;

    status = RegCreateKeyW( key1, svcName.c_str(), &key2 );
    if (status)
    {
        RegCloseKey( key1 );
        return status;
    }

    status = RegSetValueExW( key2, L"ImagePath", 0, REG_SZ, reinterpret_cast<const BYTE*>(wszLocalPath),
                             static_cast<DWORD>(sizeof( WCHAR )* (wcslen( wszLocalPath ) + 1)) );
    if (status)
    {
        RegCloseKey( key2 );
        RegCloseKey( key1 );
        return status;
    }

    status = RegSetValueExW( key2, L"Type", 0, REG_DWORD, &dwType, sizeof( DWORD ) );
    if (status)
    {
        RegCloseKey( key2 );
        RegCloseKey( key1 );
        return status;
    }

    RegCloseKey( key2 );
    RegCloseKey( key1 );

    return status;
}



}