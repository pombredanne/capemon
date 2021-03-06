/*
CAPE - Config And Payload Extraction
Copyright(C) 2015, 2016 Context Information Security. (kevin.oreilly@contextis.com)

This program is free software : you can redistribute it and / or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <tchar.h>
#include <windows.h>
#include <assert.h>
#include <Aclapi.h>
#include "Debugger.h"
#include "..\config.h"
#include "..\pipe.h"

#define PIPEBUFSIZE 512

// eflags register
#define FL_TF           0x00000100      // Trap flag
#define FL_RF           0x00010000      // Resume flag

//
// debug register DR7 bit fields
//
typedef struct _DR7 
{
    DWORD L0   : 1;    //Local enable bp0
    DWORD G0   : 1;    //Global enable bp0
    DWORD L1   : 1;    //Local enable bp1
    DWORD G1   : 1;    //Global enable bp1
    DWORD L2   : 1;    //Local enable bp2	
    DWORD G2   : 1;    //Global enable bp2
    DWORD L3   : 1;    //Local enable bp3
    DWORD G3   : 1;    //Global enable bp3
    DWORD LE   : 1;    //Local Enable
    DWORD GE   : 1;    //Global Enable
    DWORD PAD1 : 3;
    DWORD GD   : 1;    //General Detect Enable
    DWORD PAD2 : 1;
    DWORD PAD3 : 1;
    DWORD RWE0 : 2;    //Read/Write/Execute bp0
    DWORD LEN0 : 2;    //Length bp0
    DWORD RWE1 : 2;    //Read/Write/Execute bp1
    DWORD LEN1 : 2;    //Length bp1
    DWORD RWE2 : 2;    //Read/Write/Execute bp2
    DWORD LEN2 : 2;    //Length bp2
    DWORD RWE3 : 2;    //Read/Write/Execute bp3
    DWORD LEN3 : 2;    //Length bp3
} DR7, *PDR7;

#define NUMBER_OF_DEBUG_REGISTERS       4
#define MAX_DEBUG_REGISTER_DATA_SIZE    4
#define DEBUG_REGISTER_DATA_SIZES       {1, 2, 4}
#define DEBUG_REGISTER_LENGTH_MASKS     {0xFFFFFFFF, 0, 1, 0xFFFFFFFF, 3}

typedef struct _LSA_UNICODE_STRING {
	USHORT Length;
	USHORT MaximumLength;
	PWSTR  Buffer;
} LSA_UNICODE_STRING, *PLSA_UNICODE_STRING, UNICODE_STRING, *PUNICODE_STRING;

typedef struct _INJECT_STRUCT {
	ULONG_PTR LdrLoadDllAddress;
	UNICODE_STRING DllName;
	HANDLE OutHandle;
} INJECT_STRUCT, *PINJECT_STRUCT;

DWORD LengthMask[MAX_DEBUG_REGISTER_DATA_SIZE + 1] = DEBUG_REGISTER_LENGTH_MASKS;

DWORD MainThreadId;
struct ThreadBreakpoints *MainThreadBreakpointList;
LPTOP_LEVEL_EXCEPTION_FILTER OriginalExceptionHandler;
SINGLE_STEP_HANDLER SingleStepHandler;
GUARD_PAGE_HANDLER GuardPageHandler;
HANDLE hParentPipe;

extern ULONG_PTR g_our_dll_base;
extern DWORD g_our_dll_size;
extern LONG WINAPI cuckoomon_exception_handler(__in struct _EXCEPTION_POINTERS *ExceptionInfo);

extern unsigned int address_is_in_stack(DWORD Address);
extern BOOL WoW64fix(void);
extern BOOL WoW64PatchBreakpoint(unsigned int Register);
extern BOOL WoW64UnpatchBreakpoint(unsigned int Register);
extern DWORD MyGetThreadId(HANDLE hThread);

extern void DoOutputDebugString(_In_ LPCTSTR lpOutputString, ...);
extern void DoOutputErrorString(_In_ LPCTSTR lpOutputString, ...);

typedef void (WINAPI *PWIN32ENTRY)();
PWIN32ENTRY OEP;

void DebugOutputThreadBreakpoints();
BOOL ResumeAfterExecutionBreakpoint(PCONTEXT Context);
BOOL SetSingleStepMode(PCONTEXT Context, PVOID Handler);
BOOL ClearSingleStepMode(PCONTEXT Context);
unsigned int TrapIndex;

//**************************************************************************************
BOOL IsInGuardPages(PVOID Address)
//**************************************************************************************
{
    PGUARDPAGES CurrentGuardPages = GuardPageList;
    
    if (GuardPageList == NULL)
        return FALSE;

	while (CurrentGuardPages)
	{
        if ((DWORD_PTR)Address >= (DWORD_PTR)CurrentGuardPages->BaseAddress && (DWORD_PTR)Address < ((DWORD_PTR)CurrentGuardPages->BaseAddress + (DWORD_PTR)CurrentGuardPages->RegionSize))
            return TRUE;
        
        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}
    
	return FALSE;
}

//**************************************************************************************
PGUARDPAGES GetGuardPages(PVOID Address)
//**************************************************************************************
{
    PGUARDPAGES CurrentGuardPages = GuardPageList;
    
    if (Address == NULL)
	{
        DoOutputDebugString("GetGuardPages: NULL passed as argument - error.\n");
        return FALSE;
	}

    if (GuardPageList == NULL)
    {
        DoOutputDebugString("GetGuardPages: failed to obtain initial guard page list.\n");
        return FALSE;
    }
    
	while (CurrentGuardPages)
	{
        if ((DWORD_PTR)Address >= (DWORD_PTR)CurrentGuardPages->BaseAddress && (DWORD_PTR)Address < ((DWORD_PTR)CurrentGuardPages->BaseAddress + (DWORD_PTR)CurrentGuardPages->RegionSize))
            return CurrentGuardPages;

        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}
    
	DoOutputDebugString("GetGuardPages: failed to find guard pages in list.\n");
    
    return NULL;
}

//**************************************************************************************
BOOL DropGuardPages(PGUARDPAGES GuardPages)
//**************************************************************************************
{
    // DEBUG
    DoOutputDebugString("DropGuardPages entry.\n");
    
    PGUARDPAGES CurrentGuardPages, PreviousGuardPages;
    
    if (GuardPages == NULL)
	{
        DoOutputDebugString("DropGuardPages: NULL passed as argument - error.\n");
        return FALSE;
	}

    PreviousGuardPages = NULL;
    
    if (GuardPageList == NULL)
	{
        DoOutputDebugString("DropGuardPages: failed to obtain initial guard page list.\n");
        return FALSE;
	}

    CurrentGuardPages = GuardPageList;
    
	while (CurrentGuardPages)
	{
        DoOutputDebugString("DropGuardPages: CurrentGuardPages 0x%x.\n", CurrentGuardPages);
        
        if (CurrentGuardPages == GuardPages)
        {
            DoOutputDebugString("DropGuardPages: About to unlink.\n");
            // Unlink this from the list and free the memory
            if (PreviousGuardPages && CurrentGuardPages->NextGuardPages)
            {
                DoOutputDebugString("DropGuardPages: removed pages 0x%x-0x%x from guard page list.\n", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
                PreviousGuardPages->NextGuardPages = CurrentGuardPages->NextGuardPages;
            }
            else if (PreviousGuardPages && CurrentGuardPages->NextGuardPages == NULL)
            {
                DoOutputDebugString("DropGuardPages: removed pages 0x%x-0x%x from the end of the guard page list.\n", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
                PreviousGuardPages->NextGuardPages = NULL;
            }
            else if (!PreviousGuardPages)
            {
                DoOutputDebugString("DropGuardPages: removed pages 0x%x-0x%x from the head of the guard page list.\n", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
                GuardPageList = NULL;
            }
            
            DoOutputDebugString("DropGuardPages: about to free the memory!\n");
            free(CurrentGuardPages);
            
            return TRUE;            
        }
        
		PreviousGuardPages = CurrentGuardPages;
        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}
    
    DoOutputDebugString("DropGuardPages: failed to find guard pages in list.\n");
    
    return FALSE;
}

//**************************************************************************************
BOOL ReinstateGuardPages(PGUARDPAGES GuardPages)
//**************************************************************************************
{
    DWORD OldProtect;
    BOOL GuardPagesFound = FALSE;
    PGUARDPAGES CurrentGuardPages = GuardPageList;
    MEMORY_BASIC_INFORMATION MemInfo;
    SIZE_T MatchingRegionSize;
    
    if (GuardPageList == NULL)
        return FALSE;

	while (CurrentGuardPages)
	{
        if (GuardPages->BaseAddress == CurrentGuardPages->BaseAddress)
            GuardPagesFound = TRUE;

        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}
   
    if (GuardPagesFound == FALSE)
    {
        DoOutputDebugString("ReinstateGuardPages: failed to locate guard page(s) in guard page list.\n");
        return FALSE;
    }

    MatchingRegionSize = VirtualQuery(GuardPages->BaseAddress, &MemInfo, GuardPages->RegionSize);
    
    if (!MatchingRegionSize)
    {
        DoOutputErrorString("ReinstateGuardPages: failed to query guard page(s) status in region 0x%x-0x%x", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
        return FALSE;
    }
    
    if (MatchingRegionSize == GuardPages->RegionSize && MemInfo.Protect & PAGE_GUARD)
    {
        DoOutputDebugString("ReinstateGuardPages: guard page(s) already set in region 0x%x-0x%x", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
        return FALSE;
    }
    
    if (!VirtualProtect(GuardPages->BaseAddress, GuardPages->RegionSize, GuardPages->Protect | PAGE_GUARD, &OldProtect))
    {
        DoOutputErrorString("ReinstateGuardPages: failed to reinstate guard page(s) on region 0x%x size 0x%x", GuardPages->BaseAddress, GuardPages->RegionSize);
        return FALSE;
    }
    
    // This is unreliable if some pages no longer have PAGE_GUARD,
    // i.e. they have been touched, while others haven't
    //if (OldProtect != GuardPages->Protect)
    //{
    //    DoOutputDebugString("ReinstateGuardPages error: inconsistency in protecton flags for region 0x%x-0x%x - OldProtect = 0x%x, GuardPages->Protect = 0x%x\n", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize, OldProtect, GuardPages->Protect);
    //}
    
    return TRUE;
}

//**************************************************************************************
BOOL DisableGuardPages(PGUARDPAGES GuardPages)
//**************************************************************************************
{
    DWORD OldProtect;
    BOOL GuardPagesFound = FALSE;
    PGUARDPAGES CurrentGuardPages = GuardPageList;
    MEMORY_BASIC_INFORMATION MemInfo;
    SIZE_T MatchingRegionSize;
    
    if (GuardPageList == NULL)
        return FALSE;

	while (CurrentGuardPages)
	{
        if (GuardPages->BaseAddress == CurrentGuardPages->BaseAddress)
            GuardPagesFound = TRUE;

        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}
   
    if (GuardPagesFound == FALSE)
    {
        DoOutputDebugString("DisableGuardPages: failed to locate guard page(s) in guard page list.\n");
        return FALSE;
    }

    MatchingRegionSize = VirtualQuery(GuardPages->BaseAddress, &MemInfo, GuardPages->RegionSize);
    
    if (!MatchingRegionSize)
    {
        DoOutputErrorString("DisableGuardPages: failed to query guard page(s) status in region 0x%x-0x%x", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
        return FALSE;
    }
    
    if (MatchingRegionSize == GuardPages->RegionSize && !(MemInfo.Protect & PAGE_GUARD))
    {
        DoOutputDebugString("DisableGuardPages: guard page(s) not set in region 0x%x-0x%x", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
        return FALSE;
    }

    if (!VirtualProtect(GuardPages->BaseAddress, GuardPages->RegionSize, GuardPages->Protect, &OldProtect))
    {
        DoOutputErrorString("DisableGuardPages: failed to disable guard page(s) on region 0x%x-0x%x", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize);
        return FALSE;
    }
    
    //if (OldProtect != (GuardPages->Protect & PAGE_GUARD))
    //{
    //    DoOutputDebugString("DisableGuardPages error: inconsistency in protecton flags for region 0x%x-0x%x - OldProtect = 0x%x, GuardPages->Protect = 0x%x\n", GuardPages->BaseAddress, (DWORD_PTR)GuardPages->BaseAddress + GuardPages->RegionSize, OldProtect, GuardPages->Protect);
    //}
    
    return TRUE;
}

//**************************************************************************************
PGUARDPAGES CreateGuardPages()
//**************************************************************************************
{    
	if (GuardPageList == NULL)
	{
		GuardPageList = ((struct GuardPages*)malloc(sizeof(struct GuardPages)));
		
        if (GuardPageList == NULL)
        {
            DoOutputDebugString("CreateGuardPages: failed to allocate memory for initial thread breakpoint list.\n");
            return NULL;
        }
        memset(GuardPageList, 0, sizeof(struct GuardPages));
	}

	return GuardPageList;    
}

//**************************************************************************************
BOOL AddGuardPages(PVOID Address, SIZE_T RegionSize, ULONG Protect)
//**************************************************************************************
{    
    BOOL PageAlreadyGuarded;
    PGUARDPAGES CurrentGuardPages, PreviousGuardPages;
    
    PreviousGuardPages = NULL;
    
    if (GuardPageList == NULL)
        CreateGuardPages();
        
    CurrentGuardPages = GuardPageList;
    
	while (CurrentGuardPages)
	{
        if ((DWORD_PTR)Address >= (DWORD_PTR)CurrentGuardPages->BaseAddress && (DWORD_PTR)Address < ((DWORD_PTR)CurrentGuardPages->BaseAddress + (DWORD_PTR)CurrentGuardPages->RegionSize))
            PageAlreadyGuarded = TRUE;
		else
            PageAlreadyGuarded = FALSE;
        
        PreviousGuardPages = CurrentGuardPages;
        CurrentGuardPages = CurrentGuardPages->NextGuardPages;
	}    	
    
	if (PageAlreadyGuarded == FALSE)
	{
        // We haven't found it in the linked list, so create a new one
        CurrentGuardPages = PreviousGuardPages;
        
        CurrentGuardPages->NextGuardPages = ((struct GuardPages*)malloc(sizeof(struct GuardPages)));
	
        if (CurrentGuardPages->NextGuardPages == NULL)
		{
			DoOutputDebugString("AddGuardPages: Failed to allocate new guard page struct.\n");
			return FALSE;
		}
        memset(CurrentGuardPages->NextGuardPages, 0, sizeof(struct GuardPages));
        
        CurrentGuardPages->BaseAddress = Address;
        CurrentGuardPages->RegionSize = RegionSize;
        CurrentGuardPages->Protect = Protect;
        CurrentGuardPages->WriteDetected = FALSE;
        CurrentGuardPages->LastWriteAddress = NULL;
        CurrentGuardPages->PagesDumped = FALSE;
	}
    
    return TRUE;
}

//**************************************************************************************
PTHREADBREAKPOINTS GetThreadBreakpoints(DWORD ThreadId)
//**************************************************************************************
{
    DWORD CurrentThreadId;  
	
    PTHREADBREAKPOINTS CurrentThreadBreakpoint = MainThreadBreakpointList;

	while (CurrentThreadBreakpoint)
	{
		CurrentThreadId = MyGetThreadId(CurrentThreadBreakpoint->ThreadHandle);
        
        if (CurrentThreadId == ThreadId)
            return CurrentThreadBreakpoint;
		else
            CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
    
	return NULL;
}

//**************************************************************************************
PTHREADBREAKPOINTS CreateThreadBreakpoints(DWORD ThreadId)
//**************************************************************************************
{
	unsigned int Register;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint, PreviousThreadBreakpoint;

    PreviousThreadBreakpoint = NULL;
    
	if (MainThreadBreakpointList == NULL)
	{
		MainThreadBreakpointList = ((struct ThreadBreakpoints*)malloc(sizeof(struct ThreadBreakpoints)));
		
        if (MainThreadBreakpointList == NULL)
        {
            DoOutputDebugString("CreateThreadBreakpoints: failed to allocate memory for initial thread breakpoint list.\n");
            return NULL;
        }
        memset(MainThreadBreakpointList, 0, sizeof(struct ThreadBreakpoints));
		MainThreadBreakpointList->ThreadId = MainThreadId;
	}

	CurrentThreadBreakpoint = MainThreadBreakpointList;
    
    while (CurrentThreadBreakpoint)
	{  
        if (CurrentThreadBreakpoint->ThreadHandle && MyGetThreadId(CurrentThreadBreakpoint->ThreadHandle) == ThreadId)
        {
            //It already exists - shouldn't happen
            DoOutputDebugString("CreateThreadBreakpoints error: found an existing thread breakpoint list for ThreadId 0x%x\n", ThreadId);
            return NULL;
        }
        
        if ((CurrentThreadBreakpoint->ThreadId) == ThreadId)
        {
            // We have our thread breakpoint list
            break;            
        }
        
		PreviousThreadBreakpoint = CurrentThreadBreakpoint;
        CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
	
    if (!CurrentThreadBreakpoint)
    {
        // We haven't found it in the linked list, so create a new one
        CurrentThreadBreakpoint = PreviousThreadBreakpoint;
        
        CurrentThreadBreakpoint->NextThreadBreakpoints = ((struct ThreadBreakpoints*)malloc(sizeof(struct ThreadBreakpoints)));
	
        if (CurrentThreadBreakpoint->NextThreadBreakpoints == NULL)
		{
			DoOutputDebugString("CreateThreadBreakpoints: Failed to allocate new thread breakpoints.\n");
			return NULL;
		}
        memset(CurrentThreadBreakpoint->NextThreadBreakpoints, 0, sizeof(struct ThreadBreakpoints));
        
        CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
    
	if (ThreadId == GetCurrentThreadId())
	{
		if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &CurrentThreadBreakpoint->ThreadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
		{
			DoOutputDebugString("CreateThreadBreakpoints: Failed to duplicate thread handle.\n");
			free(CurrentThreadBreakpoint);
			return NULL;
		}
	}
	else
	{
		CurrentThreadBreakpoint->ThreadHandle = OpenThread(THREAD_ALL_ACCESS, FALSE, ThreadId);
		
		if (CurrentThreadBreakpoint->ThreadHandle == NULL)
		{
			DoOutputDebugString("CreateThreadBreakpoints: Failed to open thread and get a handle.\n");
			free(CurrentThreadBreakpoint);
			return NULL;
		}
	}
    
    CurrentThreadBreakpoint->ThreadId = ThreadId;
    
    for (Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++)
    {
        CurrentThreadBreakpoint->BreakpointInfo[Register].Register = Register;
        CurrentThreadBreakpoint->BreakpointInfo[Register].ThreadHandle = CurrentThreadBreakpoint->ThreadHandle;
    }
    
    return CurrentThreadBreakpoint;
}

//**************************************************************************************
BOOL GetNextAvailableBreakpoint(DWORD ThreadId, unsigned int* Register)
//**************************************************************************************
{
    DWORD CurrentThreadId;  
	unsigned int i;
    
    PTHREADBREAKPOINTS CurrentThreadBreakpoint = MainThreadBreakpointList;

	if (CurrentThreadBreakpoint == NULL)
    {
        DoOutputDebugString("GetNextAvailableBreakpoint: MainThreadBreakpointList NULL.\n");
        return FALSE;
    }
    
    while (CurrentThreadBreakpoint)
	{
		CurrentThreadId = MyGetThreadId(CurrentThreadBreakpoint->ThreadHandle);
        
        if (CurrentThreadId == ThreadId)
		{
            for (i=0; i < NUMBER_OF_DEBUG_REGISTERS; i++)
            {
                if (CurrentThreadBreakpoint->BreakpointInfo[i].Address == NULL)
                {
                    *Register = i;
                    return TRUE;
                }                
            }
        }
 
        CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
    
	return FALSE;
}

//**************************************************************************************
BOOL ContextGetNextAvailableBreakpoint(PCONTEXT Context, unsigned int* Register)
//**************************************************************************************
{
	unsigned int i; 
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
    
    CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());
 
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("ContextGetNextAvailableBreakpoint: Creating new thread breakpoints for thread 0x%x.\n", GetCurrentThreadId());
		CurrentThreadBreakpoint = CreateThreadBreakpoints(GetCurrentThreadId());
	}
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("ContextGetNextAvailableBreakpoint: Cannot create new thread breakpoints - FATAL.\n");
		return FALSE;
	}

    for (i=0; i < NUMBER_OF_DEBUG_REGISTERS; i++)
    {
        if (CurrentThreadBreakpoint->BreakpointInfo[i].Address == NULL)
        {
            *Register = i;
            return TRUE;
        }
    }

    return FALSE;    
}

//**************************************************************************************
void DebugOutputThreadBreakpoints()
//**************************************************************************************
{
    unsigned int Register;
    PTHREADBREAKPOINTS CurrentThreadBreakpoint;
	PBREAKPOINTINFO pBreakpointInfo;

    CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());
    
    for (Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++)
    {
        pBreakpointInfo = &(CurrentThreadBreakpoint->BreakpointInfo[Register]);
        
        if (pBreakpointInfo == NULL)
        {
            DoOutputDebugString("DebugOutputThreadBreakpoints: Can't get BreakpointInfo - FATAL.\n");
        }

		DoOutputDebugString("Callback = 0x%x, Address = 0x%x, Size = 0x%x, Register = %i, ThreadHandle = 0x%x, Type = 0x%x\n", 
			pBreakpointInfo->Callback, 
			pBreakpointInfo->Address, 
			pBreakpointInfo->Size, 
			pBreakpointInfo->Register, 
			pBreakpointInfo->ThreadHandle, 
			pBreakpointInfo->Type);
    }    
}

//**************************************************************************************
void ShowStack(DWORD_PTR StackPointer, unsigned int NumberOfRecords)
//**************************************************************************************
{
    unsigned int i;
    
    for (i=0; i<NumberOfRecords; i++)
        DoOutputDebugString("0x%x ([esp+0x%x]): 0x%x\n", StackPointer+4*i, (4*i), *(DWORD*)((BYTE*)StackPointer+4*i));
}

//**************************************************************************************
LONG WINAPI CAPEExceptionFilter(struct _EXCEPTION_POINTERS* ExceptionInfo)
//**************************************************************************************
{
	BREAKPOINT_HANDLER Handler;
	unsigned int bp;
	
    // Hardware breakpoints generate EXCEPTION_SINGLE_STEP rather than EXCEPTION_BREAKPOINT
    if (ExceptionInfo->ExceptionRecord->ExceptionCode==EXCEPTION_SINGLE_STEP)
    {    
		BOOL BreakpointFlag;
        PBREAKPOINTINFO pBreakpointInfo;
		PTHREADBREAKPOINTS CurrentThreadBreakpoint;
		
        // Test Dr6 to see if this is a breakpoint
        BreakpointFlag = FALSE;
        for (bp = 0; bp < NUMBER_OF_DEBUG_REGISTERS; bp++)
		{
			if (ExceptionInfo->ContextRecord->Dr6 & (DWORD_PTR)(1 << bp))
			{
                BreakpointFlag = TRUE;
            }
        }
        
        // If not it's a single-step
        if (BreakpointFlag == FALSE)
        {            
            if (SingleStepHandler)
                SingleStepHandler(ExceptionInfo);
            else if (TrapIndex)
            // this is from StepOverExecutionBreakpoint
            {
                DoOutputDebugString("CAPEExceptionFilter: Stepping over execution breakpoint to: 0x%x\n", ExceptionInfo->ExceptionRecord->ExceptionAddress);
                ResumeAfterExecutionBreakpoint(ExceptionInfo->ContextRecord);
            }
            else
            {
                DoOutputDebugString("CAPEExceptionFilter: Error, unhandled single-step exception at: 0x%x\n", ExceptionInfo->ExceptionRecord->ExceptionAddress);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        
        DoOutputDebugString("Entering CAPEExceptionFilter: breakpoint hit: 0x%x\n", ExceptionInfo->ExceptionRecord->ExceptionAddress);
		
		CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());

		if (CurrentThreadBreakpoint == NULL)
		{
			DoOutputDebugString("CAPEExceptionFilter: Can't get thread breakpoints - FATAL.\n");
			return EXCEPTION_CONTINUE_SEARCH;
		}
        
        for (bp = 0; bp < NUMBER_OF_DEBUG_REGISTERS; bp++)
		{
			if (ExceptionInfo->ContextRecord->Dr6 & (DWORD_PTR)(1 << bp))
			{
				pBreakpointInfo = &(CurrentThreadBreakpoint->BreakpointInfo[bp]);
                
                if (pBreakpointInfo == NULL)
                {
                    DoOutputDebugString("CAPEExceptionFilter: Can't get BreakpointInfo - FATAL.\n");
                    return EXCEPTION_CONTINUE_EXECUTION;
                }                
                
                if (pBreakpointInfo->Register == bp) 
                {
                    if (bp == 0 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr0))
                        DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp0 address (0x%x) different to BreakpointInfo (0x%x)!\n", ExceptionInfo->ContextRecord->Dr0, pBreakpointInfo->Address);
                        
                    if (bp == 1 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr1))
                        DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp1 address (0x%x) different to BreakpointInfo (0x%x)!\n", ExceptionInfo->ContextRecord->Dr1, pBreakpointInfo->Address);

                    if (bp == 2 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr2))
                        DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp2 address (0x%x) different to BreakpointInfo (0x%x)!\n", ExceptionInfo->ContextRecord->Dr2, pBreakpointInfo->Address);
                    
                    if (bp == 3 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr3))
                        DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp3 address (0x%x) different to BreakpointInfo (0x%x)!\n", ExceptionInfo->ContextRecord->Dr3, pBreakpointInfo->Address);
#ifndef _WIN64
                    if (bp == 0 && ((DWORD_PTR)pBreakpointInfo->Type != ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE0))
                    {
                        if (pBreakpointInfo->Type == BP_READWRITE && ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE0 == BP_WRITE && address_is_in_stack((DWORD_PTR)pBreakpointInfo->Address))
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Reinstated BP_READWRITE on breakpoint %d (WoW64 workaround)\n", pBreakpointInfo->Register);
                            
                            ContextSetBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, pBreakpointInfo->Size, (BYTE*)pBreakpointInfo->Address, pBreakpointInfo->Type, pBreakpointInfo->Callback);
                        }
                        else
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp0 type (0x%x) different to BreakpointInfo (0x%x)!\n", ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE0, pBreakpointInfo->Type);
                            CheckDebugRegisters(0, ExceptionInfo->ContextRecord);
                        }
                    }
                    if (bp == 1 && ((DWORD)pBreakpointInfo->Type != ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE1))
                    {
                        if (pBreakpointInfo->Type == BP_READWRITE && ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE1 == BP_WRITE && address_is_in_stack((DWORD_PTR)pBreakpointInfo->Address))
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Reinstated BP_READWRITE on breakpoint %d (WoW64 workaround)\n", pBreakpointInfo->Register);
                            
                            ContextSetBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, pBreakpointInfo->Size, (BYTE*)pBreakpointInfo->Address, pBreakpointInfo->Type, pBreakpointInfo->Callback);
                        }
                        else
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp1 type (0x%x) different to BreakpointInfo (0x%x)!\n", ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE1, pBreakpointInfo->Type);
                            CheckDebugRegisters(0, ExceptionInfo->ContextRecord);
                        }
                    }
                    if (bp == 2 && ((DWORD)pBreakpointInfo->Type != ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE2))
                    {
                        if (pBreakpointInfo->Type == BP_READWRITE && ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE2 == BP_WRITE && address_is_in_stack((DWORD_PTR)pBreakpointInfo->Address))
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Reinstated BP_READWRITE on stack breakpoint %d (WoW64 workaround)\n", pBreakpointInfo->Register);
                            
                            ContextSetBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, pBreakpointInfo->Size, (BYTE*)pBreakpointInfo->Address, pBreakpointInfo->Type, pBreakpointInfo->Callback);
                        }
                        else
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp2 type (0x%x) different to BreakpointInfo (0x%x)!\n", ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE2, pBreakpointInfo->Type);
                            CheckDebugRegisters(0, ExceptionInfo->ContextRecord);
                        }
                    }
                    if (bp == 3 && ((DWORD)pBreakpointInfo->Type != ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE3))
                    {
                        if (pBreakpointInfo->Type == BP_READWRITE && ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE3 == BP_WRITE && address_is_in_stack((DWORD_PTR)pBreakpointInfo->Address))
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Reinstated BP_READWRITE on breakpoint %d (WoW64 workaround)\n", pBreakpointInfo->Register);
                            
                            ContextSetBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, pBreakpointInfo->Size, (BYTE*)pBreakpointInfo->Address, pBreakpointInfo->Type, pBreakpointInfo->Callback);
                        }
                        else
                        {
                            DoOutputDebugString("CAPEExceptionFilter: Anomaly detected! bp3 type (0x%x) different to BreakpointInfo (0x%x)!\n", ((PDR7)&(ExceptionInfo->ContextRecord->Dr7))->RWE3, pBreakpointInfo->Type);
                            CheckDebugRegisters(0, ExceptionInfo->ContextRecord);
                        }
                    }
#endif // !_WIN64
                }
			}
		}

		if (pBreakpointInfo->Callback == NULL)
		{
			DoOutputDebugString("CAPEExceptionFilter: Can't get callback - FATAL.\n");
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		
		Handler = (BREAKPOINT_HANDLER)pBreakpointInfo->Callback;
		
		// Invoke the handler
        Handler(pBreakpointInfo, ExceptionInfo);
        
		return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Page guard violations generate STATUS_GUARD_PAGE_VIOLATION
    else if (ExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION)
    {    
        if (ExceptionInfo->ExceptionRecord->NumberParameters < 2)
		{
            DoOutputDebugString("CAPEExceptionFilter: Guard page exception with missing parameters, passing.\n");
            return EXCEPTION_CONTINUE_SEARCH;		
		}
		
		//DoOutputDebugString("Entering CAPEExceptionFilter: guarded page access at 0x%x by 0x%x\n", ExceptionInfo->ExceptionRecord->ExceptionInformation[1], ExceptionInfo->ExceptionRecord->ExceptionAddress);
        
        if (IsInGuardPages((PVOID)ExceptionInfo->ExceptionRecord->ExceptionInformation[1]))
        {
            if (GuardPageHandler)
            {
                if (GuardPageHandler(ExceptionInfo))
					return EXCEPTION_CONTINUE_EXECUTION;
				else
					return EXCEPTION_CONTINUE_SEARCH;
            }
            else
            {
                DoOutputDebugString("CAPEExceptionFilter: Error, no page guard handler for CAPE guard page exception.\n");
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        else
        {
            DoOutputDebugString("CAPEExceptionFilter: exception at 0x%x not within CAPE guarded page.\n", ExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    else if (ExceptionInfo->ExceptionRecord->ExceptionCode == DBG_PRINTEXCEPTION_C)
    {
        // This is likely our own DoOutputDebugString function!
        // So we let Windows handle this so we get our output.
        return EXCEPTION_CONTINUE_SEARCH;
    }
    else if (OriginalExceptionHandler)
    {
        if ((ULONG_PTR)ExceptionInfo->ExceptionRecord->ExceptionAddress >= g_our_dll_base && (ULONG_PTR)ExceptionInfo->ExceptionRecord->ExceptionAddress < (g_our_dll_base + g_our_dll_size))
        {
            // This is a CAPE exception, likely from the try/catch blocks in buffer scans
            return EXCEPTION_EXECUTE_HANDLER;
        }
        
        // As it's not a bp, and the sample has registered its own handler
        // we return EXCEPTION_EXECUTE_HANDLER
        DoOutputDebugString("CAPEExceptionFilter: Non-breakpoint exception caught, passing to sample's handler.\n");
        SetUnhandledExceptionFilter(OriginalExceptionHandler);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    
    // Some other exception occurred. Pass it to next handler.    
    return EXCEPTION_CONTINUE_SEARCH;
}

//**************************************************************************************
BOOL ContextSetDebugRegister
//**************************************************************************************
(
    PCONTEXT	Context,
    int		    Register,
    int		    Size,
    LPVOID	    Address,
    DWORD	    Type
)
{
	DWORD	Length;

    PDWORD_PTR  Dr0 = &(Context->Dr0);
    PDWORD_PTR  Dr1 = &(Context->Dr1);
    PDWORD_PTR  Dr2 = &(Context->Dr2);
    PDWORD_PTR  Dr3 = &(Context->Dr3);
    PDR7    Dr7 = (PDR7)&(Context->Dr7);

    if ((unsigned int)Type > 3)
    {
        DoOutputDebugString("ContextSetDebugRegister: %d is an invalid breakpoint type, must be 0-3.\n", Type);
        return FALSE;
    }

    if (Type == 2)
    {
        DoOutputDebugString("ContextSetDebugRegister: The value 2 is a 'reserved' breakpoint type, ultimately invalid.\n");
        return FALSE;
    }

    if (Register < 0 || Register > 3)
    {
        DoOutputDebugString("ContextSetDebugRegister: %d is an invalid register, must be 0-3.\n", Register);
        return FALSE;
    }
    
    if (Size < 0 || Size > 8)
    {
        DoOutputDebugString("ContextSetDebugRegister: %d is an invalid Size, must be 1, 2, 4 or 8.\n", Size);
        return FALSE;
    }

	DoOutputDebugString("ContextSetDebugRegister: Setting breakpoint %i within Context, Size=0x%x, Address=0x%x and Type=0x%x.\n", Register, Size, Address, Type);
	
    Length  = LengthMask[Size];

    // intel spec requires 0 for bp on execution
    if (Type == BP_EXEC)
        Length = 0;

#ifndef _WIN64
    if (Type == BP_READWRITE && address_is_in_stack((DWORD_PTR)Address))
        WoW64PatchBreakpoint(Register);
#endif	
    
    if (Register == 0)
    {
        *Dr0 = (DWORD_PTR)Address;
        Dr7->LEN0 = Length;
        Dr7->RWE0 = Type;
        Dr7->L0 = 1;    
    }
    else if (Register == 1)
    {
        *Dr1 = (DWORD_PTR)Address;
        Dr7->LEN1 = Length;
        Dr7->RWE1 = Type;
        Dr7->L1 = 1;    
    }
    else if (Register == 2)
    {
        *Dr2 = (DWORD_PTR)Address;
        Dr7->LEN2 = Length;
        Dr7->RWE2 = Type;
        Dr7->L2 = 1;    
    }
    else if (Register == 3)
    {
        *Dr3 = (DWORD_PTR)Address;
        Dr7->LEN3 = Length;
        Dr7->RWE3 = Type;
        Dr7->L3 = 1;    
    }
    
    Dr7->LE = 1;
    Context->Dr6 = 0;

	return TRUE;
}

//**************************************************************************************
BOOL SetDebugRegister
//**************************************************************************************
(
    HANDLE	hThread,
    int		Register,
    int		Size,
    LPVOID	Address,
    DWORD	Type
)
{
	DWORD	Length;
    CONTEXT	Context;

    PDWORD_PTR  Dr0 = &Context.Dr0;
    PDWORD_PTR  Dr1 = &Context.Dr1;
    PDWORD_PTR  Dr2 = &Context.Dr2;
    PDWORD_PTR  Dr3 = &Context.Dr3;
    PDR7    Dr7 = (PDR7)&(Context.Dr7);

    if ((unsigned int)Type > 3)
    {
        DoOutputDebugString("SetDebugRegister: %d is an invalid breakpoint type, must be 0-3.\n", Type);
        return FALSE;
    }

    if (Type == 2)
    {
        DoOutputDebugString("SetDebugRegister: The value 2 is a 'reserved' breakpoint type, ultimately invalid.\n");
        return FALSE;
    }

    if (Register < 0 || Register > 3)
    {
        DoOutputDebugString("SetDebugRegister: %d is an invalid register, must be 0-3.\n", Register);
        return FALSE;
    }
    
    if (Size < 0 || Size > 8)
    {
        DoOutputDebugString("SetDebugRegister: %d is an invalid Size, must be 1, 2, 4 or 8.\n", Size);
        return FALSE;
    }

	DoOutputDebugString("SetDebugRegister: Setting breakpoint %i hThread=0x%x, Size=0x%x, Address=0x%x and Type=0x%x.\n", Register, hThread, Size, Address, Type);
	
    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    
    if (!GetThreadContext(hThread, &Context))
    {
        DoOutputErrorString("SetDebugRegister: GetThreadContext failed");
        return FALSE;
    }

    Length  = LengthMask[Size];

    // intel spec requires 0 for bp on execution
    if (Type == BP_EXEC)
        Length = 0;

#ifndef _WIN64
    if (Type == BP_READWRITE && address_is_in_stack((DWORD_PTR)Address))
        WoW64PatchBreakpoint(Register);
#endif	
    
    if (Register == 0)
    {
        *Dr0 = (DWORD_PTR)Address;
        Dr7->LEN0 = Length;
        Dr7->RWE0 = Type;
        Dr7->L0 = 1;    
    }
    else if (Register == 1)
    {
        *Dr1 = (DWORD_PTR)Address;
        Dr7->LEN1 = Length;
        Dr7->RWE1 = Type;
        Dr7->L1 = 1;    
    }
    else if (Register == 2)
    {
        *Dr2 = (DWORD_PTR)Address;
        Dr7->LEN2 = Length;
        Dr7->RWE2 = Type;
        Dr7->L2 = 1;    
    }
    else if (Register == 3)
    {
        *Dr3 = (DWORD_PTR)Address;
        Dr7->LEN3 = Length;
        Dr7->RWE3 = Type;
        Dr7->L3 = 1;    
    }
    
    Dr7->LE = 1;
    Context.Dr6 = 0;

    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

	if (!SetThreadContext(hThread, &Context))
    {
        DoOutputErrorString("SetDebugRegister: SetThreadContext failed");
        return FALSE;
    }
        
	return TRUE;
}

//**************************************************************************************
BOOL ContextCheckDebugRegisters(PCONTEXT Context)
//**************************************************************************************
{   
    PDR7 Dr7;
    
    if (!Context)
    {
        DoOutputDebugString("CheckDebugRegisters - no context supplied.\n");
        return FALSE;
    }
    
    Dr7 = (PDR7)&(Context->Dr7);
    
	DoOutputDebugString("Checking breakpoints\n");
	DoOutputDebugString("Dr0 0x%x, Dr7->LEN0 %i, Dr7->RWE0 %i, Dr7->L0 %i\n", Context->Dr0, Dr7->LEN0, Dr7->RWE0, Dr7->L0);
	DoOutputDebugString("Dr1 0x%x, Dr7->LEN1 %i, Dr7->RWE1 %i, Dr7->L1 %i\n", Context->Dr1, Dr7->LEN1, Dr7->RWE1, Dr7->L1);
	DoOutputDebugString("Dr2 0x%x, Dr7->LEN2 %i, Dr7->RWE2 %i, Dr7->L2 %i\n", Context->Dr2, Dr7->LEN2, Dr7->RWE2, Dr7->L2);
	DoOutputDebugString("Dr3 0x%x, Dr7->LEN3 %i, Dr7->RWE3 %i, Dr7->L3 %i\n", Context->Dr3, Dr7->LEN3, Dr7->RWE3, Dr7->L3);
	DoOutputDebugString("Dr6 0x%x\n", Context->Dr6);

	return TRUE;
}

//**************************************************************************************
BOOL CheckDebugRegisters(HANDLE hThread, PCONTEXT pContext)
//**************************************************************************************
{
    CONTEXT	Context;
    PDWORD_PTR  Dr0 = &Context.Dr0;
    PDWORD_PTR  Dr1 = &Context.Dr1;
    PDWORD_PTR  Dr2 = &Context.Dr2;
    PDWORD_PTR  Dr3 = &Context.Dr3;
    PDR7    Dr7 = (PDR7)&(Context.Dr7);
    
    if (!hThread && !pContext)
    {
        DoOutputDebugString("CheckDebugRegisters - reqruied arguments missing.\n");
        return FALSE;
    }

    if (!hThread)
    {
        Context = *pContext;
    }
    else if (!pContext)
    {
        Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(hThread, &Context))
        {
            DoOutputDebugString("CheckDebugRegisters - failed to get thread context.\n");
            return FALSE;
        }	    
    }
    
	DoOutputDebugString("Checking breakpoints\n");
	DoOutputDebugString("*Dr0 0x%x, Dr7->LEN0 %i, Dr7->RWE0 %i, Dr7->L0 %i\n", *Dr0, Dr7->LEN0, Dr7->RWE0, Dr7->L0);
	DoOutputDebugString("*Dr1 0x%x, Dr7->LEN1 %i, Dr7->RWE1 %i, Dr7->L1 %i\n", *Dr1, Dr7->LEN1, Dr7->RWE1, Dr7->L1);
	DoOutputDebugString("*Dr2 0x%x, Dr7->LEN2 %i, Dr7->RWE2 %i, Dr7->L2 %i\n", *Dr2, Dr7->LEN2, Dr7->RWE2, Dr7->L2);
	DoOutputDebugString("*Dr3 0x%x, Dr7->LEN3 %i, Dr7->RWE3 %i, Dr7->L3 %i\n", *Dr3, Dr7->LEN3, Dr7->RWE3, Dr7->L3);
	DoOutputDebugString("Dr6 0x%x, thread handle 0x%x\n", Context.Dr6, hThread);

	return TRUE;
}

//**************************************************************************************
BOOL ContextClearAllBreakpoints(PCONTEXT Context)
//**************************************************************************************
{
	unsigned int i; 
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
    
    CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());
 
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("ContextClearAllBreakpoints: No breakpoints found for current thread 0x%x.\n", GetCurrentThreadId());
		return FALSE;
	}
    
    for (i=0; i < NUMBER_OF_DEBUG_REGISTERS; i++)
    {
        CurrentThreadBreakpoint->BreakpointInfo[i].Register = 0;
        CurrentThreadBreakpoint->BreakpointInfo[i].Size = 0;
        CurrentThreadBreakpoint->BreakpointInfo[i].Address = NULL;
        CurrentThreadBreakpoint->BreakpointInfo[i].Type = 0;
        CurrentThreadBreakpoint->BreakpointInfo[i].Callback = NULL;
    }

    Context->Dr0 = 0;
    Context->Dr1 = 0;
	Context->Dr2 = 0;
    Context->Dr3 = 0;
	Context->Dr6 = 0;
	Context->Dr7 = 0;
	    
    return TRUE;
}

//**************************************************************************************
BOOL ClearAllBreakpoints(DWORD ThreadId)
//**************************************************************************************
{
    CONTEXT	Context;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;    
    unsigned int Register;
    DWORD CurrentThreadId;  

    CurrentThreadBreakpoint = MainThreadBreakpointList;

	while (CurrentThreadBreakpoint)
	{
		CurrentThreadId = MyGetThreadId(CurrentThreadBreakpoint->ThreadHandle);
        
        if (CurrentThreadId == ThreadId)
        {
            for (Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++)
            {
                CurrentThreadBreakpoint->BreakpointInfo[Register].Register = 0;
                CurrentThreadBreakpoint->BreakpointInfo[Register].Size = 0;
                CurrentThreadBreakpoint->BreakpointInfo[Register].Address = NULL;
                CurrentThreadBreakpoint->BreakpointInfo[Register].Type = 0;
                CurrentThreadBreakpoint->BreakpointInfo[Register].Callback = NULL;
            }

            Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

            if (!GetThreadContext(CurrentThreadBreakpoint->ThreadHandle, &Context))
            {
            	DoOutputDebugString("ClearAllBreakpoints: Error getting thread context (thread %d).\n", CurrentThreadId);
                return FALSE;
            }

            Context.Dr0 = 0;
            Context.Dr1 = 0;
            Context.Dr2 = 0;
            Context.Dr3 = 0;
            Context.Dr6 = 0;
            Context.Dr7 = 0;
            
            if (!SetThreadContext(CurrentThreadBreakpoint->ThreadHandle, &Context))
            {
            	DoOutputDebugString("ClearAllBreakpoints: Error setting thread context (thread %d).\n", CurrentThreadId);
                return FALSE;
            }
            
            return TRUE;
        }
		else
            CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
    
	return FALSE;
}

//**************************************************************************************
BOOL ContextClearBreakpoint(PCONTEXT Context, PBREAKPOINTINFO pBreakpointInfo)
//**************************************************************************************
{
    PDWORD_PTR Dr0, Dr1, Dr2, Dr3;
	PDR7 Dr7;

	if (Context == NULL)
        return FALSE;
        
    Dr0 = &(Context->Dr0);
    Dr1 = &(Context->Dr1);
    Dr2 = &(Context->Dr2);
    Dr3 = &(Context->Dr3);
    Dr7 = (PDR7)&(Context->Dr7);
    
	DoOutputDebugString("ContextClearBreakpoint: Clearing Context breakpoint %i\n", pBreakpointInfo->Register);
	
    if (pBreakpointInfo->Register == 0)
    {
        *Dr0 = 0;
        Dr7->LEN0 = 0;
        Dr7->RWE0 = 0;
        Dr7->L0 = 0;    
    }
    else if (pBreakpointInfo->Register == 1)
    {
        *Dr1 = 0;
        Dr7->LEN1 = 0;
        Dr7->RWE1 = 0;
        Dr7->L1 = 0;      
    }
    else if (pBreakpointInfo->Register == 2)
    {
        *Dr2 = 0;
        Dr7->LEN2 = 0;
        Dr7->RWE2 = 0;
        Dr7->L2 = 0;       
    }
    else if (pBreakpointInfo->Register == 3)
    {
        *Dr3 = 0;
        Dr7->LEN3 = 0;
        Dr7->RWE3 = 0;
        Dr7->L3 = 0;     
    }

#ifndef _WIN64
    if (pBreakpointInfo->Type == BP_READWRITE && address_is_in_stack((DWORD_PTR)pBreakpointInfo->Address))
        WoW64UnpatchBreakpoint(pBreakpointInfo->Register);
#endif	
    
    Context->Dr6 = 0;
	
	pBreakpointInfo->Address = 0;
	pBreakpointInfo->Size = 0;
	pBreakpointInfo->Callback = 0;
	pBreakpointInfo->Type = 0;

	return TRUE;
}

//**************************************************************************************
BOOL ClearBreakpointsInRange(DWORD ThreadId, PVOID BaseAddress, SIZE_T Size)
//**************************************************************************************
{
    unsigned int Register;
    DWORD CurrentThreadId;  
	
    PTHREADBREAKPOINTS CurrentThreadBreakpoint = MainThreadBreakpointList;

    if (BaseAddress == NULL)
    {
        DoOutputDebugString("ClearBreakpointsInRange: No address supplied (may have already been cleared).\n");
        return FALSE;
    }
    
    if (Size == 0)
    {
        DoOutputDebugString("ClearBreakpointsInRange: Size supplied is zero.\n");
        return FALSE;
    }
    
    DoOutputDebugString("ClearBreakpointsInRange: Clearing breakpoints in range 0x%x - 0x%x.\n", BaseAddress, (BYTE*)BaseAddress + Size);
	
    while (CurrentThreadBreakpoint)
	{
		CurrentThreadId = MyGetThreadId(CurrentThreadBreakpoint->ThreadHandle);

        if (CurrentThreadId == ThreadId)
        {
            for (Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++)
            {
                if ((DWORD_PTR)CurrentThreadBreakpoint->BreakpointInfo[Register].Address >= (DWORD_PTR)BaseAddress 
                    && (DWORD_PTR)CurrentThreadBreakpoint->BreakpointInfo[Register].Address < (DWORD_PTR)((BYTE*)BaseAddress + Size))
                {
                    DoOutputDebugString("ClearBreakpointsInRange: Clearing breakpoint %d address 0x%x.\n", Register, CurrentThreadBreakpoint->BreakpointInfo[Register].Address);
                    ClearBreakpoint(CurrentThreadBreakpoint->ThreadId, Register);
                }
            }
            
            return TRUE;
        }
		else
            CurrentThreadBreakpoint = CurrentThreadBreakpoint->NextThreadBreakpoints;
	}
    
	return FALSE;
}

//**************************************************************************************
BOOL SetResumeFlag(PCONTEXT Context)
//**************************************************************************************
{
    if (Context == NULL)
        return FALSE;
	
    Context->EFlags |= FL_RF;
    
    return TRUE;
}

//**************************************************************************************
BOOL SetSingleStepMode(PCONTEXT Context, PVOID Handler)
//**************************************************************************************
{
	if (Context == NULL)
        return FALSE;
    
    // set the trap flag
    Context->EFlags |= FL_TF;
    
    SingleStepHandler = (SINGLE_STEP_HANDLER)Handler;

    return TRUE;
}

//**************************************************************************************
BOOL ClearSingleStepMode(PCONTEXT Context)
//**************************************************************************************
{
	if (Context == NULL)
        return FALSE;

    // Clear the trap flag
    Context->EFlags &= ~FL_TF;

    SingleStepHandler = NULL;
    
    return TRUE;
}

//**************************************************************************************
BOOL StepOverExecutionBreakpoint(PCONTEXT Context, PBREAKPOINTINFO pBreakpointInfo)
//**************************************************************************************
// This function allows us to get past an execution breakpoint while leaving it set. It
// diaables the breakpoint, sets single-step mode to step over the instruction, whereupon 
// the breakpoint is restored in ResumeAfterExecutionBreakpoint and execution resumed.
{
	PDR7 Dr7;

	if (Context == NULL)
        return FALSE;

    Dr7 = (PDR7)&(Context->Dr7);
    
	switch(pBreakpointInfo->Register)
	{
        case 0:
            Dr7->L0 = 0;
            break;
        case 1:
            Dr7->L1 = 0;
            break;
        case 2:
            Dr7->L2 = 0;
            break;
        case 3:
            Dr7->L3 = 0;
            break;
	}

    // set the trap flag
    Context->EFlags |= FL_TF;
    
    // set the 'trap index' so we know which 'register' we're skipping
    // (off by one to allow 'set'/'unset' to be signified by !0/0)
    TrapIndex = pBreakpointInfo->Register + 1;
    
    return TRUE;
}

//**************************************************************************************
BOOL ResumeAfterExecutionBreakpoint(PCONTEXT Context)
//**************************************************************************************
{
	PDR7 Dr7;

	if (Context == NULL)
        return FALSE;

    Dr7 = (PDR7)&(Context->Dr7);
    
	switch(TrapIndex-1)
	{
        case 0:
            Dr7->L0 = 1;
            break;
        case 1:
            Dr7->L1 = 1;
            break;
        case 2:
            Dr7->L2 = 1;
            break;
        case 3:
            Dr7->L3 = 1;
            break;
	}

    // Clear the trap flag
    Context->EFlags &= ~FL_TF;
    
    // clear the 'trap index'
    TrapIndex = 0;

    return TRUE;
}

//**************************************************************************************
BOOL ClearDebugRegister
//**************************************************************************************
(
    HANDLE	hThread,
    int		Register,
    int		Size,
    LPVOID	Address,
    DWORD	Type
){
    CONTEXT	Context;
    BOOL DoCloseHandle = FALSE;
    PDWORD_PTR  Dr0 = &Context.Dr0;
    PDWORD_PTR  Dr1 = &Context.Dr1;
    PDWORD_PTR  Dr2 = &Context.Dr2;
    PDWORD_PTR  Dr3 = &Context.Dr3;
    PDR7    Dr7 = (PDR7)&(Context.Dr7);
    
    if ((unsigned int)Type > 3)
    {
        DoOutputDebugString("ClearDebugRegister: %d is an invalid breakpoint type, must be 0-3.\n", Type);
        return FALSE;
    }

    if (Register < 0 || Register > 3)
    {
        DoOutputDebugString("ClearDebugRegister: %d is an invalid register, must be 0-3.\n", Register);
        return FALSE;
    }
    
    if (Size < 0 || Size > 8)
    {
        DoOutputDebugString("ClearDebugRegister: %d is an invalid Size, must be 1, 2, 4 or 8.\n", Size);
        return FALSE;
    }

    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    
	if (!GetThreadContext(hThread, &Context))
    {
        DoOutputDebugString("ClearDebugRegister: Initial GetThreadContext failed");
        return FALSE;
    }	

    if (Register == 0)
    {
        *Dr0 = 0;
        Dr7->LEN0 = 0;
        Dr7->RWE0 = 0;
        Dr7->L0 = 0;    
    }
    else if (Register == 1)
    {
        *Dr1 = 0;
        Dr7->LEN1 = 0;
        Dr7->RWE1 = 0;
        Dr7->L1 = 0;      
    }
    else if (Register == 2)
    {
        *Dr2 = 0;
        Dr7->LEN2 = 0;
        Dr7->RWE2 = 0;
        Dr7->L2 = 0;       
    }
    else if (Register == 3)
    {
        *Dr3 = 0;
        Dr7->LEN3 = 0;
        Dr7->RWE3 = 0;
        Dr7->L3 = 0;     
    }

#ifndef _WIN64
    if (Type == BP_READWRITE && address_is_in_stack((DWORD_PTR)Address))
        WoW64UnpatchBreakpoint(Register);
#endif	
    
    Context.Dr6 = 0;

    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	
    if (!SetThreadContext(hThread, &Context))
    {
        DoOutputDebugString("ClearDebugRegister: SetThreadContext failed");
        return FALSE;
    }	
        
    if (DoCloseHandle == TRUE)
        CloseHandle(hThread);
    
    return TRUE;
}

//**************************************************************************************
int ContextCheckDebugRegister(CONTEXT Context, int Register)
//**************************************************************************************
{
    PDR7 Dr7;
    
    if (Register < 0 || Register > 3)
    {
        DoOutputDebugString("ContextCheckDebugRegister: %d is an invalid register, must be 0-3.\n", Register);
        return FALSE;
    }

    Dr7 = (PDR7)&(Context.Dr7);

    if (Register == 0)
        return Dr7->L0;
    else if (Register == 1)
        return Dr7->L1;
    else if (Register == 2)
        return Dr7->L2;
    else if (Register == 3)
        return Dr7->L3;
	
	// should not happen
	return -1;
}

//**************************************************************************************
int CheckDebugRegister(HANDLE hThread, int Register)
//**************************************************************************************
{
    CONTEXT	Context;
    PDR7 Dr7;
    
    if (Register < 0 || Register > 3)
    {
        DoOutputDebugString("CheckDebugRegister: %d is an invalid register, must be 0-3.\n", Register);
        return FALSE;
    }

    Dr7 = (PDR7)&(Context.Dr7);
    
    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	
    if (!GetThreadContext(hThread, &Context))
    {
        DoOutputDebugString("CheckDebugRegister: GetThreadContext failed - FATAL\n");
        return FALSE;
    }

    if (Register == 0)
        return Dr7->L0;
    else if (Register == 1)
        return Dr7->L1;
    else if (Register == 2)
        return Dr7->L2;
    else if (Register == 3)
        return Dr7->L3;
	
	// should not happen
	return -1;
}

//**************************************************************************************
BOOL ContextSetBreakpoint
//**************************************************************************************
(
    PCONTEXT	Context,
    int			Register,
    int			Size,
    LPVOID		Address,
    DWORD		Type,
	PVOID		Callback
)
{
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
    
    if (Register > 3 || Register < 0)
    {
        DoOutputDebugString("ContextSetBreakpoint: Error - register value %d, can only have value 0-3.\n", Register);
        return FALSE;
    }
    
    if (ContextSetDebugRegister(Context, Register, Size, Address, Type) == FALSE)
	{
		DoOutputDebugString("ContextSetBreakpoint: Call to ContextSetDebugRegister failed.\n");
	}
	else
	{
		DoOutputDebugString("ContextSetBreakpoint: Call to ContextSetDebugRegister succeeded.\n");
          
        CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());
        
        if (CurrentThreadBreakpoint == NULL)
        {
            DoOutputDebugString("Error: Failed to acquire thread breakpoints.\n");
            return FALSE;
        }
        
		CurrentThreadBreakpoint->BreakpointInfo[Register].ThreadHandle  = CurrentThreadBreakpoint->ThreadHandle;
		CurrentThreadBreakpoint->BreakpointInfo[Register].Register      = Register;
		CurrentThreadBreakpoint->BreakpointInfo[Register].Size          = Size;
		CurrentThreadBreakpoint->BreakpointInfo[Register].Address       = Address;
		CurrentThreadBreakpoint->BreakpointInfo[Register].Type          = Type;
		CurrentThreadBreakpoint->BreakpointInfo[Register].Callback      = Callback;
	}

    return TRUE;
}

//**************************************************************************************
BOOL ContextSetNextAvailableBreakpoint
//**************************************************************************************
(
    PCONTEXT	    Context,
    unsigned int*	Register,
    int		        Size,
    LPVOID	        Address,
    DWORD	        Type,
	PVOID	        Callback
)
{
    if (ContextGetNextAvailableBreakpoint(Context, Register) == FALSE)
    {
        DoOutputDebugString("ContextSetNextAvailableBreakpoint: ContextGetNextAvailableBreakpoint failed\n");
        return FALSE;
    }

    return ContextSetBreakpoint(Context, *Register, Size, Address, Type, Callback);
}

//**************************************************************************************
BOOL ContextUpdateCurrentBreakpoint
//**************************************************************************************
(
    PCONTEXT	    Context,
    int		        Size,
    LPVOID	        Address,
    DWORD	        Type,
	PVOID	        Callback
)
{
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
    PBREAKPOINTINFO pBreakpointInfo;
    unsigned int bp;
    
    CurrentThreadBreakpoint = GetThreadBreakpoints(GetCurrentThreadId());
    
    if (CurrentThreadBreakpoint == NULL)
    {
        DoOutputDebugString("ContextUpdateCurrentBreakpoint: Error - Failed to acquire thread breakpoints.\n");
        return FALSE;
    }

    for (bp = 0; bp < NUMBER_OF_DEBUG_REGISTERS; bp++)
    {
        if (Context->Dr6 & (DWORD_PTR)(1 << bp))
        {
            pBreakpointInfo = &(CurrentThreadBreakpoint->BreakpointInfo[bp]);
            
            if (pBreakpointInfo == NULL)
            {
                DoOutputDebugString("ContextUpdateCurrentBreakpoint: Can't get BreakpointInfo.\n");
                return FALSE;
            }                
            
            if (pBreakpointInfo->Register == bp) 
            {
                if (bp == 0 && ((DWORD_PTR)pBreakpointInfo->Address == Context->Dr0) && ((DWORD)pBreakpointInfo->Type == ((PDR7)&(Context->Dr7))->RWE0))
                {
                    return ContextSetBreakpoint(Context, 0, Size, Address, Type, Callback); 
                }                    

                if (bp == 1 && ((DWORD_PTR)pBreakpointInfo->Address == Context->Dr1) && ((DWORD)pBreakpointInfo->Type == ((PDR7)&(Context->Dr7))->RWE1))
                {
                    return ContextSetBreakpoint(Context, 1, Size, Address, Type, Callback); 
                }                    

                if (bp == 2 && ((DWORD_PTR)pBreakpointInfo->Address == Context->Dr2) && ((DWORD)pBreakpointInfo->Type == ((PDR7)&(Context->Dr7))->RWE2))
                {
                    return ContextSetBreakpoint(Context, 2, Size, Address, Type, Callback); 
                }                    

                if (bp == 3 && ((DWORD_PTR)pBreakpointInfo->Address == Context->Dr3) && ((DWORD)pBreakpointInfo->Type == ((PDR7)&(Context->Dr7))->RWE3))
                {
                    return ContextSetBreakpoint(Context, 3, Size, Address, Type, Callback); 
                }                    
            }
        }
    }
    
    return FALSE;
}

//**************************************************************************************
DWORD WINAPI SetBreakpointThread(LPVOID lpParam) 
//**************************************************************************************
{ 
    DWORD RetVal;
    
    PBREAKPOINTINFO pBreakpointInfo = (PBREAKPOINTINFO)lpParam;
	 
	if (SuspendThread(pBreakpointInfo->ThreadHandle) == 0xFFFFFFFF)
		DoOutputErrorString("SetBreakpointThread: Call to SuspendThread failed");
    
	if (SetDebugRegister(pBreakpointInfo->ThreadHandle, pBreakpointInfo->Register, pBreakpointInfo->Size, pBreakpointInfo->Address, pBreakpointInfo->Type) == FALSE)
	{
		DoOutputErrorString("SetBreakpointThread: Call to SetDebugRegister failed");
	}

    RetVal = ResumeThread(pBreakpointInfo->ThreadHandle);
    if (RetVal == -1)
    {
        DoOutputErrorString("SetBreakpointThread: ResumeThread failed.\n");
    }
    else if (RetVal == 0)
    {
        DoOutputDebugString("SetBreakpointThread: Error - Sample thread was not suspended.\n");
    }
    else if (g_config.debug)
    {
        DoOutputDebugString("SetBreakpointThread: Sample thread was suspended, now resumed.\n");
    }

    return 1; 
} 

//**************************************************************************************
DWORD WINAPI ClearBreakpointThread(LPVOID lpParam) 
//**************************************************************************************
{ 
    DWORD RetVal;
    PBREAKPOINTINFO pBreakpointInfo = (PBREAKPOINTINFO)lpParam;
	
	if (SuspendThread(pBreakpointInfo->ThreadHandle) == 0xFFFFFFFF)
		DoOutputErrorString("ClearBreakpointThread: Call to SuspendThread failed");
	
	if (ClearDebugRegister(pBreakpointInfo->ThreadHandle, pBreakpointInfo->Register, pBreakpointInfo->Size, pBreakpointInfo->Address, pBreakpointInfo->Type) == FALSE)
	{
		DoOutputDebugString("ClearBreakpointThread: Call to ClearDebugRegister failed.\n");
	}

    RetVal = ResumeThread(pBreakpointInfo->ThreadHandle);
    if (RetVal == -1)
    {
        DoOutputErrorString("ClearBreakpointThread: ResumeThread failed.\n");
    }
    else if (RetVal == 0)
    {
        DoOutputDebugString("ClearBreakpointThread: Error - Sample thread was not suspended.\n");
    }
    else if (g_config.debug)
    {
        DoOutputDebugString("ClearBreakpointThread: Sample thread was suspended, now resumed.\n");
    }

    DebugOutputThreadBreakpoints();    
    
    return TRUE; 
}

//************************************************************************************** 
BOOL ClearBreakpointWithoutThread(DWORD ThreadId, int Register)
//**************************************************************************************
{ 
    PBREAKPOINTINFO pBreakpointInfo;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
    
    if (Register > 3 || Register < 0)
    {
        DoOutputDebugString("ClearBreakpointWithoutThread: Error - register value %d, can only have value 0-3.\n", Register);
        return FALSE;
    }  
	
    CurrentThreadBreakpoint = GetThreadBreakpoints(ThreadId);

	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("ClearBreakpointWithoutThread: Creating new thread breakpoints for thread 0x%x.\n", ThreadId);
		CurrentThreadBreakpoint = CreateThreadBreakpoints(ThreadId);
	}
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("ClearBreakpointWithoutThread: Cannot create new thread breakpoints - FATAL.\n");
		return FALSE;
	}

	pBreakpointInfo = &CurrentThreadBreakpoint->BreakpointInfo[Register];
	
	if (CurrentThreadBreakpoint->ThreadHandle == NULL)
	{
		DoOutputDebugString("ClearBreakpointWithoutThread: There is no thread handle in the thread breakpoint - Error.\n");
		return FALSE;
	}

    if (ClearDebugRegister(pBreakpointInfo->ThreadHandle, pBreakpointInfo->Register, pBreakpointInfo->Size, pBreakpointInfo->Address, pBreakpointInfo->Type) == FALSE)
	{
		DoOutputDebugString("ClearBreakpointWithoutThread: Call to ClearDebugRegister failed.\n");
        return FALSE;
	}

	//pBreakpointInfo->Register = 0;
	pBreakpointInfo->Size = 0;
	pBreakpointInfo->Address = 0;
	pBreakpointInfo->Type	  = 0;
	pBreakpointInfo->Callback = NULL;
	
    return TRUE;
}

//**************************************************************************************
BOOL SetBreakpointWithoutThread
//**************************************************************************************
(
    DWORD	ThreadId,
    int		Register,
    int		Size,
    LPVOID	Address,
    DWORD	Type,
	PVOID	Callback
)
{
    PBREAKPOINTINFO pBreakpointInfo;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
	BOOL RetVal;

    if (Register > 3 || Register < 0)
    {
        DoOutputDebugString("SetBreakpointWithoutThread: Error - register value %d, can only have value 0-3.\n", Register);
        return FALSE;
    }  
	
    CurrentThreadBreakpoint = GetThreadBreakpoints(ThreadId);
    
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetBreakpointWithoutThread: Creating new thread breakpoints for thread 0x%x.\n", ThreadId);
		CurrentThreadBreakpoint = CreateThreadBreakpoints(ThreadId);
	}
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetBreakpointWithoutThread: Cannot create new thread breakpoints - FATAL.\n");
		return FALSE;
	}

	pBreakpointInfo = &CurrentThreadBreakpoint->BreakpointInfo[Register];
	
	if (CurrentThreadBreakpoint->ThreadHandle == NULL)
	{
		DoOutputDebugString("SetBreakpointWithoutThread: There is no thread handle in the thread breakpoint - Error.\n");
		return FALSE;
	}
    
	pBreakpointInfo->ThreadHandle   = CurrentThreadBreakpoint->ThreadHandle;
	pBreakpointInfo->Register       = Register;
	pBreakpointInfo->Size           = Size;
	pBreakpointInfo->Address        = Address;
	pBreakpointInfo->Type	        = Type;
	pBreakpointInfo->Callback       = Callback;
	
    __try
    {
        RetVal = (SetDebugRegister(pBreakpointInfo->ThreadHandle, Register, Size, Address, Type));
    }
    __except(EXCEPTION_EXECUTE_HANDLER)  
    {  
        DoOutputErrorString("SetBreakpointWithoutThread: Exception calling SetDebugRegister");
        return FALSE;
    }
	
    // Debug
    DoOutputDebugString("SetBreakpointWithoutThread: bp set with register %d\n", Register);

    return TRUE; 
}

//**************************************************************************************
BOOL SetBreakpoint
//**************************************************************************************
(
    DWORD	ThreadId,
    int		Register,
    int		Size,
    LPVOID	Address,
    DWORD	Type,
	PVOID	Callback
)
{
    //if (DisableThreadSuspend)
    //    return SetBreakpointWithoutThread(ThreadId, Register, Size, Address, Type, Callback);

    PBREAKPOINTINFO pBreakpointInfo;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
	HANDLE hSetBreakpointThread;
    DWORD SetBreakpointThreadId;
    BOOL RetVal;
    
    if (Register > 3 || Register < 0)
    {
        DoOutputDebugString("SetBreakpoint: Error - register value %d, can only have value 0-3.\n", Register);
        return FALSE;
    }  
	
    CurrentThreadBreakpoint = GetThreadBreakpoints(ThreadId);

	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetBreakpoint: Creating new thread breakpoints for thread 0x%x.\n", ThreadId);
		CurrentThreadBreakpoint = CreateThreadBreakpoints(ThreadId);
	}
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetBreakpoint: Cannot create new thread breakpoints - error.\n");
		return FALSE;
	}

	pBreakpointInfo = &CurrentThreadBreakpoint->BreakpointInfo[Register];
	
	if (CurrentThreadBreakpoint->ThreadHandle == NULL)
	{
		DoOutputDebugString("SetBreakpoint: There is no thread handle in the thread breakpoint - Error.\n");
		return FALSE;
	}
    	
	pBreakpointInfo->ThreadHandle   = CurrentThreadBreakpoint->ThreadHandle;
	pBreakpointInfo->Register       = Register;
	pBreakpointInfo->Size           = Size;
	pBreakpointInfo->Address        = Address;
	pBreakpointInfo->Type	        = Type;
	pBreakpointInfo->Callback       = Callback;

    if (VECTORED_HANDLER)
        AddVectoredExceptionHandler(1, CAPEExceptionFilter);
    else
        OriginalExceptionHandler = SetUnhandledExceptionFilter(CAPEExceptionFilter);

    __try  
    {  
        hSetBreakpointThread = CreateThread( 
            NULL,               
            0,                  
            SetBreakpointThread,
            pBreakpointInfo,    
            0,                  
            &SetBreakpointThreadId);          
    }  
    __except(EXCEPTION_EXECUTE_HANDLER)  
    {  
        DoOutputErrorString("SetBreakpoint: Unable to create SetBreakpointThread thread");
    }

	if (hSetBreakpointThread) 
    {
        // If this hasn't happened in under a second, we bail
        // and set without creating a thread
        RetVal = WaitForSingleObject(hSetBreakpointThread, 1000);
        
        if (RetVal != WAIT_OBJECT_0)
        {
            TerminateThread(hSetBreakpointThread, 0);
			DoOutputDebugString("SetBreakpoint: SetBreakpointThread timeout, thread killed.\n");
            
            RetVal = ResumeThread(CurrentThreadBreakpoint->ThreadHandle);
            if (RetVal == -1)
            {
                DoOutputErrorString("SetBreakpoint: ResumeThread failed. About to set breakpoint without thread.\n");
            }
            else if (RetVal == 0)
            {
                DoOutputDebugString("SetBreakpoint: Sample thread was not suspended. About to set breakpoint without thread.\n");
            }
            else
            {
                DoOutputDebugString("SetBreakpoint: Sample thread was suspended, now resumed. About to set breakpoint without thread.\n");
            }
            
            return SetBreakpointWithoutThread(ThreadId, Register, Size, Address, Type, Callback);
        }   
        
        DoOutputDebugString("SetBreakpoint: Set bp %d type %d at address 0x%x, size %d with Callback 0x%x, ThreadHandle = 0x%x.\n", 
            pBreakpointInfo->Register, 
            pBreakpointInfo->Type,
            pBreakpointInfo->Address, 
            pBreakpointInfo->Size, 
            pBreakpointInfo->Callback, 
            pBreakpointInfo->ThreadHandle
            );

        CloseHandle(hSetBreakpointThread);
        
        return TRUE;
    }
	else
    {
        __try
        {
            RetVal = SetBreakpointWithoutThread(ThreadId, Register, Size, Address, Type, Callback);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)  
        {  
            DoOutputErrorString("SetBreakpoint: Error calling SetBreakpointWithoutThread");
            return FALSE;
        }
        
        return RetVal;
    }
}

//**************************************************************************************
BOOL ClearBreakpoint(DWORD ThreadId, int Register)
//**************************************************************************************
{
    return ClearBreakpointWithoutThread(ThreadId, Register);
/*    
    PBREAKPOINTINFO pBreakpointInfo;
	PTHREADBREAKPOINTS CurrentThreadBreakpoint;
	HANDLE hClearBreakpointThread;
    BOOL RetVal;

    if (Register > 3 || Register < 0)
    {
        DoOutputDebugString("ClearBreakpoint: Error - register value %d, can only have value 0-3.\n", Register);
        return FALSE;
    }  
		
	CurrentThreadBreakpoint = GetThreadBreakpoints(ThreadId);
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("Cannot find thread breakpoints - failed to clear.\n");
		return FALSE;
	}

	pBreakpointInfo = &CurrentThreadBreakpoint->BreakpointInfo[Register];
	
	if (CurrentThreadBreakpoint->ThreadHandle == NULL)
	{
		DoOutputDebugString("ClearBreakpoint: There is no thread handle in the thread breakpoint - Error.\n");
		return FALSE;
	}
	
	pBreakpointInfo->ThreadHandle = CurrentThreadBreakpoint->ThreadHandle;
    
    __try
    {
        hClearBreakpointThread = CreateThread(NULL, 0,  ClearBreakpointThread, pBreakpointInfo,	0, &ThreadId);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)  
    {  
        DoOutputErrorString("ClearBreakpoint: Unable to create ClearBreakpointThread thread");
    }

	if (hClearBreakpointThread) 
    {
        DoOutputDebugString("ClearBreakpoint: thread created, handle 0x%x.\n", hClearBreakpointThread);
        
        // If this hasn't happened in under a second, we bail
        // and clear without creating a thread
        RetVal = WaitForSingleObject(hClearBreakpointThread, 1000);

        DoOutputDebugString("ClearBreakpoint: Aboot tae close handle.\n");
        //CloseHandle(hClearBreakpointThread);
        
        if (RetVal != WAIT_OBJECT_0)
        {
			DoOutputDebugString("ClearBreakpoint: thread timeout, falling back to clearing without thread.\n");

            return ClearBreakpointWithoutThread(ThreadId, Register);
        }   
        
        DoOutputDebugString("ClearBreakpoint: Cleared breakpoint %d.\n", pBreakpointInfo->Register);

        return TRUE;
    }
	else
    {
        __try
        {
            RetVal = ClearBreakpointWithoutThread(ThreadId, Register);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)  
        {  
            DoOutputErrorString("ClearBreakpoint: Error calling ClearBreakpointWithoutThread");
            return FALSE;
        }
        
        return RetVal;
    }
    
	//pBreakpointInfo->Register = 0;
	pBreakpointInfo->Size = 0;
	pBreakpointInfo->Address = 0;
	pBreakpointInfo->Type	  = 0;
	pBreakpointInfo->Callback = NULL;
	
    return TRUE;
*/
}

