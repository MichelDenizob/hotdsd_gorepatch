/* ============================================================================
 *
 *     ####    ####   #####   ######       #####    ####   ######   ####   #    #
 *    #       #    #  #    #  #            #    #  #    #    ##    #    #  #    #
 *    # ###   #    #  #####   #####        #####   ######    ##    #       ######
 *    #   #   #    #  #  #    #            #       #    #    ##    #    #  #    #
 *     ####    ####   #   #   ######       #       #    #    ##     ####   #    #
 *
 *           House of the Dead: Scarlet Dawn  --  gore restoration (source)
 *                     created by   M I C H E L   D E N I Z O B
 *
 * ============================================================================ */

/* ============================================================================
 * hotd_gore_patch.c — House of the Dead: Scarlet Dawn ("Hodzero", UE4.18) gore restore.
 *
 *   ONE self-contained, documented patcher. Every change is reconstructed by
 *   logic from a *vanilla* pak and explained below.
 *
 *   build (macOS/Linux):  cc -O2 hotd_gore_patch.c -o hotd_gore_patch -lz
 *   build (Windows/MinGW): x86_64-w64-mingw32-gcc -O2 hotd_gore_patch.c -o hotd_gore_patch.exe -lz
 *   run:   hotd_gore_patch <pak> check      (dry-run: list changes, no write)
 *          hotd_gore_patch <pak> apply       (patch in place)
 *          hotd_gore_patch <pak> revert      (undo a previous apply -> original)
 *
 *   Safety: the original bytes [0..oldsize) are never modified. apply only APPENDS
 *   5 stored (uncompressed) asset copies + a fresh index + footer at EOF and
 *   repoints the index; `revert` restores the original pak exactly.
 *   (Host must be little-endian — true for x86/arm targets; UE4 pak is LE.)
 *
 * ============================================================================
 * WHAT THIS CHANGES IN THE .pak, AND WHY  (offsets are in the DECOMPRESSED asset)
 * ============================================================================
 * Gore is gated by the boolean bGoreFlag, hard-set false at init in three damage
 * components; the value lives in the Kismet bytecode as EX_False(0x28); we flip
 * it to EX_True(0x27).
 *
 * (1) BPC_ClothDamage.uexp   @5582 : bGoreFlag 0x28->0x27                 [EFFECTIVE]
 *     ClothDamage runs the per-zone damage state machine on the LIVE skin (its
 *     mesh list IS populated via BP_EnemyNormal.UCS "Add Control Mesh"). With the
 *     flag on, hits clip the BLEND_Masked skin -> holes that reveal flesh (with 4).
 * (2) BP_CrashBones.uexp      @2103 : bGoreFlag 0x28->0x27                 [EFFECTIVE]
 *     ungates the death bone-shards (7 physics props at body sockets + P_ZombieDead_01).
 * (3) BPC_BurnBodyDamage.uexp @2195 : bGoreFlag 0x28->0x27                  [NO-OP, kept]
 *     PROVEN no-op: StartDamageEffect (gated bGoreFlag==True) and StartDeadEffect
 *     (gated bGoreFlag==False) ramp BurnWeight over arrays ClothMaterials/MeatMaterials
 *     that Initialize only Array_Clear's and NEVER fills (the fillers Init*Material
 *     have ZERO call sites anywhere). Empty arrays -> nothing renders. Included for
 *     completeness (same bGoreFlag pattern as 1-2); has no visible effect.
 * (4) BP_EnemyNormal.uasset + .uexp : MESH-BIND (3 decomposed sub-operations) [EFFECTIVE]
 *     The burned-flesh body (SK_Burned_Middle) ships orphaned. We re-wire it:
 *      (4a) BIND: +3 names, +2 imports (Package -307, SkeletalMesh object -308), a
 *           29-byte SkeletalMesh ObjectProperty on the BurnedBody template, +1 EDL
 *           preload entry, then recompute header offsets + every export SerialOffset.
 *      (4b) ALWAYS-VISIBLE: flip .uexp @10689 0x28->0x27 -> the UCS call
 *           BurnedBody.SetVisibility(False,False) becomes (True,False). The flesh is
 *           visible but OCCLUDED by the opaque skin; the cloth holes (1) reveal it.
 *           (import -234 == SetVisibility, verified by disassembly.)
 *      (4c) HIDE-AT-DEATH: insert BurnedBody.SetVisibility(False,False) before the
 *           Return in StartDieMotion so the corpse goes skin->skeleton cleanly.
 *     NET (in-game validated): flesh through bullet holes alive; hidden at death,
 *     bones (2) eject.
 * ========================================================================== */

#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ============================ 64-bit file I/O ============================= */
#ifdef _WIN32
#include <windows.h>
typedef HANDLE PF;
static PF pf_open(const char *path, int rw) {
    wchar_t w[2048]; MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048);
    return CreateFileW(w, rw ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                       FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}
