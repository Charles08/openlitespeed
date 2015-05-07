/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2015  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "ntwkiolink.h"

#include <ls.h>
#include <lsdef.h>
#include <edio/multiplexer.h>
#include <edio/multiplexerfactory.h>
#include <http/connlimitctrl.h>
#include <http/hiohandlerfactory.h>
#include <http/httpaiosendfile.h>
#include <http/httplog.h>
#include <http/httpresourcemanager.h>
#include <http/httprespheaders.h>
#include <http/httplistener.h>
#include <http/httpstats.h>
#include <lsiapi/lsiapi.h>
#include <lsr/ls_strtool.h>
#include <sslpp/sslcontext.h>
#include <sslpp/sslerror.h>
#include <util/accessdef.h>
#include <util/datetime.h>
#include <util/stringtool.h>

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>

#if !defined(NO_SENDFILE)
#include <util/gsendfile.h>
#endif

#define IO_THROTTLE_READ    8
#define IO_THROTTLE_WRITE   16
#define IO_COUNTED          32

//#define HTTP2_PLAIN_DEV

//#define SPDY_PLAIN_DEV

int NtwkIOLink::s_iPrevTmToken = 0;
int NtwkIOLink::s_iTmToken = 0;

class NtwkIOLink::fp_list NtwkIOLink::s_normal
    (
        NtwkIOLink::readEx,
        NtwkIOLink::writevEx,
        NtwkIOLink::onWrite,
        NtwkIOLink::onRead,
        NtwkIOLink::close_,
        NtwkIOLink::onTimer_
    );

class NtwkIOLink::fp_list NtwkIOLink::s_normalSSL
    (
        NtwkIOLink::readExSSL,
        NtwkIOLink::writevExSSL,
        NtwkIOLink::onWriteSSL,
        NtwkIOLink::onReadSSL,
        NtwkIOLink::closeSSL,
        NtwkIOLink::onTimer_
    );

class NtwkIOLink::fp_list NtwkIOLink::s_throttle
    (
        NtwkIOLink::readExT,
        NtwkIOLink::writevExT,
        NtwkIOLink::onWriteT,
        NtwkIOLink::onReadT,
        NtwkIOLink::close_,
        NtwkIOLink::onTimer_T
    );

class NtwkIOLink::fp_list NtwkIOLink::s_throttleSSL
    (
        NtwkIOLink::readExSSL_T,
        NtwkIOLink::writevExSSL_T,
        NtwkIOLink::onWriteSSL_T,
        NtwkIOLink::onReadSSL_T,
        NtwkIOLink::closeSSL,
        NtwkIOLink::onTimerSSL_T
    );

class NtwkIOLink::fp_list_list   NtwkIOLink::s_fp_list_list_normal
    (
        &NtwkIOLink::s_normal,
        &NtwkIOLink::s_normalSSL
    );

class NtwkIOLink::fp_list_list   NtwkIOLink::s_fp_list_list_throttle
    (
        &NtwkIOLink::s_throttle,
        &NtwkIOLink::s_throttleSSL
    );

class NtwkIOLink::fp_list_list  *NtwkIOLink::s_pCur_fp_list_list =
        &NtwkIOLink::s_fp_list_list_normal;

NtwkIOLink::NtwkIOLink()
    : m_sessionHooks()
    , m_aioSFQ()
{
    m_hasBufferedData = 0;
    m_pModuleConfig = NULL;
}


NtwkIOLink::~NtwkIOLink()
{
    LsiapiBridge::releaseModuleData(LSI_MODULE_DATA_L4, getModuleData());
}


int NtwkIOLink::writev(const struct iovec *vector, int len)
{
    int written = 0;

    if (m_iHeaderToSend > 0)
    {
        m_iov.append(vector, len);

        written = writev_internal(m_iov.get(), m_iov.len(), 0);
        if (written >= m_iHeaderToSend)
        {
            m_iov.clear();
            written -= m_iHeaderToSend;
            m_iHeaderToSend = 0;
        }
        else
        {
            m_iov.pop_back(len);
            if (written > 0)
            {
                m_iHeaderToSend -= written;
                m_iov.finish(written);
                return 0;
            }
        }
    }
    else
        written = writev_internal(vector, len, 0);

    return written;
}


int NtwkIOLink::writev_internal(const struct iovec *vector, int len,
                                int flush_flag)
{
    const LsiApiHooks *pWritevHooks = LsiApiHooks::getGlobalApiHooks(
                                          LSI_HKPT_L4_SENDING);
    if (!pWritevHooks || m_sessionHooks.isDisabled(LSI_HKPT_L4_SENDING))
        return (*m_pFpList->m_writev_fp)((LsiSession *)this,
                                         (struct iovec *)vector, len);

    int ret;
    lsi_cb_param_t param;
    lsiapi_hookinfo_t hookInfo;
    param._session = (LsiSession *)this;
    int    flag_out = 0;

    hookInfo.hooks = pWritevHooks;
    hookInfo.enable_array = m_sessionHooks.getEnableArray(
                                LSI_HKPT_L4_SENDING);
    hookInfo.term_fn = (filter_term_fn)m_pFpList->m_writev_fp;
    param._cur_hook = (void *)pWritevHooks->begin();
    param._hook_info = &hookInfo;
    param._param = vector;
    param._param_len = len;
    param._flag_out = &flag_out;
    param._flag_in = flush_flag;
    ret = LsiApiHooks::runForwardCb(&param);
    m_hasBufferedData = flag_out;
    LOG_D(("[NtwkIOLink::writev] ret %d hasData %d", ret, m_hasBufferedData));
    return ret;
}


int NtwkIOLink::read(char *pBuf, int size)
{
    const LsiApiHooks *pReadHooks = LsiApiHooks::getGlobalApiHooks(
                                        LSI_HKPT_L4_RECVING);
    if (!pReadHooks || m_sessionHooks.isDisabled(LSI_HKPT_L4_RECVING))
        return (*m_pFpList->m_read_fp)(this, pBuf, size);

    int ret;
    lsi_cb_param_t param;
    lsiapi_hookinfo_t hookInfo;
    param._session = (LsiSession *)this;

    hookInfo.hooks = pReadHooks;
    hookInfo.enable_array = m_sessionHooks.getEnableArray(
                                LSI_HKPT_L4_RECVING);
    hookInfo.term_fn = (filter_term_fn)m_pFpList->m_read_fp;
    param._cur_hook = (void *)((lsiapi_hook_t *)pReadHooks->end() - 1);
    param._hook_info = &hookInfo;
    param._param = pBuf;
    param._param_len = size;
    param._flag_out = NULL;
    ret = LsiApiHooks::runBackwardCb(&param);
    LOG_D(("[NtwkIOLink::read] read  %d", ret));
    return ret;
}


