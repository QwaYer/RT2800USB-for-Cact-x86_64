/*
 * CCMP (AES-CCM) for WPA2 data frames, in software.
 *
 *   MIC     = CCM CBC-MAC over (AAD, payload) with B0 flag 0x59 (M=8, L=2)
 *   payload = CTR keystream, counters 1..n; MIC encrypted with counter 0
 *   AAD     = MAC header with the subtype, Retry/PwrMgmt/MoreData (and Order,
 *             for QoS) masked to 0 and Protected set, the sequence number
 *             zeroed, and the QoS TID for a QoS frame; Nonce = flags | A2 | PN
 *
 * The chip's hardware crypto is not programmed; the driver encrypts/decrypts
 * itself, so TXWI must mark the frame unencrypted (WIV=1) as it already does.
 */

#include "rt2800_local.h"

extern void rt2800_aes128_encrypt(const u8 key[16], const u8 in[16], u8 out[16]);

/* Packet numbers, one per key id: a PN must never repeat within a key, and the
 * peer tracks the replay window per key. */
static u64 ccmp_pn[4];

static void xor16(u8 *d, const u8 *s) { for (int i = 0; i < 16; i++) d[i] ^= s[i]; }

/* Build the AAD of an 802.11 data header (24 bytes, 26 when QoS) exactly as
 * mac80211's ccmp_gcmp_aad(): Frame Control with the subtype bits and
 * Retry/PwrMgmt/MoreData — and Order, for QoS — masked to 0 and Protected set
 * to 1, then A1/A2/A3, SC with the sequence number cleared, and for QoS its QoS
 * control reduced to the TID.  Protected is SET: the frame is protected when
 * its MIC is computed, and the sender and receiver must agree on that bit.
 * Returns the AAD length (22, or 24 for QoS). */
static int ccmp_aad(const u8 *hdr, int hdr_len, u8 *aad) {
    int qos = hdr_len >= 26;
    u8 fc1 = (u8)((hdr[1] & 0xc7) | 0x40);  /* Retry/PwrMgmt/MoreData 0, Protected 1 */

    if (qos)
        fc1 &= (u8)~0x80;                   /* Order 0 for QoS data */
    aad[0] = (u8)(hdr[0] & 0x8f);           /* subtype b4..b6 to 0 */
    aad[1] = fc1;
    memcpy(aad + 2, hdr + 4, 18);           /* A1, A2, A3 */
    aad[20] = (u8)(hdr[22] & 0x0f);         /* SC: fragment only */
    aad[21] = 0x00;                         /* sequence number cleared */
    if (qos) {
        aad[22] = (u8)(hdr[24] & 0x0f);     /* QoS control: TID only */
        aad[23] = 0x00;
        return 24;
    }
    return 22;
}

static void ccmp_mic(const u8 tk[16], const u8 nonce[13], const u8 *aad,
                     int aad_len, const u8 *payload, int plen, u8 mic[8]) {
    u8 b0[16], x[16], blk[16];

    memset(b0, 0, 16);
    b0[0] = 0x59;                   /* Adata=1, M=8, L=2 */
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (u8)(plen >> 8);
    b0[15] = (u8)plen;
    rt2800_aes128_encrypt(tk, b0, x);

    /* AAD: 2-byte length prefix + bytes, zero padded to 16. */
    memset(blk, 0, 16);
    blk[0] = (u8)(aad_len >> 8);
    blk[1] = (u8)aad_len;
    int bi = 2;
    for (int i = 0; i < aad_len; i++) {
        blk[bi++] = aad[i];
        if (bi == 16) { xor16(x, blk); rt2800_aes128_encrypt(tk, x, x); memset(blk, 0, 16); bi = 0; }
    }
    if (bi != 0) { xor16(x, blk); rt2800_aes128_encrypt(tk, x, x); }

    /* Payload blocks start fresh. */
    memset(blk, 0, 16);
    bi = 0;
    for (int i = 0; i < plen; i++) {
        blk[bi++] = payload[i];
        if (bi == 16) { xor16(x, blk); rt2800_aes128_encrypt(tk, x, x); memset(blk, 0, 16); bi = 0; }
    }
    if (bi != 0) { xor16(x, blk); rt2800_aes128_encrypt(tk, x, x); }

    memcpy(mic, x, 8);
}

