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

//网关服[C]端 -> 游戏服[S]端
void Gateway::onGameConnection(const muduo::net::TcpConnectionPtr& conn) {

	conn->getLoop()->assertInLoopThread();

	if (conn->connected()) {
		int32_t num = numConnected_.incrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "网关服[" << conn->localAddress().toIpPort() << "] -> 游戏服["
			<< conn->peerAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
	}
	else {
		int32_t num = numConnected_.decrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "网关服[" << conn->localAddress().toIpPort() << "] -> 游戏服["
			<< conn->peerAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
	}
}

//网关服[C]端 -> 游戏服[S]端
void Gateway::onGameMessage(const muduo::net::TcpConnectionPtr& conn,
	muduo::net::Buffer* buf,
	muduo::Timestamp receiveTime) {

	conn->getLoop()->assertInLoopThread();

	//LOG_ERROR << __FUNCTION__;
	//解析TCP数据包，先解析包头(header)，再解析包体(body)，避免粘包出现
	while (buf->readableBytes() >= packet::kMinPacketSZ) {

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
			break;
		}
		//数据包不足够解析，等待下次接收再解析
		else if (likely(len > buf->readableBytes())) {
			break;
		}
		else /*if (likely(len <= buf->readableBytes()))*/ {
			BufferPtr buffer(new muduo::net::Buffer(len));
			buffer->append(buf->peek(), static_cast<size_t>(len));
			buf->retrieve(len);
			packet::internal_prev_header_t* pre_header = (packet::internal_prev_header_t*)buffer->peek();
			std::string session((char const*)pre_header->session, sizeof(pre_header->session));
			assert(!session.empty() && session.size() == packet::kSessionSZ);
#if 0
			//////////////////////////////////////////////////////////////////////////
			//session -> hash(session) -> index
			//////////////////////////////////////////////////////////////////////////
			int index = hash_session_(session) % threadPool_.size();

			//扔给任务消息队列处理
			threadPool_[index]->run(
				std::bind(
					&Gateway::asyncGameHandler,
					this,
					conn, buffer, receiveTime));
#else
			//////////////////////////////////////////////////////////////////////////
			//session -> entry -> conn -> entryContext -> index
			//////////////////////////////////////////////////////////////////////////
			muduo::net::WeakTcpConnectionPtr weakConn = entities_.get(session);
			muduo::net::TcpConnectionPtr peer(weakConn.lock());
			if (peer) {
				ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
				assert(entryContext);

				int index = entryContext->getWorkerIndex();
				assert(index >= 0 && index < threadPool_.size());

				//扔给任务消息队列处理
				threadPool_[index]->run(
					std::bind(
						&Gateway::asyncGameHandler,
						this,
						weakConn, buffer, receiveTime));
			}
#endif
		}
	}
}

//网关服[C]端 -> 游戏服[S]端
void Gateway::asyncGameHandler(
	muduo::net::WeakTcpConnectionPtr const& weakConn,
	BufferPtr& buf,
	muduo::Timestamp receiveTime) {
	//LOG_ERROR << __FUNCTION__;
	//内部消息头internal_prev_header_t + 命令消息头header_t
	if (buf->readableBytes() < packet::kPrevHeaderLen + packet::kHeaderLen) {
		return;
	}
	muduo::net::TcpConnectionPtr peer(weakConn.lock());
	if (peer) {
		ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
		assert(entryContext);
		//内部消息头internal_prev_header_t
		packet::internal_prev_header_t /*const*/* pre_header = (packet::internal_prev_header_t /*const*/*)buf->peek();
		//session
		std::string session((char const*)pre_header->session, sizeof(pre_header->session));
		assert(!session.empty() && session.size() == packet::kSessionSZ);
		//userid
		int64_t userid = pre_header->userID;
		//校验userid
		assert(userid == entryContext->getUserID());
		//校验session
		if (session != entryContext->getSession()) {
			LOG_ERROR << __FUNCTION__ << " --- *** " << "check userid. " << userid
				<< "\nsession = " << buffer2HexStr((unsigned char const*)session.c_str(), session.length())
				<< "\nsave    = " << buffer2HexStr((unsigned char const*)entryContext->getSession().c_str(), entryContext->getSession().length());
			return;
		}
		//命令消息头header_t
		packet::header_t /*const*/* header = (packet::header_t /*const*/*)(buf->peek() + packet::kPrevHeaderLen);
		
		//校验CRC header->len = packet::kHeaderLen + len
		uint16_t crc = packet::getCheckSum((uint8_t const*)&header->ver, header->len - 4);
		assert(header->crc == crc);
		
		TraceMessageID(header->mainID, header->subID);
		
		muduo::net::websocket::send(peer, (uint8_t const*)header, header->len);
	}
}

//网关服[C]端 -> 游戏服[S]端
void Gateway::sendGameMessage(
	Context& entryContext,
	BufferPtr& buf, int64_t userid) {
	//printf("%s %s(%d)\n", __FUNCTION__, __FILE__, __LINE__);
	ClientConn const& clientConn = entryContext.getClientConn(servTyE::kGameTy);
	muduo::net::TcpConnectionPtr gameConn(clientConn.second.lock());
	if (gameConn) {
		assert(gameConn->connected());
#if !defined(NDEBUG)
#if 0
		assert(
			std::find(
				std::begin(clients_[servTyE::kGameTy].names_),
				std::end(clients_[servTyE::kGameTy].names_),
				clientConn.first) != clients_[servTyE::kGameTy].names_.end());
#endif
		clients_[servTyE::kGameTy].clients_->check(clientConn.first, true);
#endif
		if (buf) {
			//printf("len = %d\n", buf->readableBytes());
			gameConn->send(buf.get());
		}
	}
}

//网关服[C]端 -> 游戏服[S]端
void Gateway::onUserOfflineGame(
	Context& entryContext, bool leave) {
	MY_TRY()
	//userid
	int64_t userid = entryContext.getUserID();
	//clientip
	uint32_t clientip = entryContext.getFromIp();
	//session
	std::string const& session = entryContext.getSession();
	//aeskey
	std::string const& aeskey = entryContext.getAesKey();
	if (userid > 0 && !session.empty()) {
		//packMessage
		BufferPtr buffer = packet::packMessage(
			userid,
			session,
			aeskey,
			clientip,
			KICK_LEAVEGS,
			::Game::Common::MAIN_MESSAGE_PROXY_TO_GAME_SERVER,
			::Game::Common::MESSAGE_PROXY_TO_GAME_SERVER_SUBID::GAME_SERVER_ON_USER_OFFLINE,
			NULL);
		if (buffer) {
			TraceMessageID(
				::Game::Common::MAIN_MESSAGE_PROXY_TO_GAME_SERVER,
				::Game::Common::MESSAGE_PROXY_TO_GAME_SERVER_SUBID::GAME_SERVER_ON_USER_OFFLINE);
			assert(buffer->readableBytes() < packet::kMaxPacketSZ);
			sendGameMessage(entryContext, buffer, userid);
		}
	}
	MY_CATCH()
}