// ReSharper disable All
#include <assert.h>

#include <openssl/x509v3.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "wsland.h"

void wsland_freerdp_tls_generate(void) {
    RSA *rsa;
    X509 *x509;
    EVP_PKEY *pkey;
    BIGNUM *rsa_bn;
    X509V3_CTX ctx;
    X509_NAME *name;
    const EVP_MD *md;
    BIO *bio, *bio_x509;
    X509_EXTENSION *ext;
    BUF_MEM *mem, *mem_x509;
    ASN1_TIME *before, *after;

    long serial = 0;
    const unsigned char session_name[] = "wsland";

    pkey = EVP_PKEY_new();
    assert(pkey != NULL);
    rsa_bn = BN_new();
    assert(rsa_bn != NULL);
    rsa = RSA_new();
    assert(rsa != NULL);
    BN_set_word(rsa_bn, RSA_F4);
    assert(RSA_generate_key_ex(rsa, 2048, rsa_bn, NULL) == 1);
    BN_clear_free(rsa_bn);
    EVP_PKEY_assign_RSA(pkey, rsa);

    bio = BIO_new(BIO_s_mem());
    assert(bio != NULL);
    assert(PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    BIO_get_mem_ptr(bio, &mem);
    server.freerdp.tls_key_content = (char*)calloc(mem->length + 1, 1);
    memcpy(server.freerdp.tls_key_content, mem->data, mem->length);
    BIO_free_all(bio);

    x509 = X509_new();
    X509_set_version(x509, 2);
    RAND_bytes((unsigned char*)&serial, sizeof(serial));
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    before = X509_getm_notBefore(x509);
    X509_gmtime_adj(before, 0);
    after = X509_getm_notAfter(x509);
    X509_gmtime_adj(after, 60); /* good for a minute */
    X509_set_pubkey(x509, pkey);
    name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, session_name, sizeof(session_name) - 1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x509, x509, NULL, NULL, 0);
    ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_ext_key_usage, "serverAuth");
    assert(ext);
    X509_add_ext(x509, ext, -1);
    X509_EXTENSION_free(ext);
    md = EVP_sha256();
    assert(X509_sign(x509, pkey, md) != 0);

    bio_x509 = BIO_new(BIO_s_mem());
    assert(bio_x509 != NULL);
    PEM_write_bio_X509(bio_x509, x509);
    BIO_get_mem_ptr(bio_x509, &mem_x509);
    server.freerdp.tls_cert_content = (char*)calloc(mem_x509->length + 1, 1);
    memcpy(server.freerdp.tls_cert_content, mem_x509->data, mem_x509->length);
    BIO_free_all(bio_x509);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}