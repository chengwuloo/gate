/************************************************************************/
/*    @author create by andy_ro@qq.com                                  */
/*    @Date		   03.18.2020                                           */
/************************************************************************/
#include "EntryPtr.h"

Entry::~Entry() {
	muduo::net::TcpConnectionPtr conn(weakConn_.lock());
	if (conn) {
		conn->getLoop()->assertInLoopThread();
#ifdef _DEBUG_BUCKETS_
		LOG_WARN << __FUNCTION__ << " 客户端[" << conn->peerAddress().toIpPort() << "] -> 网关服["
			<< conn->localAddress().toIpPort() << "] timeout closing";
#endif
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
		LOG_WARN << __FUNCTION__ << " Entry::dtor[" << entryContext->getSession() << "]";
#if 0
		//不再发送数据
		conn->shutdown();
#elif 1
		//直接强制关闭连接
		conn->forceClose();
#else
		//延迟0.2s强制关闭连接
		conn->forceCloseWithDelay(0.2f);
#endif
	}
}