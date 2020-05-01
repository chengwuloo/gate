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
	rsp.addHeader("Server", "MUDUO");
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

static inline void onTimeoutExpired(const muduo::net::TcpConnectionPtr& conn, Entry::TypeE ty) {
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
	//-------------------------------------------------------------------------------------------------------------------------------------
	//  1.弹出bucket，EntryPtr引用计数递减为0
	//   (conn有效，此时已经连接超时，因为业务处理队列繁忙以致无法持有EntryPtr，进而锁定同步业务操作)
	//   (conn失效，此时连接已经关闭，可能是业务处理完毕或其它非法原因导致连接被提前关闭，但是由于Bucket一直持有EntryPtr引用计数而无法触发析构，直到timeout过期)
	//-------------------------------------------------------------------------------------------------------------------------------------
	//  2.业务处理完毕，持有EntryPtr业务处理函数退出离开其作用域，EntryPtr引用计数递减为0
	//   (此时早已连接超时，并已弹出bucket，引用计数递减但不等于0，因为业务处理函数持有EntryPtr，锁定了同步业务操作，直到业务处理完毕，引用计数递减为0触发析构)
	muduo::net::TcpConnectionPtr conn(weakConn_.lock());
	if (conn) {
		//conn->getLoop()->assertInLoopThread();
		
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
		//assert(!entryContext->getSession().empty());
		
		//判断是否锁定了同步业务操作
		switch (getLocked()) {
		case true: {
			//////////////////////////////////////////////////////////////////////////
			//早已连接超时，业务处理完毕，响应客户端时间(>timeout)
			//////////////////////////////////////////////////////////////////////////
			LOG_WARN << __FUNCTION__ << " "
				<< peerName_ << "[" << conn->peerAddress().toIpPort() << "] -> "
				<< localName_ << "[" << conn->localAddress().toIpPort() << "] Entry::dtor["
				<< entryContext->getSession() << "] finished processing";
			break;
		}
		default: {
			//////////////////////////////////////////////////////////////////////////
			//已经连接超时，没有业务处理，响应客户端时间(<timeout)
			//////////////////////////////////////////////////////////////////////////
			LOG_WARN << __FUNCTION__ << " "
				<< peerName_ << "[" << conn->peerAddress().toIpPort() << "] -> "
				<< localName_ << "[" << conn->localAddress().toIpPort() << "] Entry::dtor["
				<< entryContext->getSession() << "] timeout closing";
			//连接超时过期处理
			onTimeoutExpired(conn, ty_);
			break;
		}
		}
	}
	else {
		switch (getLocked()) {
		case true: {
			//////////////////////////////////////////////////////////////////////////
			//业务处理完毕，连接被提前关闭，响应客户端时间(<timeout)
			//////////////////////////////////////////////////////////////////////////
			//LOG_WARN << __FUNCTION__ << " Entry::dtor - ahead of finished processing";
			break;
		}
		default: {
			//////////////////////////////////////////////////////////////////////////
			//其它非法原因，连接被提前关闭，响应客户端时间(<timeout)
			//////////////////////////////////////////////////////////////////////////
			//LOG_WARN << __FUNCTION__ << " Entry::dtor - unknown closed";
			break;
		}
		}
	}
}