int NtwkIOLink::write(const char *pBuf, int size)
{
    IOVec iov(pBuf, size);
    return writev(iov.get(), iov.len());
}


void NtwkIOLink::enableThrottle(int enable)
{
    if (enable)
        s_pCur_fp_list_list = &NtwkIOLink::s_fp_list_list_throttle;
    else
        s_pCur_fp_list_list = &NtwkIOLink::s_fp_list_list_normal;
}


int NtwkIOLink::setupHandler(HiosProtocol verSpdy)
{
    HioHandler *pHandler;
#ifdef SPDY_PLAIN_DEV
    if (!isSSL() && (verSpdy == HIOS_PROTO_HTTP))
        verSpdy = HIOS_PROTO_SPDY3;
#endif

#ifdef HTTP2_PLAIN_DEV
    if (!isSSL() && (verSpdy == HIOS_PROTO_HTTP))
        verSpdy = HIOS_PROTO_HTTP2;
#endif

    pHandler = HioHandlerFactory::getHioHandler(verSpdy);
    if (!pHandler)
        return LS_FAIL;

    clearLogId();
    setProtocol(verSpdy);
    pHandler->attachStream(this);
    pHandler->onInitConnected();
    return 0;
}


int NtwkIOLink::switchToHttp2Handler(HioHandler *pSession)
{
    assert(pSession == getHandler());
    HioHandler *pHandler =
        HioHandlerFactory::getHioHandler(HIOS_PROTO_HTTP2);
    if (!pHandler)
        return LS_FAIL;

    clearLogId();
    setProtocol(HIOS_PROTO_HTTP2);
    pHandler->attachStream(this);
    pHandler->upgradedStream(pSession);
    return 0;
}


int NtwkIOLink::setLink(HttpListener *pListener,  int fd,
                        ClientInfo *pInfo, SSLContext *pSSLContext)
{
    HioStream::reset(DateTime::s_curTime);
    setfd(fd);
    m_pClientInfo = pInfo;
    setState(HIOS_CONNECTED);
    setHandler(NULL);

    assert(LSI_HKPT_L4_BEGINSESSION == 0);
    assert(LSI_HKPT_L4_ENDSESSION == 1);
    assert(LSI_HKPT_L4_RECVING == 2);
    assert(LSI_HKPT_L4_SENDING == 3);

    //Comment: admin listener is not init-ed, so pListener->getSessionHooks() is NULL and
    // then the sessionhooks will be disabled.
    m_sessionHooks.inherit(pListener->getSessionHooks(), 0);

    m_pModuleConfig = pListener->getModuleConfig();

    if (m_sessionHooks.isEnabled(LSI_HKPT_L4_BEGINSESSION))
        m_sessionHooks.runCallbackNoParam(LSI_HKPT_L4_BEGINSESSION, this);

    memset(&m_iInProcess, 0, (char *)(&m_ssl + 1) - (char *)(&m_iInProcess));
    m_iov.clear();
    HttpStats::incIdleConns();
    m_tmToken = NtwkIOLink::getToken();
    if (MultiplexerFactory::getMultiplexer()->add(this,
            POLLIN | POLLHUP | POLLERR) == -1)
        return LS_FAIL;
    //set ssl context
    if (pSSLContext)
    {
        SSL *p = pSSLContext->newSSL();
        if (p)
        {
            ConnLimitCtrl::getInstance().incSSLConn();
            setSSL(p);
            m_ssl.toAccept();
            //acceptSSL();
        }
        else
        {
            LOG_ERR((getLogger(), "newSSL() Failed!", getLogId()));
            return LS_FAIL;
        }
    }
    else
    {
        setNoSSL();
        setupHandler(HIOS_PROTO_HTTP);

    }
    pInfo->incConn();
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] concurrent conn: %d",
               getLogId(), pInfo->getConns()));
    return 0;
}


void NtwkIOLink::drainReadBuf()
{
    //clear the inbound data buffer
    char achDiscard[4096];
    int len = 4096;
    while (len == 4096)
        len = ::read(getfd(), achDiscard, len);
    if (len <= 0)
        closeSocket();
}


void NtwkIOLink::tryRead()
{
    char ch;
    if (::recv(getfd(), &ch, 1, MSG_PEEK) == 1)
        handleEvents(POLLIN);
}


int NtwkIOLink::handleEvents(short evt)
{
    int event = evt;
    if (D_ENABLED(DL_MEDIUM))
        LOG_D((getLogger(), "[%s] NtwkIOLink::handleEvents() events=%d!",
               getLogId(), event));
    if (getState() == HIOS_SHUTDOWN)
    {
        if (event & (POLLHUP | POLLERR))
            closeSocket();
        else
        {
            //resetRevent( POLLIN | POLLOUT );
            if (event & POLLIN)
            {
                if (getFlag(HIO_FLAG_ABORT | HIO_FLAG_PEER_SHUTDOWN))
                    MultiplexerFactory::getMultiplexer()->suspendRead(this);
                else
                    drainReadBuf();
            }
        }
        return 0;
    }
    m_iInProcess = 1;
    if (event & POLLIN)
        (*m_pFpList->m_onRead_fp)(this);
    if (event & (POLLHUP | POLLERR))
    {
        m_iInProcess = 0;
        onPeerClose();
        return 0;
    }
    if (event & POLLOUT)
        (*m_pFpList->m_onWrite_fp)(this);
    m_iInProcess = 0;
    if (getState() >= HIOS_CLOSING)
        onPeerClose();
    return 0;
}


int NtwkIOLink::close()
{
    return (*m_pFpList->m_close_fp)(this);
}


void NtwkIOLink::suspendRead()
{
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] NtwkIOLink::suspendRead()...", getLogId()));
    if (!((isSSL()) && (m_ssl.wantRead())))
        MultiplexerFactory::getMultiplexer()->suspendRead(this);
}


void NtwkIOLink::continueRead()
{
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] NtwkIOLink::continueRead()...", getLogId()));
    setFlag(HIO_FLAG_WANT_READ, 1);
    if ((allowRead()))
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] read resumed!", getLogId()));
        MultiplexerFactory::getMultiplexer()->continueRead(this);
    }
}


void NtwkIOLink::suspendWrite()
{
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] NtwkIOLink::suspendWrite()...", getLogId()));
    setFlag(HIO_FLAG_WANT_WRITE, 0);
    if (!((isSSL()) && (m_ssl.wantWrite())) && m_hasBufferedData == 0)
    {
        MultiplexerFactory::getMultiplexer()->suspendWrite(this);
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] write suspended", getLogId()));
    }
}


