// HookDLL.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "hooking.h"

// Globals
extern decltype(NtQuerySystemInformation)* TrueNtQuerySystemInformation;
extern decltype(NtCreateFile)* TrueNtCreateFile;
extern decltype(NtReadFile)* TrueNtReadFile;
extern decltype(NtWriteFile)* TrueNtWriteFile;
extern decltype(NtDeleteFile)* TrueNtDeleteFile;
extern decltype(LdrLoadDll)* TrueLdrLoadDll;
extern decltype(LdrGetProcedureAddress)* TrueLdrGetProcedureAddress;
extern decltype(NtAllocateVirtualMemory)* TrueNtAllocateVirtualMemory;
extern decltype(NtProtectVirtualMemory)* TrueNtProtectVirtualMemory;
extern decltype(NtQueryVirtualMemory)* TrueNtQueryVirtualMemory;
extern decltype(NtReadVirtualMemory)* TrueNtReadVirtualMemory;
extern decltype(NtWriteVirtualMemory)* TrueNtWriteVirtualMemory;
extern decltype(NtFreeVirtualMemory)* TrueNtFreeVirtualMemory;
extern decltype(NtMapViewOfSection)* TrueNtMapViewOfSection;
extern decltype(NtDelayExecution)* TrueNtDelayExecution;
extern pfnMoveFileWithProgressTransactedW TrueMoveFileWithProgressTransactedW;
extern decltype(NtOpenKey)* TrueNtOpenKey;
extern decltype(NtOpenKeyEx)* TrueNtOpenKeyEx;
extern decltype(NtCreateKey)* TrueNtCreateKey;
extern decltype(NtQueryValueKey)* TrueNtQueryValueKey;
extern decltype(NtDeleteKey)* TrueNtDeleteKey;
extern decltype(NtDeleteValueKey)* TrueNtDeleteValueKey;
extern decltype(NtCreateUserProcess)* TrueNtCreateUserProcess;
extern decltype(NtOpenProcess)* TrueNtOpenProcess;
extern decltype(NtCreateThread)* TrueNtCreateThread;
extern decltype(NtCreateThreadEx)* TrueNtCreateThreadEx;
extern decltype(NtResumeThread)* TrueNtResumeThread;
extern decltype(NtSuspendThread)* TrueNtSuspendThread;
extern decltype(NtTerminateProcess)* TrueNtTerminateProcess;
extern decltype(NtUnmapViewOfSection)* TrueNtUnmapViewOfSection;
extern decltype(RtlDecompressBuffer)* TrueRtlDecompressBuffer;

__vsnwprintf_fn_t _vsnwprintf = nullptr;
__snwprintf_fn_t _snwprintf = nullptr;
strlen_fn_t _strlen = nullptr;

//
// ETW provider GUID and global provider handle.
// GUID:
//   {a4b4ba50-a667-43f5-919b-1e52a6d69bd5}
//

GUID ProviderGuid = {
  0xa4b4ba50, 0xa667, 0x43f5, { 0x91, 0x9b, 0x1e, 0x52, 0xa6, 0xd6, 0x9b, 0xd5 }
};


REGHANDLE ProviderHandle;
#define ATTACH(x)       DetAttach(&(PVOID&)True##x,Hook##x, #x)



VOID WaitForMe(LONGLONG delayInMillis) {
	LARGE_INTEGER DelayInterval;
	DelayInterval.QuadPart = -delayInMillis;
	NtDelayExecution(FALSE, &DelayInterval);
}


CRITICAL_SECTION DbgHelpLock;


