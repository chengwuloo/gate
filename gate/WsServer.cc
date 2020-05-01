/************************************************************************/
/*    @author create by andy_ro@qq.com                                  */
/*    @Date		   03.18.2020                                           */
/************************************************************************/
#include <sstream>
#include <fstream>
#include <functional>
#include <sys/types.h>

#include <muduo/net/Reactor.h>
#include <muduo/net/libwebsocket/context.h>
#include <muduo/net/libwebsocket/server.h>
#include <muduo/net/libwebsocket/ssl.h>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/algorithm/algorithm.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/proto/detail/ignore_unused.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time.hpp>
#include <boost/thread.hpp>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

#include "proto/Game.Common.pb.h"
#include "proto/ProxyServer.Message.pb.h"
#include "proto/HallServer.Message.pb.h"
#include "proto/GameServer.Message.pb.h"

#include "public/SubNetIP.h"
#include "public/NetCardIP.h"
#include "public/utils.h"

#include "public/codec/aes.h"
#include "public/codec/mymd5.h"
#include "public/codec/base64.h"
#include "public/codec/htmlcodec.h"
#include "public/codec/urlcodec.h"

#include "Gateway.h"

//网关服[S]端 <- 客户端[C]端，websocket
bool Gateway::onCondition(const muduo::net::InetAddress& peerAddr) {
	return true;
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::onConnection(const muduo::net::TcpConnectionPtr& conn) {
	
	conn->getLoop()->assertInLoopThread();
	
	if (conn->connected()) {
		int32_t num = numConnected_.incrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "客户端[" << conn->peerAddress().toIpPort() << "] -> 网关服["
			<< conn->localAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
		
		//累计接收请求数
		numTotalReq_.incrementAndGet();

		//最大连接数限制
		if (num > kMaxConnections_) {
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
			//会调用onMessage函数
			assert(conn->getContext().empty());

			//累计未处理请求数
			numTotalBadReq_.incrementAndGet();
			return;
		}
		//////////////////////////////////////////////////////////////////////////
		//websocket::Context::ctor
		//////////////////////////////////////////////////////////////////////////
		muduo::net::websocket::hook(
			std::bind(&Gateway::onConnected, this,
				std::placeholders::_1, std::placeholders::_2),
			std::bind(&Gateway::onMessage, this,
				std::placeholders::_1, std::placeholders::_2,
				std::placeholders::_3, std::placeholders::_4),
			conn);

		EventLoopContextPtr context = boost::any_cast<EventLoopContextPtr>(conn->getLoop()->getContext());
		assert(context);
		
		EntryPtr entry(new Entry(Entry::TypeE::TcpTy, muduo::net::WeakTcpConnectionPtr(conn), "客户端", "网关服"));
		
		//指定conn上下文信息
		ContextPtr entryContext(new Context(WeakEntryPtr(entry)));
		conn->setContext(entryContext);
		{
			//给新conn绑定一个worker线程，与之相关所有逻辑业务都在该worker线程中处理
			int index = context->allocWorkerIndex();
			assert(index >= 0 && index < threadPool_.size());
			entryContext->setWorkerIndex(index);
		}
		{
			//获取EventLoop关联的Bucket
			int index = context->getBucketIndex();
			assert(index >= 0 && index < bucketsPool_.size());
			//连接成功，压入桶元素
			RunInLoop(conn->getLoop(),
				std::bind(&ConnBucket::pushBucket, bucketsPool_[index].get(), entry));
		}
		{
			//TCP_NODELAY
			conn->setTcpNoDelay(true);
		}
	}
	else {
		int32_t num = numConnected_.decrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "客户端[" << conn->peerAddress().toIpPort() << "] -> 网关服["
			<< conn->localAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
		assert(!conn->getContext().empty());
		//////////////////////////////////////////////////////////////////////////
		//websocket::Context::dtor
		//////////////////////////////////////////////////////////////////////////
		muduo::net::websocket::reset(conn);
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
#if !defined(MAP_USERID_SESSION) && 0
		//userid
		int64_t userid = entryContext->getUserID();
		if (userid > 0) {		
			//check before remove
			sessions_.remove(userid, conn);
		}
#endif
#if 0
		//理论上session不过期，worker线程不变，实际上每次断线重连，session都会变更
		std::string const& session = entryContext->getSession();
		int index = hash_session_(session) % threadPool_.size();
#else
		//获取绑定的worker线程
		int index = entryContext->getWorkerIndex();
		assert(index >= 0 && index < threadPool_.size());
#endif
		//扔给任务消息队列处理，不要在网络I/O线程中处理业务，容易引发异常
		threadPool_[index]->run(
			std::bind(
				&Gateway::asyncOfflineHandler,
				this, entryContext));
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::onConnected(
	const muduo::net::TcpConnectionPtr& conn,
	std::string const& ipaddr) {

	conn->getLoop()->assertInLoopThread();

	LOG_INFO << __FUNCTION__ << " --- *** " << "客户端真实IP[" << ipaddr << "]";

	assert(!conn->getContext().empty());
	ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
	assert(entryContext);
	{
		//保存conn真实ipaddr到ContextPtr上下文
		muduo::net::InetAddress address(ipaddr, 0);
		entryContext->setFromIp(address.ipNetEndian());
	}
	//生成uuid/session
	std::string uuid = createUUID();
	std::string session = buffer2HexStr((unsigned char const*)uuid.data(), uuid.length());
	{
		//conn绑定session
		//优化前，conn->name()断线重连->session变更->重新登陆->异地登陆通知
		//优化后，conn->name()断线重连->session过期检查->登陆校验->异地登陆判断
		entryContext->setSession(session);
	}
	{
		//////////////////////////////////////////////////////////////////////////
		//map[session] = weakConn
		//////////////////////////////////////////////////////////////////////////
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
		entities_.add(session, muduo::net::WeakTcpConnectionPtr(conn));
		LOG_WARN << __FUNCTION__ << " session[ " << session << " ]";
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::onMessage(
	const muduo::net::TcpConnectionPtr& conn,
	muduo::net::Buffer* buf, int msgType,
	muduo::Timestamp receiveTime) {
	//LOG_ERROR << __FUNCTION__ << " bufsz = " << buf->readableBytes();
	//超过最大连接数限制
	if (!conn || conn->getContext().empty()) {
		return;
	}

	conn->getLoop()->assertInLoopThread();

	const uint16_t len = buf->peekInt16();
	//数据包太大或太小
	if (likely(len > packet::kMaxPacketSZ ||
			   len < packet::kMinPacketSZ)) {
		if (conn) {
#if 0
			//不再发送数据
			conn->shutdown();
#else
			//直接强制关闭连接
			conn->forceClose();
#endif
		}
		//累计未处理请求数
		numTotalBadReq_.incrementAndGet();
	}
	///数据包不足够解析，等待下次接收再解析
	else if (likely(len > buf->readableBytes())) {
		if (conn) {
#if 0
			//不再发送数据
			conn->shutdown();
#else
			//直接强制关闭连接
			conn->forceClose();
#endif
		}
		//累计未处理请求数
		numTotalBadReq_.incrementAndGet();
	}
	else /*if (likely(len <= buf->readableBytes()))*/ {
		ContextPtr entryContext(boost::any_cast<ContextPtr>(conn->getContext()));
		assert(entryContext);
		EntryPtr entry(entryContext->getWeakEntryPtr().lock());
		if (entry) {
			{
				EventLoopContextPtr context = boost::any_cast<EventLoopContextPtr>(conn->getLoop()->getContext());
				assert(context);
				
				int index = context->getBucketIndex();
				assert(index >= 0 && index < bucketsPool_.size());
				
				//收到消息包，更新桶元素
				RunInLoop(conn->getLoop(),
					std::bind(&ConnBucket::updateBucket, bucketsPool_[index].get(), entry));
			}
			{				
#if 0
				//理论上session不过期，worker线程不变，实际上每次断线重连，session都会变更
				std::string const& session = entryContext->getSession();
				int index = hash_session_(session) % threadPool_.size();
#else
				//获取绑定的worker线程
				int index = entryContext->getWorkerIndex();
				assert(index >= 0 && index < threadPool_.size());
#endif
#if 0
				BufferPtr buffer(new muduo::net::Buffer(buf->readableBytes()));
				buffer->swap(*buf);
#else
				BufferPtr buffer(new muduo::net::Buffer(buf->readableBytes()));
				buffer->append(buf->peek(), static_cast<size_t>(buf->readableBytes()));
				buf->retrieve(buf->readableBytes());
#endif
				//扔给任务消息队列处理
				threadPool_[index]->run(
					std::bind(
						&Gateway::asyncClientHandler,
						this, entryContext->getWeakEntryPtr(), buffer, receiveTime));
			}
		}
		else {
			//timeout过期，entry失效，服务端要主动关闭sockfd，让客户端释放sockfd资源，并发起断线重连
			//Entry::dtor 析构调用
			//累计未处理请求数
			numTotalBadReq_.incrementAndGet();
			LOG_ERROR << __FUNCTION__ << " --- *** " << "entry invalid";
		}
	}
}

//网关服[S]端 <- 客户端[C]端，websocket 异步回调
void Gateway::asyncClientHandler(
	WeakEntryPtr const& weakEntry,
	BufferPtr& buf,
	muduo::Timestamp receiveTime) {
	//刚开始还在想，会不会出现超时conn被异步关闭释放掉，而业务逻辑又被处理了，却发送不了的尴尬情况，
	//假如因为超时entry弹出bucket，引用计数减1，处理业务之前这里使用shared_ptr，持有entry引用计数(加1)，
	//如果持有失败，说明弹出bucket计数减为0，entry被析构释放，conn被关闭掉了，也就不会执行业务逻辑处理，
	//如果持有成功，即使超时entry弹出bucket，引用计数减1，但并没有减为0，entry也就不会被析构释放，conn也不会被关闭，
	//直到业务逻辑处理完并发送，entry引用计数减1变为0，析构被调用关闭conn(如果conn还存在的话，业务处理完也会主动关闭conn)
	//
	//锁定同步业务操作，先锁超时对象entry，再锁conn，避免超时和业务同时处理的情况
	EntryPtr entry(weakEntry.lock());
	if (entry) {
		//entry->setLocked();
		muduo::net::TcpConnectionPtr peer(entry->getWeakConnPtr().lock());
		if (peer) {
			//LOG_ERROR << __FUNCTION__ << " bufsz = " << buf->readableBytes();
			if (buf->readableBytes() < packet::kHeaderLen) {
				//累计未处理请求数
				numTotalBadReq_.incrementAndGet();
				return;
			}
			//data, dataLen
			uint8_t const* data = (uint8_t const*)buf->peek();
			size_t len = buf->readableBytes();
			//packet::header_t
			packet::header_t* header = (packet::header_t*)(&data[0]);
			//校验CRC header->len = packet::kHeaderLen + len
			//header.len uint16_t
			//header.crc uint16_t
			//header.ver ~ header.realsize + protobuf
			uint16_t crc = packet::getCheckSum(&data[4], /*header->len*/len - 4);
			if (header->len == len &&
				header->crc == crc &&
				header->ver == 1 &&
				header->sign == HEADER_SIGN) {
				//mainID
				switch (header->mainID) {
				case Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_PROXY: {
					//网关服(Gateway)
					switch (header->enctype) {
					case packet::PUBENC_PROTOBUF_NONE: {
						//NONE
						TraceMessageID(header->mainID, header->subID);
						int cmd = packet::enword(header->mainID, header->subID);
						CmdCallbacks::const_iterator it = handlers_.find(cmd);
						if (it != handlers_.end()) {
							CmdCallback const& handler = it->second;
							handler(peer, get_pointer(buf));
						}
						break;
					}
					case packet::PUBENC_PROTOBUF_RSA: {
						//RSA
						TraceMessageID(header->mainID, header->subID);
						break;
					}
					case packet::PUBENC_PROTOBUF_AES: {
						//AES
						TraceMessageID(header->mainID, header->subID);
						break;
					}
					default: {
						//累计未处理请求数
						numTotalBadReq_.incrementAndGet();
						break;
					}
					}
					break;
				}
				case Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_HALL: {
					//大厅服(HallS)
					TraceMessageID(header->mainID, header->subID);
					{
						ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
						assert(entryContext);
						//userid
						int64_t userid = entryContext->getUserID();
						//clientip
						uint32_t clientip = entryContext->getFromIp();
						//session
						std::string const& session = entryContext->getSession();
						//aeskey
						std::string const& aeskey = entryContext->getAesKey();
						ClientConn const& clientConn = entryContext->getClientConn(servTyE::kHallTy);
						muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
						assert(header->len == len);
						assert(header->len >= packet::kHeaderLen);
#if 0
						//////////////////////////////////////////////////////////////////////////
						//玩家登陆网关服信息
						//使用hash	h.usr:proxy[1001] = session|ip:port:port:pid<弃用>
						//使用set	s.uid:1001:proxy = session|ip:port:port:pid<使用>
						//网关服ID格式：session|ip:port:port:pid
						//第一个ip:port是网关服监听客户端的标识
						//第二个ip:port是网关服监听订单服的标识
						//pid标识网关服进程id
						//////////////////////////////////////////////////////////////////////////
						//网关服servid session|ip:port:port:pid
						std::string const& servid = nodeValue_;
#endif
						BufferPtr buffer = packet::packMessage(
							userid,
							session,
							aeskey,
							clientip,
							0,
#if 0
							servid,
#endif
							buf->peek(),
							header->len);
						if (buffer) {
							//发送大厅消息
							sendHallMessage(entryContext, buffer, userid);
						}
					}
					break;
				}
				case Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_GAME_SERVER:
				case Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_GAME_LOGIC: {
					//游戏服(GameS)
					//逻辑服(LogicS，逻辑子游戏libGame_xxx.so)
					TraceMessageID(header->mainID, header->subID);
					{
						ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
						assert(entryContext);
						//userid
						int64_t userid = entryContext->getUserID();
						//clientip
						uint32_t clientip = entryContext->getFromIp();
						//session
						std::string const& session = entryContext->getSession();
						//aeskey
						std::string const& aeskey = entryContext->getAesKey();
						ClientConn const& clientConn = entryContext->getClientConn(servTyE::kHallTy);
						muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
						assert(header->len == len);
						assert(header->len >= packet::kHeaderLen);
#if 0
						//////////////////////////////////////////////////////////////////////////
						//玩家登陆网关服信息
						//使用hash	h.usr:proxy[1001] = session|ip:port:port:pid<弃用>
						//使用set	s.uid:1001:proxy = session|ip:port:port:pid<使用>
						//网关服ID格式：session|ip:port:port:pid
						//第一个ip:port是网关服监听客户端的标识
						//第二个ip:port是网关服监听订单服的标识
						//pid标识网关服进程id
						//////////////////////////////////////////////////////////////////////////
						//网关服servid session|ip:port:port:pid
						std::string const& servid = nodeValue_;
#endif
						BufferPtr buffer = packet::packMessage(
							userid,
							session,
							aeskey,
							clientip,
							0,
#if 0
							servid,
#endif
							buf->peek(),
							header->len);
						if (buffer) {
							//发送游戏消息
							sendGameMessage(entryContext, buffer, userid);
						}
					}
					break;
				}
				default: {
					//累计未处理请求数
					numTotalBadReq_.incrementAndGet();
					break;
				}
				}
			}
			else {
				//累计未处理请求数
				numTotalBadReq_.incrementAndGet();
			}
		}
		else {
			//累计未处理请求数
			numTotalBadReq_.incrementAndGet();
			LOG_ERROR << __FUNCTION__ << " --- *** " << "TcpConnectionPtr.conn invalid";
		}
		//entry->setLocked(false);
	}
	else {
		//timeout过期，entry失效，服务端要主动关闭sockfd，让客户端释放sockfd资源，并发起断线重连
		//Entry::dtor 析构调用
		//累计未处理请求数
		numTotalBadReq_.incrementAndGet();
		LOG_ERROR << __FUNCTION__ << " --- *** " << "entry invalid";
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::asyncOfflineHandler(ContextPtr const& entryContext) {
	if (entryContext) {
		LOG_ERROR << __FUNCTION__;
		//session
		std::string const& session = entryContext->getSession();
		if (!session.empty()) {
			//remove
			entities_.remove(session);
		}
		//userid
		int64_t userid = entryContext->getUserID();
		if (userid > 0) {
			//check before remove
			sessions_.remove(userid, session);
		}
		//offline hall
		onUserOfflineHall(entryContext);
		//offline game
		onUserOfflineGame(entryContext);
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
BufferPtr Gateway::packClientShutdownMsg(int64_t userid, int status) {

	::Game::Common::ProxyNotifyShutDownUserClientMessage msg;
	msg.mutable_header()->set_sign(PROTO_BUF_SIGN);
	msg.set_userid(userid);
	msg.set_status(status);
	
	BufferPtr buffer = packet::packMessage(
		::Game::Common::MAIN_MESSAGE_CLIENT_TO_PROXY,
		::Game::Common::PROXY_NOTIFY_SHUTDOWN_USER_CLIENT_MESSAGE_NOTIFY, &msg);
	
	TraceMessageID(::Game::Common::MAIN_MESSAGE_CLIENT_TO_PROXY,
		::Game::Common::PROXY_NOTIFY_SHUTDOWN_USER_CLIENT_MESSAGE_NOTIFY);
	
	return buffer;
}

BufferPtr Gateway::packNoticeMsg(
	int32_t agentid, std::string const& title,
	std::string const& content, int msgtype) {
	
	::ProxyServer::Message::NotifyNoticeMessageFromProxyServerMessage msg;
	msg.mutable_header()->set_sign(PROTO_BUF_SIGN);
	msg.add_agentid(agentid);
	msg.set_title(title.c_str());
	msg.set_message(content);
	msg.set_msgtype(msgtype);
	
	BufferPtr buffer = packet::packMessage(
		::Game::Common::MAIN_MESSAGE_CLIENT_TO_PROXY,
		::Game::Common::PROXY_NOTIFY_PUBLIC_NOTICE_MESSAGE_NOTIFY, &msg);
	
	TraceMessageID(::Game::Common::MAIN_MESSAGE_CLIENT_TO_PROXY,
		::Game::Common::PROXY_NOTIFY_PUBLIC_NOTICE_MESSAGE_NOTIFY);

	return buffer;
}

void Gateway::broadcastNoticeMsg(
	std::string const& title,
	std::string const& msg,
	int32_t agentid, int msgType) {
	//packNoticeMsg
	BufferPtr buffer = packNoticeMsg(
		agentid,
		title,
		msg,
		msgType);
	if (buffer) {
		sessions_.broadcast(buffer);
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::broadcastMessage(int mainID, int subID, ::google::protobuf::Message* msg) {
	BufferPtr buffer = packet::packMessage(mainID, subID, msg);
	if (buffer) {
		TraceMessageID(mainID, subID);
		entities_.broadcast(buffer);
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
void Gateway::refreshBlackList() {
	if (blackListControl_ == IpVisitCtrlE::kOpenAccept) {
		//Accept时候判断，socket底层控制，否则开启异步检查
		RunInLoop(server_.getLoop(), std::bind(&Gateway::refreshBlackListInLoop, this));
	}
	else if (blackListControl_ == IpVisitCtrlE::kOpen) {
		//同步刷新IP访问黑名单
		refreshBlackListSync();
	}
}

//网关服[S]端 <- 客户端[C]端，websocket
bool Gateway::refreshBlackListSync() {
	//Accept时候判断，socket底层控制，否则开启异步检查
	assert(blackListControl_ == IpVisitCtrlE::kOpen);
	{
		WRITE_LOCK(blackList_mutex_);
		blackList_.clear();
	}
	for (std::map<in_addr_t, IpVisitE>::const_iterator it = blackList_.begin();
		it != blackList_.end(); ++it) {
		LOG_DEBUG << "--- *** " << "IP访问黑名单\n"
			<< "--- *** ipaddr[" << Inet2Ipstr(it->first) << "] status[" << it->second << "]";
	}
	return false;
}

//网关服[S]端 <- 客户端[C]端，websocket
bool Gateway::refreshBlackListInLoop() {
	//Accept时候判断，socket底层控制，否则开启异步检查
	assert(blackListControl_ == IpVisitCtrlE::kOpenAccept);
	server_.getLoop()->assertInLoopThread();
	blackList_.clear();
	for (std::map<in_addr_t, IpVisitE>::const_iterator it = blackList_.begin();
		it != blackList_.end(); ++it) {
		LOG_DEBUG << "--- *** " << "IP访问黑名单\n"
			<< "--- *** ipaddr[" << Inet2Ipstr(it->first) << "] status[" << it->second << "]";
	}
	return false;
}