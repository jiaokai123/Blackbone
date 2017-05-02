#include "RemoteExec.h"
#include "../Process.h"
#include "../../Misc/DynImport.h"
#include "../../Misc/PattrernLoader.h"

#include <VersionHelpers.h>
#include <sddl.h>
#include <AccCtrl.h>
#include <Aclapi.h>

namespace blackbone
{

RemoteExec::RemoteExec( Process& proc )
    : _proc( proc )
    , _mods( _proc.modules() )
    , _memory( _proc.memory() )
    , _threads( _proc.threads() )
    , _hWorkThd( new Thread( (DWORD)0, &_memory.core() ) )
    , _hWaitEvent( NULL )
    , _apcPatched( false )
{
}

RemoteExec::~RemoteExec()
{
    TerminateWorker();
}

/// <summary>
/// Create new thread and execute code in it. Wait until execution ends
/// </summary>
/// <param name="pCode">Code to execute</param>
/// <param name="size">Code size</param>
/// <param name="callResult">Code return value</param>
/// <param name="modeSwitch">Switch wow64 thread to long mode upon creation</param>
/// <returns>Status</returns>
NTSTATUS RemoteExec::ExecInNewThread(
    PVOID pCode, size_t size,
    uint64_t& callResult,
    eThreadModeSwitch modeSwitch /*= AutoSwitch*/
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    CreateRPCEnvironment( false, false );

    // Write code
    if (!NT_SUCCESS( status = CopyCode( pCode, size ) ))
        return status;

    bool switchMode = false;
    switch (modeSwitch)
    {
    case blackbone::ForceSwitch:
        switchMode = true;
        break;

    case blackbone::AutoSwitch:
        switchMode = _proc.barrier().type == wow_64_32;
        break;
    }

    auto pExitThread = _mods.GetNtdllExport( "NtTerminateThread", switchMode ? mt_mod64 : mt_default );
    if (!pExitThread)
        return pExitThread.status;

    auto a = switchMode ? AsmFactory::GetAssembler( AsmFactory::asm64 ) 
                        : AsmFactory::GetAssembler( _proc.core().isWow64() );

    a->GenPrologue( switchMode );

    // Prepare thread to run in x64 mode
    if(switchMode)
    {
        // Allocate new x64 activation stack
        auto createActStack = _mods.GetNtdllExport( "RtlAllocateActivationContextStack", mt_mod64 );
        if (createActStack)
        {
            a->GenCall( createActStack->procAddress, { _userData.ptr() + 0x3100 } );

            (*a)->mov( (*a)->zax, _userData.ptr() + 0x3100 );
            (*a)->mov( (*a)->zax, (*a)->intptr_ptr( (*a)->zax ) );

            (*a)->mov( (*a)->zdx, asmjit::host::dword_ptr_abs( 0x30 ).setSegment( asmjit::host::gs ) );
            (*a)->mov( (*a)->intptr_ptr( (*a)->zdx, 0x2C8 ), (*a)->zax );
        }
    }

    a->GenCall( _userCode.ptr(), { } );
    a->ExitThreadWithStatus( pExitThread->procAddress, _userData.ptr() + INTRET_OFFSET );
    
    // Execute code in newly created thread
    if (!NT_SUCCESS( status = _userCode.Write( size, (*a)->getCodeSize(), (*a)->make() ) ))
        return status;

    auto thread = _threads.CreateNew( _userCode.ptr() + size, _userData.ptr()/*, HideFromDebug*/ );
    if (!thread)
        return thread.status;
    if (!thread.result()->Join())
        return LastNtStatus();

    callResult = _userData.Read<uint64_t>( INTRET_OFFSET, 0 );
    return STATUS_SUCCESS;
}

/// <summary>
/// Execute code in context of our worker thread
/// </summary>
/// <param name="pCode">Cde to execute</param>
/// <param name="size">Code size.</param>
/// <param name="callResult">Execution result</param>
/// <returns>Status</returns>
NTSTATUS RemoteExec::ExecInWorkerThread( PVOID pCode, size_t size, uint64_t& callResult )
{
    NTSTATUS dwResult = STATUS_SUCCESS;

    if (_proc.barrier().type == wow_64_32)
        return ExecInAnyThread( pCode, size, callResult, _hWorkThd );

    // Create thread if needed
    CreateRPCEnvironment();

    // Write code
    dwResult = CopyCode( pCode, size );
    if (dwResult != STATUS_SUCCESS)
        return dwResult;

    if (_hWaitEvent)
        ResetEvent( _hWaitEvent );

    // Patch KiUserApcDispatcher 
#ifdef USE64
    if (!_apcPatched && IsWindows7OrGreater() && !IsWindows8OrGreater())
    {
        if (_proc.barrier().type == wow_64_32)
        {
            auto patchBase = g_PatternLoader->data().APC64PatchAddress;

            if (patchBase != 0)
            {
                DWORD flOld = 0;
                _memory.Protect(patchBase, 6, PAGE_EXECUTE_READWRITE, &flOld);
                _memory.Write(patchBase + 0x2, (uint8_t)0x0C);
                _memory.Write( patchBase + 0x4, (uint8_t)0x90 );
                _memory.Protect( patchBase, 6, flOld, nullptr );
            }

            _apcPatched = true;
        }
        else
            _apcPatched = true;
    }
#endif

    // Execute code in thread context
    // TODO: Find out why am I passing pRemoteCode as an argument???
    auto pRemoteCode = _userCode.ptr();
    if (NT_SUCCESS( _proc.core().native()->NtQueueApcThreadT( _hWorkThd->handle(), pRemoteCode, pRemoteCode ) ))
    {
        dwResult = WaitForSingleObject( _hWaitEvent, 30 * 1000 /*wait 30s*/ );
        callResult = _userData.Read<uint64_t>( RET_OFFSET, 0 );
    }
    else
        return LastNtStatus();

    // Ensure APC function fully returns
    Sleep( 1 );

    return dwResult;
}

/// <summary>
/// Execute code in context of any existing thread
/// </summary>
/// <param name="pCode">Cde to execute</param>
/// <param name="size">Code size.</param>
/// <param name="callResult">Execution result</param>
/// <param name="thd">Target thread</param>
/// <returns>Status</returns>
NTSTATUS RemoteExec::ExecInAnyThread( PVOID pCode, size_t size, uint64_t& callResult, ThreadPtr& thd )
{
    NTSTATUS status = STATUS_SUCCESS;
    _CONTEXT32 ctx32 = { 0 };
    _CONTEXT64 ctx64 = { 0 };

    // Prepare for remote exec
    if (!NT_SUCCESS( status = CreateRPCEnvironment( false, true ) ))
        return status;

    // Write code
    if (!NT_SUCCESS( status = CopyCode( pCode, size ) ))
        return status;

    if (_hWaitEvent)
        ResetEvent( _hWaitEvent );

    if (!thd->Suspend())
        return LastNtStatus();

    auto a = AsmFactory::GetAssembler( _proc.core().isWow64() );
    if (!_proc.core().isWow64())
    {
        const int count = 15;
        static const asmjit::GpReg regs[] =
        {
            asmjit::host::rax, asmjit::host::rbx, asmjit::host::rcx, asmjit::host::rdx, asmjit::host::rsi,
            asmjit::host::rdi, asmjit::host::r8,  asmjit::host::r9,  asmjit::host::r10, asmjit::host::r11,
            asmjit::host::r12, asmjit::host::r13, asmjit::host::r14, asmjit::host::r15, asmjit::host::rbp
        };

        if (!NT_SUCCESS( status = thd->GetContext( ctx64, CONTEXT_CONTROL, true ) ))
        {
            thd->Resume();
            return status;
        }

        //
        // Preserve thread context
        // I don't care about FPU, XMM and anything else
        //
        (*a)->sub( asmjit::host::rsp, count * sizeof( uint64_t ) );   // Stack must be aligned on 16 bytes 
        (*a)->pushf();                                              //

                                                                    // Save registers
        for (int i = 0; i < count; i++)
            (*a)->mov( asmjit::Mem( asmjit::host::rsp, i * sizeof( uint64_t ) ), regs[i] );

        a->GenCall( _userCode.ptr(), { _userData.ptr() } );
        AddReturnWithEvent( *a, mt_default, rt_int32, INTRET_OFFSET );

        // Restore registers
        for (int i = 0; i < count; i++)
            (*a)->mov( regs[i], asmjit::Mem( asmjit::host::rsp, i * sizeof( uint64_t ) ) );

        (*a)->popf();
        (*a)->add( asmjit::host::rsp, count * sizeof( uint64_t ) );

        // jmp [rip]
        (*a)->dw( '\xFF\x25' );
        (*a)->dd( 0 );
        (*a)->dq( ctx64.Rip );
    }
    else
    {
        if (!NT_SUCCESS( status = thd->GetContext( ctx32, CONTEXT_CONTROL, true ) ))
        {
            thd->Resume();
            return status;
        }

        (*a)->pusha();
        (*a)->pushf();

        a->GenCall( _userCode.ptr(), { _userData.ptr() } );
        AddReturnWithEvent( *a, mt_default, rt_int32, INTRET_OFFSET );

        (*a)->popf();
        (*a)->popa();

        (*a)->push( static_cast<int>(ctx32.Eip) );
        (*a)->ret();
    }

    if (NT_SUCCESS( status = _userCode.Write( size, (*a)->getCodeSize(), (*a)->make() ) ))
    {
        if (_proc.core().isWow64())
        {
            ctx32.Eip = static_cast<uint32_t>(_userCode.ptr() + size);
            status = thd->SetContext( ctx32, true );
        }     
        else
        {
            ctx64.Rip = _userCode.ptr() + size;
            status = thd->SetContext( ctx64, true );
        }
    }

    thd->Resume();
    if (NT_SUCCESS( status ))
    {
        WaitForSingleObject( _hWaitEvent, INFINITE );
        status = _userData.Read( RET_OFFSET, callResult );
    }

    return status;
}


/// <summary>
/// Create new thread with specified entry point and argument
/// </summary>
/// <param name="pCode">Entry point</param>
/// <param name="arg">Thread function argument</param>
/// <returns>Thread exit code</returns>
DWORD RemoteExec::ExecDirect( ptr_t pCode, ptr_t arg )
{
    auto thread = _threads.CreateNew( pCode, arg/*, HideFromDebug*/ );
    if (!thread)
        return thread.status;

    thread.result()->Join();
    return thread.result()->ExitCode();
}

/// <summary>
/// Create environment for future remote procedure calls
///
/// _userData layout (x86/x64):
/// --------------------------------------------------------------------------------------------------------------------------
/// | Internal return value | Return value |  Last Status code  |  Event handle   |  Space for copied arguments and strings  |
/// -------------------------------------------------------------------------------------------------------------------------
/// |       8/8 bytes       |   8/8 bytes  |      8/8 bytes     |   16/16 bytes   |                                          |
/// --------------------------------------------------------------------------------------------------------------------------
/// </summary>
/// <param name="bThread">Create worker thread</param>
/// <param name="bEvent">Create sync event for worker thread</param>
/// <returns>Status</returns>
NTSTATUS RemoteExec::CreateRPCEnvironment( bool bThread /*= true*/, bool bEvent /*= true*/ )
{
    DWORD thdID = GetTickCount();       // randomize thread id
    NTSTATUS status = STATUS_SUCCESS;

    //
    // Allocate environment codecave
    //
    if (!_workerCode.valid())
    {
        auto mem = _memory.Allocate( 0x1000 );
        if (!mem)
            return mem.status;

        _workerCode = std::move( mem.result() );
    }

    if (!_userData.valid())
    {
        auto mem = _memory.Allocate( 0x4000, PAGE_READWRITE );
        if (!mem)
            return mem.status;

        _userData = std::move( mem.result() );
    }

    if (!_userCode.valid())
    {
        auto mem = _memory.Allocate( 0x1000 );
        if (!mem)
            return mem.status;

        _userCode = std::move( mem.result() );
    }

    // Create RPC thread
    if (bThread)
    {
        auto thd = CreateWorkerThread();
        if (!thd)
            return thd.status;

        thdID = thd.result();
    }

    // Create RPC sync event
    if (bEvent)
        status = CreateAPCEvent( thdID );

    return status;
}

/// <summary>
/// Create worker RPC thread
/// </summary>
/// <returns>Thread ID</returns>
call_result_t<DWORD> RemoteExec::CreateWorkerThread()
{
    auto a = AsmFactory::GetAssembler( _proc.core().isWow64() );
    asmjit::Label l_loop = (*a)->newLabel();

    //
    // Create execution thread
    //
    if(!_hWorkThd->valid())
    {
        /*if (_proc.barrier().type == wow_64_32)
        {
            a->SwitchTo64();

            // Align stack on 16 byte boundary
            (*a)->and_( (*a)->zsp, -16 );

            // Allocate new x64 activation stack
            auto createActStack = _mods.GetNtdllExport( "RtlAllocateActivationContextStack", mt_mod64 );
            if(createActStack)
            {
                a->GenCall( createActStack->procAddress, { _userData.ptr() + 0x3000 } );
                (*a)->mov( (*a)->zax, _userData.ptr() + 0x3000 );
                (*a)->mov( (*a)->zax, (*a)->intptr_ptr( (*a)->zax ) );

                (*a)->mov( (*a)->zdx, asmjit::host::dword_ptr_abs( 0x18 ).setSegment( asmjit::host::fs ) );
                (*a)->mov( (*a)->intptr_ptr( (*a)->zdx, 0x2c8 ), (*a)->zax );
            }
        }*/          

        auto ntdll = _mods.GetModule( L"ntdll.dll", Sections );
        auto proc = _mods.GetExport( ntdll, "NtDelayExecution" );
        auto pExitThread = _mods.GetExport( ntdll, "NtTerminateThread" );
        if (!proc || !pExitThread)
            return proc.status ? proc.status : pExitThread.status;

        /*
            for(;;)
                SleepEx(5, TRUE);

            ExitThread(SetEvent(m_hWaitEvent));
        */
        (*a)->bind( l_loop );
        a->GenCall( proc->procAddress, { TRUE, _workerCode.ptr() } );
        (*a)->jmp( l_loop );

        a->ExitThreadWithStatus( pExitThread->procAddress, _userData.ptr() );

        // Write code into process
        LARGE_INTEGER liDelay = { { 0 } };
        liDelay.QuadPart = -10 * 1000 * 5;

        _workerCode.Write( 0, liDelay );
        _workerCode.Write( sizeof(LARGE_INTEGER), (*a)->getCodeSize(), (*a)->make() );

        auto thd = _threads.CreateNew( _workerCode.ptr() + sizeof( LARGE_INTEGER ), _userData.ptr()/*, HideFromDebug*/ );
        if (!thd)
            return thd.status;

        _hWorkThd = std::move( thd.result() );
    }

    return _hWorkThd->id();
}


/// <summary>
/// Create event to synchronize APC procedures
/// </summary>
/// <param name="threadID">The thread identifier.</param>
/// <returns>Status code</returns>
NTSTATUS RemoteExec::CreateAPCEvent( DWORD threadID )
{         
    NTSTATUS status = STATUS_SUCCESS;

    if(_hWaitEvent == NULL)
    {
        auto a = AsmFactory::GetAssembler( _proc.core().isWow64() );

        wchar_t pEventName[128] = { 0 };
        uint64_t dwResult = NULL;
        size_t len = sizeof(pEventName);
        OBJECT_ATTRIBUTES obAttr = { 0 };
        UNICODE_STRING ustr = { 0 };

        // Event name
        swprintf_s( pEventName, ARRAYSIZE( pEventName ), L"\\BaseNamedObjects\\_MMapEvent_0x%x_0x%x", threadID, GetTickCount() );

        wchar_t* szStringSecurityDis = L"S:(ML;;NW;;;LW)D:(A;;GA;;;S-1-15-2-1)(A;;GA;;;WD)";
        PSECURITY_DESCRIPTOR pDescriptor = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW( szStringSecurityDis, SDDL_REVISION_1, &pDescriptor, NULL );

        // Prepare Arguments
        ustr.Length = static_cast<USHORT>(wcslen( pEventName ) * sizeof(wchar_t));
        ustr.MaximumLength = static_cast<USHORT>(len);
        ustr.Buffer = pEventName;

        obAttr.ObjectName = &ustr;
        obAttr.Length = sizeof(obAttr);
        obAttr.SecurityDescriptor = pDescriptor;

        auto pOpenEvent = _mods.GetNtdllExport( "NtOpenEvent", mt_default, Sections );
        if (!pOpenEvent)
            return pOpenEvent.status;

        status = SAFE_NATIVE_CALL( NtCreateEvent, &_hWaitEvent, EVENT_ALL_ACCESS, &obAttr, 0, static_cast<BOOLEAN>(FALSE) );
        if(pDescriptor)
            LocalFree( pDescriptor );

        if (!NT_SUCCESS( status ))
            return status;

        auto fillAttributes = [this, len, pEventName]( auto& obAttr, auto& ustr )
        {
            ustr.Buffer = static_cast<decltype(ustr.Buffer)>(this->_userData.ptr() + ARGS_OFFSET + sizeof( obAttr ) + sizeof( ustr ));
            ustr.Length = static_cast<USHORT>(wcslen( pEventName ) * sizeof( wchar_t ));
            ustr.MaximumLength = static_cast<USHORT>(len);

            obAttr.Length = sizeof( obAttr );
            obAttr.ObjectName = static_cast<decltype(obAttr.ObjectName)>(this->_userData.ptr() + ARGS_OFFSET + sizeof( obAttr ));

            NTSTATUS status  = this->_userData.Write( ARGS_OFFSET, obAttr );
            status |= this->_userData.Write( ARGS_OFFSET + sizeof( obAttr ), ustr );
            status |= this->_userData.Write( ARGS_OFFSET + sizeof( obAttr ) + sizeof( ustr ), len, pEventName );
            return status;
        };

        if (_proc.core().isWow64())
        {
            _OBJECT_ATTRIBUTES32 obAttr32 = { 0 };
            _UNICODE_STRING32 ustr32 = { 0 };
            status = fillAttributes( obAttr32, ustr32 );
        }
        else
        {
            _OBJECT_ATTRIBUTES64 obAttr64 = { 0 };
            _UNICODE_STRING64 ustr64 = { 0 };
            status = fillAttributes( obAttr64, ustr64 );
        }

        if (!NT_SUCCESS( status ))
            return status;

        a->GenCall( pOpenEvent->procAddress, {
            _userData.ptr() + EVENT_OFFSET, 
            EVENT_MODIFY_STATE | SYNCHRONIZE,
            _userData.ptr() + ARGS_OFFSET 
        } );

        // Save status
        (*a)->mov( (*a)->zdx, _userData.ptr() + ERR_OFFSET );
        (*a)->mov( asmjit::host::dword_ptr( (*a)->zdx ), asmjit::host::eax );
        (*a)->ret();

        status = ExecInNewThread( (*a)->make(), (*a)->getCodeSize(), dwResult, NoSwitch );
        if (NT_SUCCESS( status ))
            status = _userData.Read<NTSTATUS>( ERR_OFFSET, -1 );
    }

    return status;
}

/// <summary>
/// Generate assembly code for remote call.
/// </summary>
/// <param name="a">Underlying assembler object</param>
/// <param name="pfn">Remote function pointer</param>
/// <param name="args">Function arguments</param>
/// <param name="cc">Calling convention</param>
/// <param name="retType">Return type</param>
/// <returns>Status code</returns>
NTSTATUS RemoteExec::PrepareCallAssembly( 
    IAsmHelper& a, 
    ptr_t pfn,
    std::vector<AsmVariant>& args,
    eCalligConvention cc,
    eReturnType retType
    )
{
    uintptr_t data_offset = ARGS_OFFSET;

    // Invalid calling convention
    if (cc < cc_cdecl || cc > cc_fastcall)
        return STATUS_INVALID_PARAMETER_3;

    // Copy structures and strings
    for (auto& arg : args)
    {
        // Transform 64 bit imm values
        if (arg.type == AsmVariant::imm && arg.size > sizeof( uint32_t ) && a.assembler()->getArch() == asmjit::kArchX86)
        {
            arg.type = AsmVariant::dataStruct;
            arg.buf.reset( new uint8_t[arg.size] );
            memcpy( arg.buf.get(), &arg.imm_val64, arg.size );
            arg.imm_val64 = reinterpret_cast<uint64_t>(arg.buf.get());
        }

        if (arg.type == AsmVariant::dataStruct || arg.type == AsmVariant::dataPtr)
        {
            _userData.Write( data_offset, arg.size, reinterpret_cast<const void*>(arg.imm_val) );
            arg.new_imm_val = _userData.ptr() + data_offset;

            // Add some padding after data
            data_offset += arg.size + 0x10;
        }
    }

    // Insert hidden variable if return type is struct.
    // This variable contains address of buffer in which return value is copied
    if (retType == rt_struct)
    {
        args.emplace( args.begin(), AsmVariant( _userData.ptr<uintptr_t>() + ARGS_OFFSET ) );
        args.front().new_imm_val = args.front().imm_val;
        args.front().type = AsmVariant::structRet;
    }
        
    a.GenPrologue();
    a.GenCall( pfn, args, cc );

    // Retrieve result from XMM0 or ST0
    if (retType == rt_float || retType == rt_double)
    {
        a->mov( a->zax, _userData.ptr<size_t>() + RET_OFFSET );

#ifdef USE64
        if (retType == rt_double)
            a->movsd( asmjit::Mem( a->zax, 0 ), asmjit::host::xmm0 );
        else
            a->movss( asmjit::Mem( a->zax, 0 ), asmjit::host::xmm0 );
#else
        a->fstp( asmjit::Mem( a->zax, 0, retType * sizeof( float ) ) );
#endif
    }

    AddReturnWithEvent( a, mt_default, retType );
    a.GenEpilogue();

    return STATUS_SUCCESS;
}

/// <summary>
/// Copy executable code into remote codecave for future execution
/// </summary>
/// <param name="pCode">Code to copy</param>
/// <param name="size">Code size</param>
/// <returns>Status</returns>
NTSTATUS RemoteExec::CopyCode( PVOID pCode, size_t size )
{
    if (!_userCode.valid())
    {
        auto mem = _memory.Allocate( size );
        if (!mem)
            return mem.status;

        _userCode = std::move( mem.result() );
    }

    // Reallocate for larger code
    if (size > _userCode.size())
    {
        auto res = _userCode.Realloc( size );
        if (!res)
            return res.status;
    }

    return _userCode.Write( 0, size, pCode );
}

/// <summary>
/// Generate return from function with event synchronization
/// </summary>
/// <param name="a">Target assembly helper</param>
/// <param name="mt">32/64bit loader</param>
/// <param name="retType">Function return type</param>
/// <param name="retOffset">Return value offset</param>
void RemoteExec::AddReturnWithEvent(
    IAsmHelper& a,
    eModType mt /*= mt_default*/,
    eReturnType retType /*= rt_int32 */,
    uint32_t retOffset /*= RET_OFFSET*/ 
    )
{
    // Allocate block if missing
    if (!_userData.valid())
    {
        auto mem = _memory.Allocate( 0x4000, PAGE_READWRITE );
        if (!mem)
            return;

        _userData = std::move( mem.result() );
    }

    ptr_t ptr = _userData.ptr();
    auto pSetEvent = _proc.modules().GetNtdllExport( "NtSetEvent", mt );
    if(pSetEvent)
        a.SaveRetValAndSignalEvent( pSetEvent->procAddress, ptr + retOffset, ptr + EVENT_OFFSET, ptr + ERR_OFFSET, retType );
}


/// <summary>
/// Terminate existing worker thread
/// </summary>
void RemoteExec::TerminateWorker()
{
    // Close event
    if(_hWaitEvent)
    {
        CloseHandle( _hWaitEvent );
        _hWaitEvent = NULL;
    }

    // Stop thread
    if(_hWorkThd->valid())
    {
        _hWorkThd->Terminate();
        _hWorkThd->Join();
        _hWorkThd->Close();
        _workerCode.Free();
    }
}

/// <summary>
/// Reset instance
/// </summary>
void RemoteExec::reset()
{
    TerminateWorker();

    _userCode.Reset();
    _userData.Reset();
    _workerCode.Reset();

    _apcPatched = false;
}

}