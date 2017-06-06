/* Copyright (c) 2017 Arrow Electronics, Inc.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Apache License 2.0
 * which accompanies this distribution, and is available at
 * http://apache.org/licenses/LICENSE-2.0
 * Contributors: Arrow Electronics, Inc.
 */

#define MODULE_NAME "HTTP_Client"

#include "http/client.h"
#include <config.h>
#include <debug.h>
#include <bsd/socket.h>
#include <time/time.h>
#include <arrow/mem.h>
#if defined(_ARIS_)
# if defined(ETH_MODE)
#  include "nx_api.h"
# endif
#elif defined(__XCC__)
#define WOLFSSL SSL
#define WOLFSSL_CTX SSL_CTX
#define wolfSSL_write qcom_SSL_write
#define wolfSSL_read qcom_SSL_read
#define IPPROTO_TCP 0
#define SSL_SUCCESS 0
#endif

#include <ssl/ssl.h>

#if !defined(MAX_BUFFER_SIZE)
#define MAX_BUFFER_SIZE 1024
#endif

#define CHUNK_SIZE 256

#define CHECK_CONN_ERR(ret) \
    if ( ret < 0 ) { \
      DBG("Connection error (%d)", ret); \
      return ret; \
    }

#define PRTCL_ERR() \
  { \
    DBG("Protocol error"); \
    return -1; \
  }

#define client_send(buf, size, cli) (*(cli->_w_func))((uint8_t*)(buf), (uint16_t)(size), (cli))
#define client_recv(buf, size, cli) (*(cli->_r_func))((uint8_t*)(buf), (uint16_t)(size), (cli))

static char http_buffer[CHUNK_SIZE];

static int simple_read(uint8_t *buf, uint16_t len, void *c) {
    http_client_t *cli = (http_client_t *)c;
    int ret;
    ret = recv(cli->sock, (char*)buf, (int)len, 0);
//    if (ret > 0) buf[ret] = 0x00;
    HTTP_DBG("%d|%s|", ret, buf);
    return ret;
}

static int simple_write(uint8_t *buf, uint16_t len, void *c) {
    http_client_t *cli = (http_client_t *)c;
    if ( !len ) len = (uint16_t)strlen((char*)buf);
    HTTP_DBG("%d|%s|", len, buf);
    return send(cli->sock, (char*)buf, (int)len, 0);
}

static int ssl_read(uint8_t *buf, uint16_t len, void *c) {
    http_client_t *cli = (http_client_t *)c;
    int ret = ssl_recv(cli->sock, (char*)buf, len);//wolfSSL_read(cli->ssl, buf, (int)len);
//    if (ret > 0) buf[ret] = 0x00;
    HTTP_DBG("[%d]{%s}", ret, buf);
    return ret;
}

static int ssl_write(uint8_t *buf, uint16_t len, void *c) {
    http_client_t *cli = (http_client_t *)c;
    if ( !len && buf ) len = (uint16_t)strlen((char*)buf);
    HTTP_DBG("[%d]|%s|",len, buf);
    int ret = ssl_send(cli->sock, (char*)buf, len);//wolfSSL_write(cli->ssl, buf, (int)len);
    return ret;
}

void http_client_init(http_client_t *cli, int newsession) {
	cli->response_code = 0;
	if ( newsession ) {
	    cli->sock = -1;
		cli->timeout = 5000;
		cli->_r_func = simple_read;
		cli->_w_func = simple_write;
	}
}

void http_client_free(http_client_t *cli) {
#if defined(__XCC__)
  if (cli->ssl) {
    qcom_SSL_shutdown(cli->ssl);
    qcom_SSL_ctx_free(cli->ctx);
  }
#endif
    ssl_close(cli->sock);
    if ( cli->sock >= 0 ) soc_close(cli->sock);
}