static int pf_ok(PF h) { return h != INVALID_HANDLE_VALUE; }
static int64_t pf_size(PF h) { LARGE_INTEGER s; return GetFileSizeEx(h, &s) ? s.QuadPart : -1; }
static int pf_seek(PF h, int64_t o) { LARGE_INTEGER li; li.QuadPart = o; return SetFilePointerEx(h, li, NULL, FILE_BEGIN) ? 0 : -1; }
static int pf_read(PF h, int64_t o, void *b, int64_t n) { if (pf_seek(h, o)) return -1; int64_t d = 0;
    while (d < n) { DWORD g = 0, w = (DWORD)((n - d > 0x40000000) ? 0x40000000 : (n - d)); if (!ReadFile(h, (char*)b + d, w, &g, NULL) || !g) return -1; d += g; } return 0; }
static int pf_write(PF h, int64_t o, const void *b, int64_t n) { if (pf_seek(h, o)) return -1; int64_t d = 0;
    while (d < n) { DWORD p = 0, w = (DWORD)((n - d > 0x40000000) ? 0x40000000 : (n - d)); if (!WriteFile(h, (const char*)b + d, w, &p, NULL) || !p) return -1; d += p; } return 0; }
static void pf_close(PF h) { if (pf_ok(h)) CloseHandle(h); }
static int pf_truncate(PF h, int64_t s) { return (pf_seek(h, s) || !SetEndOfFile(h)) ? -1 : 0; }
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
typedef int PF;
static PF pf_open(const char *path, int rw) { return open(path, rw ? O_RDWR : O_RDONLY); }
static int pf_ok(PF h) { return h >= 0; }
static int64_t pf_size(PF h) { struct stat st; return fstat(h, &st) ? -1 : (int64_t)st.st_size; }
static int pf_read(PF h, int64_t o, void *b, int64_t n) { int64_t d = 0; while (d < n) { ssize_t r = pread(h, (char*)b + d, (size_t)(n - d), o + d); if (r <= 0) return -1; d += r; } return 0; }
static int pf_write(PF h, int64_t o, const void *b, int64_t n) { int64_t d = 0; while (d < n) { ssize_t r = pwrite(h, (const char*)b + d, (size_t)(n - d), o + d); if (r <= 0) return -1; d += r; } return 0; }
static void pf_close(PF h) { if (pf_ok(h)) close(h); }
static int pf_truncate(PF h, int64_t s) { return ftruncate(h, (off_t)s); }
#endif

/* ============================ LE byte helpers ============================= */
static int32_t rd32(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }
static int64_t rd64(const unsigned char *p) { int64_t v; memcpy(&v, p, 8); return v; }
static void wr32(unsigned char *p, int32_t v) { memcpy(p, &v, 4); }
static void wr64(unsigned char *p, int64_t v) { memcpy(p, &v, 8); }

#define DIE(...) do { fprintf(stderr, "ERROR: " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define CHECK(c, ...) do { if (!(c)) DIE(__VA_ARGS__); } while (0)

/* ================================ SHA-1 =================================== */
/* public-domain (Steve Reid), compact. */
typedef struct { uint32_t s[5]; uint64_t n; unsigned char b[64]; } SHA1;
#define ROL(v, k) (((v) << (k)) | ((v) >> (32 - (k))))
static void sha1_tr(uint32_t st[5], const unsigned char buf[64]) {
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], w[80];
    for (int i = 0; i < 16; i++) w[i] = (buf[i*4] << 24) | (buf[i*4+1] << 16) | (buf[i*4+2] << 8) | buf[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}
static void sha1_init(SHA1 *h) { h->s[0] = 0x67452301; h->s[1] = 0xEFCDAB89; h->s[2] = 0x98BADCFE; h->s[3] = 0x10325476; h->s[4] = 0xC3D2E1F0; h->n = 0; }
static void sha1_upd(SHA1 *h, const unsigned char *d, size_t len) {
    size_t i = (size_t)(h->n & 63); h->n += len;
    for (size_t k = 0; k < len; k++) { h->b[i++] = d[k]; if (i == 64) { sha1_tr(h->s, h->b); i = 0; } }
}
static void sha1_fin(SHA1 *h, unsigned char out[20]) {
    uint64_t bits = h->n * 8; unsigned char c = 0x80; sha1_upd(h, &c, 1);
    c = 0; while ((h->n & 63) != 56) sha1_upd(h, &c, 1);
    unsigned char L[8]; for (int i = 0; i < 8; i++) L[i] = (unsigned char)(bits >> (56 - 8 * i)); sha1_upd(h, L, 8);
    for (int i = 0; i < 5; i++) { out[i*4] = h->s[i] >> 24; out[i*4+1] = h->s[i] >> 16; out[i*4+2] = h->s[i] >> 8; out[i*4+3] = h->s[i]; }
}
static void sha1(const unsigned char *d, size_t n, unsigned char out[20]) { SHA1 h; sha1_init(&h); sha1_upd(&h, d, n); sha1_fin(&h, out); }

