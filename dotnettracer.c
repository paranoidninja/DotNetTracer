#include <windows.h>
#include <stdio.h>
#include <wbemidl.h>
#include <wmistr.h>
#include <evntrace.h>

// compile with:
// x86_64-w64-mingw32-gcc dotnettracer.c -o dotnettracer.exe -ladvapi32

#define AssemblyDCStart_V1 155
#define PROCESS_TRACE_MODE_REAL_TIME                0x00000100
#define PROCESS_TRACE_MODE_EVENT_RECORD             0x10000000
#define CLR_LOADER_KEYWORD 0x8
#define CLR_STARTENUMERATION_KEYWORD 0x40
// https://docs.microsoft.com/en-us/dotnet/framework/performance/clr-etw-providers
GUID clrRuntimeProvider = { 0xe13c0d23, 0xccbc, 0x4e12, { 0x93, 0x1b, 0xd9, 0xcc, 0x2e, 0xee, 0x27, 0xe4 } };
extern ULONG EnableTraceEx(LPCGUID ProviderId, LPCGUID SourceId, TRACEHANDLE TraceHandle, ULONG IsEnabled, UCHAR Level, ULONGLONG MatchAnyKeyword, ULONGLONG MatchAllKeyword, ULONG EnableProperty, PEVENT_FILTER_DESCRIPTOR EnableFilterDesc);
extern ULONG EnableTraceEx2(TRACEHANDLE TraceHandle, LPCGUID ProviderId, ULONG ControlCode, UCHAR Level, ULONGLONG MatchAnyKeyword, ULONGLONG MatchAllKeyword, ULONG Timeout, PENABLE_TRACE_PARAMETERS EnableParameters);

typedef struct _EVENT_DESCRIPTOR {
    USHORT Id;
    UCHAR Version;
    UCHAR Channel;
    UCHAR Level;
    UCHAR Opcode;
    USHORT Task;
    ULONGLONG Keyword;
} EVENT_DESCRIPTOR, *PEVENT_DESCRIPTOR;
typedef const EVENT_DESCRIPTOR *PCEVENT_DESCRIPTOR;

typedef struct _EVENT_HEADER {
    USHORT Size;
    USHORT HeaderType;
    USHORT Flags;
    USHORT EventProperty;
    ULONG ThreadId;
    ULONG ProcessId;
    LARGE_INTEGER TimeStamp;
    GUID ProviderId;
    EVENT_DESCRIPTOR EventDescriptor;
    union {
        struct {
            ULONG KernelTime;
            ULONG UserTime;
        } DUMMYSTRUCTNAME;
        ULONG64 ProcessorTime;
    } DUMMYUNIONNAME;
    GUID ActivityId;
} EVENT_HEADER, *PEVENT_HEADER;

typedef struct _EVENT_HEADER_EXTENDED_DATA_ITEM {
    USHORT Reserved1;
    USHORT ExtType;
    struct {
        USHORT Linkage   : 1;
        USHORT Reserved2 : 15;
    };
    USHORT DataSize;
    ULONGLONG DataPtr;
} EVENT_HEADER_EXTENDED_DATA_ITEM, *PEVENT_HEADER_EXTENDED_DATA_ITEM;

typedef struct _EVENT_RECORD_DUP {
    EVENT_HEADER EventHeader;
    ETW_BUFFER_CONTEXT BufferContext;
    USHORT ExtendedDataCount;
    USHORT UserDataLength;
    PEVENT_HEADER_EXTENDED_DATA_ITEM ExtendedData;
    PVOID UserData;
    PVOID UserContext;
} EVENT_RECORD_DUP, *PEVENT_RECORD_DUP;

#pragma pack(1)
typedef struct _AssemblyLoadUnloadRundown_V1
{
    ULONG64 AssemblyID;
    ULONG64 AppDomainID;
    ULONG64 BindingID;
    ULONG AssemblyFlags;
    WCHAR FullyQualifiedAssemblyName[1];
} AssemblyLoadUnloadRundown_V1, * PAssemblyLoadUnloadRundown_V1;
#pragma pack()

