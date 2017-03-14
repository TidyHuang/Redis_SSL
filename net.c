/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <limits.h>
#include <stdlib.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "net.h"
#include "sds.h"

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

static void redisContextCloseFd(redisContext *c) {
    if (c && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    int errorno = errno;  /* snprintf() may change errno */
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    __redis_strerror_r(errorno, (char *)(buf + len), sizeof(buf) - len);
    __redisSetError(c,type,buf);
}

static int redisSetReuseAddr(redisContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int redisCreateSocket(redisContext *c, int type) {
    int s;
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (redisSetReuseAddr(c) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

static int redisSetBlocking(redisContext *c, int blocking) {
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisKeepAlive(redisContext *c, int interval) {
    int val = 1;
    int fd = c->fd;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval;

#ifdef _OSX
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#endif
#endif

    return REDIS_OK;
}

struct timeval subtractTimeval( struct timeval a, struct timeval b )
{
    struct timeval difference;

    difference.tv_sec = a.tv_sec - b.tv_sec;
    difference.tv_usec = a.tv_usec - b.tv_usec;

    if (a.tv_usec < b.tv_usec) {
        b.tv_sec -= 1L;
        b.tv_usec += 1000000L;
    }

    return difference;
}

int compareTimeval( struct timeval a, struct timeval b )
{
    if (a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_usec > b.tv_usec) ) {
        return 1;
    } else if( a.tv_sec == b.tv_sec && a.tv_usec == b.tv_usec ) {
        return 0;
    }

    return -1;
}

int subtractTimevalMicroSecond(struct timeval a, struct timeval b)
{
    struct timeval diff = subtractTimeval(a, b);
    return diff.tv_sec*1000000 + diff.tv_usec;
}

static int redisSetTcpNoDelay(redisContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisContextTimeoutMsec(redisContext *c, long *result)
{
    const struct timeval *timeout = c->timeout;
    long msec = -1;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            *result = msec;
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return REDIS_OK;
}

static int redisContextWaitReady(redisContext *c, long msec) {
    struct pollfd   wfd[1];

    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            redisContextCloseFd(c);
            return REDIS_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            redisContextCloseFd(c);
            return REDIS_ERR;
        }

        if (redisCheckSocketError(c) != REDIS_OK)
            return REDIS_ERR;

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    redisContextCloseFd(c);
    return REDIS_ERR;
}

int redisCheckSocketError(redisContext *c) {
    int err = 0;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        return REDIS_ERR;
    }

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    int s, rv, n;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & REDIS_BLOCK);
    int reuseaddr = (c->flags & REDIS_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = REDIS_CONN_TCP;
    c->tcp.port = port;

    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but atleast it works and doesn't leak memory.
     **/
    if (c->tcp.host != addr) {
        if (c->tcp.host)
            free(c->tcp.host);

        c->tcp.host = strdup(addr);
    }

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_IO, "Invalid timeout specified");
        goto error;
    }

    if (source_addr == NULL) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = strdup(source_addr);
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Try with IPv6 if no IPv4 address was found. We do it in this order since
     * in a Redis client you can't afford to test if you have IPv6 connectivity
     * as this would add latency to every connect. Otherwise a more sensible
     * route could be: Use IPv6 if both addresses are available and there is IPv6
     * connectivity. */
    if ((rv = getaddrinfo(c->tcp.host,_port,&hints,&servinfo)) != 0) {
         hints.ai_family = AF_INET6;
         if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
            __redisSetError(c,REDIS_ERR_OTHER,gai_strerror(rv));
            return REDIS_ERR;
        }
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
addrretry:
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        c->fd = s;
        if (redisSetBlocking(c,0) != REDIS_OK)
            goto error;
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }

            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*) &n,
                               sizeof(n)) < 0) {
                    goto error;
                }
            }

            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                redisContextCloseFd(c);
                continue;
            } else if (errno == EINPROGRESS && !blocking) {
                /* This is ok. */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                if (++reuses >= REDIS_CONNECT_RETRIES) {
                    goto error;
                } else {
                    redisContextCloseFd(c);
                    goto addrretry;
                }
            } else {
                if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                    goto error;
            }
        }
        if (blocking && redisSetBlocking(c,1) != REDIS_OK)
            goto error;
        if (redisSetTcpNoDelay(c) != REDIS_OK)
            goto error;

        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

error:
    rv = REDIS_ERR;
end:
    freeaddrinfo(servinfo);
    return rv;  // Need to return REDIS_OK if alright
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _redisContextConnectTcp(c, addr, port, timeout, NULL);
}

