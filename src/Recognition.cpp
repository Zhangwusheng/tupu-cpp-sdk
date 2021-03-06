/******************************************************************************
 * TUPU Recognition API SDK
 * Copyright(c)2013-2016, TUPU Technology
 * http://www.tuputech.com
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <openssl/sha.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <curl/curl.h>

#include "jsmn.h"
#include "Base64.hpp"
#include "TImage.hpp"
#include "Recognition.hpp"


#ifndef VERB_LEV
#define VERB_LEV 0
#endif

#define TUPU_API "http://api.open.tuputech.com/v3/recognition/"
#define USER_AGENT "tupu-client/1.0"

#define RND_LEN 3
#define HEX_LEN RND_LEN * 2 + 1
#define PRIV_KEY_SIZE 1000

using namespace std;

namespace TUPU
{

enum {
    OPC_OK = 0,
    OPC_WRONGPARAM = -1, //Wrong parameter(s)
    OPC_SIGNFAILED = -2, //Failed to sign request
    OPC_SENDFAILED = -3, //Failed to send request
    OPC_REQFAILED = -4, //Request failed
    OPC_PARSEFAILED = -5, //Failed to parse response data
    OPC_VERIFYFAILED = -6, //Failed to verify response signature
    OPC_OTHERS = -10
};

typedef struct {
  char *memory;
  size_t size;
} MemChunk;

static int get_private_key_content(const string & key_path, char *buf, size_t key_size);
static RSA * read_private_key(char *buf);
static RSA * read_public_key(const string & pubkeyStr);
static RSA * read_tupu_pubkey();
static int sign_with_sha256(const string & message, RSA * p_rsa, string & result);
static int verify_with_sha256(const string & message, const string & signature, RSA * p_rsa);
static void random_str(size_t len, char *output);
static void parse_json_value(const char *src, size_t len, string & result);
static size_t write_memory(void *contents, size_t size, size_t nmemb, void *userp);
static void compose_form(struct curl_httppost ** post, const vector<TImage> & images,
    const string & timestamp, const string & nonce,
    const string & signature, const string & uid);
#if VERB_LEV >= 2
static int debug_trace(CURL *curl, curl_infotype type, char *data, size_t size, void *userptr);
#endif


static FILE *errStream = stderr;
void setErrorOutputStream(FILE *stream) { errStream = stream; }
void resetErrorOutputStream() { errStream = stderr; }

const char * opErrorString(int err) {
    if (err > 0)
        return curl_easy_strerror((CURLcode)err);
    else if (err < 0) {
        switch (err) {
            case OPC_WRONGPARAM:
                return "Wrong parameter";
            case OPC_SIGNFAILED:
                return "Failed to sign request";
            case OPC_SENDFAILED:
                return "Failded to send request";
            case OPC_REQFAILED:
                return "Request failed";
            case OPC_PARSEFAILED:
                return "Failed to parse response data";
            case OPC_VERIFYFAILED:
                return "Failed to verify response signature";
            case OPC_OTHERS:
                return "Other unclassified error";
        }
    }
    return "";
}


Recognition::Recognition(const string & rsaPrivateKeyPath)
    : m_rsaPrivateKey(NULL)
    , m_tupuPublicKey(NULL)
    , m_apiUrl(TUPU_API)
    , m_ua(USER_AGENT)
    , m_priKeyBuf(NULL)
{
    generalInit(rsaPrivateKeyPath);
}

Recognition::Recognition(const string & rsaPrivateKeyPath, const string & apiUrl)
    : m_rsaPrivateKey(NULL)
    , m_tupuPublicKey(NULL)
    , m_apiUrl(apiUrl)
    , m_ua(USER_AGENT)
    , m_priKeyBuf(NULL)
{
    generalInit(rsaPrivateKeyPath);
}

Recognition::~Recognition()
{
    //RSA_free(m_rsaPrivateKey);
    RSA_free(m_tupuPublicKey);

    free(m_priKeyBuf);

    curl_global_cleanup();
}

void Recognition::generalInit(const string & rsaPrivateKeyPath)
{
    //m_rsaPrivateKey = read_private_key(rsaPrivateKeyPath);
    m_priKeyBuf = (char*)malloc(PRIV_KEY_SIZE);
    memset(m_priKeyBuf, 0, PRIV_KEY_SIZE);
    get_private_key_content(rsaPrivateKeyPath, m_priKeyBuf, PRIV_KEY_SIZE-1);
    
    m_tupuPublicKey = read_tupu_pubkey();

    curl_global_init(CURL_GLOBAL_ALL);
}


int Recognition::performWithURL(const string & secretId, string & result, long *statusCode,
    const vector<string> & images, const vector<string> & tags)
{
    vector<TImage> imgList;

    unsigned int i = 0;
    const char * tag = NULL;
    while (i < images.size())
    {
        TImage image;
        image.setURL(images[i]);

        if (i < tags.size() && !tags[i].empty())
            tag = tags[i].c_str();
        if (tag)
            image.setTag(tag);

        imgList.push_back(image);

        i++;
    }

    return perform(secretId, imgList, result, statusCode);
}

int Recognition::performWithPath(const string & secretId, string & result, long *statusCode,
    const vector<string> & images, const vector<string> & tags)
{
    vector<TImage> imgList;

    unsigned int i = 0;
    const char * tag = NULL;
    while (i < images.size())
    {
        TImage image;
        image.setPath(images[i]);

        if (i < tags.size() && !tags[i].empty())
            tag = tags[i].c_str();
        if (tag)
            image.setTag(tag);

        imgList.push_back(image);

        i++;
    }

    return perform(secretId, imgList, result, statusCode);
}


int Recognition::perform(const string & secretId, const vector<TImage> & images,
    string & result, long *statusCode)
{
    if (secretId.size() <=0 || images.size() <= 0)
        return OPC_WRONGPARAM;

    int opc = OPC_OK;
    char nonce[HEX_LEN];
    random_str(RND_LEN, nonce);

    time_t ts = time(NULL);
    char tsBuf[30];
    sprintf(tsBuf, "%ld", ts);


    stringstream s;
    s << secretId << "," << tsBuf << "," << nonce;
    string msg = s.str(), signature;

    RSA * rsaPrivateKey = read_private_key(m_priKeyBuf);
    int rc = sign_with_sha256(msg, rsaPrivateKey, signature);
    RSA_free(rsaPrivateKey);
    if (1 != rc) {
        return OPC_SIGNFAILED;
    }

    struct curl_httppost *formpost = NULL;
    compose_form(&formpost, images, tsBuf, nonce, signature, m_uid);
    if (NULL == formpost)
        return OPC_OTHERS;

    opc = sendRequest(secretId, formpost, result, statusCode);
    curl_formfree(formpost);
    return opc;
}


int Recognition::sendRequest(const string & secretId, struct curl_httppost *post,
    string & result, long *statusCode)
{
    struct curl_slist *headerlist = NULL;
    static const char buf[] = "Expect: 100-continue";

    CURL *curl = NULL;
    CURLcode res = CURLE_OK;
    int opc = OPC_OK;

    MemChunk chunk;
    chunk.memory = (char*)malloc(1);  /* will be grown as needed by the realloc above */ 
    chunk.size = 0; 

    headerlist = curl_slist_append(headerlist, buf);

    curl = curl_easy_init();

    string url = m_apiUrl + secretId;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // This option is here to allow multi-threaded unix applications to still set/use
    // all timeout options etc, without risking getting signals.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1); 

    // set form-data to post
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);

    /* send all data to this function  */ 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    /* we pass our 'chunk' struct to the callback function */ 
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    curl_easy_setopt(curl, CURLOPT_USERAGENT, m_ua.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

#if VERB_LEV >= 1
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#if VERB_LEV >= 2
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_trace);
#endif
#endif

    res = curl_easy_perform(curl);

    long httpcode = 0;
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &httpcode);
    if (res != CURLE_OK) {
        fprintf(errStream, "curl_easy_perform() failed: (%d) %s\n", res, curl_easy_strerror(res));
        opc = res;//OPC_SENDFAILED;
    }
    else if (httpcode >= 200 && httpcode < 300) {
        opc = handleResponse(chunk.memory, chunk.size, result);
    }
    else {
        opc = OPC_REQFAILED;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all (headerlist);

    free(chunk.memory);

    *statusCode = httpcode;
    return opc;
}

