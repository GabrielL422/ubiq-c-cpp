#include "ubiq/platform.h"

#include "ubiq/platform/internal/header.h"
#include "ubiq/platform/internal/request.h"
#include "ubiq/platform/internal/algorithm.h"
#include "ubiq/platform/internal/credentials.h"

#include <sys/param.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>

#include "cJSON/cJSON.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

struct ubiq_platform_encryption
{
    /* http[s]://host/api/v0 */
    const char * restapi;
    struct ubiq_platform_rest_handle * rest;

    char * session;
    int fragment;

    struct {
        struct {
            void * buf;
            size_t len;
        } raw, enc;

        char * fingerprint;

        struct {
            unsigned int max, cur;
        } uses;
    } key;

    const struct ubiq_platform_algorithm * algo;
    EVP_CIPHER_CTX * ctx;
};

void
ubiq_platform_encryption_destroy(
    struct ubiq_platform_encryption * e)
{
    /*
     * if there is a session and a fingerprint
     * and the key was used less times than requested,
     * then update the server with the actual number
     * of uses
     */
    if (e->session && e->key.fingerprint &&
        e->key.uses.cur < e->key.uses.max) {
        const char * const fmt = "%s/encryption/key/%s/%s";

        cJSON * json;
        char * url, * str;
        int len, res;

        /* create the request url using the fingerprint and session */

        len = snprintf(
            NULL, 0, fmt, e->restapi, e->key.fingerprint, e->session);
        url = malloc(len + 1);
        snprintf(
            url, len + 1, fmt, e->restapi, e->key.fingerprint, e->session);

        /* the json object to send */

        json = cJSON_CreateObject();
        cJSON_AddItemToObject(
            json, "requested", cJSON_CreateNumber(e->key.uses.max));
        cJSON_AddItemToObject(
            json, "actual", cJSON_CreateNumber(e->key.uses.cur));
        str = cJSON_Print(json);
        cJSON_Delete(json);

        /* and send the request */

        res = ubiq_platform_rest_request(
            e->rest,
            HTTP_RM_PATCH, url, "application/json", str, strlen(str));

        free(str);
        free(url);

        if (res != 0 ||
            ubiq_platform_rest_response_code(e->rest) != HTTP_RC_NO_CONTENT) {
            /*
             * TODO: there's not much to do if the http request fails
             * since the encryption object itself is being destroyed,
             * and the function doesn't return a value. this failure
             * should probably be logged somewhere.
             */
        }
    }

    ubiq_platform_rest_handle_destroy(e->rest);

    free(e->key.fingerprint);
    free(e->key.enc.buf);
    free(e->key.raw.buf);

    free(e->session);

    /* don't free cipher */
    if (e->ctx) {
        EVP_CIPHER_CTX_free(e->ctx);
    }

    free(e);
}

static
int
ubiq_platform_encryption_new(
    const char * const host,
    const char * const papi, const char * const sapi,
    struct ubiq_platform_encryption ** const enc)
{
    static const char * const api_path = "api/v0";

    struct ubiq_platform_encryption * e;
    size_t len;
    int res;

    res = -ENOMEM;
    len = ubiq_platform_snprintf_api_url(NULL, 0, host, api_path) + 1;
    e = calloc(1, sizeof(*e) + len);
    if (e) {
        ubiq_platform_snprintf_api_url((char *)(e + 1), len, host, api_path);
        e->restapi = (char *)(e + 1);

        res = ubiq_platform_rest_handle_create(papi, sapi, &e->rest);
        if (res != 0) {
            free(e);
            e = NULL;
        }
    }

    *enc = e;
    return res;
}

/*
 * openssl requires a callback to obtain a password.
 * in this case, the password is just passed in the
 * void pointer and copied to the result buffer.
 */
static
int
get_password_callback(char * const buf, const int size,
                      const int rw,
                      void * const udata)
{
    const char * const pwstr = udata;
    const int pwlen = strlen(pwstr);
    const int len = MIN(size, pwlen);
    memcpy(buf, pwstr, len);
    return len;
}

