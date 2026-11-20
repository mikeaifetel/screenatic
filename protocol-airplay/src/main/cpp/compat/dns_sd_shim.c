/* See dns_sd.h in this directory. */
#include "dns_sd.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Service registration: no-op (NsdManager publishes from Kotlin) ---- */

struct _DNSServiceRef_t { int unused; };

DNSServiceErrorType DNSServiceRegister(DNSServiceRef *sdRef, DNSServiceFlags flags,
                                       uint32_t interfaceIndex, const char *name,
                                       const char *regtype, const char *domain, const char *host,
                                       uint16_t port, uint16_t txtLen, const void *txtRecord,
                                       DNSServiceRegisterReply callBack, void *context) {
    (void)flags; (void)interfaceIndex; (void)name; (void)regtype; (void)domain; (void)host;
    (void)port; (void)txtLen; (void)txtRecord; (void)callBack; (void)context;
    if (!sdRef) return kDNSServiceErr_BadParam;
    *sdRef = (DNSServiceRef)calloc(1, sizeof(struct _DNSServiceRef_t));
    return *sdRef ? kDNSServiceErr_NoError : kDNSServiceErr_NoMemory;
}

void DNSServiceRefDeallocate(DNSServiceRef sdRef) { free(sdRef); }

/* ---- TXT records: Bonjour wire format, i.e. repeated [len][key=value] ---- */

typedef struct {
    uint8_t *buf;
    uint16_t len;
    uint16_t cap;
    int owned;
} txt_t;

static txt_t *txt_of(const TXTRecordRef *ref) {
    txt_t *t;
    memcpy(&t, ref->PrivateData, sizeof(t));
    return t;
}

void TXTRecordCreate(TXTRecordRef *ref, uint16_t bufferLen, void *buffer) {
    txt_t *t = (txt_t *)calloc(1, sizeof(txt_t));
    if (t && buffer && bufferLen) {
        t->buf = (uint8_t *)buffer;
        t->cap = bufferLen;
    }
    memset(ref, 0, sizeof(*ref));
    memcpy(ref->PrivateData, &t, sizeof(t));
}

void TXTRecordDeallocate(TXTRecordRef *ref) {
    txt_t *t = txt_of(ref);
    if (!t) return;
    if (t->owned) free(t->buf);
    free(t);
    memset(ref, 0, sizeof(*ref));
}

/* Remove an existing "key" or "key=..." entry so SetValue replaces it. */
static void txt_remove(txt_t *t, const char *key) {
    size_t klen = strlen(key);
    uint16_t i = 0;
    while (i < t->len) {
        uint8_t n = t->buf[i];
        const uint8_t *e = t->buf + i + 1;
        if (n >= klen && strncasecmp((const char *)e, key, klen) == 0 && (n == klen || e[klen] == '=')) {
            memmove(t->buf + i, t->buf + i + 1 + n, t->len - (i + 1 + n));
            t->len -= (uint16_t)(1 + n);
            return;
        }
        i += (uint16_t)(1 + n);
    }
}

DNSServiceErrorType TXTRecordSetValue(TXTRecordRef *ref, const char *key,
                                      uint8_t valueSize, const void *value) {
    txt_t *t = txt_of(ref);
    if (!t || !key) return kDNSServiceErr_BadParam;
    size_t klen = strlen(key);
    size_t entry = klen + (value ? 1u + valueSize : 0u);
    if (klen == 0 || entry > 255) return kDNSServiceErr_BadParam;

    txt_remove(t, key);
    size_t need = (size_t)t->len + 1 + entry;
    if (need > 0xFFFF) return kDNSServiceErr_NoMemory;
    if (need > t->cap) {
        if (!t->owned && t->buf) return kDNSServiceErr_NoMemory; /* caller buffer full */
        size_t cap = t->cap ? t->cap : 256;
        while (cap < need) cap *= 2;
        if (cap > 0xFFFF) cap = 0xFFFF;
        uint8_t *nb = (uint8_t *)realloc(t->owned ? t->buf : NULL, cap);
        if (!nb) return kDNSServiceErr_NoMemory;
        t->buf = nb;
        t->cap = (uint16_t)cap;
        t->owned = 1;
    }
    uint8_t *p = t->buf + t->len;
    *p++ = (uint8_t)entry;
    memcpy(p, key, klen);
    p += klen;
    if (value) {
        *p++ = '=';
        memcpy(p, value, valueSize);
    }
    t->len = (uint16_t)need;
    return kDNSServiceErr_NoError;
}

uint16_t TXTRecordGetLength(const TXTRecordRef *ref) {
    txt_t *t = txt_of(ref);
    return t ? t->len : 0;
}

const void *TXTRecordGetBytesPtr(const TXTRecordRef *ref) {
    txt_t *t = txt_of(ref);
    return t ? t->buf : NULL;
}
