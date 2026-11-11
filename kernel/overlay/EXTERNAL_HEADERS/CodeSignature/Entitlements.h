#ifndef _CODESIGNATURE_ENTITLEMENTS_H
#define _CODESIGNATURE_ENTITLEMENTS_H
/*
 * redwood stub for iOS 26's closed CodeSignature framework.  These are entitlement name strings
 * used only to gate the hardened web-browser-engine features, which this port does not host; the
 * exact values matter only for matching those entitlements, which nothing here carries.
 */
#define kCSWebBrowserHostEntitlement       "com.apple.developer.web-browser-engine.host"
#define kCSWebBrowserGPUEntitlement        "com.apple.developer.web-browser-engine.rendering"
#define kCSWebBrowserNetworkEntitlement    "com.apple.developer.web-browser-engine.networking"
#define kCSWebBrowserWebContentEntitlement "com.apple.developer.web-browser-engine.webcontent"
#endif /* _CODESIGNATURE_ENTITLEMENTS_H */
