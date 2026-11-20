#include <assert.h>

#include <openssl/x509v3.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "backend.h"


// ReSharper disable CppJoinDeclarationAndAssignment
void wsland_backend_generate_crypto(struct wsland_backend *backend) {
    long serial = 0;
    const unsigned char session_name[] = "wsland";

    BUF_MEM *mem;
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    assert(pkey != NULL);
    BIO *bio = BIO_new(BIO_s_mem());
    assert(bio != NULL);
    assert(PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    BIO_get_mem_ptr(bio, &mem); // ReSharper disable once CppDFAMemoryLeak
    backend->key_content = calloc(mem->length + 1, 1);
    assert(backend->key_content != NULL);
    memcpy(backend->key_content, mem->data, mem->length);
    BIO_free_all(bio);

    X509V3_CTX ctx;
    X509 *x509 = X509_new();
    X509_set_version(x509, 2);
    RAND_bytes((unsigned char*)&serial, sizeof(serial));
    ASN1_INTEGER_set(X509_get_serialNumber(x509), serial);
    ASN1_TIME *before = X509_getm_notBefore(x509);
    X509_gmtime_adj(before, 0);
    ASN1_TIME *after = X509_getm_notAfter(x509);
    X509_gmtime_adj(after, 60); /* good for a minute */
    X509_set_pubkey(x509, pkey);
    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, session_name, sizeof(session_name) - 1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x509, x509, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_ext_key_usage, "serverAuth");
    assert(ext);
    X509_add_ext(x509, ext, -1);
    X509_EXTENSION_free(ext);
    const EVP_MD *md = EVP_sha256();
    assert(X509_sign(x509, pkey, md) != 0);

    BUF_MEM *mem_x509;
    BIO *bio_x509 = BIO_new(BIO_s_mem());
    assert(bio_x509 != NULL);
    PEM_write_bio_X509(bio_x509, x509);
    BIO_get_mem_ptr(bio_x509, &mem_x509);
    backend->cert_content = (char*)calloc(mem_x509->length + 1, 1);
    memcpy(backend->cert_content, mem_x509->data, mem_x509->length);
    BIO_free_all(bio_x509);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}