static
int
ubiq_platform_encryption_parse_response(
    struct ubiq_platform_encryption * const e,
    const char * const srsa, const cJSON * const r)
{
    EVP_PKEY * prvkey;
    const cJSON * j;
    int res;

    res = 0;
    prvkey = NULL;

    if (res == 0) {
        /*
         * decrypt the private key using the srsa as a password
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "encrypted_private_key");
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            BIO * const bp = BIO_new_mem_buf(j->valuestring, -1);
            prvkey = PEM_read_bio_PrivateKey(
                bp, NULL, get_password_callback, (void *)srsa);
            BIO_free(bp);
            if (!prvkey) {
                res = -1;
            }
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        /*
         * save the session id
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "encryption_session");
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            e->session = strdup(j->valuestring);
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        /*
         * save the key fingerprint
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "key_fingerprint");
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            e->key.fingerprint = strdup(j->valuestring);
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        /*
         * unwrap the data key
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "wrapped_data_key");
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            EVP_ENCODE_CTX * ectx;
            EVP_PKEY_CTX * pctx;
            void * buf;
            size_t len;
            int outl;

            /*
             * the key has to be base64 decoded. just malloc a buffer
             * the same size as the existing string since the decoded
             * value will be smaller than the encoded one
             */
            len = strlen(j->valuestring);
            buf = malloc(len);

            /*
             * base64 decode the string. the init/update/final scheme
             * removes the padding (as opposed to the decodeblock
             * function which leaves it in.
             */
            ectx = EVP_ENCODE_CTX_new();
            EVP_DecodeInit(ectx);
            EVP_DecodeUpdate(ectx, buf, &outl, j->valuestring, len);
            len = outl;
            EVP_DecodeFinal(ectx, (char *)buf + len, &outl);
            len += outl;
            EVP_ENCODE_CTX_free(ectx);

            /*
             * unwrap the data key using the private rsa key that
             * was decrypted earlier
             */
            pctx = EVP_PKEY_CTX_new(prvkey, NULL);
            EVP_PKEY_decrypt_init(pctx);
            EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING);
            EVP_PKEY_decrypt(pctx, NULL, &e->key.raw.len, NULL, 0);
            e->key.raw.buf = malloc(e->key.raw.len);
            if (EVP_PKEY_decrypt(pctx,
                                 e->key.raw.buf, &e->key.raw.len,
                                 buf, len) <= 0) {
                res = -1;
            }
            EVP_PKEY_CTX_free(pctx);

            free(buf);
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        /*
         * the encrypted data key is stored at the front end of
         * any encrypted data. base64 decode it and just store
         * the result
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "encrypted_data_key");
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            EVP_ENCODE_CTX * ectx;
            EVP_PKEY_CTX * pctx;
            int outl;

            e->key.enc.len = strlen(j->valuestring);
            e->key.enc.buf = malloc(e->key.enc.len);

            /*
             * use the init/update/final scheme to automatically
             * handle padding
             */
            ectx = EVP_ENCODE_CTX_new();
            EVP_DecodeInit(ectx);
            EVP_DecodeUpdate(ectx,
                             e->key.enc.buf, &outl,
                             j->valuestring, e->key.enc.len);
            e->key.enc.len = outl;
            EVP_DecodeFinal(ectx,
                            (char *)e->key.enc.buf + e->key.enc.len,
                            &outl);
            e->key.enc.len += outl;
            EVP_ENCODE_CTX_free(ectx);
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        /*
         * save the maximum number of uses of the key
         */
        j = cJSON_GetObjectItemCaseSensitive(r, "max_uses");
        if (cJSON_IsNumber(j)) {
            e->key.uses.max = j->valueint;
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        j = cJSON_GetObjectItemCaseSensitive(r, "security_model");
        if (cJSON_IsObject(j)) {
            const cJSON * k;

            if (res == 0) {
                /*
                 * the algorithm name should correspond with the openssl
                 * name which is convenient for us. use the resulting
                 * cipher object pointer to look up the corresponding
                 * ubiq algorithm
                 */
                k = cJSON_GetObjectItemCaseSensitive(j, "algorithm");
                if (cJSON_IsString(k) && k->valuestring != NULL) {
                    e->algo = ubiq_platform_algorithm_get_bycipher(
                        EVP_get_cipherbyname(k->valuestring));
                    if (!e->algo) {
                        res = -1;
                    }
                } else {
                    res = -1;
                }
            }

            if (res == 0) {
                /*
                 * keep track of whether fragmentation is enabled
                 */
                k = cJSON_GetObjectItemCaseSensitive(
                    j, "enable_data_fragmentation");
                if (cJSON_IsBool(k)) {
                    e->fragment = cJSON_IsTrue(k);
                } else {
                    res = -1;
                }
            }
        } else {
            res = -1;
        }
    }

    if (prvkey) {
        EVP_PKEY_free(prvkey);
    }

    return res;
}