/* ===================== FName hashes (for new names) ======================= */
static uint32_t H0T[256];
static void h0_init(void) {
    for (int n = 0; n < 256; n++) { uint32_t c = (uint32_t)n << 24;
        for (int i = 0; i < 8; i++) c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        H0T[n] = c; }
}
static uint16_t h0(const char *s) {           /* NonCasePreservingHash (Strihash_DEPRECATED) */
    uint32_t h = 0; for (; *s; s++) { unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 0x20;   /* ASCII names: 1 UTF-8 byte/char */
        h = ((h >> 8) & 0x00FFFFFF) ^ H0T[(h ^ c) & 0xFF]; }
    return (uint16_t)(h & 0xFFFF);
}
static uint16_t h1(const char *s) {            /* CasePreservingHash (StrCrc32 over 4 bytes/char) */
    size_t n = strlen(s); unsigned char *b = malloc(n * 4); CHECK(b, "oom");
    for (size_t i = 0; i < n; i++) { b[i*4] = (unsigned char)s[i]; b[i*4+1] = b[i*4+2] = b[i*4+3] = 0; }
    uint32_t c = (uint32_t)crc32(0L, b, (uInt)(n * 4)); free(b); return (uint16_t)(c & 0xFFFF);
}

/* ============================ pak v4 index ================================ */
#define PAK_MAGIC1 0xe1
#define CM0_HDR 53                 /* on-disk record header preceding a stored payload */

typedef struct {
    int64_t name_pos; int name_bytes;     /* raw FString name field in the index buffer */
    int64_t rec_pos;  int rec_bytes;      /* raw FPakEntry record in the index buffer */
    int64_t off, size, us; int32_t cm;
    int64_t blocks_pos; int nblocks;      /* for cm!=0: where (cs,ce) pairs live in the index */
    int target;                           /* index into targets[] or -1 */
    char name[256];
} Entry;

/* advance past an FString at idx+*p; copy (ascii) into out[outsz]; return new p */
static int64_t read_str(const unsigned char *idx, int64_t p, char *out, int outsz) {
    int n = rd32(idx + p); p += 4;
    if (n >= 0) { int m = n - 1 < outsz - 1 ? n - 1 : outsz - 1; if (m < 0) m = 0;
        memcpy(out, idx + p, m); out[m] = 0; p += n; }
    else { out[0] = 0; p += (-n) * 2; }   /* UTF-16 (mount only; entry names are ASCII) */
    return p;
}

/* --------------------- the 5 assets we touch ----------------------------- */
typedef struct { const char *suffix; int gore_off; const char *label; } Target;
static Target targets[5] = {
    { "Blueprints/DamageEffect/BPC_ClothDamage.uexp", 5582, "ClothDamage" },
    { "Blueprints/CrashBone/BP_CrashBones.uexp",      2103, "CrashBones" },
    { "Blueprints/DamageEffect/BPC_BurnBodyDamage.uexp", 2195, "BurnBodyDamage (NO-OP)" },
    { "Human_Common/BP_EnemyNormal.uasset",           -1,   "BP_EnemyNormal.uasset (mesh-bind)" },
    { "Human_Common/BP_EnemyNormal.uexp",             -1,   "BP_EnemyNormal.uexp (mesh-bind)" },
};
#define T_CLOTH 0
#define T_CRASH 1
#define T_BURN  2
#define T_EN_UA 3
#define T_EN_UX 4

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && memcmp(s + ls - lf, suf, lf) == 0;
}

/* ===================== decompress a pak entry ============================= */
static unsigned char *decompress_entry(PF pf, const unsigned char *idx, Entry *e, int64_t *outlen) {
    unsigned char *out = malloc(e->us ? e->us : 1); CHECK(out, "oom");
    if (e->cm == 0) { CHECK(!pf_read(pf, e->off + CM0_HDR, out, e->us), "read cm0"); *outlen = e->us; return out; }
    int64_t written = 0;
    for (int k = 0; k < e->nblocks; k++) {
        int64_t cs = rd64(idx + e->blocks_pos + k * 16);
        int64_t ce = rd64(idx + e->blocks_pos + k * 16 + 8);
        long clen = (long)(ce - cs);
        unsigned char *cbuf = malloc(clen); CHECK(cbuf, "oom");
        CHECK(!pf_read(pf, cs, cbuf, clen), "read block");
        uLongf dl = (uLongf)(e->us - written);
        CHECK(uncompress(out + written, &dl, cbuf, clen) == Z_OK, "inflate block %d", k);
        written += dl; free(cbuf);
    }
    CHECK(written == e->us, "decompressed %lld != us %lld", (long long)written, (long long)e->us);
    *outlen = written; return out;
}

/* ====================== (1)(2)(3) gore-flag flip ========================== */
static void flip_gore(unsigned char *buf, int off, const char *label) {
    if (buf[off] == 0x27) { printf("    [gore] %-24s already 0x27 (skip)\n", label); return; }
    CHECK(buf[off] == 0x28, "%s @%d: expected 0x28, got 0x%02x", label, off, buf[off]);
    buf[off] = 0x27; printf("    [gore] %-24s @%d  0x28 -> 0x27\n", label, off);
}