void NtwkIOLink::continueWrite()
{
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] NtwkIOLink::continueWrite()...", getLogId()));
    //if( getFlag( HIO_FLAG_WANT_WRITE ) )
    //    return;
    setFlag(HIO_FLAG_WANT_WRITE, 1);
    if (allowWrite())
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] write resumed!", getLogId()));
        /*        short revents = getRevents();
                if ( revents & POLLOUT )
                    handleEvents( revents );
                else*/
        MultiplexerFactory::getMultiplexer()->continueWrite(this);
    }
}


void NtwkIOLink::switchWriteToRead()
{
    setFlag(HIO_FLAG_WANT_READ, 1);
    setFlag(HIO_FLAG_WANT_WRITE, 0);
    MultiplexerFactory::getMultiplexer()->switchWriteToRead(this);
}


///////////////////////////////////////////////////////////////////////
// SSL
///////////////////////////////////////////////////////////////////////

void NtwkIOLink::updateSSLEvent()
{
    if (isWantWrite())
    {
        dumpState("updateSSLEvent", "CW");
        MultiplexerFactory::getMultiplexer()->continueWrite(this);
    }
    if (isWantRead())
    {
        dumpState("updateSSLEvent", "CR");
        MultiplexerFactory::getMultiplexer()->continueRead(this);
    }
}


void NtwkIOLink::checkSSLReadRet(int ret)
{
    if (ret > 0)
    {
        bytesRecv(ret);
        HttpStats::incSSLBytesRead(ret);
        setActiveTime(DateTime::s_curTime);
        //updateSSLEvent();
    }
    else if (!ret)
    {
        if (m_ssl.wantWrite())
        {
            dumpState("checkSSLReadRet", "CW");
            MultiplexerFactory::getMultiplexer()->continueWrite(this);
        }
    }
    else if (getState() != HIOS_SHUTDOWN)
        setState(HIOS_CLOSING);
}


int NtwkIOLink::readExSSL(LsiSession *pIS, char *pBuf, int size)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pIS);
    int ret;
    assert(pBuf);
    ret = pThis->getSSL()->read(pBuf, size);
    pThis->checkSSLReadRet(ret);
    //DEBUG CODE:
//    if ( ret > 0 )
//        ::write( 1, pBuf, ret );
    return ret;
}


int NtwkIOLink::writevExSSL(LsiSession *pOS, const iovec *vector,
                            int count)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pOS);
    int ret = 0;

    const struct iovec *vect;
    const char *pBuf;
    int bufSize;
    int written;

    char *pBufEnd;
    char *pCurEnd;
    char achBuf[4096];
    pBufEnd = achBuf + 4096;
    pCurEnd = achBuf;
    for (int i = 0; i < count ;)
    {
        vect = &vector[i];
        pBuf = (const char *) vect->iov_base;
        bufSize = vect->iov_len;
        if (bufSize < 1024)
        {
            if (pBufEnd - pCurEnd > bufSize)
            {
                memmove(pCurEnd, pBuf, bufSize);
                pCurEnd += bufSize;
                ++i;
                if (i < count)
                    continue;
            }
            pBuf = achBuf;
            bufSize = pCurEnd - pBuf;
            pCurEnd = achBuf;
        }
        else if (pCurEnd != achBuf)
        {
            pBuf = achBuf;
            bufSize = pCurEnd - pBuf;
            pCurEnd = achBuf;
        }
        else
            ++i;
        written = pThis->getSSL()->write(pBuf, bufSize);

        if (D_ENABLED(DL_LESS))
            LOG_D((pThis->getLogger(), "[%s] SSL write() return %d!",
                   pThis->getLogId(), written));

        if (written > 0)
        {
            pThis->bytesSent(written);
            HttpStats::incSSLBytesWritten(written);
            pThis->setActiveTime(DateTime::s_curTime);
            ret += written;
            if (written < bufSize)
            {
                pThis->updateSSLEvent();
                break;
            }
        }
        else if (!written)
        {
            if (pThis->m_ssl.wantRead())
                MultiplexerFactory::getMultiplexer()->continueRead(pThis);
            //pThis->setSSLAgain();
            break;
        }
        else if (pThis->getState() != HIOS_SHUTDOWN)
        {
            if (D_ENABLED(DL_LESS))
                LOG_D((pThis->getLogger(), "[%s] SSL_write() failed: %s "
                       , pThis->getLogId(), SSLError().what()));

            pThis->setState(HIOS_CLOSING);
            return LS_FAIL;
        }
    }
    return ret;
}


void NtwkIOLink::setSSLAgain()
{
    if (m_ssl.wantRead() || getFlag(HIO_FLAG_WANT_READ))
    {
        dumpState("setSSLAgain", "CR");
        MultiplexerFactory::getMultiplexer()->continueRead(this);
    }
    else
    {
        dumpState("setSSLAgain", "SR");
        MultiplexerFactory::getMultiplexer()->suspendRead(this);
    }

    if (m_ssl.wantWrite() || getFlag(HIO_FLAG_WANT_WRITE))
    {
        dumpState("setSSLAgain", "CW");
        MultiplexerFactory::getMultiplexer()->continueWrite(this);
    }
    else
    {
        dumpState("setSSLAgain", "SW");
        MultiplexerFactory::getMultiplexer()->suspendWrite(this);
    }
}


int NtwkIOLink::flush()
{
    int ret;
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] NtwkIOLink::flush...", getLogId()));

//     int nodelay = 1;
//     ::setsockopt( getfd(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof( int ) );

    if (m_hasBufferedData || (m_iHeaderToSend > 0))
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] NtwkIOLink::flush buffered data ...",
                   getLogId()));
        ret = writev_internal(m_iov.get(), m_iov.len(), LSI_CB_FLAG_IN_FLUSH);
        if (m_iHeaderToSend > 0)
        {
            if (ret >= m_iHeaderToSend)
            {
                m_iov.clear();
                m_iHeaderToSend = 0;
            }
            else
            {
                if (ret > 0)
                {
                    m_iHeaderToSend -= ret;
                    m_iov.finish(ret);
                    ret = LS_DONE;
                }
                return ret;
            }
        }
        if (m_hasBufferedData)
            return LS_DONE;
    }

    if (!isSSL())
        return LS_DONE;

    //For SSL part
    ret = getSSL()->flush();
    if (ret == LS_AGAIN)
    {
        if (m_ssl.wantRead())
        {
            dumpState("flush", "CR");
            MultiplexerFactory::getMultiplexer()->continueRead(this);
        }
        else
        {
            dumpState("flush", "CW");
            MultiplexerFactory::getMultiplexer()->continueRead(this);
        }
    }
    else if (ret == LS_FAIL)
        setState(HIOS_CLOSING);

    return ret;
}