VOID GetStackWalk()
{
	PCONTEXT InitialContext = NULL;
	STACKTRACE StackTrace;
	UINT MaxFrames = 50;
	STACKFRAME64 StackFrame;
	DWORD MachineType = 0;
	CONTEXT Context = {};

	if (InitialContext == NULL)
	{
		//
		// Use current context.
		//
		// N.B. GetThreadContext cannot be used on the current thread.
		// Capture own context - on i386, there is no API for that.
		//
#ifdef _M_IX86
		ZeroMemory(&Context, sizeof(CONTEXT));

		Context.ContextFlags = CONTEXT_CONTROL;

		//
		// Those three registers are enough.
		//
		__asm
		{
		Label:
			mov[Context.Ebp], ebp;
			mov[Context.Esp], esp;
			mov eax, [Label];
			mov[Context.Eip], eax;
		}
#else
		RtlCaptureContext(&Context);
#endif  
	}
	else
	{
		CopyMemory(&Context, InitialContext, sizeof(CONTEXT));
	}
	//
	// Set up stack frame.
	//
	ZeroMemory(&StackFrame, sizeof(STACKFRAME64));

#ifdef _M_IX86
	MachineType = IMAGE_FILE_MACHINE_I386;
	StackFrame.AddrPC.Offset = Context.Eip;
	StackFrame.AddrPC.Mode = AddrModeFlat;
	StackFrame.AddrFrame.Offset = Context.Ebp;
	StackFrame.AddrFrame.Mode = AddrModeFlat;
	StackFrame.AddrStack.Offset = Context.Esp;
	StackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
	MachineType = IMAGE_FILE_MACHINE_AMD64;
	StackFrame.AddrPC.Offset = Context.Rip;
	StackFrame.AddrPC.Mode = AddrModeFlat;
	StackFrame.AddrFrame.Offset = Context.Rsp;
	StackFrame.AddrFrame.Mode = AddrModeFlat;
	StackFrame.AddrStack.Offset = Context.Rsp;
	StackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
	MachineType = IMAGE_FILE_MACHINE_IA64;
	StackFrame.AddrPC.Offset = Context.StIIP;
	StackFrame.AddrPC.Mode = AddrModeFlat;
	StackFrame.AddrFrame.Offset = Context.IntSp;
	StackFrame.AddrFrame.Mode = AddrModeFlat;
	StackFrame.AddrBStore.Offset = Context.RsBSP;
	StackFrame.AddrBStore.Mode = AddrModeFlat;
	StackFrame.AddrStack.Offset = Context.IntSp;
	StackFrame.AddrStack.Mode = AddrModeFlat;
#else
#error "Unsupported platform"
#endif

	//
	// Allocate a buffer large enough to hold the symbol information on the stack and get 
	// a pointer to the buffer. We also have to set the size of the symbol structure itself
	// and the number of bytes reserved for the name.
	// 

	BYTE buffer[sizeof(SYMBOL_INFO) + 1024];
	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = 1024;

	UINT FrameCount = 0;
	DWORD64 displacement = 0;
	WCHAR pszFilename[MAX_PATH + 1];
	WCHAR* ModuleName = NULL;
	DWORD dwResult = 0;
	
	//
	// Dbghelp is is singlethreaded, so acquire a lock.
	//
	// Note that the code assumes that 
	// SymInitialize( GetCurrentProcess(), NULL, TRUE ) has 
	// already been called.
	//
	EnterCriticalSection(&DbgHelpLock);

	while (FrameCount < MaxFrames)
	{
		if (!StackWalk64(MachineType, GetCurrentProcess(), GetCurrentThread(),
			&StackFrame, MachineType == IMAGE_FILE_MACHINE_I386 ? NULL : &Context,
			NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
		{
			//
			// Maybe it failed, maybe we have finished walking the stack.
			//
			break;
		}

		if (StackFrame.AddrPC.Offset != 0)
		{
			//
			// Valid frame.
			//
			//StackTrace->Frames[StackTrace->FrameCount++] = StackFrame.AddrPC.Offset;
			FrameCount++;

			dwResult = GetMappedFileNameW(GetCurrentProcess(), (LPVOID)StackFrame.AddrPC.Offset, pszFilename, MAX_PATH);
			if (dwResult) {
				ModuleName = (WCHAR*)FindFileName(pszFilename);
			}
			else
			{
				ModuleName = (WCHAR*)L"N/A";
			}
		
			//
			// Retrieves symbol information for the specified address.
			//
			if (SymFromAddr(GetCurrentProcess(), StackFrame.AddrPC.Offset, &displacement, pSymbol)) {
					LogMessage(L"Module: %s, SymbolName:%ws, SymbolAddress:0x%08llx, Offset:0x%p",
						ModuleName, MultiByteToWide(pSymbol->Name),
						pSymbol->Address, StackFrame.AddrPC.Offset);
			}
			else 
			{
				LogMessage(L"Module: %s, SymbolName:N/A, SymbolAddress: N/A, Offset:0x%p",
					ModuleName,  StackFrame.AddrPC.Offset);
			}
		}
		else
		{
			//
			// Base reached.
			//
			break;
		}
	}

	LeaveCriticalSection(&DbgHelpLock);
}



PTEB GetCurrentTeb()
{
	// wow64 teb
	//static constexpr ULONG ThreadBasicInformation = 0;
	//THREAD_BASIC_INFORMATION tbi{};
	//_NtQueryInformationThread(NtCurrentThread(), (THREADINFOCLASS)(ThreadBasicInformation),
	//	&tbi, sizeof(tbi), nullptr);
	//return (PTEB)(tbi.TebBaseAddress);
	return NtCurrentTeb();
}


PULONG_PTR GetCurrentNestingLevelPtr()
{
	return (PULONG_PTR)(&GetCurrentTeb()->TlsSlots[63]);
}


VOID ReleaseHookGuard()
{
	PULONG_PTR level = GetCurrentNestingLevelPtr();
	(*level)--;
}


BOOL IsInsideHook() {
	PULONG_PTR level = GetCurrentNestingLevelPtr();
	if (*level == 0) {
		(*level)++;
		return TRUE;
	}
	return FALSE;
}


LONG CheckDetourAttach(LONG err)
{
	switch (err)
	{
	case ERROR_INVALID_BLOCK:		/*printf("ERROR_INVALID_BLOCK: The function referenced is too small to be detoured.");*/ break;
	case ERROR_INVALID_HANDLE:		/*printf("ERROR_INVALID_HANDLE: The ppPointer parameter is null or points to a null pointer.");*/ break;
	case ERROR_INVALID_OPERATION:/*	printf("ERROR_INVALID_OPERATION: No pending transaction exists."); */break;
	case ERROR_NOT_ENOUGH_MEMORY:	/*printf("ERROR_NOT_ENOUGH_MEMORY: Not enough memory exists to complete the operation.");*/ break;
	case NO_ERROR: break;
	default: /*printf("CheckDetourAttach failed with unknown error code.");*/ break;
	}
	return err;

}


static const char *DetRealName(const char* psz)
{
	const char * pszBeg = psz;
	// Move to end of name.
	while (*psz) {
		psz++;
	}
	// Move back through A-Za-z0-9 names.
	while (psz > pszBeg &&
		((psz[-1] >= 'A' && psz[-1] <= 'Z') ||
		(psz[-1] >= 'a' && psz[-1] <= 'z') ||
			(psz[-1] >= '0' && psz[-1] <= '9'))) {
		psz--;
	}
	return psz;
}


VOID DetAttach(PVOID *ppvReal, PVOID pvMine, const char* psz)
{
	PVOID pvReal = NULL;
	if (ppvReal == NULL) {
		ppvReal = &pvReal;
	}

	LONG l = DetourAttach(ppvReal, pvMine);
	if (l != 0) {
		WCHAR Buffer[128];
		_snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
			L"DetourAttach failed: `%s': error %d", DetRealName(psz), l);
		EtwEventWriteString(ProviderHandle, 0, 0, Buffer);

		//Decode((PBYTE)*ppvReal, 3);
	}
}


VOID DetDetach(PVOID *ppvReal, PVOID pvMine, PCHAR psz)
{
	LONG l = DetourDetach(ppvReal, pvMine);
	if (l != 0) {
		WCHAR Buffer[128];
		_snwprintf(Buffer, RTL_NUMBER_OF(Buffer),
			L"Detach failed: `%s': error %d", DetRealName(psz), l);
		EtwEventWriteString(ProviderHandle, 0, 0, Buffer);
	}
}


PVOID GetAPIAddress(PSTR FunctionName, PWSTR ModuleName) {


	NTSTATUS Status;

	ANSI_STRING RoutineName;
	RtlInitAnsiString(&RoutineName, FunctionName);

	UNICODE_STRING ModulePath;
	RtlInitUnicodeString(&ModulePath, ModuleName);

	HANDLE ModuleHandle = NULL;
	Status = LdrGetDllHandle(NULL, 0, &ModulePath, &ModuleHandle);
	if (!NT_SUCCESS(Status)) {
		return NULL;
	}

	PVOID Address;
	Status = LdrGetProcedureAddress(ModuleHandle, &RoutineName, 0, &Address);
	if (!NT_SUCCESS(Status)) {
		return NULL;
	}

	return Address;
}


VOID SetupHook()
{

	//
	// Register ETW provider.
	//

	EtwEventRegister(&ProviderGuid,
		NULL,
		NULL,
		&ProviderHandle);

	//
	// Set up the symbol options so that we can gather information from the current
	// executable's PDB files, as well as the Microsoft symbol servers.  We also want
	// to undecorate the symbol names we're returned.  If you want, you can add other
	// symbol servers or paths via a semi-colon separated list in SymInitialized.
	//

	HANDLE hProcess = NtCurrentProcess();
	if (!SymInitialize(hProcess, NULL, TRUE)) {
		LogMessage(L"SymInitialize returned error : %d", GetLastError());
		return;
	}
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);


	//
	// Begin a new transaction for attaching or detaching detours.
	//

	if (DetourTransactionBegin() != NO_ERROR) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"DetourTransactionBegin() failed");
		return;
	}

	// 
	// Enlist a thread for update in the current transaction.
	//

	if (DetourUpdateThread(NtCurrentThread()) != NO_ERROR) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"DetourUpdateThread() failed");
		return;
	}

	//
	// Save real API addresses.
	//

	TrueLdrLoadDll = LdrLoadDll;
	TrueLdrGetProcedureAddress = LdrGetProcedureAddress;
	TrueNtCreateFile = NtCreateFile;
	TrueNtWriteFile = NtWriteFile;
	TrueNtReadFile = NtReadFile;
	TrueNtDeleteFile = NtDeleteFile;
	TrueNtDelayExecution = NtDelayExecution;
	TrueNtProtectVirtualMemory = NtProtectVirtualMemory;
	TrueNtQueryVirtualMemory = NtQueryVirtualMemory;
	TrueNtReadVirtualMemory = NtReadVirtualMemory;
	TrueNtWriteVirtualMemory = NtWriteVirtualMemory;
	TrueNtFreeVirtualMemory = NtFreeVirtualMemory;
	TrueNtMapViewOfSection = NtMapViewOfSection;
	TrueNtUnmapViewOfSection = NtUnmapViewOfSection;
	TrueNtAllocateVirtualMemory = NtAllocateVirtualMemory;
	TrueNtProtectVirtualMemory = NtProtectVirtualMemory;
	TrueNtOpenKey = NtOpenKey;
	TrueNtOpenKeyEx = NtOpenKeyEx;
	TrueNtCreateKey = NtCreateKey;
	TrueNtQueryValueKey = NtQueryValueKey;
	TrueNtDeleteKey = NtDeleteKey;
	TrueNtDeleteValueKey = NtDeleteValueKey;
	TrueNtCreateUserProcess = NtCreateUserProcess;
	TrueNtCreateThread = NtCreateThread;
	TrueNtCreateThreadEx = NtCreateThreadEx;
	TrueNtResumeThread = NtResumeThread;
	TrueNtSuspendThread = NtSuspendThread;
	TrueNtOpenProcess = NtOpenProcess;
	TrueNtTerminateProcess = NtTerminateProcess;
	TrueRtlDecompressBuffer = RtlDecompressBuffer;

	//
	// Resolve the ones not exposed by ntdll.
	//

	TrueMoveFileWithProgressTransactedW = (pfnMoveFileWithProgressTransactedW)GetAPIAddress(
		(PSTR)"MoveFileWithProgressTransactedW", (PWSTR)L"kernelbase.dll");
	if (TrueMoveFileWithProgressTransactedW == NULL) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"MoveFileWithProgressTransactedW() is NULL");
	}
	_vsnwprintf = (__vsnwprintf_fn_t)GetAPIAddress((PSTR)"_vsnwprintf", (PWSTR)L"ntdll.dll");
	if (_vsnwprintf == NULL) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"_vsnwprintf() is NULL");
	}
	_snwprintf = (__snwprintf_fn_t)GetAPIAddress((PSTR)"_snwprintf", (PWSTR)L"ntdll.dll");
	if (_vsnwprintf == NULL) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"_snwprintf() is NULL");
	}

	InitializeCriticalSection(&DbgHelpLock);

	//
	// Detours the APIs.
	//

	ATTACH(LdrLoadDll);
	ATTACH(LdrGetProcedureAddress);
	//ATTACH(NtDelayExecution);
	//ATTACH(NtProtectVirtualMemory);
	//ATTACH(NtQueryVirtualMemory);
	//ATTACH(NtReadVirtualMemory);
	//ATTACH(NtWriteVirtualMemory);
	//ATTACH(NtFreeVirtualMemory);
	//ATTACH(NtMapViewOfSection);
	//ATTACH(NtAllocateVirtualMemory);
	//ATTACH(NtProtectVirtualMemory);
	//ATTACH(MoveFileWithProgressTransactedW);
	ATTACH(NtCreateFile);
	//ATTACH(NtOpenKey);
	//ATTACH(NtOpenKeyEx);
	//ATTACH(NtCreateKey);
	//ATTACH(NtQueryValueKey);
	//ATTACH(NtDeleteKey);
	//ATTACH(NtDeleteValueKey);
	//ATTACH(NtCreateUserProcess);
	//ATTACH(NtCreateUserProcess);
	//ATTACH(NtCreateThread);
	//ATTACH(NtCreateThreadEx);
	//ATTACH(NtSuspendThread);
	//ATTACH(NtResumeThread);
	//ATTACH(NtOpenProcess);
	//ATTACH(NtTerminateProcess);
	//ATTACH(NtReadFile);
	//ATTACH(NtWriteFile);
	//ATTACH(NtDeleteFile);
	//ATTACH(NtUnmapViewOfSection);
	//ATTACH(RtlDecompressBuffer);

	//
	// Commit the current transaction.
	//

	LONG error = DetourTransactionCommit();
	if (error == NO_ERROR) {
		EtwEventWriteString(ProviderHandle, 0, 0, L"Detours Attached");
	}
	else {
		EtwEventWriteString(ProviderHandle, 0, 0, L"Detours Attached failed");
	}

	EtwEventWriteString(ProviderHandle, 0, 0, L"Hook DLL Loaded");

}