#define HTTP_VERS " HTTP/1.1\r\n"
static int send_start(http_client_t *cli, http_request_t *req) {
    char *buf = http_buffer;
    int ret;
    ret = snprintf(buf, CHUNK_SIZE-1, "%s %s", P_VALUE(req->meth), P_VALUE(req->uri));
    buf[ret] = '\0';
    if ( req->query ) {
        char queryString[CHUNK_SIZE];
        strcpy(queryString, "?");
        http_query_t *query = req->query;
        while ( query ) {
          if ( (int)strlen(P_VALUE(query->key)) + (int)strlen(P_VALUE(query->value)) + 3 < (int)CHUNK_SIZE ) break;
            strcat(queryString, P_VALUE(query->key));
            strcat(queryString, "=");
            strcat(queryString, P_VALUE(query->value));
            if ( CHUNK_SIZE - strlen(buf) - sizeof(HTTP_VERS)-1 < strlen(queryString) ) break;
            strcat(buf, queryString);
            strcpy(queryString, "&");
            query = query->next;
        }
    }
    strcat(buf, HTTP_VERS);
    if ( (ret = client_send(buf, 0, cli)) < 0 ) {
        return ret;
    }
    ret = snprintf(buf, CHUNK_SIZE-1, "Host: %s:%d\r\n", P_VALUE(req->host), req->port);
    buf[ret] = '\0';
    if ( (ret = client_send(buf, 0, cli)) < 0 ) {
        return ret;
    }
    return 0;
}

static int send_header(http_client_t *cli, http_request_t *req) {
    char *buf = http_buffer;
    int ret;
    if ( !IS_EMPTY(req->payload.buf) && req->payload.size > 0 ) {
        if ( req->is_chunked ) {
            ret = client_send("Transfer-Encoding: chunked\r\n", 0, cli);
        } else {
            ret = snprintf(buf, sizeof(http_buffer)-1, "Content-Length: %lu\r\n", (long unsigned int)req->payload.size);
            if ( ret < 0 ) return ret;
            buf[ret] = '\0';
            ret = client_send(buf, ret, cli);
        }
        if ( ret < 0 ) return ret;
        ret = snprintf(buf, sizeof(http_buffer)-1, "Content-Type: %s\r\n", P_VALUE(req->content_type.value));
        if ( ret < 0 ) return ret;
        buf[ret] = '\0';
        if ( (ret = client_send(buf, ret, cli)) < 0 ) return ret;
    }

    http_header_t *head = req->header;
    while( head ) {
      ret = snprintf(buf, sizeof(http_buffer)-1, "%s: %s\r\n", P_VALUE(head->key), P_VALUE(head->value));
    	if ( ret < 0 ) return ret;
    	buf[ret] = '\0';
        if ( (ret = client_send(buf, ret, cli)) < 0 ) return ret;
        head = head->next;
    }
    return client_send((uint8_t*)"\r\n", 2, cli);
}

static int send_payload(http_client_t *cli, http_request_t *req) {
    if ( !IS_EMPTY(req->payload.buf) && req->payload.size > 0 ) {
        if ( req->is_chunked ) {
            char *data = P_VALUE(req->payload.buf);
            int len = (int)req->payload.size;
            int trData = 0;
            while ( len >= 0 ) {
                char buf[6];
                int chunk = len > CHUNK_SIZE ? CHUNK_SIZE : len;
                int ret = sprintf(buf, "%02X\r\n", chunk);
                client_send(buf, ret, cli);
                if ( chunk ) client_send(data + trData, chunk, cli);
                trData += chunk;
                len -= chunk;
                client_send("\r\n", 0, cli);
                if ( !chunk ) break;
            }
            return 0;
        } else {
          int ret = client_send(P_VALUE(req->payload.buf), req->payload.size, cli);
          return ret;
        }
    }
    return -1;
}

static int receive_response(http_client_t *cli, http_response_t *res, char *buf, uint32_t *len) {
    int ret;
    if ( (ret = client_recv(buf, 20, cli)) < 0 ) return ret;
    *len = (uint32_t)ret;
    buf[*len] = 0;
    char* crlfPtr = strstr(buf, "\r\n");

    while( crlfPtr == NULL ) {
        if( *len < CHUNK_SIZE - 1 ) {
            if ( (ret = client_recv(buf + *len, 1, cli)) < 0 ) return ret;
            *len += ret;
            buf[*len] = 0;
        } else {
            return -1;
        }
        crlfPtr = strstr(buf, "\r\n");
    }

    int crlfPos = crlfPtr - buf;
    if ( crlfPos < 0 && crlfPos > (int)(*len) )
        return -1;
    buf[crlfPos] = '\0';
    DBG("resp: {%s}", buf);

    if( sscanf(buf, "HTTP/1.1 %4d", &res->m_httpResponseCode) != 1 ) {
        DBG("Not a correct HTTP answer : %s\n", buf);
        return -1;
    }

    if ( *len < (uint32_t)crlfPos + 2 ) {
        DBG("receive_response memmove warning [%08x] %d, %d", (int)buf, crlfPos, *len);
    }
    memmove(buf, buf+crlfPos+2, *len - (uint32_t)(crlfPos + 2) + 1 ); //Be sure to move NULL-terminating char as well
    *len -= (uint32_t)(crlfPos + 2);

//    if( (res->m_httpResponseCode < 200) || (res->m_httpResponseCode >= 300) ) {
        //Did not return a 2xx code; TODO fetch headers/(&data?) anyway and implement a mean of writing/reading headers
        DBG("Response code %d", res->m_httpResponseCode);
//        HTTP_DBG("Protocol error");
//        return -1;
//    }
    cli->response_code = res->m_httpResponseCode;
    return 0;
}

