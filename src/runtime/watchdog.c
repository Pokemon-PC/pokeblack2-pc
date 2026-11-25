/* watchdog.c - where is the game? Every PB2_WATCH seconds (default 8) the
 * game thread is suspended and its call stack printed (host addresses:
 * tools/symbolize.py turns them into game function names). */
#include "rt.h"

#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

static HANDLE sGameThread;

void rt_write_err(const char *s, int n) {
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)n, &w, NULL);
}

/* fprintf(stderr) without the CRT lock (the suspended game thread may hold it) */
static void errf(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) {
        rt_write_err(b, n < (int)sizeof b ? n : (int)sizeof b - 1);
    }
}
static int sPeriod;
static DWORD64 sBase;

int gFnTrace;
u32 gFnRing[FNTRACE_N][2];
unsigned gFnPos;

static void print_fntrace(void) {
    if (!gFnTrace) {
        return;
    }
    unsigned end = gFnPos, n = end < 64 ? end : 64;
    for (unsigned i = end - n; i != end; i++) {
        errf("[pb2:fn] %08X (lr %08X)\n", gFnRing[i % FNTRACE_N][0], gFnRing[i % FNTRACE_N][1]);
    }
}

static void print_stack(CONTEXT c) {
    print_fntrace();
    for (int depth = 0; depth < 40 && c.Rip; depth++) {
        /* relative to the image base chosen by the linker (0x140000000) */
        errf("[pb2:stack] #%02d 0x%016llX\n", depth,
                (unsigned long long)(c.Rip - sBase + 0x140000000ull));
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!fn) {
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        PVOID hd;
        DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &hd, &est, NULL);
    }
}

static DWORD WINAPI watch_main(LPVOID arg) {
    (void)arg;
    for (;;) {
        Sleep((DWORD)sPeriod * 1000);
        if (SuspendThread(sGameThread) == (DWORD)-1) {
            continue;
        }
        CONTEXT c;
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(sGameThread, &c)) {
            rt_log("watchdog: game thread at frame %u, r0=%08X r1=%08X sp=%08X lr=%08X", gFrame, R(0), R(1), R(13), R(14));
            void thread_dump(void);
            thread_dump();
            void sysarea_dump(void);
            sysarea_dump();
            print_stack(c);
        }
        ResumeThread(sGameThread);
    }
    return 0;
}

static u32 sWatchAddr;
static int sWatchHits;

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && sWatchAddr && (ep->ContextRecord->Dr6 & 1)) {
        ep->ContextRecord->Dr6 = 0;
        static long from = -1;
        if (from < 0) {
            from = getenv("PB2_WATCHFROM") ? atol(getenv("PB2_WATCHFROM")) : 0;
        }
        if (gFrame >= (u32)from && sWatchHits++ < 40) {
            rt_log("WATCH %08X = %08X at frame %u; game r0=%08X r1=%08X r2=%08X r3=%08X sp=%08X lr=%08X",
                   sWatchAddr, *(volatile u32 *)(uintptr_t)sWatchAddr, gFrame, R(0), R(1), R(2), R(3), R(13), R(14));
            CONTEXT c = *ep->ContextRecord;
            for (int i = 0; i < 6 && c.Rip; i++) {
                errf("[pb2:watch] #%d 0x%016llX\n", i, (unsigned long long)(c.Rip - sBase + 0x140000000ull));
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                if (!fn) {
                    break;
                }
                PVOID hd;
                DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &hd, &est, NULL);
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    rt_log("CRASH 0x%08lX at host 0x%llX, address 0x%llX; game r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X sp=%08X lr=%08X",
           (unsigned long)code, (unsigned long long)(ep->ContextRecord->Rip - sBase + 0x140000000ull),
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], R(0), R(1), R(2), R(3), R(4), R(5), R(13), R(14));
    print_stack(*ep->ContextRecord);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);   /* ExitProcess can hang on a crashed thread */
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI set_dr(LPVOID arg) {
    HANDLE t = arg;
    SuspendThread(t);
    CONTEXT c;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(t, &c);
    c.Dr0 = sWatchAddr;
    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);   /* L0, write, 4 bytes */
    SetThreadContext(t, &c);
    ResumeThread(t);
    return 0;
}

/* PB2_PROFILE=file: samples where the game thread is, 1000 times a second;
 * the host addresses and their counts are written to the file at exit
 * (tools/profile.py turns them into a list of functions). */
#define PROF_SLOTS (1 << 20)
static DWORD64 *sProfAddr;
static u32 *sProfCount;
static volatile LONG sProfStop;
static const char *sProfPath;
static HANDLE sProfThread;

static DWORD WINAPI prof_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    u32 from = getenv("PB2_PROFILE_FROM") ? (u32)atol(getenv("PB2_PROFILE_FROM")) : 0;
    while (!sProfStop) {
        Sleep(1);
        if (gFrame < from) {
            continue;
        }
        if (SuspendThread(sProfThread) == (DWORD)-1) {
            continue;
        }
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(sProfThread, &c)) {
            DWORD64 a = c.Rip - sBase + 0x140000000ull;
            u32 h = (u32)((a * 0x9E3779B97F4A7C15ull) >> 44) & (PROF_SLOTS - 1);
            while (sProfAddr[h] && sProfAddr[h] != a) {
                h = (h + 1) & (PROF_SLOTS - 1);
            }
            sProfAddr[h] = a;
            sProfCount[h]++;
        }
        ResumeThread(sProfThread);
    }
    return 0;
}

static void prof_dump(void) {
    sProfStop = 1;
    Sleep(5);
    FILE *f = fopen(sProfPath, "w");
    if (!f) {
        return;
    }
    for (u32 i = 0; i < PROF_SLOTS; i++) {
        if (sProfAddr[i]) {
            fprintf(f, "0x%016llX %u\n", (unsigned long long)sProfAddr[i], sProfCount[i]);
        }
    }
    fclose(f);
}

void watchdog_start(void) {
    sBase = (DWORD64)GetModuleHandleA(NULL);
    gFnTrace = getenv("PB2_FNTRACE") != NULL;
    AddVectoredExceptionHandler(1, on_crash);
    const char *wa = getenv("PB2_WATCHADDR");
    if (wa) {
        sWatchAddr = (u32)strtoul(wa, NULL, 16) & ~3u;
        HANDLE self;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                        THREAD_ALL_ACCESS, FALSE, 0);
        HANDLE h = CreateThread(NULL, 64 * 1024, set_dr, self, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        rt_log("hardware write watch on %08X", sWatchAddr);
    }
    sProfPath = getenv("PB2_PROFILE");
    if (sProfPath) {
        sProfAddr = calloc(PROF_SLOTS, sizeof *sProfAddr);
        sProfCount = calloc(PROF_SLOTS, sizeof *sProfCount);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sProfThread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        CreateThread(NULL, 64 * 1024, prof_main, NULL, 0, NULL);
        atexit(prof_dump);
    }
    const char *e = getenv("PB2_WATCH");
    sPeriod = e ? atoi(e) : 8;
    if (sPeriod <= 0) {
        return;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sGameThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    CreateThread(NULL, 64 * 1024, watch_main, NULL, 0, NULL);
}