int NtwkIOLink::onWriteSSL(NtwkIOLink *pThis)
{
    pThis->dumpState("onWriteSSL", "none");
    if (pThis->m_ssl.wantWrite())
    {
        if (!pThis->m_ssl.isConnected() || (pThis->m_ssl.lastRead()))
        {
            pThis->SSLAgain();
            return 0;
        }
    }
    return pThis->doWrite();
}


int NtwkIOLink::onReadSSL(NtwkIOLink *pThis)
{
    pThis->dumpState("onReadSSL", "none");
    if (pThis->m_ssl.wantRead())
    {
        int last = pThis->m_ssl.lastWrite();
        if (!pThis->m_ssl.isConnected() || (last))
        {
            pThis->SSLAgain();
//            if (( !pThis->m_ssl.isConnected() )||(last ))
            return 0;
        }
    }
    return pThis->doRead();
}


int NtwkIOLink::closeSSL(NtwkIOLink *pThis)
{
    if (D_ENABLED(DL_LESS))
        LOG_D((pThis->getLogger(), "[%s] Shutting down SSL ...",
               pThis->getLogId()));
    pThis->m_ssl.shutdown(0);
    pThis->m_ssl.release();
    ConnLimitCtrl::getInstance().decSSLConn();
    pThis->setNoSSL();
    return close_(pThis);
}


///////////////////////////////////////////////////////////////////////
// Plain
///////////////////////////////////////////////////////////////////////


int NtwkIOLink::close_(NtwkIOLink *pThis)
{
    pThis->setState(HIOS_SHUTDOWN);

    if (pThis->getFlag(HIO_FLAG_PEER_SHUTDOWN | HIO_FLAG_ABORT))
        pThis->closeSocket();
    else
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((pThis->getLogger(), "[%s] Shutting down out-bound socket ...",
                   pThis->getLogId()));
        ::shutdown(pThis->getfd(), SHUT_WR);
        MultiplexerFactory::getMultiplexer()->switchWriteToRead(pThis);
        if (!(pThis->m_iPeerShutdown & IO_COUNTED))
        {
            ConnLimitCtrl::getInstance().decConn();
            pThis->m_pClientInfo->decConn();
            pThis->m_iPeerShutdown |= IO_COUNTED;
            if (D_ENABLED(DL_LESS))
                LOG_D((pThis->getLogger(),
                       "[%s] Available Connections: %d, concurrent conn: %d",
                       pThis->getLogId(),
                       ConnLimitCtrl::getInstance().availConn(),
                       pThis->m_pClientInfo->getConns()));
        }
    }
//    pThis->closeSocket();
    return LS_FAIL;
}


void NtwkIOLink::closeSocket()
{
    if (getfd() == -1)
        return;

    ConnLimitCtrl &ctrl = ConnLimitCtrl::getInstance();

    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] Close socket ...", getLogId()));

    if (m_sessionHooks.isEnabled(LSI_HKPT_L4_ENDSESSION))
        m_sessionHooks.runCallbackNoParam(LSI_HKPT_L4_ENDSESSION, this);

    MultiplexerFactory::getMultiplexer()->remove(this);
    if (m_pFpList == s_pCur_fp_list_list->m_pSSL)
    {
        m_ssl.release();
        ctrl.decSSLConn();
        setNoSSL();
    }
    if (!(m_iPeerShutdown & IO_COUNTED))
    {
        ctrl.decConn();
        m_pClientInfo->decConn();
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(),
                   "[%s] Available Connections: %d, concurrent conn: %d",
                   getLogId(), ctrl.availConn(), m_pClientInfo->getConns()));
    }


    //printf( "socket: %d closed\n", getfd() );
    ::close(getfd());
    setfd(-1);
    m_aioSFQ.pop_all();
    m_hasBufferedData = 0;
    m_pModuleConfig = NULL;
    if (getHandler())
    {
        getHandler()->recycle();
        setHandler(NULL);
    }
    //recycle itself.
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] Recycle NtwkIoLink", getLogId()));
    HttpResourceManager::getInstance().recycle(this);
}


int NtwkIOLink::onRead(NtwkIOLink *pThis)
{
    if (pThis->getHandler())
        return pThis->getHandler()->onReadEx();
    return LS_FAIL;
}


int NtwkIOLink::onWrite(NtwkIOLink *pThis)
{
    return pThis->doWrite();
}


static int matchToken(int token)
{
    if (NtwkIOLink::getPrevToken() < NtwkIOLink::getToken())
        return ((token > NtwkIOLink::getPrevToken())
                && (token <= NtwkIOLink::getToken()));
    else
        return ((token > NtwkIOLink::getPrevToken())
                || (token <= NtwkIOLink::getToken()));
}


void NtwkIOLink::onTimer()
{
    if (matchToken(this->m_tmToken))
    {
        if (this->hasBufferedData() && this->allowWrite())
            this->flush();
        if (m_aioSFQ.size())
        {
            Aiosfcb *cb = (Aiosfcb *)m_aioSFQ.begin();
            if (cb->getFlag(AIOSFCB_FLAG_TRYAGAIN))
                addAioSFJob(cb);
        }
        if (detectClose())
            return;
        (*m_pFpList->m_onTimer_fp)(this);
        if ( getState() == HIOS_CLOSING )
        {
            onPeerClose();
        }
        
    }
}


void NtwkIOLink::onTimer_(NtwkIOLink *pThis)
{
    if (pThis->getHandler())
        pThis->getHandler()->onTimerEx();
}


int NtwkIOLink::checkReadRet(int ret, int size)
{
    //Note: read return 0 means TCP receive FIN, the other side shutdown the write
    //       side of the socket, should leave it alone. Should get other signals
    //       if it is closed. should I?
    //      Content-length must be present in request header, and client
    //      can not shutdown the write side to indicating the end of the request
    //      body, so it is ok to do it.
    if (ret < size)
        resetRevent(POLLIN);
    if (ret > 0)
    {
        bytesRecv(ret);
        HttpStats::incBytesRead(ret);
        setActiveTime(DateTime::s_curTime);
    }
    else if (ret == 0)
    {
        if (getState() != HIOS_SHUTDOWN)
        {
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] End of stream detected, CLOSING!",
                       getLogId()));
            //have the connection closed quickly
            setFlag(HIO_FLAG_PEER_SHUTDOWN, 1);
            setState(HIOS_CLOSING);
        }
        ret = -1;
    }
    else if (ret == -1)
    {
        switch (errno)
        {
        case ECONNRESET:
            //incase client shutdown the writting side after sending the request
            // and waiting for the response, we can't close the connection before
            // we finish write the response back.
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] read error: %s\n",
                       getLogId(), strerror(errno)));
        case EAGAIN:
        case EINTR:
            ret = 0;
            break;
        default:
            if (getState() != HIOS_SHUTDOWN)
                setState(HIOS_CLOSING);
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] read error: %s\n",
                       getLogId(), strerror(errno)));
        }
    }
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] Read from client: %d\n", getLogId(), ret));
    return ret;

}


