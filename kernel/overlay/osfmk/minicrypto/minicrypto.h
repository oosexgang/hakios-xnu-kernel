/*
 * minicrypto: a minimal stand-in for Apple's corecrypto kext, enough for the
 * open-source kernel to boot without it. See mc_provider.c.
 */
#ifndef _MINICRYPTO_H_
#define _MINICRYPTO_H_

#include <corecrypto/ccdigest.h>

extern const struct ccdigest_info mc_md5_di;
extern const struct ccdigest_info mc_sha1_di;
extern const struct ccdigest_info mc_sha384_di;
extern const struct ccdigest_info mc_sha512_di;

void mc_digest(const struct ccdigest_info *di, size_t len, const void *data, void *digest);
const char *mc_digest_selftest(void);

/* AES (mc_aes.c) */
struct ccmode_ecb;
struct ccmode_cbc;
struct ccmode_ctr;
extern const struct ccmode_ecb mc_aes_ecb_encrypt, mc_aes_ecb_decrypt;
extern const struct ccmode_cbc mc_aes_cbc_encrypt, mc_aes_cbc_decrypt;
extern const struct ccmode_ctr mc_aes_ctr_crypt;
const char *mc_aes_selftest(void);

#endif /* _MINICRYPTO_H_ */
