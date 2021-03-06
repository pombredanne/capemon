extern HMODULE s_hInst;
extern WCHAR s_wzDllPath[MAX_PATH];
extern CHAR s_szDllPath[MAX_PATH];
BOOL TranslatePathFromDeviceToLetter(__in TCHAR *DeviceFilePath, __out TCHAR* DriveLetterFilePath, __inout LPDWORD lpdwBufferSize);
int DumpMemory(LPVOID Buffer, unsigned int Size);
BOOL DumpPEsInRange(LPVOID Buffer, unsigned int Size);
int DumpCurrentProcessNewEP(DWORD NewEP);
int DumpCurrentProcess();
int DumpProcess(HANDLE hProcess, DWORD_PTR ImageBase);
int DumpPE(LPVOID Buffer);
int ScyllaDumpPE(DWORD_PTR Buffer);
int ScanForNonZero(LPVOID Buffer, unsigned int Size);
int ScanPageForNonZero(LPVOID Address);
int ScanForPE(LPVOID Buffer, unsigned int Size, LPVOID* Offset);
int ScanForDisguisedPE(LPVOID Buffer, unsigned int Size, LPVOID* Offset);
int IsDisguisedPE(LPVOID Buffer, unsigned int Size);
int DumpImageInCurrentProcess(DWORD ImageBase);
void DumpSectionViewsForPid(DWORD Pid);
unsigned int DumpSize;
SYSTEM_INFO SystemInfo;

//Global switch for debugger
#define DEBUGGER_ENABLED        0
#define GUARD_PAGES_ENABLED     0

typedef struct InjectionSectionView
{
    HANDLE                          SectionHandle;
    PVOID                           LocalView;
    SIZE_T                          ViewSize;
	int                             TargetProcessId;
    struct InjectionSectionView     *NextSectionView;
} INJECTIONSECTIONVIEW, *PINJECTIONSECTIONVIEW;

PINJECTIONSECTIONVIEW AddSectionView(HANDLE SectionHandle, PVOID LocalView, SIZE_T ViewSize);
PINJECTIONSECTIONVIEW GetSectionView(HANDLE SectionHandle);
BOOL DropSectionView(PINJECTIONSECTIONVIEW SectionView);

typedef struct InjectionInfo
{
    int                         ProcessId;
	HANDLE	                    ProcessHandle;
    DWORD_PTR                   ImageBase;
    DWORD_PTR                   EntryPoint;
    BOOL                        WriteDetected;
    BOOL                        ImageDumped;
    LPVOID                     BufferBase;
    unsigned int                BufferSizeOfImage;
    HANDLE                      SectionHandle;
//    struct InjectionSectionView *SectionViewList;
    struct InjectionInfo        *NextInjectionInfo;
} INJECTIONINFO, *PINJECTIONINFO;

struct InjectionInfo *InjectionInfoList;

PINJECTIONINFO GetInjectionInfo(DWORD ProcessId);
PINJECTIONINFO CreateInjectionInfo(DWORD ProcessId);

struct InjectionSectionView *SectionViewList;

//
// MessageId: STATUS_SUCCESS
//
// MessageText:
//
//  STATUS_SUCCESS
//
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)

//
// MessageId: STATUS_BAD_COMPRESSION_BUFFER
//
// MessageText:
//
// The specified buffer contains ill-formed data.
//
#define STATUS_BAD_COMPRESSION_BUFFER    ((NTSTATUS)0xC0000242L)

#define	PE_HEADER_LIMIT		0x200	// Range to look for PE header within candidate buffer

#define SIZE_OF_LARGEST_IMAGE ((ULONG)0x77000000)

#pragma comment(lib, "Wininet.lib")

#define	DATA				0
#define	EXECUTABLE			1
#define	DLL			        2

#define PLUGX_SIGNATURE		0x5658	// 'XV'
#define	PE_HEADER_LIMIT		0x200	// Range to look for PE header within candidate buffer

typedef struct CapeMetadata 
{
	char*	ProcessPath;
	char*	ModulePath;
    DWORD   Pid;
    DWORD   DumpType;
    char*	TargetProcess;  // For injection
    DWORD	TargetPid;      // "
    PVOID   Address;        // For shellcode/modules
	SIZE_T  Size;           // "
} CAPEMETADATA, *PCAPEMETADATA;

struct CapeMetadata *CapeMetaData;

BOOL SetCapeMetaData(DWORD DumpType, DWORD TargetPid, HANDLE hTargetProcess, PVOID Address);

enum {
    PROCDUMP                = 0,
    
    COMPRESSION             = 1,
    
    INJECTION_PE            = 3,
    INJECTION_SHELLCODE     = 4,
    //INJECTION_RUNPE         = 5,

    EXTRACTION_PE           = 8,
    EXTRACTION_SHELLCODE    = 9,
    
    PLUGX_PAYLOAD           = 0x10,
    PLUGX_CONFIG            = 0x11,
    
    EVILGRAB_PAYLOAD        = 0x14,
    EVILGRAB_DATA           = 0x15    
};

HANDLE EvilGrabRegHandle;