VOID eventHandler(PEVENT_RECORD_DUP EventRecord) {
    PEVENT_HEADER eventHeader = &EventRecord->EventHeader;
    PEVENT_DESCRIPTOR eventDescriptor = &eventHeader->EventDescriptor;
    AssemblyLoadUnloadRundown_V1* assemblyUserData;
    switch (eventDescriptor->Id) {
    case AssemblyDCStart_V1:
        assemblyUserData = (AssemblyLoadUnloadRundown_V1*)EventRecord->UserData;
        printf("[+] PID: %lu | TID: %lu | Assembly: %ls\n", eventHeader->ProcessId, eventHeader->ThreadId, assemblyUserData->FullyQualifiedAssemblyName);
        break;
    }
}

void startTracing(CHAR* traceName) {
    TRACEHANDLE hTrace = 0;
    ULONG errVal = 0;
    ULONG bufSize = 0;
    PEVENT_TRACE_PROPERTIES eventTracer = NULL;
    bufSize = sizeof(EVENT_TRACE_PROPERTIES) + strlen(traceName) + sizeof(WCHAR);
    eventTracer = (PEVENT_TRACE_PROPERTIES)calloc(bufSize, 1);
    eventTracer->Wnode.BufferSize = bufSize;
    eventTracer->Wnode.ClientContext = 2;
    eventTracer->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    eventTracer->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_PAGED_MEMORY;
    eventTracer->EnableFlags = EVENT_TRACE_FLAG_NO_SYSCONFIG;
    eventTracer->LogFileNameOffset = 0;
    eventTracer->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    errVal = StartTraceA(&hTrace, traceName, eventTracer);
    if (errVal) {
        if (errVal == 183) {
            printf("Collector Already exists. Use 'read' to read from the collector, or 'stop' to stop the collector\n");
            ExitProcess(0);
        }
        printf("[-] E1: %lu\n", errVal);
        ExitProcess(0);
    }

    ENABLE_TRACE_PARAMETERS enableParameters = { 0 };
    enableParameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    // errVal = EnableTraceEx(&clrRuntimeProvider, NULL, hTrace, 1, TRACE_LEVEL_VERBOSE, 0x8, 0, 0, NULL); //Detailed diagnostic events
    errVal = EnableTraceEx2(hTrace, &clrRuntimeProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, CLR_LOADER_KEYWORD | CLR_STARTENUMERATION_KEYWORD, 0, 0, &enableParameters);
    if (errVal) {
        printf("[-] E2: %lu\n", errVal);
        ExitProcess(0);
    }
    printf("[+] Tracing Enabled\n");
}

void readTracing(CHAR* traceName) {
    EVENT_TRACE_LOGFILEA logTracer = { 0 };
    logTracer.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logTracer.LoggerName = (LPSTR)traceName;
    logTracer.EventRecordCallback = (PEVENT_RECORD_CALLBACK)eventHandler;
    TRACEHANDLE hTrace = OpenTraceA(&logTracer);
    if (hTrace == INVALID_PROCESSTRACE_HANDLE) {
        printf("[-] E3: lu\n", GetLastError());
        ExitProcess(0);
    }

    printf("[+] Reading ETW Events..\n");
    ULONG errVal = ProcessTrace(&hTrace, 1, NULL, NULL);
    if (errVal) {
        printf("[-] E4: %lu\n", errVal);
        ExitProcess(0);
    }
}

void stopTracing(CHAR* traceName) {
    PEVENT_TRACE_PROPERTIES eventTracer = NULL;
    SIZE_T bufSize = sizeof(EVENT_TRACE_PROPERTIES) + strlen(traceName) + sizeof(WCHAR);
    eventTracer = (PEVENT_TRACE_PROPERTIES)calloc(bufSize, 1);
    eventTracer->Wnode.BufferSize = bufSize;
    eventTracer->LogFileNameOffset = 0;
    eventTracer->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ULONG errVal = StopTraceA(0, traceName, eventTracer);
    if (errVal) {
        printf("[-] E5: %lu\n", errVal);
        ExitProcess(0);
    }
    printf("[+] Tracing stopped\n");
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage:\n[+] Start tracing: %s start <traceName>\n", argv[0]);
        printf("[+] Read exising trace: %s read <traceName>\n", argv[0]);
        printf("[+] Stop tracing: %s stop <traceName>\n", argv[0]);
        return 0;
    }

    CHAR *traceName = argv[2];
    if (strcmp((argv[1]), "start") == 0) {
        startTracing(traceName);
        readTracing(traceName);
    } else if (strcmp((argv[1]), "read") == 0) {
        readTracing(traceName);
    } else if (strcmp((argv[1]), "stop") == 0) {     
        stopTracing(traceName);
    }

    return 0;
}
