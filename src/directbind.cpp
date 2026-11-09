// The ONE list of old imports that are bound straight to the real v3 function (no shim).
//
// A direct bind is only correct when the old code and the v3 function agree on EVERYTHING the call
// needs: signature, calling convention, class layout, and nobody calls back into old code. An import
// is direct only if it is listed here AND the import map has an exact v3 match for it (EDirect with a
// v3 ordinal). Anything else gets a shim or a trap stub, and shows up in the log.
//
// Do NOT list a function that
//   - takes or returns a double / float, or takes a 64-bit integer by value: the old toolchain passes
//     the two words of a double in the other order (high word first) and packs 64-bit arguments without
//     the 8-byte alignment EABI requires (sqrt, pow, atof, strtoll ...). Needs a shim that reads the old layout.
//   - takes a callback (function pointer): v3 would call old code directly instead of through
//     TOldRt::Call* (qsort, bsearch, atexit, signal, thread entry points).
//   - is setjmp / longjmp (jmp_buf size and layout differ, and it would bypass the TOldRt bookkeeping).
//   - depends on a class layout that differs between v2 and v3 (needs a facade, see fsshim.cpp).
// A VARIADIC function (printf family, TDes::Format) can only be listed when the game's own format
// strings contain no %f / %e / %g and no 64-bit conversions (%lld, %Ld). Check the binary for each new
// game (triage) and keep the VARIADIC note on the entry.
//
// Entries have no code and therefore no owning subsystem: owner is "directbind" for all of them.
#include <e32std.h>
#include "directbind.h"
#include "shimreg.h"