int Recognition::handleResponse(const char * resp, size_t resp_len, string & result)
{
    jsmn_parser parser;
#define TK_LEN 10
    jsmntok_t tokens[TK_LEN];

    jsmn_init(&parser);
    int r = jsmn_parse(&parser, resp, resp_len, tokens, TK_LEN);
    if (r < 0) {
        fprintf(errStream, "Failed to parse JSON in response: %d\n", r);
        return OPC_PARSEFAILED;
    }

    string signature, json;

    for (int i = 0; i < r; i++) {
        jsmntok_t tk = tokens[i];
        if (JSMN_STRING == tk.type) {
            string key(resp + tk.start, tk.end - tk.start);
            jsmntok_t tv = tokens[i+1];
            const char *vs = resp + tv.start;
            size_t vl = tv.end - tv.start;
            i++; //skip the value token

            if (0 == key.compare("json"))
                parse_json_value(vs, vl, json);
            else if (0 == key.compare("signature"))
                signature = string(vs, vl);
        }
    }
    // printf("Response result:\n%s\n-------\n", string(resp, resp_len).c_str());
    // printf("JSON result:\n%s\n-------\n", json.c_str());
    int rc = verify_with_sha256(json, signature, m_tupuPublicKey);
    if (1 != rc) {
        return OPC_VERIFYFAILED;
    }
    result = json;

    return OPC_OK;
}






