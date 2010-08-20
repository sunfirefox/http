/*
    rx.c -- Http receiver. Parses http requests and client responses.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void addMatchEtag(HttpConn *conn, char *etag);
static int getChunkPacketSize(HttpConn *conn, MprBuf *buf);
static char *getToken(HttpConn *conn, cchar *delim);
static bool parseAuthenticate(HttpConn *conn, char *authDetails);
static void parseHeaders(HttpConn *conn, HttpPacket *packet);
static bool parseIncoming(HttpConn *conn, HttpPacket *packet);
static bool parseRange(HttpConn *conn, char *value);
static void parseRequestLine(HttpConn *conn, HttpPacket *packet);
static void parseResponseLine(HttpConn *conn, HttpPacket *packet);
static bool processCompletion(HttpConn *conn);
static bool processContent(HttpConn *conn, HttpPacket *packet);
static bool processParsed(HttpConn *conn);
static bool processRunning(HttpConn *conn);

/*********************************** Code *************************************/

HttpRx *httpCreateRx(HttpConn *conn)
{
    HttpRx      *rx;

#if FUTURE
    MprHeap     *arena;
    /*  
        Create a request memory arena. From this arena, are all allocations made for this entire request.
        Arenas are scalable, non-thread-safe virtual memory blocks.
     */
    arena  = mprAllocArena(conn, "request", HTTP_REC_MEM, 0, NULL);
    if (arena == 0) {
        return 0;
    }
    rx = mprAllocObjZeroed(arena, HttpRx);
    rx->arena = arena;
    if (rx == 0) {
        return 0;
    }
#else
    rx = mprAllocObjZeroed(conn, HttpRx);
    if (rx == 0) {
        return 0;
    }
#endif
    rx->conn = conn;
    rx->length = -1;
    rx->ifMatch = 1;
    rx->ifModified = 1;
    rx->remainingContent = 0;
    rx->method = 0;
    rx->pathInfo = mprStrdup(rx, "/");
    rx->scriptName = mprStrdup(rx, "");
    rx->status = 0;
    rx->statusMessage = "";
    rx->mimeType = "";
    rx->needInputPipeline = !conn->server;
    rx->headers = mprCreateHash(rx, HTTP_SMALL_HASH_SIZE);
    mprSetHashCase(rx->headers, 0);
    return rx;
}


void httpDestroyRx(HttpConn *conn)
{
    if (conn->input) {
        if (mprGetParent(conn->input) != conn && httpGetPacketLength(conn->input) > 0) {
            conn->input = httpSplitPacket(conn, conn->input, 0);
        } else {
            conn->input = 0;
        }
    }
    if (conn->rx) {
        if (conn->server) {
            httpValidateLimits(conn->server, HTTP_VALIDATE_CLOSE_REQUEST, conn);
        }
#if FUTURE
        mprFree(conn->rx->arena);
#else
        mprFree(conn->rx);
#endif
        conn->rx = 0;
    }
    if (conn->server) {
        httpPrepServerConn(conn);
    }
}


/*  
    Process incoming requests and drive the state machine. This will process as many requests as possible before returning. 
    All socket I/O is non-blocking, and this routine must not block. Note: packet may be null.
 */
void httpProcess(HttpConn *conn, HttpPacket *packet)
{
    mprAssert(conn);

    conn->canProceed = 1;
    conn->advancing = 1;

    while (conn->canProceed) {
        LOG(conn, 7, "httpProcess, state %d, error %d", conn->state, conn->error);

        switch (conn->state) {
        case HTTP_STATE_BEGIN:
        case HTTP_STATE_CONNECTED:
            conn->canProceed = parseIncoming(conn, packet);
            break;

        case HTTP_STATE_PARSED:
            conn->canProceed = processParsed(conn);
            break;

        case HTTP_STATE_CONTENT:
            conn->canProceed = processContent(conn, packet);
            break;

        case HTTP_STATE_RUNNING:
            conn->canProceed = processRunning(conn);
            break;

        case HTTP_STATE_COMPLETE:
            conn->canProceed = processCompletion(conn);
            break;
        }
        packet = conn->input;
    }
    conn->advancing = 0;
}


/*  
    Parse the incoming http message. Return true to keep going with this or subsequent request, zero means
    insufficient data to proceed.
 */
static bool parseIncoming(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLoc     *loc;
    char        *start, *end;
    int         len;

    if (packet == NULL) {
        return 0;
    }
    if (conn->server && !httpValidateLimits(conn->server, HTTP_VALIDATE_OPEN_REQUEST, conn)) {
        return 0;
    }
    if (conn->rx == NULL) {
        conn->rx = httpCreateRx(conn);
        conn->tx = httpCreateTx(conn, NULL);
    }
    rx = conn->rx;
    tx = conn->tx;

    if ((len = mprGetBufLength(packet->content)) == 0) {
        return 0;
    }
    start = mprGetBufStart(packet->content);
    if ((end = mprStrnstr(start, "\r\n\r\n", len)) == 0) {
        return 0;
    }
    len = end - start;
    mprAddNullToBuf(packet->content);

    if (len >= conn->limits->headerSize) {
        httpLimitError(conn, HTTP_CODE_REQUEST_TOO_LARGE, "Header too big");
        return 0;
    }
    if (conn->server) {
        parseRequestLine(conn, packet);
    } else {
        parseResponseLine(conn, packet);
    }
    if (!conn->connError) {
        parseHeaders(conn, packet);
    }
    if (conn->server) {
        httpSetState(conn, HTTP_STATE_PARSED);        
        loc = (rx->loc) ? rx->loc : conn->server->loc;
        httpCreatePipeline(conn, rx->loc, tx->handler);
#if FUTURE
        //  MOB -- TODO
        if (0 && tx->handler->flags & HTTP_STAGE_THREAD && !conn->threaded) {
            threadRequest(conn);
            return 0;
        }
#endif
    } else if (!(100 <= rx->status && rx->status < 200)) {
        httpSetState(conn, HTTP_STATE_PARSED);        
    }
    return 1;
}