/* ====================== (4) BP_EnemyNormal mesh-bind ====================== */
/* header field offsets (i32 unless noted) for this exact cook */
enum { H_TOTAL = 24, H_NAME_COUNT = 41, H_NAME_OFF = 45, H_EXPORT_COUNT = 57, H_EXPORT_OFF = 61,
       H_IMPORT_COUNT = 65, H_IMPORT_OFF = 69, H_DEPENDS_OFF = 73, H_ASSETREG_OFF = 165,
       H_BULK_START = 169 /*i64*/, H_PRELOAD_COUNT = 185, H_PRELOAD_OFF = 189 };
enum { EXPORT_STRIDE = 104, E_SERIAL_SIZE = 28 /*i64*/, E_SERIAL_OFF = 36 /*i64*/,
       E_FIRST_DEP = 84, E_CREATE_BEFORE_SER = 92 };
#define BB_EXPORT_INDEX 398
#define BB_UEXP_OFF 30645
#define BB_NONE_OFF 55
#define BB_FIRST_DEP 1411
#define PRELOAD_INSERT_INDEX 1413
static const char *PATH_NAME =
    "/Game/HodZero_v1/Characters/Enemy/Others/Burned_Middle/Meshes/SK_Burned_Middle";

/* find a name's index in the uasset name table (linear; only used a handful of times) */
static int name_index(const unsigned char *ua, const char *want) {
    int cnt = rd32(ua + H_NAME_COUNT); int64_t p = rd32(ua + H_NAME_OFF);
    for (int i = 0; i < cnt; i++) {
        int ln = rd32(ua + p); p += 4;
        if (ln > 0 && (int)strlen(want) == ln - 1 && memcmp(ua + p, want, ln - 1) == 0) return i;
        p += (ln >= 0 ? ln : (-ln) * 2) + 4;   /* + h0,h1 */
    }
    return -1;
}
static const char *name_at(const unsigned char *ua, int idx) {  /* returns ptr to NUL-terminated name */
    int cnt = rd32(ua + H_NAME_COUNT); int64_t p = rd32(ua + H_NAME_OFF);
    for (int i = 0; i < cnt; i++) { int ln = rd32(ua + p); p += 4;
        if (i == idx) return (const char *)(ua + p); p += (ln >= 0 ? ln : (-ln) * 2) + 4; }
    return "";
}

/* append one name-table entry: i32 len, ascii+NUL, u16 h0, u16 h1 */
static int emit_name(unsigned char *dst, const char *name) {
    int raw = (int)strlen(name) + 1; wr32(dst, raw); memcpy(dst + 4, name, raw);
    uint16_t a = h0(name), b = h1(name); memcpy(dst + 4 + raw, &a, 2); memcpy(dst + 4 + raw + 2, &b, 2);
    return 4 + raw + 4;
}
static void put_fname(unsigned char *d, int idx) { wr32(d, idx); wr32(d + 4, 0); }