//**************************************************************************************
BOOL SetNextAvailableBreakpoint
//**************************************************************************************
(
    DWORD	        ThreadId,
    unsigned int*	Register,
    int		        Size,
    LPVOID	        Address,
    DWORD	        Type,
	PVOID	        Callback
)
{
	PTHREADBREAKPOINTS CurrentThreadBreakpoint = GetThreadBreakpoints(ThreadId);

	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetNextAvailableBreakpoint: Creating new thread breakpoints for thread 0x%x.\n", ThreadId);
		CurrentThreadBreakpoint = CreateThreadBreakpoints(ThreadId);
	}
	
	if (CurrentThreadBreakpoint == NULL)
	{
		DoOutputDebugString("SetNextAvailableBreakpoint: Cannot create new thread breakpoints - FATAL.\n");
		return FALSE;
	}

    if (GetNextAvailableBreakpoint(ThreadId, Register) == FALSE)
    {
        DoOutputDebugString("SetNextAvailableBreakpoint: GetNextAvailableBreakpoint failed\n");
        return FALSE;
    }

    return SetBreakpoint(ThreadId, *Register, Size, Address, Type, Callback);
}

//**************************************************************************************
BOOL InitialiseDebugger(void)
//**************************************************************************************
{
    HANDLE MainThreadHandle;

	MainThreadId = GetCurrentThreadId();

	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &MainThreadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
	{
		DoOutputDebugString("Failed to duplicate thread handle.\n");
		return FALSE;
	}

	MainThreadBreakpointList = CreateThreadBreakpoints(MainThreadId);

    if (MainThreadBreakpointList == NULL)
    {
		DoOutputDebugString("Failed to create thread breakpoints struct.\n");
		return FALSE;        
    }
    
    if (MainThreadBreakpointList->ThreadHandle == NULL)
    {
		DoOutputDebugString("InitialiseDebugger error: main thread handle not set.\n");
		return FALSE;        
    }
    
    // Initialise any global variables
    ChildProcessId = 0;
    SingleStepHandler = NULL;
    VECTORED_HANDLER = TRUE;

#ifndef _WIN64
    // Ensure wow64 patch is installed if needed
    WoW64fix();
#endif	
    
    return TRUE;
}