static int traceRequest(HttpConn *conn, HttpPacket *packet)
{
    MprBuf  *content;
    cchar   *endp;
    int     len;

    mprLog(conn, 6, "Request from %s:%d to %s:%d", conn->ip, conn->port, conn->sock->ip, conn->sock->port);
    if (httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, conn->tx->extension) >= 0) {
        content = packet->content;
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (endp - mprGetBufStart(content) + 4) : 0;
        httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, packet, len, 0);
        return 1;
    }
    return 0;
}

/*  
    Parse the first line of a http request. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Requests look like: METHOD URL HTTP/1.X.
 */
static void parseRequestLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    char        *method, *uri, *protocol;
    int         methodFlags, traced, level;

    mprLog(conn, 4, "New request from %s:%d to %s:%d", conn->ip, conn->port, conn->sock->ip, conn->sock->port);

    rx = conn->rx;
    protocol = uri = 0;
    methodFlags = 0;
    traced = traceRequest(conn, packet);

    method = getToken(conn, " ");
    switch (method[0]) {
    case 'D':
        if (strcmp(method, "DELETE") == 0) {
            methodFlags = HTTP_DELETE;
        }
        break;

    case 'G':
        if (strcmp(method, "GET") == 0) {
            methodFlags = HTTP_GET;
        }
        break;

    case 'P':
        if (strcmp(method, "POST") == 0) {
            methodFlags = HTTP_POST;
            rx->needInputPipeline = 1;

        } else if (strcmp(method, "PUT") == 0) {
            methodFlags = HTTP_PUT;
            rx->needInputPipeline = 1;
        }
        break;

    case 'H':
        if (strcmp(method, "HEAD") == 0) {
            methodFlags = HTTP_HEAD;
            httpOmitBody(conn);
        }
        break;

    case 'O':
        if (strcmp(method, "OPTIONS") == 0) {
            methodFlags = HTTP_OPTIONS;
            httpOmitBody(conn);
        }
        break;

    case 'T':
        if (strcmp(method, "TRACE") == 0) {
            methodFlags = HTTP_TRACE;
            httpOmitBody(conn);
        }
        break;
    }
    if (methodFlags == 0) {
        httpProtocolError(conn, HTTP_CODE_BAD_METHOD, "Unknown method");
    }

    uri = getToken(conn, " ");
    if (*uri == '\0') {
        httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
    } else if ((int) strlen(uri) >= conn->limits->uriSize) {
        httpError(conn, HTTP_CODE_REQUEST_URL_TOO_LARGE, "Bad request. URI too long");
    }
    protocol = getToken(conn, "\r\n");
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        conn->keepAliveCount = 0;
        if (methodFlags & (HTTP_POST|HTTP_PUT)) {
            rx->remainingContent = MAXINT;
            rx->needInputPipeline = 1;
        }
        conn->http10 = 1;
        conn->protocol = "HTTP/1.0";
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpProtocolError(conn, HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
    }
    rx->flags |= methodFlags;
    rx->method = method;

    if (httpSetUri(conn, uri) < 0) {
        httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad URL format");
    }
    httpSetState(conn, HTTP_STATE_FIRST);

    if (!traced && (level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_FIRST, NULL)) >= 0) {
        mprLog(conn, level, "%s %s %s", method, uri, protocol);
    }
}


/*  
    Parse the first line of a http response. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Response status lines look like: HTTP/1.X CODE Message
 */
static void parseResponseLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    MprBuf      *content;
    cchar       *endp;
    char        *protocol, *status;
    int         len, level;

    rx = conn->rx;

    protocol = getToken(conn, " ");
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        conn->keepAliveCount = 0;
        conn->http10 = 1;
        conn->protocol = "HTTP/1.0";
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpProtocolError(conn, HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
    }

    status = getToken(conn, " ");
    if (*status == '\0') {
        httpProtocolError(conn, HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
    }
    rx->status = atoi(status);
    rx->statusMessage = getToken(conn, "\r\n");

    if ((int) strlen(rx->statusMessage) >= conn->limits->uriSize) {
        httpError(conn, HTTP_CODE_REQUEST_URL_TOO_LARGE, "Bad response. Status message too long");
    }
    if (httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, conn->tx->extension) >= 0) {
        content = packet->content;
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (endp - mprGetBufStart(content) + 4) : 0;
        httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, packet, len, 0);

    } else if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_FIRST, conn->tx->extension)) >= 0) {
        mprLog(rx, level, "%s %d %s", protocol, rx->status, rx->statusMessage);
    }
}


/*  
    Parse the request headers. Return true if the header parsed.
 */