static void ccmp_ctr(const u8 tk[16], const u8 nonce[13], u8 *data, int len,
                     u16 start) {
    u8 a[16], s[16];
    u16 counter = start;

    for (int off = 0; off < len; off += 16) {
        memset(a, 0, 16);
        a[0] = 0x01;                /* L-1 = 1 */
        memcpy(a + 1, nonce, 13);
        a[14] = (u8)(counter >> 8);
        a[15] = (u8)counter;
        rt2800_aes128_encrypt(tk, a, s);

        int n = len - off;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++)
            data[off + i] ^= s[i];
        counter++;
    }
}

static void ccmp_nonce(const u8 *hdr, int hdr_len, u64 pn, u8 nonce[13]) {
    memset(nonce, 0, 13);
    if (hdr_len >= 26)
        nonce[0] = (u8)(hdr[24] & 0x0f);   /* Nonce Flags: QoS TID */
    memcpy(nonce + 1, hdr + 10, 6);        /* A2 */
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = (u8)(pn >> (8 * i));
}

/* Encrypt a data frame: out = MAC header | CCMP header | ciphertext | MIC.
 * key_id goes into the CCMP header (0 = pairwise key, 1..3 = group key), the
 * same field the receiver uses to pick the key.  Returns the total length, or
 * -1. */
int rt2800_ccmp_encrypt(const u8 tk[16], const u8 *hdr, int hdr_len,
                        const u8 *payload, int plen, u8 key_id,
                        u8 *out, int out_cap) {
    if (out_cap < hdr_len + 8 + plen + 8)
        return -1;

    u64 pn = ++ccmp_pn[key_id & 0x3];
    u8 aad[32], nonce[13], mic[8];
    int aad_len;

    aad_len = ccmp_aad(hdr, hdr_len, aad);
    ccmp_nonce(hdr, hdr_len, pn, nonce);
    ccmp_mic(tk, nonce, aad, aad_len, payload, plen, mic);

    memcpy(out, hdr, hdr_len);
    u8 *ccmp = out + hdr_len;
    /* On-air CCMP header (mac80211 ccmp_pn2hdr): PN0, PN1, a reserved byte,
     * then the key id + ExtIV, then PN2..PN5.  The PN is most significant
     * first on the air while `pn` here is least significant first, so hdr[0]
     * carries the top byte and hdr[7] the bottom one. */
    ccmp[0] = (u8)(pn >> 40);
    ccmp[1] = (u8)(pn >> 32);
    ccmp[2] = 0x00;                          /* reserved */
    ccmp[3] = (u8)((key_id << 6) | 0x20);    /* key id, ExtIV 1 */
    ccmp[4] = (u8)(pn >> 24);
    ccmp[5] = (u8)(pn >> 16);
    ccmp[6] = (u8)(pn >> 8);
    ccmp[7] = (u8)pn;

    u8 *ct = ccmp + 8;
    memcpy(ct, payload, plen);
    ccmp_ctr(tk, nonce, ct, plen, 1);

    u8 m[16];
    memset(m, 0, 16);
    memcpy(m, mic, 8);
    /* RFC 3610 / NIST 800-38C: the authentication tag is XORed with the
     * keystream block at counter 0; the payload uses counters 1..n. */
    ccmp_ctr(tk, nonce, m, 8, 0);
    memcpy(ct + plen, m, 8);

    return hdr_len + 8 + plen + 8;
}

