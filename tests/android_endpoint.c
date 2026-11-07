/* Android-port SHL7 endpoint harness for interop testing.
 *
 * Compiles the real android/host/dplay.c + com.c with the same guest-VM
 * stubs as pc_decomp_strhld/tests/dplay_selfcheck.c, but exposes ONE
 * dp_object as a host or client over real TCP/UDP sockets, printing
 * received guest messages to stdout so a Python SHL7 endpoint
 * (shl7.py, the Windows-provider prototype) can be tested against it.
 *
 * Usage: android_endpoint host|client
 * Build (Linux, native):
 *   gcc -std=gnu11 -I../../pc_decomp_strhld/android/runtime \
 *       -I../../pc_decomp_strhld/android/host android_endpoint.c \
 *       -o android_endpoint -lm -lpthread
 */
#define _GNU_SOURCE
#include "../../pc_decomp_strhld/android/host/dplay.c"
#include "../../pc_decomp_strhld/android/host/com.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
ptrdiff_t g_mem_base = (ptrdiff_t)VOID_GUEST_BASE;
static uint32_t allocation = 0x1000000, callbacks, sessions;

void void_log(const char *format, ...) {
    va_list args; va_start(args, format);
    fprintf(stderr, "[android-dplay] "); vfprintf(stderr, format, args); fputs("\n", stderr);
    va_end(args);
}
int void_vm_readable(uint32_t p, uint32_t n) { return p >= 0x400000 && (uint64_t)p + n <= 0x4000000; }
int void_vm_writable(uint32_t p, uint32_t n) { return void_vm_readable(p, n); }
uint32_t void_galloc(uint32_t n) {
    uint32_t p = allocation; allocation += (n + 15) & ~15u;
    assert(allocation < 0x4000000); memset(GPTR(p), 0, n); return p;
}
void void_gfree(uint32_t p) { (void)p; }
uint32_t void_thunk(void_fn fn, const char *name) {
    (void)fn; (void)name; static uint32_t address = GUEST_THUNK_BASE; return address += 4;
}
_Noreturn void void_fatal(const char *format, ...) { (void)format; abort(); }
_Noreturn void void_lift_trap(uint32_t p, const char *reason) { fprintf(stderr, "trap %x %s\n", p, reason); abort(); }
uint64_t void_monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
void void_blocking_begin(void) {}
void void_blocking_end(void) {}
uint32_t void_call_guest(uint32_t fn, int n, const uint32_t *args, int pops) {
    assert(pops == 1); callbacks++;
    if (fn == 1) return 1;
    assert(fn == 2 && n == 4);
    if (args[2] == 0) { sessions++; return 1; }
    return 0;
}

static uint32_t invoke(dp_object *d, unsigned slot, int n, ...) {
    uint32_t stack = 0x410000; g_esp = stack; MEM32(stack) = 0x12345678; MEM32(stack + 4) = d->com.guest;
    va_list args; va_start(args, n);
    for (int i = 1; i < n; i++) MEM32(stack + 4 + 4 * i) = va_arg(args, uint32_t);
    va_end(args);
    const void_fn *methods = d->lobby ? lobby_methods : dp_methods;
    methods[slot](); assert(g_esp == stack + 4 + 4 * (uint32_t)n); return g_eax;
}

static void put_wide(uint32_t p, const char *s) {
    for (int i = 0; s[i]; i++) MEM16(p + 2 * i) = (uint16_t)s[i];
    MEM16(p + 2 * strlen(s)) = 0;
}

/* Session desc matching tests/shl7.py: guidInstance=0xAA*16 at +8,
 * guidApplication=00..0f at +24, maxPlayers=8 at +40, name ptr at +48. */
static uint32_t make_desc(void) {
    uint32_t desc = void_galloc(512), name = desc + 80;
    MEM32(desc) = 80; MEM32(desc + 40) = 8; MEM32(desc + 48) = name;
    for (int i = 0; i < 16; i++) { MEM8(desc + 8 + i) = 0xAA; MEM8(desc + 24 + i) = (uint8_t)i; }
    put_wide(name, "AndroidSession");
    return desc;
}

static uint32_t make_player_name(const char *s) {
    uint32_t p = void_galloc(512);
    MEM32(p) = 16; MEM32(p + 8) = p + 16; MEM32(p + 12) = p + 16;
    put_wide(p + 16, s);
    return p;
}