static void parseHeaders(HttpConn *conn, HttpPacket *packet)
{
    Http        *http;
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLimits  *limits;
    MprBuf      *content;
    char        *key, *value, *tok, *tp;
    cchar       *oldValue;
    int         len, count, keepAlive;

    http = conn->http;
    rx = conn->rx;
    tx = conn->tx;
    content = packet->content;
    conn->rx->headerPacket = packet;
    limits = conn->limits;
    keepAlive = 0;

    for (count = 0; content->start[0] != '\r' && !conn->error; count++) {
        if (count >= limits->headerCount) {
            httpLimitError(conn, HTTP_CODE_BAD_REQUEST, "Too many headers");
            break;
        }
        if ((key = getToken(conn, ":")) == 0 || *key == '\0') {
            httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad header format");
            break;
        }
        value = getToken(conn, "\r\n");
        while (isspace((int) *value)) {
            value++;
        }
        mprStrLower(key);

        LOG(rx, 8, "Key %s, value %s", key, value);
        if (strspn(key, "%<>/\\") > 0) {
            httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad header key value");
            break;
        }
        if ((oldValue = mprLookupHash(rx->headers, key)) != 0) {
            mprAddHash(rx->headers, key, mprAsprintf(rx->headers, -1, "%s, %s", oldValue, value));
        } else {
            mprAddHash(rx->headers, key, value);
        }

        switch (key[0]) {
        case 'a':
            if (strcmp(key, "authorization") == 0) {
                value = mprStrdup(rx, value);
                rx->authType = mprStrTok(value, " \t", &tok);
                rx->authDetails = tok;

            } else if (strcmp(key, "accept-charset") == 0) {
                rx->acceptCharset = value;

            } else if (strcmp(key, "accept") == 0) {
                rx->accept = value;

            } else if (strcmp(key, "accept-encoding") == 0) {
                rx->acceptEncoding = value;
            }
            break;

        case 'c':
            if (strcmp(key, "content-length") == 0) {
                if (rx->length >= 0) {
                    httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Mulitple content length headers");
                    break;
                }
                rx->length = atoi(value);
                if (rx->length < 0) {
                    httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad content length");
                    break;
                }
                if (rx->length >= conn->limits->receiveBodySize) {
                    httpLimitError(conn, HTTP_CODE_REQUEST_TOO_LARGE,
                        "Request content length %d bytes is too big. Limit %d.", rx->length, conn->limits->receiveBodySize);
                    break;
                }
                rx->contentLength = value;
                mprAssert(rx->length >= 0);
                if (conn->server || strcmp(tx->method, "HEAD") != 0) {
                    rx->remainingContent = rx->length;
                    rx->needInputPipeline = 1;
                }

            } else if (strcmp(key, "content-range") == 0) {
                /*
                    This headers specifies the range of any posted body data
                    Format is:  Content-Range: bytes n1-n2/length
                    Where n1 is first byte pos and n2 is last byte pos
                 */
                char    *sp;
                int     start, end, size;

                start = end = size = -1;
                sp = value;
                while (*sp && !isdigit((int) *sp)) {
                    sp++;
                }
                if (*sp) {
                    start = (int) mprAtoi(sp, 10);

                    if ((sp = strchr(sp, '-')) != 0) {
                        end = (int) mprAtoi(++sp, 10);
                    }
                    if ((sp = strchr(sp, '/')) != 0) {
                        /*
                            Note this is not the content length transmitted, but the original size of the input of which
                            the client is transmitting only a portion.
                         */
                        size = (int) mprAtoi(++sp, 10);
                    }
                }
                if (start < 0 || end < 0 || size < 0 || end <= start) {
                    httpError(conn, HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad content range");
                    break;
                }
                rx->inputRange = httpCreateRange(conn, start, end);

            } else if (strcmp(key, "content-type") == 0) {
                rx->mimeType = value;
                rx->form = strstr(rx->mimeType, "application/x-www-form-urlencoded") != 0;

            } else if (strcmp(key, "cookie") == 0) {
                if (rx->cookie && *rx->cookie) {
                    rx->cookie = mprStrcat(rx, -1, rx->cookie, "; ", value, NULL);
                } else {
                    rx->cookie = value;
                }

            } else if (strcmp(key, "connection") == 0) {
                rx->connection = value;
                if (mprStrcmpAnyCase(value, "KEEP-ALIVE") == 0) {
                    keepAlive++;
                } else if (mprStrcmpAnyCase(value, "CLOSE") == 0) {
                    conn->keepAliveCount = -1;
                }
            }
            break;

        case 'h':
            if (strcmp(key, "host") == 0) {
                rx->hostName = value;
            }
            break;

        case 'i':
            if ((strcmp(key, "if-modified-since") == 0) || (strcmp(key, "if-unmodified-since") == 0)) {
                MprTime     newDate = 0;
                char        *cp;
                bool        ifModified = (key[3] == 'M');

                if ((cp = strchr(value, ';')) != 0) {
                    *cp = '\0';
                }
                if (mprParseTime(conn, &newDate, value, MPR_UTC_TIMEZONE, NULL) < 0) {
                    mprAssert(0);
                    break;
                }
                if (newDate) {
                    rx->since = newDate;
                    rx->ifModified = ifModified;
                    rx->flags |= HTTP_REC_IF_MODIFIED;
                }

            } else if ((strcmp(key, "if-match") == 0) || (strcmp(key, "if-none-match") == 0)) {
                char    *word, *tok;
                bool    ifMatch = key[3] == 'M';

                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = ifMatch;
                rx->flags |= HTTP_REC_IF_MODIFIED;
                value = mprStrdup(conn, value);
                word = mprStrTok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = mprStrTok(0, " ,", &tok);
                }

            } else if (strcmp(key, "if-range") == 0) {
                char    *word, *tok;

                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = 1;
                rx->flags |= HTTP_REC_IF_MODIFIED;
                value = mprStrdup(conn, value);
                word = mprStrTok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = mprStrTok(0, " ,", &tok);
                }
            }
            break;

        case 'k':
            if (strcmp(key, "keep-alive") == 0) {
                /*
                    Keep-Alive: timeout=N, max=1
                 */
                len = (int) strlen(value);
                if (len > 2 && value[len - 1] == '1' && value[len - 2] == '=' && tolower((int)(value[len - 3])) == 'x') {

                    /*  
                        IMPORTANT: Deliberately close the connection one request early. This ensures a client-led 
                        termination and helps relieve server-side TIME_WAIT conditions.
                     */
                    conn->keepAliveCount = 0;
                }
            }
            break;                
                
        case 'l':
            if (strcmp(key, "location") == 0) {
                rx->redirect = value;
            }
            break;

        case 'p':
            if (strcmp(key, "pragma") == 0) {
                rx->pragma = value;
            }
            break;

        case 'r':
            if (strcmp(key, "range") == 0) {
                if (!parseRange(conn, value)) {
                    httpError(conn, HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad range");
                }
            } else if (strcmp(key, "referer") == 0) {
                rx->referer = value;
            }
            break;

        case 't':
            if (strcmp(key, "transfer-encoding") == 0) {
                mprStrLower(value);
                if (strcmp(value, "chunked") == 0) {
                    rx->flags |= HTTP_REC_CHUNKED;
                    /*  
                        This will be revised by the chunk filter as chunks are processed and will be set to zero when the
                        last chunk has been received.
                     */
                    rx->remainingContent = MAXINT;
                    rx->needInputPipeline = 1;
                }
            }
            break;

#if BLD_DEBUG
        case 'x':
            if (strcmp(key, "x-appweb-chunk-size") == 0) {
                tx->chunkSize = atoi(value);
                if (tx->chunkSize <= 0) {
                    tx->chunkSize = 0;
                } else if (tx->chunkSize > conn->limits->chunkSize) {
                    tx->chunkSize = conn->limits->chunkSize;
                }
            }
            break;
#endif

        case 'u':
            if (strcmp(key, "user-agent") == 0) {
                rx->userAgent = value;
            }
            break;

        case 'w':
            if (strcmp(key, "www-authenticate") == 0) {
                tp = value;
                while (*value && !isspace((int) *value)) {
                    value++;
                }
                *value++ = '\0';
                mprStrLower(tp);
                mprFree(conn->authType);
                conn->authType = mprStrdup(conn, tp);
                if (!parseAuthenticate(conn, value)) {
                    httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad Authentication header");
                    break;
                }
            }
            break;
        }
    }
    if (conn->protocol == 0 && !keepAlive) {
        conn->keepAliveCount = 0;
    }
    if (!(rx->flags & HTTP_REC_CHUNKED)) {
        /*  
            Step over "\r\n" after headers. As an optimization, don't do this if chunked so chunking can parse a single
            chunk delimiter of "\r\nSIZE ...\r\n"
         */
        if (mprGetBufLength(content) >= 2) {
            mprAdjustBufStart(content, 2);
        }
    }
    if (rx->remainingContent == 0) {
        rx->eof = 1;
    }
}