int NtwkIOLink::readEx(LsiSession *pIS, char *pBuf, int size)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pIS);
    int ret;
    assert(pBuf);
    ret = ::read(pThis->getfd(), pBuf, size);
    ret = pThis->checkReadRet(ret, size);
//    if ( ret > 0 )
//        ::write( 1, pBuf, ret );
    return ret;
}


#if !defined( NO_SENDFILE )

size_t NtwkIOLink::sendfileSetUp(size_t size)
{
    if (m_iHeaderToSend > 0)
    {
        writev(NULL, 0);
        if (m_iHeaderToSend > 0)
            return 0;
    }
    ThrottleControl *pCtrl = getThrottleCtrl();

    if (pCtrl)
    {
        int Quota = pCtrl->getOSQuota();
        if (size > (unsigned int)Quota + (Quota >> 3))
            size = Quota;
    }
    else
        size = size & ((1 << 30) - 1);
    if (size <= 0)
        return 0;

    return size;
}


int NtwkIOLink::sendfileFinish(int written)
{
    int len;
    ThrottleControl *pCtrl = getThrottleCtrl();

#if defined(linux) || defined(__linux) || defined(__linux__) || \
    defined(__gnu_linux__)
    if (written == 0)
    {
        written = -1;
        errno = EPIPE;
    }
#endif
    len = checkWriteRet(written);
    if (pCtrl)
    {
        int Quota = pCtrl->getOSQuota();
        if (Quota - len < 10)
        {
            pCtrl->useOSQuota(Quota);
            MultiplexerFactory::getMultiplexer()->suspendWrite(this);
        }
        else
            pCtrl->useOSQuota(len);
    }
    return len;
}


int NtwkIOLink::sendfile(int fdSrc, off_t off, size_t size)
{
    int written;

    if ((size = sendfileSetUp(size)) == 0)
        return 0;
    written = gsendfile(getfd(), fdSrc, &off, size);

    return sendfileFinish(written);
}


int NtwkIOLink::addAioSFJob(Aiosfcb *cb)
{
    int ret = HttpAioSendFile::getHttpAioSendFile()->addJob(cb);
    if (ret)
    {
        cb->setFlag(AIOSFCB_FLAG_TRYAGAIN);
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] Add Job Failed, Try Again Flag Set.",
                   getLogId()));
    }
    else
        cb->clearFlag(AIOSFCB_FLAG_TRYAGAIN);
    return ret;
}


int NtwkIOLink::aiosendfile(Aiosfcb *cb)
{
    size_t size = cb->getSize();

    if ((size = sendfileSetUp(size)) == 0)
        return 0;

    cb->setSize(size);
    cb->setSendFd(getfd());
    m_aioSFQ.append(cb);

    if (m_aioSFQ.size() == 1)
        addAioSFJob(cb);
    else if (D_ENABLED(DL_MEDIUM))
        LOG_D((getLogger(), "[%s] ntwkiolink busy with another session\n",
               getLogId()));
    return 1;
}


int NtwkIOLink::aiosendfiledone(Aiosfcb *cb)
{
    int len, written = cb->getRet();
    Aiosfcb *pDone = (Aiosfcb *)m_aioSFQ.pop_front();
    if (pDone != cb)
        return LS_FAIL;

    len = sendfileFinish(written);

    if (m_aioSFQ.size() != 0)
        addAioSFJob((Aiosfcb *)m_aioSFQ.begin());

    return (len < 0 ? -1 : 0);
}


#endif


int NtwkIOLink::writevEx(LsiSession *pOS, const iovec *vector, int count)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pOS);
    int len = ::writev(pThis->getfd(), vector, count);
    len = pThis->checkWriteRet(len);
    //if (pThis->wantWrite() && pThis->m_hasBufferedData)
    //    MultiplexerFactory::getMultiplexer()->continueWrite( pThis );

    //TEST: debug code
//    if ( len > 0 )
//    {
//        int left = len;
//        const struct iovec* pVec = vector.get();
//        while( left > 0 )
//        {
//            int writeLen = pVec->iov_len;
//            if ( writeLen > left )
//                writeLen = left;
//            ::write( 1, pVec->iov_base, writeLen );
//            ++pVec;
//            left -= writeLen;
//        }
//    }
    return len;

}


int NtwkIOLink::sendRespHeaders(HttpRespHeaders *pHeader)
{
    if (pHeader)
    {
        pHeader->outputNonSpdyHeaders(&m_iov);
        m_iHeaderToSend = pHeader->getTotalLen();
    }
    return 0;
}


int NtwkIOLink::checkWriteRet(int len)
{
    if (len > 0)
    {
        bytesSent(len);
        HttpStats::incBytesWritten(len);
        setActiveTime(DateTime::s_curTime);
    }
    else if (len == -1)
    {
        switch (errno)
        {
        case EINTR:
        case EAGAIN:
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] [write] errstr=%s!\n", getLogId(),
                       strerror(errno)));
            len = 0;
            break;
        default:
            if (getState() != HIOS_SHUTDOWN)
            {
                if (m_hasBufferedData == 0)
                {
                    setState(HIOS_CLOSING);
                    setFlag(HIO_FLAG_ABORT, 1);
                }
            }
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] write error: %s\n",
                       getLogId(), strerror(errno)));
        }
    }
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(),  "[%s] Written to client: %d\n", getLogId(), len));
    return len;
}


int NtwkIOLink::detectClose()
{
    if (getState() == HIOS_SHUTDOWN)
    {
        if (D_ENABLED(DL_MEDIUM))
            LOG_D((getLogger(), "[%s] Shutdown time out!", getLogId()));
        closeSocket();
    }
    else if (getState() == HIOS_CONNECTED)
    {
        char ch;
        if ((getClientInfo()->getAccess() == AC_BLOCK) ||
            ((DateTime::s_curTime - getActiveTime() > 10) &&
             (::recv(getfd(), &ch, 1, MSG_PEEK) == 0)))
        {
            if (D_ENABLED(DL_LESS))
                LOG_D((getLogger(), "[%s] peer connection close detected!\n", getLogId()));
            //have the connection closed faster
            onPeerClose();
            return 1;
        }
    }
    return 0;
}


int NtwkIOLink::detectCloseNow()
{
    char ch;
    if (::recv(getfd(), &ch, 1, MSG_PEEK) == 0)
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] peer connection close detected!\n", getLogId()));
        //have the connection closed faster
        setFlag(HIO_FLAG_PEER_SHUTDOWN, 1);
        close();
        return 1;
    }
    return 0;
}