static void setup(dp_object *d, int is_client) {
    uint32_t iid = void_galloc(16), object = void_galloc(4);
    const uint32_t dp4[] = { 0x0AB1C530, 0x11D14745, 0x0000A1A7, 0xFCAB03F8 };
    memcpy(GPTR(iid), dp4, 16);
    assert(!invoke(d, 0, 3, iid, object) && MEM32(object) == d->com.guest);
    invoke(d, 2, 1);
    uint32_t connection = void_galloc(64);
    if (!is_client) {
        memcpy(GPTR(connection), guid_sp, 16); MEM32(connection + 16) = 16; memcpy(GPTR(connection + 20), guid_tcp, 16);
    } else {
        memcpy(GPTR(connection), guid_size, 16); MEM32(connection + 16) = 4; MEM32(connection + 20) = 54;
        memcpy(GPTR(connection + 24), guid_inet, 16); MEM32(connection + 40) = 10; memcpy(GPTR(connection + 44), "127.0.0.1", 10);
    }
    assert(!invoke(d, 38, 3, connection, 0u));
}

static void print_recv(dp_object *d, uint32_t from, uint32_t to, uint32_t out, uint32_t size) {
    uint32_t kind = from == 0 ? MEM32(out) : 0;
    printf("RECV kind=%u from=%u to=%u size=%u data=", kind, from, to, size);
    if (from != 0) for (uint32_t i = 0; i < size; i++) putchar(MEM8(out + i));
    putchar('\n');
    fflush(stdout);
    (void)d;
}

static int pump(dp_object *d, int seconds, int reply) {
    uint32_t from = void_galloc(4), to = void_galloc(4), size = void_galloc(4), out = void_galloc(200000);
    uint64_t end = void_monotonic_ns() + (uint64_t)seconds * 1000000000u;
    while (void_monotonic_ns() < end) {
        MEM32(size) = 200000;
        uint32_t r = invoke(d, 25, 6, from, to, 1u, out, size);
        if (r == 0) {
            print_recv(d, MEM32(from), MEM32(to), out, MEM32(size));
            if (reply && MEM32(from) != 0) {
                const char *pong = "pong from android host";
                uint32_t payload = void_galloc(64);
                memcpy(GPTR(payload), pong, strlen(pong));
                invoke(d, 26, 6, 1u, MEM32(from), 1u, payload, (uint32_t)strlen(pong));
            }
            continue;
        }
        if (r != DP_NO_MESSAGES) { printf("RECV_ERROR %08x\n", r); fflush(stdout); return 1; }
        poll(NULL, 0, 20);
    }
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2 && (!strcmp(argv[1], "host") || !strcmp(argv[1], "client")));
    int is_client = !strcmp(argv[1], "client");
    assert(mmap((void *)(VOID_GUEST_BASE + 0x400000), 0x3c00000,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    dp_object *d = new_dp(0, 1); assert(d);
    setup(d, is_client);
    uint32_t desc = make_desc();
    uint32_t player_name = make_player_name(is_client ? "AndroidClient" : "AndroidHost");
    uint32_t player_id = void_galloc(4);
    if (!is_client) {
        assert(!invoke(d, 24, 3, desc, 2u));                       /* Open CREATE */
        assert(!invoke(d, 6, 7, player_id, player_name, 0u, 0u, 0u, 0u, 0u));
        printf("HOST_READY id=%u\n", MEM32(player_id)); fflush(stdout);
        pump(d, 12, 1);
        invoke(d, 4, 1); invoke(d, 2, 1);
        puts("HOST_DONE"); fflush(stdout);
    } else {
        assert(!invoke(d, 13, 6, desc, 0u, 2u, 0u, 1u));           /* EnumSessions */
        printf("FOUND %u\n", d->found_count); fflush(stdout);
        assert(d->found_count >= 1);
        /* Like dplay_selfcheck.c: join with the discovered session desc
         * (host's random guidInstance), keeping our local name pointer. */
        memcpy(GPTR(desc), d->found[0].session.desc, 80);
        MEM32(desc + 48) = desc + 80;
        assert(!invoke(d, 24, 3, desc, 1u));                       /* Open JOIN */
        assert(!invoke(d, 6, 7, player_id, player_name, 0u, 0u, 0u, 0u, 0u));
        printf("CLIENT_READY id=%u\n", MEM32(player_id)); fflush(stdout);
        const char *hello = "hello from android client";
        uint32_t payload = void_galloc(64);
        memcpy(GPTR(payload), hello, strlen(hello));
        assert(!invoke(d, 26, 6, MEM32(player_id), 1u, 1u, payload, (uint32_t)strlen(hello)));
        pump(d, 6, 0);
        invoke(d, 4, 1); invoke(d, 2, 1);
        puts("CLIENT_DONE"); fflush(stdout);
    }
    return 0;
}