/*  
    Parse an authentication response (client side only)
 */
static bool parseAuthenticate(HttpConn *conn, char *authDetails)
{
    HttpRx  *rx;
    char    *value, *tok, *key, *dp, *sp;
    int     seenComma;

    rx = conn->rx;
    key = (char*) authDetails;

    while (*key) {
        while (*key && isspace((int) *key)) {
            key++;
        }
        tok = key;
        while (*tok && !isspace((int) *tok) && *tok != ',' && *tok != '=') {
            tok++;
        }
        *tok++ = '\0';

        while (isspace((int) *tok)) {
            tok++;
        }
        seenComma = 0;
        if (*tok == '\"') {
            value = ++tok;
            while (*tok != '\"' && *tok != '\0') {
                tok++;
            }
        } else {
            value = tok;
            while (*tok != ',' && *tok != '\0') {
                tok++;
            }
            seenComma++;
        }
        *tok++ = '\0';

        /*
            Handle back-quoting
         */
        if (strchr(value, '\\')) {
            for (dp = sp = value; *sp; sp++) {
                if (*sp == '\\') {
                    sp++;
                }
                *dp++ = *sp++;
            }
            *dp = '\0';
        }

        /*
            algorithm, domain, nonce, oqaque, realm, qop, stale
            We don't strdup any of the values as the headers are persistently saved.
         */
        switch (tolower((int) *key)) {
        case 'a':
            if (mprStrcmpAnyCase(key, "algorithm") == 0) {
                mprFree(rx->authAlgorithm);
                rx->authAlgorithm = value;
                break;
            }
            break;

        case 'd':
            if (mprStrcmpAnyCase(key, "domain") == 0) {
                mprFree(conn->authDomain);
                conn->authDomain = mprStrdup(conn, value);
                break;
            }
            break;

        case 'n':
            if (mprStrcmpAnyCase(key, "nonce") == 0) {
                mprFree(conn->authNonce);
                conn->authNonce = mprStrdup(conn, value);
                conn->authNc = 0;
            }
            break;

        case 'o':
            if (mprStrcmpAnyCase(key, "opaque") == 0) {
                mprFree(conn->authOpaque);
                conn->authOpaque = mprStrdup(conn, value);
            }
            break;

        case 'q':
            if (mprStrcmpAnyCase(key, "qop") == 0) {
                mprFree(conn->authQop);
                conn->authQop = mprStrdup(conn, value);
            }
            break;

        case 'r':
            if (mprStrcmpAnyCase(key, "realm") == 0) {
                mprFree(conn->authRealm);
                conn->authRealm = mprStrdup(conn, value);
            }
            break;

        case 's':
            if (mprStrcmpAnyCase(key, "stale") == 0) {
                rx->authStale = mprStrdup(rx, value);
                break;
            }

        default:
            /*  For upward compatibility --  ignore keywords we don't understand */
            ;
        }
        key = tok;
        if (!seenComma) {
            while (*key && *key != ',') {
                key++;
            }
            if (*key) {
                key++;
            }
        }
    }
    if (strcmp(rx->conn->authType, "basic") == 0) {
        if (conn->authRealm == 0) {
            return 0;
        }
        return 1;
    }
    /* Digest */
    if (conn->authRealm == 0 || conn->authNonce == 0) {
        return 0;
    }
    if (conn->authQop) {
        if (conn->authDomain == 0 || conn->authOpaque == 0 || rx->authAlgorithm == 0 || rx->authStale == 0) {
            return 0;
        }
    }
    return 1;
}