VOID Unhook()
{
	DetourTransactionBegin();
	DetourUpdateThread(NtCurrentThread());

	//DetourDetach(&(PVOID&)TrueLdrLoadDll, HookLdrLoadDll);
	//DetourDetach(&(PVOID&)TrueLdrGetProcedureAddress, HookLdrGetProcedureAddress);
	//DetourDetach(&(PVOID&)TrueNtCreateFile, HookNtCreateFile);
	//DetourDetach(&(PVOID&)TrueMoveFileWithProgressTransactedW, HookMoveFileWithProgressTransactedW);
	//DetourDetach(&(PVOID&)TrueNtAllocateVirtualMemory, HookNtAllocateVirtualMemory);

	DetourTransactionCommit();
}



WCHAR* MultiByteToWide(CHAR* lpMultiByteStr)
{
	//int Size = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, szSource, strlen(szSource), NULL, 0);
	//WCHAR *wszDest = reinterpret_cast<WCHAR*>(RtlAllocateHeap(RtlProcessHeap(), 0, Size));
	//SecureZeroMemory(wszDest, Size);
	//MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, szSource, strlen(szSource), wszDest, Size);

		/* Get the required size */
	size_t iNumChars = strlen(lpMultiByteStr);

	/* Allocate new wide string */
	SIZE_T Size = (1 + iNumChars) * sizeof(WCHAR);

	WCHAR *lpWideCharStr = reinterpret_cast<WCHAR*>(RtlAllocateHeap(RtlProcessHeap(), 0, Size));
	WCHAR *It;
	It = lpWideCharStr;
	if (lpWideCharStr) {
		SecureZeroMemory(lpWideCharStr, Size);
		while (iNumChars) {

			*lpWideCharStr = *lpMultiByteStr;
			lpWideCharStr++;
			lpMultiByteStr++;
			iNumChars--;
		}

	}
	return It;

	//return wszDest;
}