int ubiq_platform_encryption_create(
    const struct ubiq_platform_credentials * const creds,
    const unsigned int uses,
    struct ubiq_platform_encryption ** const enc)
{
    struct ubiq_platform_encryption * e;
    int res;

    const char * const host = ubiq_platform_credentials_get_host(creds);
    const char * const papi = ubiq_platform_credentials_get_papi(creds);
    const char * const sapi = ubiq_platform_credentials_get_sapi(creds);
    const char * const srsa = ubiq_platform_credentials_get_srsa(creds);

    res = ubiq_platform_encryption_new(host, papi, sapi, &e);
    if (res == 0) {
        const char * const fmt = "%s/encryption/key";

        cJSON * json;
        char * url, * str;
        int len;

        /*
         * create the url for the request
         */
        len = snprintf(NULL, 0, fmt, e->restapi);
        url = malloc(len + 1);
        snprintf(url, len + 1, fmt, e->restapi);

        /*
         * request body just contains the number of
         * desired uses of the key
         */
        json = cJSON_CreateObject();
        cJSON_AddItemToObject(json, "uses", cJSON_CreateNumber(uses));
        str = cJSON_Print(json);
        cJSON_Delete(json);

        res = ubiq_platform_rest_request(
            e->rest,
            HTTP_RM_POST, url, "application/json", str, strlen(str));

        free(str);
        free(url);

        /*
         * if the request was successful, parse the response
         */

        if (res == 0 &&
            ubiq_platform_rest_response_code(e->rest) == HTTP_RC_CREATED) {
            const void * rsp;
            size_t len;
            cJSON * json;

            rsp = ubiq_platform_rest_response_content(e->rest, &len);
            res = (json = cJSON_ParseWithLength(rsp, len)) ? 0 : INT_MIN;

            if (res == 0) {
                res = ubiq_platform_encryption_parse_response(e, srsa, json);
                cJSON_Delete(json);
            }
        } else {
            res = -1;
        }
    }

    if (res == 0) {
        *enc = e;
    } else {
        ubiq_platform_encryption_destroy(e);
    }

    return res;
}

int
ubiq_platform_encryption_begin(
    struct ubiq_platform_encryption * const enc,
    void ** const ctbuf, size_t * const ctlen)
{
    int res;

    if (enc->ctx) {
        /* encryption already in progress */
        res = -EEXIST;
    } else if (enc->key.uses.cur >= enc->key.uses.max) {
        /* key is all used up */
        res = -EDQUOT;
    } else {
        /*
         * good to go, build a header; create the context
         */
        const size_t ivlen = EVP_CIPHER_iv_length(enc->algo->cipher);
        union ubiq_platform_header * hdr;
        size_t len;

        len = sizeof(*hdr) + ivlen + enc->key.enc.len;
        hdr = malloc(len);

        /* the fixed-size portion of the header */

        hdr->pre.version = 0;
        hdr->v0.sbz = 0;
        hdr->v0.algorithm = enc->algo->id;
        hdr->v0.ivlen = ivlen;
        hdr->v0.keylen = htons(enc->key.enc.len);

        /* add on the initialization vector */
        if (RAND_bytes((unsigned char *)(hdr + 1), ivlen)) {
            /* add the encrypted key */
            memcpy((char *)(hdr + 1) + ivlen, enc->key.enc.buf,
                   enc->key.enc.len);

            *ctbuf = (void *)hdr;
            *ctlen = len;

            /* set up the encryption context for the update() calls */
            enc->ctx = EVP_CIPHER_CTX_new();
            EVP_EncryptInit(enc->ctx, enc->algo->cipher,
                            enc->key.raw.buf, (unsigned char *)(hdr + 1));

            enc->key.uses.cur++;

            res = 0;
        } else {
            res = -ENODATA;
            free(hdr);
        }
    }

    return res;
}