#if FUTURE
static void httpThreadEvent(HttpConn *conn)
{
    httpCallEvent(conn, 0);
}


static void threadRequest(HttpConn *conn)
{
    mprAssert(!conn->dispatcher->enabled);
    mprAssert(conn->dispatcher != conn->server->dispatcher);

    conn->threaded = 1;
    conn->startingThread = 1;
    mprInitEvent(conn->dispatcher, &conn->runEvent, "runEvent", 0, (MprEventProc) httpThreadEvent, conn, 0);
    mprQueueEvent(conn->dispatcher, &conn->runEvent);
    mprAssert(!conn->dispatcher->enabled);
}
#endif


static bool processParsed(HttpConn *conn)
{
    if (!conn->abortPipeline) {
        httpStartPipeline(conn);
        if (!conn->error && !conn->writeComplete && conn->rx->remainingContent > 0) {
            /* If no remaining content, wait till the processing stage to avoid duplicate writable events */
            httpWritable(conn);
        }
    }
    httpSetState(conn, HTTP_STATE_CONTENT);
    return 1;
}


static bool analyseContent(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    MprBuf      *content;
    int         nbytes, remaining;

    rx = conn->rx;
    tx = conn->tx;
    q = &tx->queue[HTTP_QUEUE_RECEIVE];

    content = packet->content;
    if (rx->flags & HTTP_REC_CHUNKED) {
        if ((remaining = getChunkPacketSize(conn, content)) == 0) {
            /* Need more data or bad chunk specification */
            if (mprGetBufLength(content) > 0) {
                conn->input = packet;
            }
            return 0;
        }
    } else {
        remaining = rx->remainingContent;
    }
    nbytes = min(remaining, mprGetBufLength(content));
    mprAssert(nbytes >= 0);

    if (httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_BODY, NULL) >= 0) {
        httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_BODY, packet, nbytes, 0);
    }
    LOG(conn, 7, "processContent: packet of %d bytes, remaining %d", mprGetBufLength(content), remaining);

    if (nbytes > 0) {
        mprAssert(httpGetPacketLength(packet) > 0);
        remaining -= nbytes;
        rx->remainingContent -= nbytes;
        rx->receivedContent += nbytes;

        if (rx->receivedContent >= conn->limits->receiveBodySize) {
            httpLimitError(conn, HTTP_CODE_REQUEST_TOO_LARGE, "Request content body of %d bytes is too big. Limit %d.",
                rx->receivedContent, conn->limits->receiveBodySize);
            return 0;
        }
        if (packet == rx->headerPacket) {
            /* Preserve headers if more data to come. Otherwise handlers may free the packet and destory the headers */
            packet = httpSplitPacket(conn, packet, 0);
        } else {
            mprStealBlock(tx, packet);
        }
        conn->input = 0;
        if (remaining == 0 && mprGetBufLength(packet->content) > nbytes) {
            /*  Split excess data belonging to the next pipelined request.  */
            LOG(conn, 7, "processContent: Split packet of %d at %d", httpGetPacketLength(packet), nbytes);
            conn->input = httpSplitPacket(conn, packet, nbytes);
        }
        if ((q->count + httpGetPacketLength(packet)) > q->max) {
            /*  
                MOB -- should flow control instead
                httpLimitError(q->conn, HTTP_CODE_REQUEST_TOO_LARGE, "Too much body data");
            */
            return 0;
        }
        if (conn->error) {
            /* Discard input data if the request has an error */
            mprFree(packet);
        } else {
            httpSendPacketToNext(q, packet);
        }

    } else {
        if (conn->input != rx->headerPacket) {
            mprFree(packet);
        }
        conn->input = 0;
    }
    return 1;
}

/*  
    Process request body data (typically post or put content)
 */