//**************************************************************************************
DWORD_PTR GetNestedStackPointer(void)
//**************************************************************************************
{   
    CONTEXT context;

    RtlCaptureContext(&context);
 
#ifdef _WIN64
    return (DWORD_PTR)context.Rsp;
#else
    return (DWORD_PTR)context.Esp;
#endif        
}

#ifndef _WIN64
//**************************************************************************************
__declspec (naked dllexport) void DebuggerInit(void)
//**************************************************************************************
{   
    DWORD StackPointer;

    _asm
        {
        push	ebp
        mov		ebp, esp
        // we need the stack pointer
        mov		StackPointer, esp
        sub		esp, __LOCAL_SIZE
		pushad
        }
	
	if (InitialiseDebugger() == FALSE)
        DoOutputDebugString("Debugger initialisation failure!\n");
	
// Package specific code

// End of package specific code

	DoOutputDebugString("Debugger initialisation complete, about to execute OEP.\n");

    _asm
    {
        popad
		mov     esp, ebp
        pop     ebp
        jmp		OEP
    }
}
#else
#pragma optimize("", off)
//**************************************************************************************
void DebuggerInit(void)
//**************************************************************************************
{   
    DWORD_PTR StackPointer;

    StackPointer = GetNestedStackPointer() - 8; // this offset has been determined experimentally - TODO: tidy
    
	if (InitialiseDebugger() == FALSE)
        DoOutputDebugString("Debugger initialisation failure!\n");
	else
        DoOutputDebugString("Debugger initialised, ESP = 0x%x\n", StackPointer);
    
// Package specific code

// End of package specific code

	DoOutputDebugString("Debugger initialisation complete, about to execute OEP.\n");

    OEP();
}
#pragma optimize("", on)
#endif