///////////////////////////////////////////////////////////////////////
// Throttle
///////////////////////////////////////////////////////////////////////

int NtwkIOLink::onReadT(NtwkIOLink *pThis)
{
    return pThis->doReadT();
}


int NtwkIOLink::onWriteT(NtwkIOLink *pThis)
{
    if (pThis->allowWrite())
        return pThis->doWrite();
    else
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
    return 0;
}


void NtwkIOLink::dumpState(const char *pFuncName, const char *action)
{
    if (D_ENABLED(DL_MORE))
        LOG_D((getLogger(),  "[%s] %s(), %s, wantRead: %d, wantWrite: %d,"
               " allowWrite: %d, allowRead: %d,"
               " m_ssl.wantRead: %d, m_ssl.wantWrite: %d, "
               " m_ssl.lastRead: %d, m_ssl.lastWrite: %d",
               getLogId(),  pFuncName, action, isWantRead(), isWantWrite(),
               allowWrite(), allowRead(),
               m_ssl.wantRead(), m_ssl.wantWrite(),
               m_ssl.lastRead(), m_ssl.lastWrite()
              ));

}


void NtwkIOLink::onTimer_T(NtwkIOLink *pThis)
{
//    if ( D_ENABLED( DL_MORE ))
//        LOG_D(( pThis->getLogger(),  "[%s] conn token:%d, global Token: %d\n",
//                    pThis->getLogId(), pThis->m_tmToken, HttpGlobals::s_tmToken ));
//        if ( D_ENABLED( DL_MORE ))
//            LOG_D(( pThis->getLogger(),  "[%s] output avail:%d. state: %d \n",
//                    pThis->getLogId(),
//                    pThis->getClientInfo()->getThrottleCtrl().getOSQuota(),
//                    pThis->getState() ));

    if (pThis->hasBufferedData() && pThis->allowWrite())
        pThis->flush();

    if (pThis->allowWrite() && pThis->isWantWrite())
    {
        //pThis->doWrite();
        //if (  pThis->allowWrite() && pThis->wantWrite() )
        pThis->dumpState("onTimer_T", "CW");
        MultiplexerFactory::getMultiplexer()->continueWrite(pThis);
    }
    if (pThis->allowRead() && pThis->isWantRead())
    {
        //if ( pThis->getState() != HSS_WAITING )
        //    pThis->doReadT();
        //if ( pThis->allowRead() && pThis->wantRead() )
        pThis->dumpState("onTimer_T", "CR");
        MultiplexerFactory::getMultiplexer()->continueRead(pThis);
    }
    if (pThis->getHandler())
        pThis->getHandler()->onTimerEx();

}


int NtwkIOLink::readExT(LsiSession *pIS, char *pBuf, int size)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pIS);
    ThrottleControl *pTC = pThis->getThrottleCtrl();
    int iQuota = pTC->getISQuota();
    if (iQuota <= 0)
    {
        pThis->dumpState("readExT", "SR");
        MultiplexerFactory::getMultiplexer()->suspendRead(pThis);
        return 0;
    }
    if (size > iQuota)
        size = iQuota;
    assert(pBuf);
    int ret = ::read(pThis->getfd(), pBuf, size);
    ret = pThis->checkReadRet(ret, size);
    if (ret > 0)
    {
        pTC->useISQuota(ret);
//        ::write( 1, pBuf, ret );
        if (!pTC->getISQuota())
        {
            pThis->dumpState("readExT", "SR");
            MultiplexerFactory::getMultiplexer()->suspendRead(pThis);
        }
    }
    return ret;
}


int NtwkIOLink::writevExT(LsiSession *pOS, const iovec *vector, int count)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pOS);
    int len = 0;
    ThrottleControl *pCtrl = pThis->getThrottleCtrl();
    int Quota = pCtrl->getOSQuota();
    if (Quota <= 0)
    {
        pThis->dumpState("writevExT", "SW");
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
        return 0;
    }

    int total = 0;
    for (int i = 0; i < count; ++i)
        total += vector[i].iov_len;

//    if ( D_ENABLED( DL_LESS ))
//        LOG_D(( pThis->getLogger(),  "[%s] Quota:%d, to write: %d\n",
//                    pThis->getLogId(), Quota, total ));
    if ((unsigned int)total > (unsigned int)Quota + (Quota >> 3))
    {
        IOVec iov;
        iov.append(vector, count);
        total = iov.shrinkTo(Quota, Quota >> 3);
        len = ::writev(pThis->getfd(), iov.begin(), iov.len());
    }
    else
        len = ::writev(pThis->getfd(), vector, count);

    len = pThis->checkWriteRet(len);
    if (Quota - len < 10)
    {
        pCtrl->useOSQuota(Quota);
        pThis->dumpState("writevExT", "SW");
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
    }
    else
        pCtrl->useOSQuota(len);
    return len;

}


///////////////////////////////////////////////////////////////////////
// Throttle + SSL
///////////////////////////////////////////////////////////////////////


//void NtwkIOLink::onTimerSSL_T( NtwkIOLink * pThis )
//{
//    if ( pThis->detectClose() )
//        return;
//    if ( pThis->allowWrite() &&
//        (( pThis->m_ssl.wantWrite() )||pThis->wantWrite() ))
//        pThis->doWriteT();
//    if ( pThis->allowRead() &&
//        (( pThis->m_ssl.wantRead() )||pThis->wantRead() ))
//    {
//        pThis->doReadT();
//    }
//    pThis->onTimerEx();
//}


void NtwkIOLink::onTimerSSL_T(NtwkIOLink *pThis)
{
    if (pThis->allowWrite() && (pThis->m_ssl.wantWrite()))
        onWriteSSL_T(pThis);
    if (pThis->allowRead() && (pThis->m_ssl.wantRead()))
        onReadSSL_T(pThis);
    if (pThis->allowWrite() && pThis->isWantWrite())
    {
        pThis->doWrite();
        if (pThis->allowWrite() && pThis->isWantWrite())
        {
            pThis->dumpState("onTimerSSL_T", "CW");
            MultiplexerFactory::getMultiplexer()->continueWrite(pThis);
        }
    }
    if (pThis->allowRead() && pThis->isWantRead())
    {
        //if ( pThis->getState() != HSS_WAITING )
        //    pThis->doReadT();
        //if ( pThis->allowRead() && pThis->wantRead() )
        pThis->dumpState("onTimerSSL_T", "CR");
        MultiplexerFactory::getMultiplexer()->continueRead(pThis);
    }
    if (pThis->getHandler())
        pThis->getHandler()->onTimerEx();
}