static bool processContent(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpQueue   *q;

    rx = conn->rx;
    q = &conn->tx->queue[HTTP_QUEUE_RECEIVE];

    if (packet == NULL) {
        return 0;
    }
    if (conn->complete || conn->connError || rx->remainingContent <= 0) {
        httpSetState(conn, HTTP_STATE_RUNNING);
        return 1;
    }
    mprAssert(packet);
    if (!analyseContent(conn, packet)) {
        if (conn->connError) {
            /* Abort the content state if there is a connection oriented error */
            httpSetState(conn, HTTP_STATE_RUNNING);
        }
        return conn->error;
    }
    if (rx->remainingContent == 0) {
        if (!(rx->flags & HTTP_REC_CHUNKED) || (rx->chunkState == HTTP_CHUNK_EOF)) {
            rx->eof = 1;
            httpSendPacketToNext(q, httpCreateEndPacket(rx));
        }
        httpSetState(conn, HTTP_STATE_RUNNING);
        return 1;
    }
    httpServiceQueues(conn);
    return conn->connError || (conn->input ? mprGetBufLength(conn->input->content) : 0);
}


static bool processRunning(HttpConn *conn)
{
    int     canProceed;

    if (conn->abortPipeline) {
        httpSetState(conn, HTTP_STATE_COMPLETE);
    } else {
        if (conn->server) {
            httpProcessPipeline(conn);
        }
        if (conn->server) {
            if (conn->complete || conn->writeComplete || conn->error) {
                httpSetState(conn, HTTP_STATE_COMPLETE);
                canProceed = 1;
            } else {
                httpWritable(conn);
                canProceed = httpServiceQueues(conn);
            }
        } else {
            canProceed = httpServiceQueues(conn);
            httpFinalize(conn);
            conn->complete = 1;
            httpSetState(conn, HTTP_STATE_COMPLETE);
            canProceed = 1;
        }
    }
    return canProceed;
}


static bool processCompletion(HttpConn *conn)
{
    HttpPacket  *packet;
    Mpr         *mpr;
    bool        more;

    mprAssert(conn->state == HTTP_STATE_COMPLETE);

    mpr = mprGetMpr(conn);

#if FUTURE
    mprLog(conn, 4, "Request complete used %,d K, mpr usage %,d K, page usage %,d K",
        conn->arena->allocBytes / 1024, mpr->heap.allocBytes / 1024, mpr->pageHeap.allocBytes / 1024);
#endif

    packet = conn->input;
    more = packet && !conn->connError && (mprGetBufLength(packet->content) > 0);
    if (mprGetParent(packet) != conn) {
        if (more) {
            conn->input = httpSplitPacket(conn, packet, 0);
        } else {
            conn->input = 0;
        }
    }
    if (conn->server) {
        httpDestroyRx(conn);
        return more;
    }
    return 0;
}


void httpCloseRx(HttpConn *conn)
{
    if (!conn->rx->eof) {
        conn->connError = 1;
    }
    httpFinalize(conn);
    if (conn->state < HTTP_STATE_COMPLETE && !conn->advancing) {
        httpProcess(conn, NULL);
    }
}


/*  
    Optimization to correctly size the packets to the chunk filter.
 */
static int getChunkPacketSize(HttpConn *conn, MprBuf *buf)
{
    HttpRx      *rx;
    char        *start, *cp;
    int         need, size;

    rx = conn->rx;
    need = 0;

    switch (rx->chunkState) {
    case HTTP_CHUNK_DATA:
        need = rx->remainingContent;
        if (need != 0) {
            break;
        }
        /* Fall through */

    case HTTP_CHUNK_START:
        start = mprGetBufStart(buf);
        if (mprGetBufLength(buf) < 3) {
            return 0;
        }
        if (start[0] != '\r' || start[1] != '\n') {
            httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
            return 0;
        }
        for (cp = &start[2]; cp < (char*) buf->end && *cp != '\n'; cp++) {}
        if ((cp - start) < 2 || (cp[-1] != '\r' || cp[0] != '\n')) {
            /* Insufficient data */
            if ((cp - start) > 80) {
                httpProtocolError(conn, HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
                return 0;
            }
            return 0;
        }
        need = cp - start + 1;
        size = (int) mprAtoi(&start[2], 16);
        if (size == 0 && &cp[2] < buf->end && cp[1] == '\r' && cp[2] == '\n') {
            /*
                This is the last chunk (size == 0). Now need to consume the trailing "\r\n".
                We are lenient if the request does not have the trailing "\r\n" as required by the spec.
             */
            need += 2;
        }
        break;

    default:
        mprAssert(0);
    }
    rx->remainingContent = need;
    return need;
}


bool httpContentNotModified(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTime     modified;
    bool        same;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->flags & HTTP_REC_IF_MODIFIED) {
        /*  
            If both checks, the last modification time and etag, claim that the request doesn't need to be
            performed, skip the transfer. TODO - need to check if fileInfo is actually set.
         */
        modified = (MprTime) tx->fileInfo.mtime * MPR_TICKS_PER_SEC;
        same = httpMatchModified(conn, modified) && httpMatchEtag(conn, tx->etag);
        if (rx->ranges && !same) {
            /*
                Need to transfer the entire resource
             */
            mprFree(rx->ranges);
            rx->ranges = 0;
        }
        return same;
    }
    return 0;
}


HttpRange *httpCreateRange(HttpConn *conn, int start, int end)
{
    HttpRange     *range;

    range = mprAllocObjZeroed(conn->rx, HttpRange);
    if (range == 0) {
        return 0;
    }
    range->start = start;
    range->end = end;
    range->len = end - start;

    return range;
}