/* meshbind: returns new ua/ux (caller frees). Asserts every magic vs the stock asset. */
static void meshbind(const unsigned char *ua, int ualen, const unsigned char *ux, int uxlen,
                     unsigned char **ua_out, int *ua_outlen, unsigned char **ux_out, int *ux_outlen) {
    int total = rd32(ua + H_TOTAL), name_count = rd32(ua + H_NAME_COUNT);
    int export_count = rd32(ua + H_EXPORT_COUNT), export_off = rd32(ua + H_EXPORT_OFF);
    int import_count = rd32(ua + H_IMPORT_COUNT), import_off = rd32(ua + H_IMPORT_OFF);
    int depends_off = rd32(ua + H_DEPENDS_OFF), assetreg_off = rd32(ua + H_ASSETREG_OFF);
    int64_t bulk_start = rd64(ua + H_BULK_START);
    int preload_count = rd32(ua + H_PRELOAD_COUNT), preload_off = rd32(ua + H_PRELOAD_OFF);

    CHECK(total == ualen && total == 90908, "uasset total %d/%d", total, ualen);
    CHECK(name_count == 812 && import_count == 306 && export_count == 459, "counts");
    CHECK(import_off == 26720 && export_off == 35288 && depends_off == 83024, "section offsets");
    CHECK(preload_count == 1511 && preload_off == 84864, "preload");
    CHECK(uxlen == 34191 && memcmp(ux + uxlen - 4, "\xc1\x83\x2a\x9e", 4) == 0, "uexp tag/len");

    /* names abut imports */
    int names_end; { int64_t p = rd32(ua + H_NAME_OFF); for (int i = 0; i < name_count; i++) { int ln = rd32(ua + p); p += 4 + (ln >= 0 ? ln : (-ln) * 2) + 4; } names_end = (int)p; }
    CHECK(names_end == import_off, "names_end %d != import_off %d", names_end, import_off);

    int i_engine = name_index(ua, "/Script/Engine"), i_core = name_index(ua, "/Script/CoreUObject");
    int i_pkg = name_index(ua, "Package"), i_objprop = name_index(ua, "ObjectProperty");
    CHECK(i_engine >= 0 && i_core >= 0 && i_pkg >= 0 && i_objprop >= 0, "missing required name");

    /* BurnedBody export sanity */
    int bb = export_off + BB_EXPORT_INDEX * EXPORT_STRIDE;
    CHECK(rd32(ua + bb) == -138 && rd64(ua + bb + E_SERIAL_SIZE) == 67, "BurnedBody class/size");
    CHECK(rd64(ua + bb + E_SERIAL_OFF) - total == BB_UEXP_OFF, "BurnedBody serial off");
    CHECK(rd32(ua + bb + E_FIRST_DEP) == BB_FIRST_DEP && rd32(ua + bb + E_CREATE_BEFORE_SER) == 1, "BurnedBody deps");

    /* (a) NAMES: append the 3 missing */
    int sk_mesh_i = name_count, sk_burned_i = name_count + 1, path_i = name_count + 2;
    unsigned char namebuf[512]; int dN = 0;
    dN += emit_name(namebuf + dN, "SkeletalMesh");
    dN += emit_name(namebuf + dN, "SK_Burned_Middle");
    dN += emit_name(namebuf + dN, PATH_NAME);

    /* (b) IMPORTS: Package outer (-307), SkeletalMesh object (-308) */
    int obj_pkg = -(import_count + 1), obj_mesh = -(import_count + 2);
    unsigned char impbuf[56]; int dI = 0;
    put_fname(impbuf + dI, i_core); put_fname(impbuf + dI + 8, i_pkg); wr32(impbuf + dI + 16, 0); put_fname(impbuf + dI + 20, path_i); dI += 28;
    put_fname(impbuf + dI, i_engine); put_fname(impbuf + dI + 8, sk_mesh_i); wr32(impbuf + dI + 16, obj_pkg); put_fname(impbuf + dI + 20, sk_burned_i); dI += 28;

    /* (c) PROPERTY tag (29 B): FName(SkeletalMesh) FName(ObjectProperty) size=4 arr=0 guid=0 value=-308 */
    unsigned char prop[29];
    put_fname(prop, sk_mesh_i); put_fname(prop + 8, i_objprop); wr32(prop + 16, 4); wr32(prop + 20, 0); prop[24] = 0; wr32(prop + 25, obj_mesh);
    int dX = 29, dP = 4;

    /* ---- build new .uexp: insert prop at BurnedBody's None terminator ---- */
    int at = BB_UEXP_OFF + BB_NONE_OFF;
    int none_idx = rd32(ux + at); CHECK(rd32(ux + at + 4) == 0 && strcmp(name_at(ua, none_idx), "None") == 0, "None terminator");
    int nuxlen = uxlen + dX; unsigned char *nux = malloc(nuxlen); CHECK(nux, "oom");
    memcpy(nux, ux, at); memcpy(nux + at, prop, dX); memcpy(nux + at + dX, ux + at, uxlen - at);

    /* ---- build new .uasset by ascending-offset splices ---- */
    int preload_ins = preload_off + PRELOAD_INSERT_INDEX * 4;
    int nualen = ualen + dN + dI + dP; unsigned char *nua = malloc(nualen); CHECK(nua, "oom");
    int q = 0;
    memcpy(nua + q, ua, names_end); q += names_end;                 /* [0 .. names_end) */
    memcpy(nua + q, namebuf, dN); q += dN;                          /* + names */
    memcpy(nua + q, ua + names_end, export_off - names_end); q += export_off - names_end;  /* [names_end .. export_off) */
    memcpy(nua + q, impbuf, dI); q += dI;                           /* + imports */
    memcpy(nua + q, ua + export_off, preload_ins - export_off); q += preload_ins - export_off; /* [export_off .. preload_ins) */
    wr32(nua + q, obj_mesh); q += dP;                               /* + preload entry */
    memcpy(nua + q, ua + preload_ins, ualen - preload_ins); q += ualen - preload_ins; /* [preload_ins .. end) */
    CHECK(q == nualen, "splice size");

    /* ---- header fixups ---- */
    wr32(nua + H_NAME_COUNT, name_count + 3);
    wr32(nua + H_IMPORT_COUNT, import_count + 2);
    wr32(nua + H_PRELOAD_COUNT, preload_count + 1);
    wr32(nua + H_IMPORT_OFF, import_off + dN);
    wr32(nua + H_EXPORT_OFF, export_off + dN + dI);
    wr32(nua + H_DEPENDS_OFF, depends_off + dN + dI);
    wr32(nua + H_ASSETREG_OFF, assetreg_off + dN + dI);
    wr32(nua + H_PRELOAD_OFF, preload_off + dN + dI);
    wr32(nua + H_TOTAL, total + dN + dI + dP);
    wr64(nua + H_BULK_START, bulk_start + dN + dI + dP + dX);

    /* ---- export table: SerialOffset cascade + FirstExportDependency shift ---- */
    int new_export_off = export_off + dN + dI, base = dN + dI + dP;
    for (int k = 0; k < export_count; k++) {
        int r = new_export_off + k * EXPORT_STRIDE;
        int64_t so = rd64(nua + r + E_SERIAL_OFF), uexp_off = so - total;
        int64_t nso = so + base + (uexp_off > BB_UEXP_OFF ? dX : 0);
        wr64(nua + r + E_SERIAL_OFF, nso);
        int fed = rd32(nua + r + E_FIRST_DEP);
        if (fed >= PRELOAD_INSERT_INDEX) wr32(nua + r + E_FIRST_DEP, fed + 1);
    }
    int nbb = new_export_off + BB_EXPORT_INDEX * EXPORT_STRIDE;
    wr64(nua + nbb + E_SERIAL_SIZE, 67 + dX);                       /* 67 -> 96 */
    wr32(nua + nbb + E_CREATE_BEFORE_SER, rd32(nua + nbb + E_CREATE_BEFORE_SER) + 1);  /* 1 -> 2 */

    CHECK(rd32(nua + H_TOTAL) == nualen, "new total");
    printf("    [meshbind] +%dB names, +%dB imports (mesh=%d), +%dB prop @uexp%d, +1 EDL dep\n",
           dN, dI, obj_mesh, dX, at);
    *ua_out = nua; *ua_outlen = nualen; *ux_out = nux; *ux_outlen = nuxlen;
}