int NtwkIOLink::onReadSSL_T(NtwkIOLink *pThis)
{
    //pThis->dumpState( "onReadSSL_t", "none" );
    if (pThis->m_ssl.wantRead())
    {
        int last = pThis->m_ssl.lastWrite();
        if (!pThis->m_ssl.isConnected() || (last))
        {
            pThis->SSLAgain();
//            if (( !pThis->m_ssl.isConnected() )||(last ))
            return 0;
        }
    }
    return pThis->doReadT();
}


int NtwkIOLink::onWriteSSL_T(NtwkIOLink *pThis)
{
    //pThis->dumpState( "onWriteSSL_T", "none" );
    if (pThis->m_ssl.wantWrite())
    {
        int last = pThis->m_ssl.lastRead();
        if (!pThis->m_ssl.isConnected() || (last))
        {
            pThis->SSLAgain();
//            if (( !pThis->m_ssl.isConnected() )||(last ))
            return 0;
        }

    }
    if (pThis->allowWrite())
        return pThis->doWrite();
    else
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
    return 0;
}


static char s_errUseSSL[] =
    "HTTP/1.0 200 OK\r\n"
    "Cache-Control: private, no-cache, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: Close\r\n\r\n"
    "<html><head><title>400 Bad Request</title></head><body>\n"
    "<h2>HTTPS is required</h2>\n"
    "<p>This is an SSL protected page, please use the HTTPS scheme instead of "
    "the plain HTTP scheme to access this URL.<br />\n"
    "<blockquote>Hint: The URL should starts with <b>https</b>://</blockquote> </p>\n"
    "<hr />\n"
    "Powered By LiteSpeed Web Server<br />\n"
    "<a href='http://www.litespeedtech.com'><i>http://www.litespeedtech.com</i></a>\n"
    "</body></html>\n";

static char s_redirectSSL1[] =
    "HTTP/1.0 301 Moved Permanently\r\n"
    "Location: https://";

static char s_redirectSSL2[] =
    "\r\nCache-Control: private, no-cache, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Server:LiteSpeed\r\n"
    "Content-Length: 0\r\n"
    "Connection: Close\r\n\r\n";


int NtwkIOLink::get_url_from_reqheader(char *buf, int length, char **puri,
                                       int *uri_len, char **phost, int *host_len)
{
    const char *pBufEnd = (const char *)buf + length;
    if (strncasecmp(buf, "GET ", 4) != 0 &&
        strncasecmp(buf, "POST", 4) != 0 &&
        strncasecmp(buf, "HEAD", 4) != 0)
        return LS_FAIL;

    char *pStart = strcasestr(buf, (const char *)"host:");
    if (!pStart || pBufEnd - pStart < 6)
        return -2;

    *phost = pStart + 5;
    char *pEnd = (char *)memchr(*phost, '\n', pBufEnd - *phost);
    if (!pEnd)
        return -3;

    *pEnd = 0x00;
    *phost = StringTool::strTrim(*phost);
    *host_len = strlen(*phost);

    *puri = buf + 4;
    //Must remove the leading space to avoid pEnd to become it.
    skip_leading_space((const char **)puri);

    //Only search the puri(buf +4) to phost area for space
    pEnd = (char *)memchr(*puri, ' ', *phost - *puri);
    if (!pEnd)
        return -4;

    skip_leading_space((const char **)&pEnd);
    if (strncasecmp(pEnd, "HTTP/", 5) != 0)
        return -5;

    *pEnd = 0x00; //set 0 to end of the puri
    *puri = StringTool::strTrim(*puri);
    *uri_len = strlen(*puri);
    return 0;
}


void NtwkIOLink::handle_acceptSSL_EIO_Err()
{
    //The buf is null terminated string
    char buf[8192 + 1] = {0};
    unsigned int offset = 0, length = 0;
    offset = m_ssl.getSSL()->packet_length;
    if (offset > 8192)
        offset = 8192;
    memcpy(buf, (char *)m_ssl.getSSL()->packet, offset);
    length = offset;

    //Normally it is 11 bytes, checking with 8192, just to avoid buffer overflow
    if (offset < 8192)
    {
        int ret = ::read(getfd(), buf + offset, 8192 - offset);
        if (ret > 0)
            length += ret;
    }

    int uri_len = 0, host_len = 0;
    char *puri = NULL, *phost = NULL;
    int rc_parse = get_url_from_reqheader(buf, length, &puri, &uri_len, &phost,
                                          &host_len);
    if (rc_parse == 0)
    {
        iovec iov[4] = {{s_redirectSSL1, sizeof(s_redirectSSL1) - 1},
            {phost, (size_t)host_len},
            {puri, (size_t)uri_len},
            {s_redirectSSL2, sizeof(s_redirectSSL2) - 1}
        };
        ::writev(getfd(), iov, 4);
    }
    else
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(),
                   "[%s] SSL_accept() failed!: %s, get_url_from_reqheader return %d.",
                   getLogId(), SSLError().what(), rc_parse));
        ::write(getfd(), s_errUseSSL, sizeof(s_errUseSSL) - 1);
    }
}


int NtwkIOLink::acceptSSL()
{
    int ret = m_ssl.accept();
    if (ret == 1)
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] [SSL] accepted!\n", getLogId()));
        if ((ClientInfo::getPerClientHardLimit() < 1000)
            && (m_pClientInfo->getAccess() != AC_TRUST)
            && (m_pClientInfo->incSslNewConn() >
                (ClientInfo::getPerClientHardLimit() << 1)))
        {
            LOG_WARN((getLogger(), "[%s] [SSL] too many new SSL connections: %d, "
                      "possible SSL negociation based attack, block!", getLogId(),
                      m_pClientInfo->getSslNewConn()));
            m_pClientInfo->setOverLimitTime(DateTime::s_curTime);
            m_pClientInfo->setAccess(AC_BLOCK);
        }

    }
    else if (errno == EIO)
        handle_acceptSSL_EIO_Err();

    return ret;
}


int NtwkIOLink::sslSetupHandler()
{
    unsigned int spdyVer = m_ssl.getSpdyVersion();
    if (spdyVer >= HIOS_PROTO_MAX)
    {
        LOG_ERR((getLogger(), "[%s] bad SPDY version: %d, use HTTP", getLogId(),
                 spdyVer));
        spdyVer = HIOS_PROTO_HTTP;
    }
    else
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] Next Protocol Negociation result: %s\n",
                   getLogId(), getProtocolName((HiosProtocol)spdyVer)));
    }
    return setupHandler((HiosProtocol)spdyVer);
}