int httpGetContentLength(HttpConn *conn)
{
    if (conn->rx == 0) {
        mprAssert(conn->rx);
        return 0;
    }
    return conn->rx->length;
    return 0;
}


cchar *httpGetCookies(HttpConn *conn)
{
    if (conn->rx == 0) {
        mprAssert(conn->rx);
        return 0;
    }
    return conn->rx->cookie;
}


cchar *httpGetHeader(HttpConn *conn, cchar *key)
{
    cchar   *value;
    char    *lower;

    if (conn->rx == 0) {
        mprAssert(conn->rx);
        return 0;
    }
    lower = mprStrdup(conn, key);
    mprStrLower(lower);
    value = mprLookupHash(conn->rx->headers, lower);
    mprFree(lower);
    return value;
}


//  MOB -- why does this allocate?
char *httpGetHeaders(HttpConn *conn)
{
    HttpRx      *rx;
    MprHash     *hp;
    char        *headers, *key, *cp;
    int         len;

    if (conn->rx == 0) {
        mprAssert(conn->rx);
        return 0;
    }
    rx = conn->rx;
    headers = 0;
    for (len = 0, hp = mprGetFirstHash(rx->headers); hp; ) {
        headers = mprReallocStrcat(rx, -1, headers, hp->key, NULL);
        key = &headers[len];
        for (cp = &key[1]; *cp; cp++) {
            *cp = tolower((int) *cp);
            if (*cp == '-') {
                cp++;
            }
        }
        headers = mprReallocStrcat(rx, -1, headers, ": ", hp->data, "\n", NULL);
        len = strlen(headers);
        hp = mprGetNextHash(rx->headers, hp);
    }
    return headers;
}


MprHashTable *httpGetHeaderHash(HttpConn *conn)
{
    if (conn->rx == 0) {
        mprAssert(conn->rx);
        return 0;
    }
    return conn->rx->headers;
}


cchar *httpGetQueryString(HttpConn *conn)
{
    return conn->rx->parsedUri->query;
}


int httpGetStatus(HttpConn *conn)
{
    return conn->rx->status;
}


char *httpGetStatusMessage(HttpConn *conn)
{
    return conn->rx->statusMessage;
}


int httpSetUri(HttpConn *conn, cchar *uri)
{
    HttpRx   *rx;

    rx = conn->rx;

    /*  
        Parse and tokenize the uri. Then decode and validate the URI path portion.
     */
    rx->parsedUri = httpCreateUri(rx, uri, 0);
    if (rx->parsedUri == 0) {
        return MPR_ERR_BAD_ARGS;
    }

    /*
        Start out with no scriptName and the entire URI in the pathInfo. Stages may rewrite.
     */
    rx->uri = rx->parsedUri->uri;
    conn->tx->extension = rx->parsedUri->ext;
    mprFree(rx->pathInfo);
    rx->pathInfo = httpNormalizeUriPath(rx, mprUriDecode(rx, rx->parsedUri->path));
    rx->scriptName = mprStrdup(rx, "");
    return 0;
}


static void waitHandler(HttpConn *conn, struct MprEvent *event)
{
    httpCallEvent(conn, event->mask);
    httpEnableConnEvents(conn);
}


/*  
    Wait for the Http object to achieve a given state. Timeout is total wait time in msec. If <= 0, then dont wait.
 */
int httpWait(HttpConn *conn, MprDispatcher *dispatcher, int state, int timeout)
{
    Http        *http;
    MprTime     expire;
    int         eventMask, remainingTime, addedHandler, saveAsync;

    http = conn->http;

    if (timeout <= 0) {
        timeout = 0;
    }
    if (conn->state <= HTTP_STATE_BEGIN) {
        mprAssert(conn->state >= HTTP_STATE_BEGIN);
        return MPR_ERR_BAD_STATE;
    } 
    saveAsync = conn->async;
    if (conn->waitHandler.fd < 0) {
        conn->async = 1;
        eventMask = MPR_READABLE;
        if (!conn->writeComplete) {
            eventMask |= MPR_WRITABLE;
        }
        mprInitWaitHandler(conn, &conn->waitHandler, conn->sock->fd, eventMask, conn->dispatcher,
            (MprEventProc) waitHandler, conn);
        addedHandler = 1;
    } else addedHandler = 0;

    http->now = mprGetTime(conn);
    expire = http->now + timeout;
    while (!conn->error && conn->state < state && conn->sock && !mprIsSocketEof(conn->sock)) {
        remainingTime = (int) (expire - http->now);
        if (remainingTime <= 0) {
            break;
        }
        mprAssert(!mprSocketHasPendingData(conn->sock));
        mprServiceEvents(conn, dispatcher, remainingTime, MPR_SERVICE_ONE_THING);
    }
    if (addedHandler && conn->waitHandler.fd >= 0) {
        mprRemoveWaitHandler(&conn->waitHandler);
        conn->async = saveAsync;
    }
    if (conn->sock == 0 || conn->error) {
        return MPR_ERR_CONNECTION;
    }
    if (conn->state < state) {
        return MPR_ERR_TIMEOUT;
    }
    return 0;
}


/*  
    Set the connector as write blocked and can't proceed.
 */
void httpWriteBlocked(HttpConn *conn)
{
    mprLog(conn, 7, "Write Blocked");
    conn->canProceed = 0;
    conn->writeBlocked = 1;
}


