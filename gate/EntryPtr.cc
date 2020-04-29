/************************************************************************/
/*    @author create by andy_ro@qq.com                                  */
/*    @Date		   03.18.2020                                           */
/************************************************************************/
#include "EntryPtr.h"

#include <muduo/net/http/HttpContext.h>
#include <muduo/net/http/HttpRequest.h>
#include <muduo/net/http/HttpResponse.h>

static void setFailedResponse(muduo::net::HttpResponse& rsp,
	muduo::net::HttpResponse::HttpStatusCode code = muduo::net::HttpResponse::k200Ok,
	std::string const& msg = "") {
	rsp.setStatusCode(code);
	rsp.setStatusMessage("OK");
	rsp.addHeader("Server", "TXQP");
#if 0
	rsp.setContentType("text/html;charset=utf-8");
	rsp.setBody("<html><body>" + msg + "</body></html>");
#elif 0
	rsp.setContentType("application/xml;charset=utf-8");
	rsp.setBody(msg);
#else
	rsp.setContentType("text/plain;charset=utf-8");
	rsp.setBody(msg);
#endif
}

static inline void onTimeout(const muduo::net::TcpConnectionPtr& conn, Entry::TypeE ty) {
	switch (ty) {
	case Entry::TypeE::HttpTy: {
		//HTTP应答包(header/body)
		muduo::net::HttpResponse rsp(true);
		setFailedResponse(rsp,
			muduo::net::HttpResponse::k404NotFound,
			"HTTP/1.1 505 timeout\r\n\r\n");
		muduo::net::Buffer buf;
		rsp.appendToBuffer(&buf);
		//发送完毕，关闭连接
		conn->send(&buf);
#if 0
		//不再发送数据
		conn->shutdown();
#elif 0
		//直接强制关闭连接
		conn->forceClose();
#elif 0
		//延迟0.2s强制关闭连接
		conn->forceCloseWithDelay(0.2f);
#endif
		break;
	}
	case Entry::TypeE::TcpTy: {
#if 0
		//不再发送数据
		conn->shutdown();
#elif 1
		//直接强制关闭连接
		conn->forceClose();
#elif 0
		//延迟0.2s强制关闭连接
		conn->forceCloseWithDelay(0.2f);
#endif
		break;
	}
	default: assert(false); break;
	}
}

//连接超时处理机制
Entry::~Entry() {
	//触发析构调用销毁对象释放资源，有以下两种可能：
	//----------------------------------------------------------------------------------------------------------------------------------
	//  1.连接超时弹出bucket，EntryPtr引用计数递减为0
	//   (此时已经连接超时，因为业务处理队列繁忙以致无法持有EntryPtr，进而锁定同步业务操作)
	//----------------------------------------------------------------------------------------------------------------------------------
	//  2.业务处理完毕，持有EntryPtr业务处理函数退出离开其作用域，EntryPtr引用计数递减为0
	//   (此时早已连接超时，并已弹出bucket，引用计数递减但不等于0，因为业务处理函数持有EntryPtr，锁定了同步业务操作，直到业务处理完毕，引用计数递减为0触发析构)
	muduo::net::TcpConnectionPtr conn(weakConn_.lock());
	if (conn) {
		conn->getLoop()->assertInLoopThread();
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
		if (entryContext->getSession().empty()) {
			//////////////////////////////////////////////////////////////////////////
			//已经连接超时，没有业务处理，响应客户端时间(<timeout)
			//////////////////////////////////////////////////////////////////////////
			LOG_WARN << __FUNCTION__ << " "
				<< peerName_ << "[" << conn->peerAddress().toIpPort() << "] -> "
				<< localName_ << "[" << conn->localAddress().toIpPort() << "] Entry::dtor["
				<< entryContext->getSession() << "] timeout closing";
			onTimeout(conn, ty_);
		}
		else {
			//////////////////////////////////////////////////////////////////////////
			//早已连接超时，业务处理完毕，响应客户端时间(>timeout)
			//////////////////////////////////////////////////////////////////////////
			LOG_WARN << __FUNCTION__ << " "
				<< peerName_ << "[" << conn->peerAddress().toIpPort() << "] -> "
				<< localName_ << "[" << conn->localAddress().toIpPort() << "] Entry::dtor["
				<< entryContext->getSession() << "] finished processing";
		}
	}
	else {
		//LOG_WARN << __FUNCTION__ << " Entry::dtor";
	}
}