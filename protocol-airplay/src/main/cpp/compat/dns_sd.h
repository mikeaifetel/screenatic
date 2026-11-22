/*
 * Minimal <dns_sd.h> for building UxPlay's lib/dnssd.c on Android.
 *
 * Only what dnssd.c uses: TXT-record helpers (implemented for real, Bonjour wire
 * format) and DNSServiceRegister / DNSServiceRefDeallocate (no-ops). Publishing is
 * done in Kotlin with NsdManager using the TXT records UxPlay builds here.
 */
#ifndef SCREENATIC_COMPAT_DNS_SD_H
#define SCREENATIC_COMPAT_DNS_SD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef union _TXTRecordRef_t { char PrivateData[16]; char *ForceNaturalAlignment; } TXTRecordRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;

enum { kDNSServiceErr_NoError = 0, kDNSServiceErr_NoMemory = -65539, kDNSServiceErr_BadParam = -65540 };

typedef void (*DNSServiceRegisterReply)(DNSServiceRef sdRef, DNSServiceFlags flags,
                                        DNSServiceErrorType errorCode, const char *name,
                                        const char *regtype, const char *domain, void *context);

DNSServiceErrorType DNSServiceRegister(DNSServiceRef *sdRef, DNSServiceFlags flags,
                                       uint32_t interfaceIndex, const char *name,
                                       const char *regtype, const char *domain, const char *host,
                                       uint16_t port, uint16_t txtLen, const void *txtRecord,
                                       DNSServiceRegisterReply callBack, void *context);
void DNSServiceRefDeallocate(DNSServiceRef sdRef);

void TXTRecordCreate(TXTRecordRef *txtRecord, uint16_t bufferLen, void *buffer);
void TXTRecordDeallocate(TXTRecordRef *txtRecord);
DNSServiceErrorType TXTRecordSetValue(TXTRecordRef *txtRecord, const char *key,
                                      uint8_t valueSize, const void *value);
uint16_t TXTRecordGetLength(const TXTRecordRef *txtRecord);
const void *TXTRecordGetBytesPtr(const TXTRecordRef *txtRecord);

#ifdef __cplusplus
}
#endif

#endif /* SCREENATIC_COMPAT_DNS_SD_H */