BOOL SendDebuggerMessage(DWORD Input)
{ 
    BOOL fSuccess;
	DWORD cbReplyBytes, cbWritten; 
    //struct DEBUGGER_DATA DebuggerData;
   
    //memset(&DebuggerData, 0, sizeof(struct DEBUGGER_DATA));

    cbReplyBytes = sizeof(DWORD_PTR);
    
    if (hParentPipe == NULL)
    {   
        DoOutputErrorString("SendDebuggerMessage: hParentPipe NULL.");
        return FALSE;
    }

    // Write the reply to the pipe. 
    fSuccess = WriteFile
    ( 
        hParentPipe,        // handle to pipe 
        &Input,     		// buffer to write from 
        cbReplyBytes, 		// number of bytes to write 
        &cbWritten,   		// number of bytes written 
        NULL          		// not overlapped I/O 
    );

    if (!fSuccess || cbReplyBytes != cbWritten)
    {   
        DoOutputErrorString("SendDebuggerMessage: Failed to send message via pipe");
        return FALSE;
    }

    DoOutputDebugString("SendDebuggerMessage: Sent message via pipe.\n");
    
    return TRUE;
}

//**************************************************************************************
DWORD WINAPI DebuggerLaunch(LPVOID lpParam)
//**************************************************************************************
{ 
	HANDLE hPipe; 
	BOOL   fSuccess = FALSE, NT5; 
	DWORD  cbRead, cbToWrite, cbWritten, dwMode;
	PVOID  FuncAddress;

	char lpszPipename[MAX_PATH]; 
    OSVERSIONINFO VersionInfo;
    
	DoOutputDebugString("DebuggerLaunch: About to connect to CAPEpipe.\n");

    memset(lpszPipename, 0, MAX_PATH*sizeof(CHAR));
    sprintf_s(lpszPipename, MAX_PATH, "\\\\.\\pipe\\CAPEpipe_%x", GetCurrentProcessId());
	while (1) 
	{ 
		hPipe = CreateFile(
		lpszPipename,   
		GENERIC_READ |  
		GENERIC_WRITE,  
		0,              
		NULL,           
		OPEN_EXISTING,  
		0,              
		NULL);          

		if (hPipe != INVALID_HANDLE_VALUE) 
			break; 

		if (GetLastError() != ERROR_PIPE_BUSY) 
		{
			DoOutputErrorString("DebuggerLaunch: Could not open pipe"); 
			return -1;
		}

		if (!WaitNamedPipe(lpszPipename, 20)) 
		{ 
			DoOutputDebugString("DebuggerLaunch: Could not open pipe: 20 ms wait timed out.\n"); 
			return -1;
		} 
	} 

	// The pipe connected; change to message-read mode. 
	dwMode = PIPE_READMODE_MESSAGE; 
	fSuccess = SetNamedPipeHandleState
    (
		hPipe,  
		&dwMode,
		NULL,   
		NULL    
	);
	if (!fSuccess) 
	{
		DoOutputDebugString("DebuggerLaunch: SetNamedPipeHandleState failed.\n"); 
		return -1;
	}

	// Send VA of DebuggerInit to loader
	FuncAddress = &DebuggerInit;
	
	cbToWrite = sizeof(PVOID);
	
	fSuccess = WriteFile
    (
		hPipe,       
		&FuncAddress,
		cbToWrite,   
		&cbWritten,  
		NULL         
    );
	if (!fSuccess) 
	{
		DoOutputErrorString("DebuggerLaunch: WriteFile to pipe failed"); 
		return -1;
	}

	DoOutputDebugString("DebuggerLaunch: DebuggerInit VA sent to loader: 0x%x\n", FuncAddress);

	fSuccess = ReadFile(
		hPipe,    				
		&OEP, 
		sizeof(DWORD_PTR),  		 
		&cbRead,
		NULL);  
        
	if (!fSuccess && GetLastError() == ERROR_MORE_DATA)
	{
		DoOutputDebugString("DebuggerLaunch: ReadFile on Pipe: ERROR_MORE_DATA\n");
		CloseHandle(hPipe);
		return -1;
	}
	
	if (!fSuccess)
	{
		DoOutputErrorString("DebuggerLaunch: ReadFile (OEP) from pipe failed");
		CloseHandle(hPipe);
		return -1;
	}

	DoOutputDebugString("Read OEP from pipe: 0x%x\n", OEP);
    
    fSuccess = ReadFile(
        hPipe,    				
        &OEP, 
        sizeof(DWORD_PTR),  		 
        &cbRead,
        NULL);  
        
    if (!fSuccess && GetLastError() == ERROR_MORE_DATA)
    {
        DoOutputDebugString("DebuggerLaunch: ReadFile on Pipe: ERROR_MORE_DATA\n");
        CloseHandle(hPipe);
        return -1;
    }
    
    if (!fSuccess && GetLastError() == ERROR_BROKEN_PIPE)
    {
        DoOutputDebugString("DebuggerLaunch: Pipe closed, no further updates to OEP\n");
        CloseHandle(hPipe);
    }
    else if (!fSuccess)
    {
        DoOutputErrorString("ReadFile from pipe failed");
        CloseHandle(hPipe);
        return -1;
    }
    else
        DoOutputDebugString("Read thread EP from pipe: 0x%x\n", OEP);
  
    ZeroMemory(&VersionInfo, sizeof(OSVERSIONINFO));
    VersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&VersionInfo);

    NT5 = (VersionInfo.dwMajorVersion == 5);
    
    if (NT5)
    {
       	DoOutputDebugString("NT5: Leaving debugger thread alive.\n");
        while(1)
        {
            Sleep(500000);
        }
    }

    DoOutputDebugString("NT6+: Terminating debugger thread.\n");
    
	return 0; 
}