int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

#define FILL_SSL_ERR_MSG(c, err_type, err_msg)\
        char errorbuf[1024]; __redisSetError(c, err_type, err_msg);\
        ERR_error_string(sizeof(errorbuf), errorbuf);\
        __redisSetError(c, err_type, err_msg)

int init_SSL(redisContext *c, char *certfile, char *keyfile, char *CAfile, char *certdir)
{
    // Set up a SSL_CTX object, which will tell our BIO object how to do its work
    setupSSL();

    SSL_CTX *ctx = SSL_CTX_new(TLSv1_client_method());
    if (ctx == NULL) {
        return REDIS_ERR;
    }

    c->ssl.ctx = ctx;
    if (certfile) {
        if (SSL_CTX_use_certificate_file(ctx, certfile, SSL_FILETYPE_PEM) <= 0) {
            FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error: Load client cert\n");
            return REDIS_ERR;
        }

        if (!keyfile) keyfile = certfile;
        if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) <= 0) {
            FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error: Load client private key\n");
            return REDIS_ERR;
        }
        if (SSL_CTX_check_private_key(ctx) <= 0) {
            FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error:  Check cert/key pair\n");
            return REDIS_ERR;
        }
        //SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        //SSL_CTX_set_verify_depth(ctx, 4);
    }

    int status = SSL_CTX_load_verify_locations(ctx, CAfile, certdir);

    return REDIS_OK;
}

/*  Return -1 on hard error (abort), 0 on timeout, >= 1 on successful wakeup */
static int
_BIO_wait(BIO *cbio, int msecs)
{
    if (!BIO_should_retry(cbio)) {
        return (-1);
    }

    struct pollfd pfd;
    BIO_get_fd(cbio, &pfd.fd);
    pfd.events = 0;
    pfd.revents = 0;

    if (BIO_should_io_special(cbio)) {
        pfd.events = POLLOUT | POLLWRBAND;
    } else if (BIO_should_read(cbio)) {
        pfd.events = POLLIN | POLLPRI | POLLRDBAND;
    } else if (BIO_should_write(cbio)) {
        pfd.events = POLLOUT | POLLWRBAND;
    } else {
        return (-1);
    }

    if (msecs < 0) {
        /*  POSIX requires -1 for "no timeout" although some libcs
         *                 accept any negative value. */
        msecs = -1;
    }
    int result = poll(&pfd, 1, msecs);

    /*  Timeout or poll internal error */
    if (result <= 0) {
        return (result);
    }

    /*  Return 1 if the event was not an error */
    return (pfd.revents & pfd.events ? 1 : -1);
}