static void addMatchEtag(HttpConn *conn, char *etag)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->etags == 0) {
        rx->etags = mprCreateList(rx);
    }
    mprAddItem(rx->etags, etag);
}


/*  
    Get the next input token. The content buffer is advanced to the next token. This routine always returns a
    non-zero token. The empty string means the delimiter was not found. The delimiter is a string to match and not
    a set of characters. HTTP header header parsing does not work as well using classical strtok parsing as you must
    know when the "/r/n/r/n" body delimiter has been encountered. Strtok will eat such delimiters.
 */
static char *getToken(HttpConn *conn, cchar *delim)
{
    MprBuf  *buf;
    char    *token, *nextToken;
    int     len;

    buf = conn->input->content;
    token = mprGetBufStart(buf);
    nextToken = mprStrnstr(mprGetBufStart(buf), delim, mprGetBufLength(buf));
    if (nextToken) {
        *nextToken = '\0';
        len = (int) strlen(delim);
        nextToken += len;
        buf->start = nextToken;
    } else {
        buf->start = mprGetBufEnd(buf);
    }
    return token;
}


/*  
    Match the entity's etag with the client's provided etag.
 */
bool httpMatchEtag(HttpConn *conn, char *requestedEtag)
{
    HttpRx  *rx;
    char    *tag;
    int     next;

    rx = conn->rx;
    if (rx->etags == 0) {
        return 1;
    }
    if (requestedEtag == 0) {
        return 0;
    }

    for (next = 0; (tag = mprGetNextItem(rx->etags, &next)) != 0; ) {
        if (strcmp(tag, requestedEtag) == 0) {
            return (rx->ifMatch) ? 0 : 1;
        }
    }
    return (rx->ifMatch) ? 1 : 0;
}


/*  
    If an IF-MODIFIED-SINCE was specified, then return true if the resource has not been modified. If using
    IF-UNMODIFIED, then return true if the resource was modified.
 */
bool httpMatchModified(HttpConn *conn, MprTime time)
{
    HttpRx   *rx;

    rx = conn->rx;

    if (rx->since == 0) {
        /*  If-Modified or UnModified not supplied. */
        return 1;
    }
    if (rx->ifModified) {
        /*  Return true if the file has not been modified.  */
        return !(time > rx->since);
    } else {
        /*  Return true if the file has been modified.  */
        return (time > rx->since);
    }
}


/*  
    Format is:  Range: bytes=n1-n2,n3-n4,...
    Where n1 is first byte pos and n2 is last byte pos

    Examples:
        Range: 0-49             first 50 bytes
        Range: 50-99,200-249    Two 50 byte ranges from 50 and 200
        Range: -50              Last 50 bytes
        Range: 1-               Skip first byte then emit the rest

    Return 1 if more ranges, 0 if end of ranges, -1 if bad range.
 */
static bool parseRange(HttpConn *conn, char *value)
{
    HttpRx      *rx;
    HttpRange   *range, *last, *next;
    char        *tok, *ep;

    rx = conn->rx;

    value = mprStrdup(conn, value);
    if (value == 0) {
        return 0;
    }

    /*  
        Step over the "bytes="
     */
    tok = mprStrTok(value, "=", &value);

    for (last = 0; value && *value; ) {
        range = mprAllocObjZeroed(rx, HttpRange);
        if (range == 0) {
            return 0;
        }

        /*  A range "-7" will set the start to -1 and end to 8
         */
        tok = mprStrTok(value, ",", &value);
        if (*tok != '-') {
            range->start = (int) mprAtoi(tok, 10);
        } else {
            range->start = -1;
        }
        range->end = -1;

        if ((ep = strchr(tok, '-')) != 0) {
            if (*++ep != '\0') {
                /*
                    End is one beyond the range. Makes the math easier.
                 */
                range->end = (int) mprAtoi(ep, 10) + 1;
            }
        }
        if (range->start >= 0 && range->end >= 0) {
            range->len = (int) (range->end - range->start);
        }
        if (last == 0) {
            rx->ranges = range;
        } else {
            last->next = range;
        }
        last = range;
    }

    /*  
        Validate ranges
     */
    for (range = rx->ranges; range; range = range->next) {
        if (range->end != -1 && range->start >= range->end) {
            return 0;
        }
        if (range->start < 0 && range->end < 0) {
            return 0;
        }
        next = range->next;
        if (range->start < 0 && next) {
            /* This range goes to the end, so can't have another range afterwards */
            return 0;
        }
        if (next) {
            if (next->start >= 0 && range->end > next->start) {
                return 0;
            }
        }
    }
    conn->tx->currentRange = rx->ranges;
    return (last) ? 1: 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2010. All Rights Reserved.
    Copyright (c) Michael O'Brien, 1993-2010. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the GPL open source license described below or you may acquire
    a commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.TXT distributed with
    this software for full details.

    This software is open source; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 2 of the License, or (at your
    option) any later version. See the GNU General Public License for more
    details at: http://www.embedthis.com/downloads/gplLicense.html

    This program is distributed WITHOUT ANY WARRANTY; without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

    This GPL license does NOT permit incorporating this software into
    proprietary programs. If you are unable to comply with the GPL, you must
    acquire a commercial license to use this software. Commercial licenses
    for this software and support services are available from Embedthis
    Software at http://www.embedthis.com

    @end
 */