/* (4b) UCS BurnedBody.SetVisibility(False,False) -> (True,False) : always-visible */
static void set_burnedbody_visible(unsigned char *ux) {
    CHECK(ux[10689] == 0x28 || ux[10689] == 0x27, "uexp@10689=0x%02x", ux[10689]);
    if (ux[10689] == 0x28) { ux[10689] = 0x27; printf("    [meshbind] BurnedBody.SetVisibility(False->True) @uexp10689 (always-visible)\n"); }
}

/* (4c) insert BurnedBody.SetVisibility(False,False) before Return in StartDieMotion.
 *      ua modified in place (same size); returns the grown ux (caller frees). */
static void hide_at_death(unsigned char *ua, const unsigned char *ux, int uxlen,
                          unsigned char **ux_out, int *ux_outlen) {
    /* Context(InstanceVar 351 BurnedBody) skip12 rprop0 FinalFunc(-234 SetVisibility) False False EndParms */
    unsigned char INS[22] = { 0x19, 0x01 };
    wr32(INS + 2, 351); wr32(INS + 6, 12); wr32(INS + 10, 0); INS[14] = 0x1c; wr32(INS + 15, -234);
    INS[19] = 0x28; INS[20] = 0x28; INS[21] = 0x16;

    int total = rd32(ua + H_TOTAL), export_count = rd32(ua + H_EXPORT_COUNT), export_off = rd32(ua + H_EXPORT_OFF);
    int rec = -1; int64_t so = 0, ss = 0;
    for (int k = 0; k < export_count; k++) {
        int r = export_off + k * EXPORT_STRIDE; int nidx = rd32(ua + r + 16);
        if (strcmp(name_at(ua, nidx), "StartDieMotion") == 0) { rec = r; ss = rd64(ua + r + E_SERIAL_SIZE); so = rd64(ua + r + E_SERIAL_OFF); break; }
    }
    CHECK(rec >= 0, "StartDieMotion not found");
    int uo = (int)(so - total), st = 32;
    int A = rd32(ux + uo + st - 8), B = rd32(ux + uo + st - 4);   /* ScriptBytecodeSize(mem), SerializedScriptSize(disk) */
    CHECK(A == 40 && B == 28, "StartDieMotion bc sizes %d/%d", A, B);
    int ins = uo + st + B - 3;
    CHECK(memcmp(ux + ins, "\x04\x0b\x53", 3) == 0, "Return;EndOfScript not at expected pos");

    int nuxlen = uxlen + 22; unsigned char *nux = malloc(nuxlen); CHECK(nux, "oom");
    memcpy(nux, ux, ins); memcpy(nux + ins, INS, 22); memcpy(nux + ins + 22, ux + ins, uxlen - ins);
    wr32(nux + uo + st - 8, A + 34); wr32(nux + uo + st - 4, B + 22);

    wr64(ua + rec + E_SERIAL_SIZE, ss + 22);                       /* StartDieMotion SerialSize +22 */
    for (int k = 0; k < export_count; k++) { int r = export_off + k * EXPORT_STRIDE; int64_t o = rd64(ua + r + E_SERIAL_OFF); if (o > so) wr64(ua + r + E_SERIAL_OFF, o + 22); }
    wr64(ua + H_BULK_START, rd64(ua + H_BULK_START) + 22);
    printf("    [meshbind] hide-at-death: +22B SetVisibility(False,False) in StartDieMotion\n");
    *ux_out = nux; *ux_outlen = nuxlen;
}

/* ============================== main logic ================================ */
typedef struct { unsigned char *data; int len; unsigned char sha[20]; int64_t append_off; } Payload;

static unsigned char *fpak0(int64_t off, int n, const unsigned char sha[20]) {
    static unsigned char r[CM0_HDR];
    wr64(r, off); wr64(r + 8, n); wr64(r + 16, n); wr32(r + 24, 0);
    memcpy(r + 28, sha, 20); r[48] = 0; wr32(r + 49, 0); return r;
}

/* ================================ revert ================================== */
/* Undo a previous apply. apply leaves the original footer+index intact inside
 * [0..oldsize) and appends our records after it; the 5 target entries become cm0
 * records whose smallest offset is exactly oldsize. So: find oldsize = min target
 * offset, verify the original footer sits (self-consistently) at oldsize-45, then
 * truncate back to it. Refuses (never truncates) if that signature isn't present. */