//**************************************************************************************
BOOL DebugNewProcess(unsigned int ProcessId, unsigned int ThreadId, DWORD CreationFlags)
//**************************************************************************************
{
    HANDLE hProcess, hThread; 
	char lpszPipename[MAX_PATH];
    BOOL fSuccess, fConnected;
    CONTEXT Context;
    DWORD cbBytesRead, cbWritten, cbReplyBytes; 

    memset(lpszPipename, 0, MAX_PATH*sizeof(CHAR));
    sprintf_s(lpszPipename, MAX_PATH, "\\\\.\\pipe\\CAPEpipe_%x", ProcessId);

	hProcess = OpenProcess(PROCESS_ALL_ACCESS, TRUE, ProcessId);
    if (hProcess == NULL)
    {
        DoOutputErrorString("DebugNewProcess: OpenProcess failed");
        return FALSE;
    }

    hThread = OpenThread(THREAD_ALL_ACCESS, TRUE, ThreadId);
    if (hThread == NULL) 
    {
        DoOutputErrorString("DebugNewProcess: OpenThread failed");
        return FALSE;
    }

    hParentPipe = CreateNamedPipe
    ( 
        lpszPipename,             	
        PIPE_ACCESS_DUPLEX,       	
        PIPE_TYPE_MESSAGE |       	
        PIPE_READMODE_MESSAGE |   	
        PIPE_WAIT,                	
        PIPE_UNLIMITED_INSTANCES, 	
        PIPEBUFSIZE,                
        PIPEBUFSIZE,                
        0,                        	
        NULL
    );								

    if (hParentPipe == INVALID_HANDLE_VALUE) 
    {
        DoOutputErrorString("DebugNewProcess: CreateNamedPipe failed");
        return FALSE;
    }

    DoOutputDebugString("DebugNewProcess: Announcing new process to Cuckoo, pid: %d\n", ProcessId);
    pipe("DEBUGGER:%d,%d", ProcessId, ThreadId);

    fConnected = ConnectNamedPipe(hParentPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED); 
    fSuccess = FALSE;
    cbBytesRead = 0;
    
    if (!fConnected) 
    {
        DoOutputDebugString("DebugNewProcess: The client could not connect, closing pipe.\n");
        CloseHandle(hParentPipe);
        return FALSE;
    }

    DoOutputDebugString("DebugNewProcess: Client connected\n");
    
    fSuccess = ReadFile
    ( 
        hParentPipe,        
        &RemoteFuncAddress, 
        sizeof(DWORD_PTR),		
        &cbBytesRead, 		
        NULL          		
    );
    
    if (!fSuccess || cbBytesRead == 0)
    {   
        if (GetLastError() == ERROR_BROKEN_PIPE)
        {
            DoOutputErrorString("DebugNewProcess: Client disconnected.");
        }
        else
        {
            DoOutputErrorString("DebugNewProcess: ReadFile failed.");
        }
    }

    if (!RemoteFuncAddress)
    {
        DoOutputErrorString("DebugNewProcess: Successfully read from pipe, however RemoteFuncAddress = 0.");
        return FALSE;
    }
    
    Context.ContextFlags = CONTEXT_ALL;
    if (!GetThreadContext(hThread, &Context))
    {
        DoOutputDebugString("DebugNewProcess: GetThreadContext failed - FATAL\n");
        return FALSE;
    }

#ifdef _WIN64
    OEP = (PVOID)Context.Rcx;
#else
    OEP = (PVOID)Context.Eax;
#endif    
    
    cbWritten = 0;
    cbReplyBytes = sizeof(DWORD_PTR);

    // Send the OEP to the new process 
    fSuccess = WriteFile
    ( 
        hParentPipe,     
        &OEP,		     
        cbReplyBytes,
        &cbWritten,  
        NULL         
    );
    if (!fSuccess || cbReplyBytes != cbWritten)
    {   
        DoOutputErrorString("DebugNewProcess: Failed to send OEP via pipe.");
        return FALSE;
    }

    DoOutputDebugString("DebugNewProcess: Sent OEP 0x%x via pipe\n", OEP);

    Context.ContextFlags = CONTEXT_ALL;
    
#ifdef _WIN64
    Context.Rcx = RemoteFuncAddress;		// set the new EP to debugger_init
#else
    Context.Eax = RemoteFuncAddress;		
#endif  
    
    if (!SetThreadContext(hThread, &Context))
    {
        DoOutputDebugString("DebugNewProcess: Failed to set new EP\n");
        return FALSE;
    }

#ifdef _WIN64
    DoOutputDebugString("DebugNewProcess: Set new EP to DebuggerInit: 0x%x\n", Context.Rcx);
#else
    DoOutputDebugString("DebugNewProcess: Set new EP to DebuggerInit: 0x%x\n", Context.Eax);
#endif
    
    CloseHandle(hProcess);
    CloseHandle(hThread);

	return TRUE;
}

//**************************************************************************************
int launch_debugger()
//**************************************************************************************
{
	DWORD NewThreadId;
	HANDLE hDebuggerLaunch;

    hDebuggerLaunch = CreateThread(
        NULL,		
        0,             
        DebuggerLaunch,
        NULL,		
        0,             
        &NewThreadId); 

    if (hDebuggerLaunch == NULL) 
    {
       DoOutputDebugString("CAPE: Failed to create debugger launch thread.\n");
       return 0;
    }
    else
    {
        DoOutputDebugString("CAPE: Launching debugger.\n");
    }

	CloseHandle(hDebuggerLaunch);
    
    return 1;
}