static void mv(u8 *d, const u8 *s, int n) {   /* d < s, forward copy is safe */
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

/* Decrypt in place: f = MAC header | CCMP header | ciphertext | MIC.
 * Returns the payload length, or -1 on MIC failure. */
int rt2800_ccmp_decrypt(const u8 tk[16], u8 *f, int flen, int hdr_len) {
    if (flen < hdr_len + 8 + 8)
        return -1;

    u8 *ccmp = f + hdr_len;
    /* Inverse of the header layout written above (mac80211 ccmp_hdr2pn): the
     * PN is spread over bytes 0,1,4,5,6,7, most significant first. */
    u64 pn = ((u64)ccmp[0] << 40) | ((u64)ccmp[1] << 32) |
             ((u64)ccmp[4] << 24) | ((u64)ccmp[5] << 16) |
             ((u64)ccmp[6] << 8) | (u64)ccmp[7];

    int plen = flen - hdr_len - 8 - 8;
    if (plen <= 0)
        return -1;

    u8 aad[32], nonce[13];
    int aad_len;

    aad_len = ccmp_aad(f, hdr_len, aad);
    ccmp_nonce(f, hdr_len, pn, nonce);

    u8 *ct = ccmp + 8;
    u8 m[16], mic[8];
    memset(m, 0, 16);
    memcpy(m, ct + plen, 8);
    /* The tag was encrypted with the counter-0 keystream block (RFC 3610). */
    ccmp_ctr(tk, nonce, m, 8, 0);
    memcpy(mic, m, 8);

    ccmp_ctr(tk, nonce, ct, plen, 1);

    u8 chk[8];
    ccmp_mic(tk, nonce, aad, aad_len, ct, plen, chk);
    if (memcmp(chk, mic, 8) != 0)
        return -1;

    mv(f + hdr_len, ct, plen);
    return plen;
}

/* Known-answer test of the whole CCMP core.  The two layout bugs that stopped
 * WPA2 data on real hardware (2026-10-09) were internally self-consistent, so a
 * round-trip test passed while nothing interoperated; only an outside vector
 * catches that.  This frame was produced by OpenSSL's EVP_aes_128_ccm (with the
 * mac80211 AAD and the standard CCMP header) and is checked both ways here, so a
 * running module can prove through CACT_WLANCTL_STATUS that its datapath crypto
 * is the interoperable one.  Returns true on success. */
bool rt2800_ccmp_selftest(void) {
    static const u8 tk[16] = {
        0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85,
        0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd8, 0x2f,
    };
    static const u8 hdr[24] = {
        0x08, 0x01, 0x00, 0x00, 0x00, 0x1c, 0xf0, 0xd0,
        0x2c, 0x20, 0x44, 0x33, 0x4c, 0x46, 0x0b, 0xec,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    };
    static const u8 payload[40] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    };
    static const u8 frame[80] = {          /* hdr | CCMP hdr | ciphertext | MIC */
        0x08, 0x01, 0x00, 0x00, 0x00, 0x1c, 0xf0, 0xd0,
        0x2c, 0x20, 0x44, 0x33, 0x4c, 0x46, 0x0b, 0xec,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01,
        0x79, 0x67, 0xf4, 0x1c, 0x59, 0xc0, 0xfc, 0xdc,
        0x82, 0x31, 0x1d, 0x30, 0xc1, 0xb4, 0x9f, 0x03,
        0x53, 0xac, 0xdd, 0xe5, 0xea, 0x7e, 0xab, 0xe4,
        0x88, 0x0c, 0xe2, 0xd1, 0x6e, 0x73, 0x32, 0x3d,
        0x9d, 0x8a, 0xb5, 0xc8, 0x83, 0xb1, 0x3f, 0xc3,
        0x26, 0xd9, 0x75, 0x11, 0xd0, 0x16, 0x95, 0x37,
    };
    u8 out[96];
    u8 back[80];
    u64 save = ccmp_pn[0];
    bool ok = false;

    ccmp_pn[0] = 0;                        /* the vector uses PN = 1 */
    if (rt2800_ccmp_encrypt(tk, hdr, 24, payload, 40, 0, out, sizeof(out)) == 80 &&
        memcmp(out, frame, 80) == 0) {
        memcpy(back, frame, 80);
        if (rt2800_ccmp_decrypt(tk, back, 80, 24) == 40 &&
            memcmp(back + 24, payload, 40) == 0)
            ok = true;
    }
    ccmp_pn[0] = save;
    return ok;
}