int
ubiq_platform_encryption_update(
    struct ubiq_platform_encryption * enc,
    const void * const ptbuf, const size_t const ptlen,
    void ** const ctbuf, size_t * const ctlen)
{
    int res;

    res = -EBADF;
    if (enc->ctx) {
        void * buf;
        int len;

        len = ptlen + EVP_CIPHER_CTX_block_size(enc->ctx);
        buf = malloc(len);

        if (EVP_EncryptUpdate(enc->ctx, buf, &len, ptbuf, ptlen)) {
            *ctbuf = buf;
            *ctlen = len;
            res = 0;
        } else {
            free(buf);
            res = -1;
        }
    }

    return res;
}

int
ubiq_platform_encryption_end(
    struct ubiq_platform_encryption * enc,
    void ** const ctbuf, size_t * const ctlen)
{
    int res;

    res = -EBADF;
    if (enc->ctx) {
        void * buf;
        int len, outl;

        len = EVP_CIPHER_CTX_block_size(enc->ctx) + enc->algo->taglen;
        buf = malloc(len);

        EVP_EncryptFinal(enc->ctx, buf, &outl);
        assert(len - outl >= enc->algo->taglen);

        if (enc->algo->taglen) {
            /*
             * don't forget the tag for algorithms that need/use it
             */
            EVP_CIPHER_CTX_ctrl(enc->ctx,
                                EVP_CTRL_AEAD_GET_TAG,
                                enc->algo->taglen, (char *)buf + outl);
        }

        EVP_CIPHER_CTX_free(enc->ctx);
        enc->ctx = NULL;

        *ctbuf = buf;
        *ctlen = outl + enc->algo->taglen;

        res = 0;
    }

    return res;
}

int
ubiq_platform_encrypt(
    const struct ubiq_platform_credentials * const creds,
    const void * ptbuf, const size_t ptlen,
    void ** ctbuf, size_t * ctlen)
{
    struct ubiq_platform_encryption * enc;
    int res;

    struct {
        void * buf;
        size_t len;
    } pre, upd, end;

    pre.buf = upd.buf = end.buf = NULL;

    enc = NULL;
    res = ubiq_platform_encryption_create(creds, 1, &enc);

    if (res == 0) {
        res = ubiq_platform_encryption_begin(
            enc, &pre.buf, &pre.len);
    }

    if (res == 0) {
        res = ubiq_platform_encryption_update(
            enc, ptbuf, ptlen, &upd.buf, &upd.len);
    }

    if (res == 0) {
        res = ubiq_platform_encryption_end(
            enc, &end.buf, &end.len);
    }

    if (enc) {
        ubiq_platform_encryption_destroy(enc);
    }

    if (res == 0) {
        *ctlen = pre.len + upd.len + end.len;
        *ctbuf = malloc(*ctlen);

        memcpy(*ctbuf, pre.buf, pre.len);
        memcpy((char *)*ctbuf + pre.len, upd.buf, upd.len);
        memcpy((char *)*ctbuf + pre.len + upd.len, end.buf, end.len);
    }

    free(end.buf);
    free(upd.buf);
    free(pre.buf);

    return res;
}