static const TShimRegEntry KDirectBindEntries[] =
    {
    // ---- euser (v2 ordinal, name from the v2 SDK lib) ----
	//{ "euser",   21, 0, EShimRegDirect, "directbind", NULL },   //User::After(TTimeIntervalMicroSeconds32)(moved to shims)
    { "euser",    1, 0, EShimRegDirect, "directbind", NULL },   // memset
    { "euser",    2, 0, EShimRegDirect, "directbind", NULL },   // memcpy
    { "euser",   45, 0, EShimRegDirect, "directbind", NULL },   // User::Alloc(int)
    { "euser",   85, 0, EShimRegDirect, "directbind", NULL },   // TDes8::Append(TChar)
    { "euser",   86, 0, EShimRegDirect, "directbind", NULL },   // TDes8::Append(const unsigned char*, int)
    { "euser",   89, 0, EShimRegDirect, "directbind", NULL },   // TDes16::Append(TChar)
    { "euser",   91, 0, EShimRegDirect, "directbind", NULL },   // TDes16::Append(const TDesC16&)
    { "euser",   98, 0, EShimRegDirect, "directbind", NULL },   // TDesC8::AtC(int) const
    { "euser",   99, 0, EShimRegDirect, "directbind", NULL },   // TDesC16::AtC(int) const
    { "euser",  244, 0, EShimRegDirect, "directbind", NULL },   // TDes8::Copy(const unsigned char*)
    { "euser",  247, 0, EShimRegDirect, "directbind", NULL },   // TDes8::Copy(const TDesC16&)
    { "euser",  251, 0, EShimRegDirect, "directbind", NULL },   // TDes16::Copy(const TDesC16&)
    { "euser",  317, 0, EShimRegDirect, "directbind", NULL },   // HBufC8::Des()
    { "euser",  467, 0, EShimRegDirect, "directbind", NULL },   // TDes16::Format(TRefByValue<const TDesC16>, ...)   -- VARIADIC: integer / %s formats only
    { "euser",  476, 0, EShimRegDirect, "directbind", NULL },   // User::Free(void*)
    { "euser",  621, 0, EShimRegDirect, "directbind", NULL },   // RThread::Kill(int)
    { "euser",  625, 0, EShimRegDirect, "directbind", NULL },   // User::Language()
	{ "euser",  108, 0, EShimRegDirect, "directbind", NULL },   // RTimer::At(TRequestStatus&, const TTime&)   (Cannons' sleep helper; no callback) (TTime by reference, may be 4-byte aligned)
    { "euser",  284, 0, EShimRegDirect, "directbind", NULL },   // RTimer::CreateLocal()   (RTimer is a 4-byte handle in v2 and v3)
    { "euser",  852, 0, EShimRegDirect, "directbind", NULL },   // TDes16::PtrZ()
    { "euser", 1086, 0, EShimRegDirect, "directbind", NULL },   // TRect::Size() const   (TSize comes back through a hidden pointer in both ABIs)
	{ "euser", 1370, 0, EShimRegDirect, "directbind", NULL },   // TRect::TRect(TSize const&)
    { "euser", 1210, 0, EShimRegDirect, "directbind", NULL },   // User::WaitForRequest(TRequestStatus&)
    { "euser", 1486, 0, EShimRegDirect, "directbind", NULL },   // TTime::operator+=(TTimeIntervalMicroSeconds32)   (the 32-bit variant)
	{ "euser",  694, 0, EShimRegDirect, "directbind", NULL },   // UserHal::MemoryInfo(TDes8&): TMemoryInfoV1 has the same layout in v2 and v3
    { "euser",  836, 0, EShimRegDirect, "directbind", NULL },   // RDebug::Print(TRefByValue<const TDesC16>, ...)   -- VARIADIC: Candy's own formats are %s / %d only
    { "euser",  855, 0, EShimRegDirect, "directbind", NULL },   // TDesC8::Ptr() const
    { "euser",  856, 0, EShimRegDirect, "directbind", NULL },   // TDesC16::Ptr() const
    { "euser",  933, 0, EShimRegDirect, "directbind", NULL },   // User::ResetInactivityTime()
    { "euser", 1029, 0, EShimRegDirect, "directbind", NULL },   // TSize::SetSize(int, int)
    { "euser", 1244, 0, EShimRegDirect, "directbind", NULL },   // TDes8::ZeroTerminate()
    { "euser", 1409, 0, EShimRegDirect, "directbind", NULL },   // TPtrC16::TPtrC16(const TDesC16&)
	{ "euser",  568, 0, EShimRegDirect, "directbind", NULL },   // TDes16::Insert(int, const TDesC16&)
	{ "euser",  577, 0, EShimRegDirect, "directbind", NULL },   // User::Invariant()
	{ "euser",  956, 0, EShimRegDirect, "directbind", NULL },   // TDesC16::Right(int) const
    // ---- avkon ----
    { "avkon",  880, 0, EShimRegDirect, "directbind", NULL },   // LatinPlain12(): returns the host's real CFont*
    // ---- estlib: plain C library functions, listed one by one (anything not listed is a trap) ----
    { "estlib",    3, 0, EShimRegDirect, "directbind", NULL },   // malloc
    { "estlib",    9, 0, EShimRegDirect, "directbind", NULL },   // strlen
    { "estlib",   49, 0, EShimRegDirect, "directbind", NULL },   // fclose
    { "estlib",   54, 0, EShimRegDirect, "directbind", NULL },   // fgetc
    // { "estlib",   58, 0, EShimRegDirect, "directbind", NULL },   // fopen (moved to shims)
    { "estlib",   60, 0, EShimRegDirect, "directbind", NULL },   // fputc
    { "estlib",   62, 0, EShimRegDirect, "directbind", NULL },   // fread
    { "estlib",   65, 0, EShimRegDirect, "directbind", NULL },   // fseek
    { "estlib",   67, 0, EShimRegDirect, "directbind", NULL },   // ftell
    { "estlib",   68, 0, EShimRegDirect, "directbind", NULL },   // fwrite
    { "estlib",   82, 0, EShimRegDirect, "directbind", NULL },   // sprintf   -- VARIADIC: integer / %s formats only
    { "estlib",  134, 0, EShimRegDirect, "directbind", NULL },   // rand
    { "estlib",  178, 0, EShimRegDirect, "directbind", NULL },   // strcat
    { "estlib",  181, 0, EShimRegDirect, "directbind", NULL },   // strcpy
	{ "estlib",    5, 0, EShimRegDirect, "directbind", NULL },   // realloc
    { "estlib",    6, 0, EShimRegDirect, "directbind", NULL },   // free
    { "estlib",   59, 0, EShimRegDirect, "directbind", NULL },   // fprintf   -- VARIADIC: Cannons uses %s %d %u here (the one %lf format is in a writer nothing calls)
    { "estlib",   83, 0, EShimRegDirect, "directbind", NULL },   // sscanf    -- VARIADIC: Cannons uses %x %d %c %[set] only, no float formats
    { "estlib",  128, 0, EShimRegDirect, "directbind", NULL },   // atoi
    { "estlib",  136, 0, EShimRegDirect, "directbind", NULL },   // srand
    { "estlib",  160, 0, EShimRegDirect, "directbind", NULL },   // isalpha
    { "estlib",  174, 0, EShimRegDirect, "directbind", NULL },   // memcmp
    { "estlib",  177, 0, EShimRegDirect, "directbind", NULL },   // strcasecmp
    { "estlib",  179, 0, EShimRegDirect, "directbind", NULL },   // strchr
    { "estlib",  180, 0, EShimRegDirect, "directbind", NULL },   // strcmp
    { "estlib",  184, 0, EShimRegDirect, "directbind", NULL },   // strdup
    { "estlib",  188, 0, EShimRegDirect, "directbind", NULL },   // strncpy
    { "estlib",  190, 0, EShimRegDirect, "directbind", NULL },   // strrchr
    { "estlib",  192, 0, EShimRegDirect, "directbind", NULL },   // strstr
	{ "estlib",  56, 0, EShimRegDirect, "directbind", NULL },   // fgets
	{ "estlib",  126, 0, EShimRegDirect, "directbind", NULL },   // abs
    { "estlib",  291, 0, EShimRegDirect, "directbind", NULL },   // CloseSTDLIB   -- lifecycle call (frees the calling thread's libc state), not a plain function
    // ---- ezlib ----
    { "ezlib",   47, 0, EShimRegDirect, "directbind", NULL },   // crc32
    { "ezlib",   56, 0, EShimRegDirect, "directbind", NULL },   // get_crc_table
    { "ezlib",   57, 0, EShimRegDirect, "directbind", NULL },   // inflateEnd
    { "ezlib",   58, 0, EShimRegDirect, "directbind", NULL },   // inflateInit2_
    { "ezlib",   64, 0, EShimRegDirect, "directbind", NULL }   // inflate
    };

TInt DirectBind_Init()
    {
    return ShimReg_Add(KDirectBindEntries, sizeof(KDirectBindEntries) / sizeof(KDirectBindEntries[0]));
    }