static
int get_private_key_content(const string & key_path, char *buf, size_t key_size)
{
    FILE * file = NULL;

    if (NULL == (file = fopen(key_path.c_str(),"r")))
    {
        perror("open key file error");
        return -1;
    }
    fread(buf, key_size, 1, file);

    fclose(file);
    return 0;
}

static
RSA * read_private_key(char *buf)
{
    BIO *bio = NULL;
    RSA * p_rsa = NULL;
    
    //read private key from buffer
    if (NULL == (bio = BIO_new_mem_buf(buf, -1)))
    {
        fprintf(stderr, "BIO_new_mem_buf failed!\n");
    }
    else if (NULL == (p_rsa = PEM_read_bio_RSAPrivateKey(bio, NULL, NULL, NULL)))
    {
        ERR_load_crypto_strings();
        char errBuf[512];
        ERR_error_string_n(ERR_get_error(), errBuf, sizeof(errBuf));
        fprintf(stderr, "Failed to load private key [%s]\n", errBuf);
    }

    BIO_free_all(bio);

    return p_rsa;
}

static
RSA * read_public_key(const string & pubkeyStr)
{
    BIO *bio = NULL;
    RSA *rsa = NULL;
    char *chPublicKey = const_cast<char *>(pubkeyStr.c_str());
    //read public key from string
    if (NULL == (bio = BIO_new_mem_buf(chPublicKey, -1)))
    {
        fprintf(stderr, "BIO_new_mem_buf failed!\n");
        return NULL;
    }
    //get rsa struct from bio
    rsa = PEM_read_bio_RSA_PUBKEY(bio, NULL, NULL, NULL);
    if (!rsa)
    {
        ERR_load_crypto_strings();
        char errBuf[512];
        ERR_error_string_n(ERR_get_error(), errBuf, sizeof(errBuf));
        fprintf(stderr, "Failed to load public key [%s]\n", errBuf);

        BIO_free_all(bio);
        return NULL;
    }

    BIO_free_all(bio);

    return rsa;
}

