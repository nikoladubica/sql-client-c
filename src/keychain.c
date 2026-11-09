#include "keychain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#define SERVICE "sqlc"

/* Base query: generic password with this service and account. */
static CFMutableDictionaryRef base_query(const char *svc, const char *profile) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(NULL, svc, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, profile, kCFStringEncodingUTF8);
    if (!q || !service || !account) {
        if (q) CFRelease(q);
        if (service) CFRelease(service);
        if (account) CFRelease(account);
        return NULL;
    }
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}

static void describe(OSStatus st, char *err, int errlen) {
    if (!err || errlen <= 0) return;
    CFStringRef msg = SecCopyErrorMessageString(st, NULL);
    if (!msg || !CFStringGetCString(msg, err, errlen, kCFStringEncodingUTF8))
        snprintf(err, (size_t)errlen, "Keychain error %d", (int)st);
    if (msg) CFRelease(msg);
}

int keychain_set_item(const char *svc, const char *profile, const char *password,
                      char *err, int errlen) {
    CFMutableDictionaryRef q = base_query(svc, profile);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)password, (CFIndex)strlen(password));
    if (!q || !data) {
        if (q) CFRelease(q);
        if (data) CFRelease(data);
        snprintf(err, (size_t)errlen, "Out of memory");
        return -1;
    }

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, attrs);
    CFRelease(attrs);

    if (st == errSecItemNotFound) {
        CFStringRef label = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s: %s"), svc, profile);
        if (label) { CFDictionarySetValue(q, kSecAttrLabel, label); CFRelease(label); }
        CFDictionarySetValue(q, kSecValueData, data);
        st = SecItemAdd(q, NULL);
    }
    CFRelease(data);
    CFRelease(q);

    if (st != errSecSuccess) { describe(st, err, errlen); return -1; }
    return 0;
}

char *keychain_get_item(const char *svc, const char *profile) {
    CFMutableDictionaryRef q = base_query(svc, profile);
    if (!q) return NULL;
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = NULL;
    OSStatus st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (st != errSecSuccess || !result) return NULL;

    char *pw = NULL;
    if (CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef d = result;
        CFIndex len = CFDataGetLength(d);
        pw = malloc((size_t)len + 1);
        if (pw) {
            memcpy(pw, CFDataGetBytePtr(d), (size_t)len);
            pw[len] = '\0';
        }
    }
    CFRelease(result);
    return pw;
}

void keychain_delete_item(const char *svc, const char *profile) {
    CFMutableDictionaryRef q = base_query(svc, profile);
    if (!q) return;
    SecItemDelete(q);
    CFRelease(q);
}

int keychain_set(const char *profile, const char *password, char *err, int errlen) {
    return keychain_set_item(SERVICE, profile, password, err, errlen);
}

char *keychain_get(const char *profile) { return keychain_get_item(SERVICE, profile); }

void keychain_delete(const char *profile) { keychain_delete_item(SERVICE, profile); }