int NtwkIOLink::SSLAgain()
{
    if (D_ENABLED(DL_LESS))
        LOG_D((getLogger(), "[%s] [SSL] SSLAgain()!\n", getLogId()));
    int ret = 0;
    switch (m_ssl.getStatus())
    {
    case SSLConnection::CONNECTING:
        ret = m_ssl.connect();
        break;
    case SSLConnection::ACCEPTING:
        ret = acceptSSL();
        if (ret == 1)
            sslSetupHandler();
        break;
    case SSLConnection::SHUTDOWN:
        ret = m_ssl.shutdown(1);
        break;
    case SSLConnection::CONNECTED:
        if (m_ssl.lastRead())
        {
            if (getHandler())
                return getHandler()->onReadEx();
            else
                return LS_FAIL;
        }
        if (m_ssl.lastWrite())
            return doWrite();
    }
    switch (ret)
    {
    case 0:
        setSSLAgain();
        break;
    case -1:
        setState(HIOS_CLOSING);
        break;
    }
    return ret;

}


int NtwkIOLink::readExSSL_T(LsiSession *pIS, char *pBuf, int size)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pIS);
    ThrottleControl *pTC = pThis->getThrottleCtrl();
    int iQuota = pTC->getISQuota();
    if (iQuota <= 0)
    {
        MultiplexerFactory::getMultiplexer()->suspendRead(pThis);
        return 0;
    }
    if (size > iQuota)
        size = iQuota;
    pThis->m_iPeerShutdown &= ~IO_THROTTLE_READ;
    int ret = pThis->getSSL()->read(pBuf, size);
    pThis->checkSSLReadRet(ret);
    if (ret > 0)
    {
        pTC->useISQuota(ret);
        //::write( 1, pBuf, ret );
        if (!pTC->getISQuota())
        {
            MultiplexerFactory::getMultiplexer()->suspendRead(pThis);
            pThis->m_iPeerShutdown |= IO_THROTTLE_READ;
        }
    }
    return ret;
}


int NtwkIOLink::writevExSSL_T(LsiSession *pOS, const iovec *vector,
                              int count)
{
    NtwkIOLink *pThis = static_cast<NtwkIOLink *>(pOS);
    ThrottleControl *pCtrl = pThis->getThrottleCtrl();
    int Quota = pCtrl->getOSQuota();
    if (Quota <= pThis->m_iSslLastWrite / 2)
    {
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
        return 0;
    }
    unsigned int allowed = (unsigned int)Quota + (Quota >> 3);
    int ret = 0;
    const struct iovec *vect = vector;
    const struct iovec *pEnd = vector + count;

    //Make OpenSSL happy, not to retry with smaller buffer
    if (Quota < pThis->m_iSslLastWrite)
        Quota = allowed = pThis->m_iSslLastWrite;
    char *pBufEnd;
    char *pCurEnd;
    char achBuf[4096];
    pBufEnd = achBuf + 4096;
    pCurEnd = achBuf;
    for (; ret < Quota && vect < pEnd;)
    {
        const char *pBuf = (const char *) vect->iov_base;
        int bufSize;
        // Use "<=" instead of "<", may get access violation
        // when ret = 0, and allowed = vect->iov_len
        if (vect->iov_len <= allowed - ret)
            bufSize = vect->iov_len;
        else
        {
            bufSize = Quota - ret;
            if (*(pBuf + bufSize) == '\n')
                ++bufSize;
        }
        if (bufSize < 1024)
        {
            if (pBufEnd - pCurEnd > bufSize)
            {
                memmove(pCurEnd, pBuf, bufSize);
                pCurEnd += bufSize;
                ++vect;
                if ((vect < pEnd) && (ret + (pCurEnd - achBuf) < Quota))
                    continue;
            }
            pBuf = achBuf;
            bufSize = pCurEnd - pBuf;
            pCurEnd = achBuf;
        }
        else if (pCurEnd != achBuf)
        {
            pBuf = achBuf;
            bufSize = pCurEnd - pBuf;
            pCurEnd = achBuf;
        }
        else
            ++vect;
        int written = pThis->getSSL()->write(pBuf, bufSize);
        if (D_ENABLED(DL_MORE))
            LOG_D(("[%s] to write %d bytes, written %d bytes",
                   pThis->getLogId(), bufSize, written));
        if (written > 0)
        {
            pThis->bytesSent(written);
            HttpStats::incSSLBytesWritten(written);
            pThis->m_iSslLastWrite = 0;
            pThis->setActiveTime(DateTime::s_curTime);
            ret += written;
            if (written < bufSize)
            {
                pThis->updateSSLEvent();
                break;
            }
        }
        else if (!written)
        {
            pThis->m_iSslLastWrite = bufSize;
            pThis->setSSLAgain();
            break;
        }
        else if (pThis->getState() != HIOS_SHUTDOWN)
        {
            if (D_ENABLED(DL_MORE))
                LOG_D(("[%s] SSL error: %s, mark connection to be closed",
                       pThis->getLogId(), SSLError().what()));
            pThis->setState(HIOS_CLOSING);
            return LS_FAIL;
        }
    }
    if (Quota - ret < 10)
    {
        pCtrl->useOSQuota(Quota);
        MultiplexerFactory::getMultiplexer()->suspendWrite(pThis);
        pThis->m_iPeerShutdown |= IO_THROTTLE_WRITE;
    }
    else
    {
        pCtrl->useOSQuota(ret);
        pThis->m_iPeerShutdown &= ~IO_THROTTLE_WRITE;
    }
    return ret;
}


void NtwkIOLink::suspendEventNotify()
{
    if (!MultiplexerFactory::s_iMultiplexerType)
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] remove fd:%d from multiplexer!\n", getLogId(),
                   getfd()));
        MultiplexerFactory::getMultiplexer()->remove(this);
    }
}


void NtwkIOLink::resumeEventNotify()
{
    if (!MultiplexerFactory::s_iMultiplexerType)
    {
        if (D_ENABLED(DL_LESS))
            LOG_D((getLogger(), "[%s] add fd:%d back to multiplexer!\n", getLogId(),
                   getfd()));
        MultiplexerFactory::getMultiplexer()->add(this, POLLHUP | POLLERR);
    }
}


void NtwkIOLink::changeClientInfo(ClientInfo *pInfo)
{
    if (pInfo == m_pClientInfo)
        return;
    m_pClientInfo->decConn();
    pInfo->incConn();
    m_pClientInfo = pInfo;
}


static const char *s_pProtoString[] = { "", ":SPDY2", ":SPDY3", ":SPDY31", ":HTTP2" };
const char *NtwkIOLink::buildLogId()
{
    AutoStr2 &id = getIdBuf();

    int len ;
    char *p = id.buf();
    len = ls_snprintf(id.buf(), MAX_LOGID_LEN, "%s:%hu%s",
                      m_pClientInfo->getAddrString(), getRemotePort(),
                      s_pProtoString[(int)getProtocol() ]);
    id.setLen(len);
    p += len;

    return id.c_str();
}