int http_client_do(http_client_t *cli, http_request_t *req, http_response_t *res) {
    int ret;
    memset(res, 0x00, sizeof(http_response_t));
    if ( cli->sock < 0 ) {
    	ret = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    	if ( ret < 0 ) return ret;
    	cli->sock = ret;
    	struct sockaddr_in serv;
    	struct hostent *serv_resolve;
      serv_resolve = gethostbyname(P_VALUE(req->host));
    	if (serv_resolve == NULL) {
    		DBG("ERROR, no such host");
    		return -1;
    	}
    	memset(&serv, 0, sizeof(serv));
    	serv.sin_family = PF_INET;
    	bcopy((char *)serv_resolve->h_addr,
    			(char *)&serv.sin_addr.s_addr,
				(uint32_t)serv_resolve->h_length);
    	serv.sin_port = htons(req->port);

    	struct timeval tv;
    	tv.tv_sec =     (time_t)        ( cli->timeout / 1000 );
    	tv.tv_usec =    (suseconds_t)   (( cli->timeout % 1000 ) * 1000);
    	setsockopt(cli->sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));

    	ret = connect(cli->sock, (struct sockaddr*)&serv, sizeof(serv));
    	if ( ret < 0 ) {
    		DBG("connect fail");
    		soc_close(cli->sock);
    		cli->sock = -1;
    		return -1;
    	}
    	HTTP_DBG("connect done");
    	if ( req->is_cipher ) {
    		if ( ssl_connect(cli->sock) < 0 ) {
    			HTTP_DBG("SSL connect fail");
    			ssl_close(cli->sock);
    			soc_close(cli->sock);
    			cli->sock = -1;
    			return -1;
    		}
    		cli->_r_func = ssl_read;
    		cli->_w_func = ssl_write;
    	} else {
    		cli->_r_func = simple_read;
    		cli->_w_func = simple_write;
    	}
    }

    if ( send_start(cli, req) < 0 ) {
        DBG("send start fail");
        return -1;
    }

    if ( send_header(cli, req) < 0 ) {
        DBG("send header fail");
        return -1;
    }

    if ( !IS_EMPTY(req->payload.buf) ) {
        if ( send_payload(cli, req) < 0 ) {
            DBG("send payload fail");
            return -1;
        }
    }

    HTTP_DBG("Receiving response");

    uint32_t trfLen;
    ret = receive_response(cli, res, http_buffer, &trfLen);
    if ( ret < 0 ) {
        DBG("Connection error (%d)", ret);
        return -1;
    }

    HTTP_DBG("Reading headers %d", trfLen);
    char *crlfPtr;
    int crlfPos;
    res->header = NULL;
    memset(&res->content_type, 0x0, sizeof(http_header_t));
    memset(&res->payload, 0x0, sizeof(http_payload_t));
    res->is_chunked = 0;

    int recvContentLength = -1;
    //Now get headers
    char *buf = http_buffer;
    buf[trfLen] = 0;

    while( 1 ) {
        crlfPtr = strstr(buf, "\r\n");
        if(crlfPtr == NULL) {
            if( trfLen < CHUNK_SIZE - 1 ) {
                uint32_t newTrfLen;
                ret = client_recv(buf + trfLen, 40, cli);
                CHECK_CONN_ERR(ret);
                newTrfLen = (uint32_t)ret;
                trfLen += newTrfLen;
                buf[trfLen] = 0;
                HTTP_DBG("Read %d chars; In buf: [%s]", newTrfLen, buf);
                continue;
            } else {
                PRTCL_ERR();
            }
        }

        crlfPos = crlfPtr - buf;

        if(crlfPos == 0) {
            HTTP_DBG("Headers read done");
            memmove(buf, &buf[2], trfLen - 2 + 1); //Be sure to move NULL-terminating char as well
            trfLen -= 2;
            break;
        } else if ( crlfPos < 0 ) CHECK_CONN_ERR(-1);

        buf[crlfPos] = '\0';
        char key[CHUNK_SIZE];
        char value[CHUNK_SIZE];

        int n = sscanf(buf, "%256[^:]: %256[^\r\n]", key, value);
        if ( n == 2 ) {
            HTTP_DBG("Read header : %s: %s", key, value);
            if( !strcmp(key, "Content-Length") ) {
                sscanf(value, "%8d", &recvContentLength);
            } else if( !strcmp(key, "Transfer-Encoding") ) {
                if( !strcmp(value, "Chunked") || !strcmp(value, "chunked") )
                    res->is_chunked = 1;
            } else if( !strcmp(key, "Content-Type") ) {
                http_response_set_content_type(res, property(value, is_stack));
            } else {
#if defined(HTTP_PARSE_HEADER)
                http_response_add_header(res,
                                         p_stack(key),
                                         p_stack(value);
#endif
            }
            memmove(buf, crlfPtr+2, trfLen - (uint32_t)(crlfPos + 2) + 1);
            trfLen -= (uint32_t)(crlfPos + 2);
        } else {
            DBG("Could not parse header");
            PRTCL_ERR();
        }
    }

    uint32_t chunk_len;
    HTTP_DBG("get payload form buf: %d", trfLen);
    HTTP_DBG("get payload form buf: [%s]", buf);
    do {
        if ( res->is_chunked ) {
            while( (crlfPtr = strstr(buf, "\r\n")) == NULL ) {
                if ( trfLen + 10 > CHUNK_SIZE ) {
                    memmove(buf, buf+10, trfLen-10);
                    trfLen -= 10;
                }
                uint32_t newTrfLen = 0;
                ret = client_recv(buf+trfLen, 10, cli);
                if ( ret > 0 ) newTrfLen = (uint32_t)ret;
                trfLen += newTrfLen;
                buf[trfLen] = 0;
            }
            ret = sscanf(buf, "%4x\r\n", (unsigned int*)&chunk_len);
            if ( ret != 1 ) {
                memmove(buf, crlfPtr+2, trfLen - (uint32_t)crlfPos);
                trfLen -= (uint32_t)crlfPos;
                chunk_len = 0;
                return -1; // fail
            }
            HTTP_DBG("detect chunk %d, %d", chunk_len, ret);
            crlfPtr = strstr(buf, "\r\n");
            crlfPos = crlfPtr + 2 - buf;
            memmove(buf, crlfPtr+2, trfLen - (uint32_t)crlfPos);
            trfLen -= (uint32_t)crlfPos;
        } else {
            chunk_len = MAX_BUFFER_SIZE - trfLen;
        }
        if ( !chunk_len ) break;
        while ( chunk_len ) {
            uint32_t need_to_read = CHUNK_SIZE-10;
            if ( (int)chunk_len < CHUNK_SIZE-10) need_to_read = chunk_len;
            HTTP_DBG("need to read %d/%d", need_to_read, trfLen);
            while ( (int)trfLen < (int)need_to_read ) {
                uint32_t newTrfLen = 0;
                HTTP_DBG("get chunk add %d", need_to_read-trfLen);
                ret = client_recv(buf+trfLen, need_to_read-trfLen, cli);
                if ( ret >= 0 ) newTrfLen = (uint32_t)ret;
                else { // ret < 0 - error
                    need_to_read = trfLen;
                    chunk_len = need_to_read;
                    newTrfLen = 0;
                }
                trfLen += newTrfLen;
                buf[trfLen] = 0;
            }
            HTTP_DBG("add payload{%d:%s}", need_to_read, buf);
            http_response_add_payload(res, p_stack(buf), need_to_read);
            if ( trfLen == need_to_read ) {
                trfLen = 0;
                buf[0] = 0;
            } else {
                memmove(buf, buf + need_to_read, trfLen - need_to_read);
                trfLen -= need_to_read;
            }
            chunk_len -= need_to_read;
        }
    } while(1);

    HTTP_DBG("body{%s}", P_VALUE(res->payload.buf));
    return 0;
}