int redisContextConnectSSL(redisContext *c, const char* addr, int port, struct timeval *timeout,
        char *certfile, char *keyfile,
        char *CAfile, char *certdir) {

    printf("certfile %s, keyfile %s, cafile %s, cerdir %s\n", certfile, keyfile, CAfile, certdir);
    struct timeval  start_time;
    int             has_timeout = 0;
    int             is_nonblocking = 0;

    c->ssl.sd = -1;
    c->ssl.ctx = NULL;
    c->ssl.ssl = NULL;
    c->ssl.bio = NULL;

    if (init_SSL(c, certfile, keyfile, CAfile, certdir) != REDIS_OK) {
        cleanupSSL(&c->ssl);
        return REDIS_ERR;
    }

    // Create our BIO object for SSL connections.
    BIO *bio = BIO_new_ssl_connect(ctx);
    if (bio == NULL) {
        FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error:  Create BIO fail");
        // We need to free up the SSL_CTX before we leave.
        cleanupSSL(&c->ssl);
        return REDIS_ERR;
    }
    // Create a SSL object pointer, which our BIO object will provide.
    SSL *ssl = NULL;
    BIO_get_ssl(bio, &ssl);
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

    c->ssl.ssl = ssl;
    c->ssl.bio = bio;

    // Set the SSL to automatically retry on failure.
    char *connect_str = (char*)calloc(1, strlen(addr) + 10);
    sprintf(connect_str, "%s:%d", addr, port);

    c->ssl.conn_str = connect_str;
    BIO_set_conn_hostname(bio, connect_str);

    if ((c->flags & REDIS_BLOCK) == 0) {
        is_nonblocking = 1;
        BIO_set_nbio(bio, 1);
    }

    if (NULL != timeout) {
        BIO_set_nbio(bio, 1);
        has_timeout = 1;
        gettimeofday(&start_time, NULL);
    }

    while(1) {
        struct timeval  cur_time, elapsed_time;
        int time_left;
        if (has_timeout) {
            gettimeofday(&cur_time, NULL);
            elapsed_time = subtractTimeval( cur_time, start_time );

            if (compareTimeval( elapsed_time, *timeout) > 0) {
                FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error: Connection timed out.");
                cleanupSSL( &(c->ssl) );
                return REDIS_ERR;
            }
        }

        // Same as before, try to connect.
        if (BIO_do_connect(bio) <= 0 ) {
            if (has_timeout) {
                time_left = subtractTimevalMicroSecond(*timeout, elapsed_time);
                if (_BIO_wait(bio, time_left) != 1) {
                    FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error: Failed to connect.");
                    cleanupSSL( &(c->ssl) );
                    return REDIS_ERR;
                }
            } else {
                FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER,"SSL Error: Failed to connect");
                cleanupSSL(&(c->ssl));
                return REDIS_ERR;
            }
        } else {
            // connect is done...
            break;
        }
    }

    while(1) {
        struct timeval  cur_time, elapsed_time;
        int time_left;

        if (has_timeout) {
            gettimeofday(&cur_time, NULL);
            elapsed_time = subtractTimeval( cur_time, start_time );
            if (compareTimeval(elapsed_time, *timeout) > 0) {
                FILL_SSL_ERR_MSG(c, REDIS_ERR_OTHER, "SSL Error: Connection time out before handshake");
                cleanupSSL( &(c->ssl) );
                return REDIS_ERR;
            }
        }

        // Now we need to do the SSL handshake, so we can communicate.
        if (BIO_do_handshake(bio) <= 0) {
            if (has_timeout) {
                time_left = subtractTimevalMicroSecond( *timeout, elapsed_time);
                if (_BIO_wait(bio, time_left) != 1) {
                    FILL_SSL_ERR_MSG(c,REDIS_ERR_OTHER,"SSL Error: handshake failure");
                    cleanupSSL( &(c->ssl) );
                    return REDIS_ERR;
                }
            }
        } else {
            // handshake is done...
            break;
        }
    }

    long verify_result = SSL_get_verify_result(ssl);
    if( verify_result == X509_V_OK) {
        X509* peerCertificate = SSL_get_peer_certificate(ssl);

        char commonName [512];
        X509_NAME * name = X509_get_subject_name(peerCertificate);
        X509_NAME_get_text_by_NID(name, NID_commonName, commonName, sizeof(commonName));

        // TODO: Add a redis config parameter for the common name to check for.
        //       Since Redis connections are generally done with an IP address directly,
        //       we can't just assume that the name we get on the addr is an actual name.
        //
        //    if(strcasecmp(commonName, "BradBroerman") != 0) {
        //      __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Error validating cert common name.\n\n" );
        //      cleanupSSL( &(c->ssl) );
        //      return REDIS_ERR;

    } else {
        FILL_SSL_ERR_MSG(c,REDIS_ERR_OTHER,"SSL Error: Error retrieving peer certificate.\n" );
        cleanupSSL( &(c->ssl) );
        return REDIS_ERR;
    }

    printf("verify done");

    BIO_set_nbio(bio,is_nonblocking);

    FILL_SSL_ERR_MSG(c, REDIS_OK, "SSL Connection finished");

    return REDIS_OK;
}

void cleanupSSL(SSLConnection *conn) {

    if (!conn) return;

    if (conn->ctx) {
        SSL_CTX_free(conn->ctx);
        conn->ctx = NULL;
    }

    if (conn->bio) {
        BIO_free_all(conn->bio);
        conn->bio = NULL;
    }

    if (conn->conn_str) {
        free(conn->conn_str);
        conn->conn_str = NULL;
    }

    return;
}

void setupSSL(void) {
    CRYPTO_malloc_init();
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    SSL_library_init();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();
}

int redisContextConnectUnix(redisContext *c, const char *path, const struct timeval *timeout) {
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un sa;
    long timeout_msec = -1;

    if (redisCreateSocket(c,AF_LOCAL) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,0) != REDIS_OK)
        return REDIS_ERR;

    c->connection_type = REDIS_CONN_UNIX;
    if (c->unix_sock.path != path)
        c->unix_sock.path = strdup(path);

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    if (redisContextTimeoutMsec(c,&timeout_msec) != REDIS_OK)
        return REDIS_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && redisSetBlocking(c,1) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