static
RSA * read_tupu_pubkey()
{
    string pubkeyStr = "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDyZneSY2eGnhKrArxaT6zswVH9\n"
        "/EKz+CLD+38kJigWj5UaRB6dDUK9BR6YIv0M9vVQZED2650tVhS3BeX04vEFhThn\n"
        "NrJguVPidufFpEh3AgdYDzOQxi06AN+CGzOXPaigTurBxZDIbdU+zmtr6a8bIBBj\n"
        "WQ4v2JR/BA6gVHV5TwIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
    return read_public_key(pubkeyStr);
}

static
void to_sha256(const string & message, unsigned char *digest)
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, message.c_str(), message.size());
    SHA256_Final(digest, &c);
    OPENSSL_cleanse(&c, sizeof(c));
}

static
int sign_with_sha256(const string & message, RSA * p_rsa, string & result)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    to_sha256(message, digest);

    unsigned char * sig = NULL;
    unsigned int sig_len = 0;
    int rsa_len = RSA_size(p_rsa);
 
    sig = (unsigned char *)malloc(rsa_len);
    memset(sig, 0, rsa_len);
 
    int rc = RSA_sign(NID_sha256, digest, sizeof digest, sig, &sig_len, p_rsa);
    if (1 == rc)
    {
        result = base64_encode(sig, sig_len);
    }
    
    free(sig);

    return rc;
}

static
int verify_with_sha256(const string & message, const string & signature, RSA * p_rsa)
{
    unsigned char * sig = NULL;
    size_t sig_len = 0;
    base64_decode(signature, (void**)&sig, &sig_len);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    to_sha256(message, digest);
 
    int rc = RSA_verify(NID_sha256, digest, sizeof digest, sig, sig_len, p_rsa);
    if (1 != rc) {
        char errBuf[512];
        ERR_error_string_n(ERR_get_error(), errBuf, sizeof(errBuf));
        fprintf(errStream, "Verification failed [%s]\n", errBuf);
    }

    free(sig);

    return rc;
}

static
void random_str(size_t len, char *output)
{
    unsigned char *a = (unsigned char*)malloc(len);
    if (RAND_bytes(a, len))
    {
        for (size_t i = 0; i < len; i++)
        {
            sprintf(output + i * 2, "%02x", a[i]);
        }
    
    }
    free(a);
}

static
void parse_json_value(const char *src, size_t len, string & result)
{
    char *buf = (char*)malloc(len+1);
    memset(buf, 0, len+1);
    size_t j = 0;
    size_t lasti = len - 1;
    for (size_t i = 0; i < len; i++) {
        //if ((i == 0 || src[i-1] != '\\') && src[i] == '\\' && src[i+1] == '"')
        char c = src[i];
        if (src[i] == '\\' && i < lasti)
        {
            char nx = src[i+1];
            if (nx == 'n' || nx == 'r' || nx == 't' || nx == '\\' || nx == '\'' || nx == '"' || nx == '?')
            {
                c = nx;
                i++;
            }
        }
        buf[j++] = c;
    }
    result = string(buf);
    free(buf);
}

static
size_t write_memory(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    MemChunk *mem = (MemChunk *)userp;

    mem->memory = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if(mem->memory == NULL)
    {
        /* out of memory! */ 
        fprintf(errStream, "not enough memory (realloc returned NULL)\n");
        return 0;
    }
 
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
 
    return realsize;
}