static int do_revert(const char *path) {
    PF pf = pf_open(path, 1); CHECK(pf_ok(pf), "cannot open %s", path);
    int64_t fs = pf_size(pf);
    unsigned char foot[45]; CHECK(!pf_read(pf, fs - 45, foot, 45), "read footer");
    CHECK((unsigned char)foot[1] == PAK_MAGIC1 && rd32(foot + 5) == 4, "not a UE4 pak v4");
    int64_t ioff = rd64(foot + 9), isize = rd64(foot + 17);
    unsigned char *idx = malloc(isize); CHECK(idx, "oom"); CHECK(!pf_read(pf, ioff, idx, isize), "read index");

    int64_t p = 0; char mnt[256]; p = read_str(idx, p, mnt, sizeof mnt);    /* mount */
    int num = rd32(idx + p); p += 4;
    int found = 0, all_cm0 = 1; int64_t oldsize = -1;
    for (int i = 0; i < num; i++) {
        char name[256]; int64_t rp = read_str(idx, p, name, sizeof name);
        int64_t off = rd64(idx + rp); int32_t cm = rd32(idx + rp + 24);
        int64_t q = rp + 28 + 20; if (cm != 0) { int nb = rd32(idx + q); q += 4 + (int64_t)nb * 16; } q += 1 + 4;
        for (int t = 0; t < 5; t++) if (ends_with(name, targets[t].suffix)) {
            found++; if (cm != 0) all_cm0 = 0; if (oldsize < 0 || off < oldsize) oldsize = off; break; }
        p = q;
    }
    free(idx);
    CHECK(found == 5, "found %d/5 target entries (not this game's pak?)", found);
    if (!all_cm0 || oldsize <= 0) { printf("targets are not appended -- pak looks original; nothing to revert.\n"); pf_close(pf); return 0; }

    /* the original 45-byte footer must sit exactly at oldsize-45 and be self-consistent */
    unsigned char of[45]; CHECK(!pf_read(pf, oldsize - 45, of, 45), "read original footer");
    CHECK((unsigned char)of[1] == PAK_MAGIC1 && rd32(of + 5) == 4, "no original footer at oldsize-45 -- refuse (won't corrupt)");
    int64_t oio = rd64(of + 9), ois = rd64(of + 17);
    CHECK(oio >= 0 && oio < oldsize && oio + ois + 45 == oldsize,
          "original footer inconsistent -- refuse: %lld+%lld+45 != %lld", (long long)oio, (long long)ois, (long long)oldsize);

    CHECK(!pf_truncate(pf, oldsize), "truncate");
    pf_close(pf);
    printf("REVERTED: %s  %lld -> %lld bytes (original pak restored).\n", path, (long long)fs, (long long)oldsize);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "check") && strcmp(argv[2], "apply") && strcmp(argv[2], "revert"))) {
        fprintf(stderr, "usage: %s <pak> check|apply|revert\n", argv[0]); return 2;
    }
    const char *path = argv[1];
    if (!strcmp(argv[2], "revert")) return do_revert(path);
    int apply = !strcmp(argv[2], "apply");
    h0_init();

    PF pf = pf_open(path, apply);
    CHECK(pf_ok(pf), "cannot open %s", path);
    int64_t fs = pf_size(pf);
    unsigned char foot[45]; CHECK(!pf_read(pf, fs - 45, foot, 45), "read footer");
    CHECK((unsigned char)foot[1] == PAK_MAGIC1 && rd32(foot + 5) == 4, "not a UE4 pak v4");
    int64_t ioff = rd64(foot + 9), isize = rd64(foot + 17);
    unsigned char *idx = malloc(isize); CHECK(idx, "oom"); CHECK(!pf_read(pf, ioff, idx, isize), "read index");

    /* parse index */
    int64_t p = 0; char tmp[256]; p = read_str(idx, p, tmp, sizeof tmp);     /* mount */
    int64_t prefix_len; int num = rd32(idx + p); p += 4; prefix_len = p;
    Entry *ents = calloc(num, sizeof(Entry)); CHECK(ents, "oom");
    for (int i = 0; i < num; i++) {
        Entry *e = &ents[i]; e->name_pos = p; p = read_str(idx, p, e->name, sizeof e->name); e->name_bytes = (int)(p - e->name_pos);
        e->rec_pos = p; e->off = rd64(idx + p); e->size = rd64(idx + p + 8); e->us = rd64(idx + p + 16); e->cm = rd32(idx + p + 24); p += 28;
        p += 20;                                  /* sha */
        e->nblocks = 0; e->blocks_pos = 0;
        if (e->cm != 0) { e->nblocks = rd32(idx + p); p += 4; e->blocks_pos = p; p += (int64_t)e->nblocks * 16; }
        p += 1 + 4;                               /* enc + cbs */
        e->rec_bytes = (int)(p - e->rec_pos);
        e->target = -1;
        for (int t = 0; t < 5; t++) if (ends_with(e->name, targets[t].suffix)) { e->target = t; break; }
    }
    CHECK(p == isize, "index walk %lld != %lld", (long long)p, (long long)isize);

    /* locate the 5 target entries */
    Entry *te[5] = {0};
    for (int i = 0; i < num; i++) if (ents[i].target >= 0) { CHECK(!te[ents[i].target], "duplicate target %s", targets[ents[i].target].suffix); te[ents[i].target] = &ents[i]; }
    for (int t = 0; t < 5; t++) CHECK(te[t], "target not found: %s", targets[t].suffix);

    printf("pak=%s  size=%lld  entries=%d\n", path, (long long)fs, num);

    /* ---- build the 5 patched payloads BY LOGIC ---- */
    Payload pl[5];
    /* (1)(2)(3) gore flips */
    for (int t = 0; t < 3; t++) {
        int64_t len; unsigned char *d = decompress_entry(pf, idx, te[t], &len);
        flip_gore(d, targets[t].gore_off, targets[t].label);
        pl[t].data = d; pl[t].len = (int)len;
    }
    /* (4) BP_EnemyNormal mesh-bind: meshbind -> always-visible -> hide-at-death */
    {
        int64_t ualen0, uxlen0; unsigned char *ua0 = decompress_entry(pf, idx, te[T_EN_UA], &ualen0);
        unsigned char *ux0 = decompress_entry(pf, idx, te[T_EN_UX], &uxlen0);
        unsigned char *ua1, *ux1; int ua1len, ux1len;
        meshbind(ua0, (int)ualen0, ux0, (int)uxlen0, &ua1, &ua1len, &ux1, &ux1len);
        set_burnedbody_visible(ux1);
        unsigned char *ux2; int ux2len; hide_at_death(ua1, ux1, ux1len, &ux2, &ux2len);
        pl[T_EN_UA].data = ua1; pl[T_EN_UA].len = ua1len;     /* ua1 modified in place by hide_at_death */
        pl[T_EN_UX].data = ux2; pl[T_EN_UX].len = ux2len;
        free(ua0); free(ux0); free(ux1);
    }
    for (int t = 0; t < 5; t++) sha1(pl[t].data, pl[t].len, pl[t].sha);

    /* ---- lay out appended records + new index ---- */
    int64_t off = fs;
    for (int t = 0; t < 5; t++) { pl[t].append_off = off; off += CM0_HDR + pl[t].len; }
    int64_t new_index_off = off;
    printf("\nappend-and-repoint plan (stored cm=0):\n");
    for (int t = 0; t < 5; t++) printf("  %-28s -> %8d bytes @ %lld\n", targets[t].label, pl[t].len, (long long)pl[t].append_off);
    printf("new index @ %lld   (undo later: run this tool with 'revert')\n", (long long)new_index_off);

    if (!apply) { printf("\nCHECK only -- no write.\n"); pf_close(pf); return 0; }

    /* ---- write appended records ---- */
    int64_t w = fs;
    for (int t = 0; t < 5; t++) {
        CHECK(w == pl[t].append_off, "layout");
        CHECK(!pf_write(pf, w, fpak0(0, pl[t].len, pl[t].sha), CM0_HDR), "write rec hdr"); w += CM0_HDR;
        CHECK(!pf_write(pf, w, pl[t].data, pl[t].len), "write payload"); w += pl[t].len;
    }
    CHECK(w == new_index_off, "index pos");

    /* ---- rebuild the full index, repointing the 5 entries ---- */
    unsigned char *nidx = malloc(isize + 5 * CM0_HDR + 16); CHECK(nidx, "oom");
    int np = 0; memcpy(nidx, idx, prefix_len); np = (int)prefix_len;
    for (int i = 0; i < num; i++) {
        Entry *e = &ents[i];
        memcpy(nidx + np, idx + e->name_pos, e->name_bytes); np += e->name_bytes;
        if (e->target >= 0) { memcpy(nidx + np, fpak0(pl[e->target].append_off, pl[e->target].len, pl[e->target].sha), CM0_HDR); np += CM0_HDR; }
        else { memcpy(nidx + np, idx + e->rec_pos, e->rec_bytes); np += e->rec_bytes; }
    }
    CHECK(!pf_write(pf, new_index_off, nidx, np), "write index");

    /* ---- fresh footer with recomputed index hash ---- */
    unsigned char ish[20]; sha1(nidx, np, ish);
    unsigned char nf[45]; memset(nf, 0, 45); nf[1] = 0xe1; nf[2] = 0x12; nf[3] = 0x6f; nf[4] = 0x5a;
    wr32(nf + 5, 4); wr64(nf + 9, new_index_off); wr64(nf + 17, np); memcpy(nf + 25, ish, 20);
    CHECK(!pf_write(pf, new_index_off + np, nf, 45), "write footer");
    pf_close(pf);

    char hex[41]; for (int i = 0; i < 6; i++) sprintf(hex + i * 2, "%02x", ish[i]);
    printf("\nAPPLIED. new index@%lld size=%d footer_sha=%s\n", (long long)new_index_off, np, hex);
    return 0;
}