static
void compose_form(struct curl_httppost ** post, const vector<TImage> & images,
    const string & timestamp, const string & nonce,
    const string & signature, const string & uid)
{
    struct curl_httppost* last = NULL;
    curl_formadd(post, &last,
               CURLFORM_COPYNAME, "timestamp",
               CURLFORM_COPYCONTENTS, timestamp.c_str(),
               CURLFORM_END);
    curl_formadd(post, &last,
               CURLFORM_COPYNAME, "nonce",
               CURLFORM_COPYCONTENTS, nonce.c_str(),
               CURLFORM_END);
    curl_formadd(post, &last,
               CURLFORM_COPYNAME, "signature",
               CURLFORM_COPYCONTENTS, signature.c_str(),
               CURLFORM_END);

    if (!uid.empty()) {
        curl_formadd(post, &last,
                   CURLFORM_COPYNAME, "uid",
                   CURLFORM_COPYCONTENTS, uid.c_str(),
                   CURLFORM_END);
    }

    unsigned int i = 0;
    do
    {
        struct curl_httppost* prev = last;
        const TImage & img = images[i];
        if (!img.path().empty())
        {
            //Upload file with file path
            curl_formadd(post, &last,
               CURLFORM_COPYNAME, "image",
               CURLFORM_FILE, img.path().c_str(),
               CURLFORM_END);
        }
        else if (!img.url().empty())
        {
            //File URL
            curl_formadd(post, &last,
               CURLFORM_COPYNAME, "image",
               CURLFORM_COPYCONTENTS, img.url().c_str(),
               CURLFORM_END);
        }
        else if (img.buffer())
        {
            //CURLFORM_CONTENTTYPE, "application/octet-stream",
            /* Add a buffer to upload */
            curl_formadd(post, &last,
                CURLFORM_COPYNAME, "image",
                CURLFORM_BUFFER, img.filename().c_str(),
                CURLFORM_BUFFERPTR, img.buffer(),
                CURLFORM_BUFFERLENGTH, img.bufferLength(),
                CURLFORM_END);
        }

        if (prev != last && !img.tag().empty())
        {
            curl_formadd(post, &last,
               CURLFORM_COPYNAME, "tag",
               CURLFORM_COPYCONTENTS, img.tag().c_str(),
               CURLFORM_END);
        }
    } while (++i < images.size());
}



#if VERB_LEV >= 2
static
void dump(const char *text, FILE *stream, unsigned char *ptr, size_t size)
{
    size_t i;
    size_t c;
    unsigned int width = 0x10;

    fprintf(stream, "%s, %10.10ld bytes (0x%8.8lx)\n", text, (long)size, (long)size);

    for (i = 0; i < size; i += width)
    {
        fprintf(stream, "%4.4lx: ", (long)i);

        /* show hex to the left */
        for(c = 0; c < width; c++)
        {
            if(i+c < size)
                fprintf(stream, "%02x ", ptr[i+c]);
            else
                fputs("   ", stream);
        }

        /* show data on the right */
        for (c = 0; (c < width) && (i+c < size); c++)
        {
            char x = (ptr[i+c] >= 0x20 && ptr[i+c] < 0x80) ? ptr[i+c] : '.';
            fputc(x, stream);
        }

        fputc('\n', stream); /* newline */
    }
}

static
int debug_trace(CURL *curl, curl_infotype type, char *data, size_t size, void *userptr)
{
    const char *text;
    (void)curl; /* prevent compiler warning */

    switch (type) {
        case CURLINFO_TEXT:
            fprintf(stderr, "== Info: %s", data);
        default: /* in case a new one is introduced to shock us */
            return 0;

        case CURLINFO_HEADER_OUT:
            text = "=> Send header";
            break;
        case CURLINFO_DATA_OUT:
            text = "=> Send data";
            break;
        case CURLINFO_SSL_DATA_OUT:
            text = "=> Send SSL data";
            break;
        case CURLINFO_HEADER_IN:
            text = "<= Recv header";
            break;
        case CURLINFO_DATA_IN:
            text = "<= Recv data";
            break;
        case CURLINFO_SSL_DATA_IN:
            text = "<= Recv SSL data";
            break;
    }
 
  dump(text, stderr, (unsigned char *)data, size);
  return 0;
}
#endif //VERB_LEV >= 2


} //namespace TUPU
