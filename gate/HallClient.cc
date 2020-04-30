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

//网关服[C]端 -> 大厅服[S]端
void Gateway::onHallConnection(const muduo::net::TcpConnectionPtr& conn) {

	conn->getLoop()->assertInLoopThread();

	if (conn->connected()) {
		int32_t num = numConnected_.incrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "网关服[" << conn->localAddress().toIpPort() << "] -> 大厅服["
			<< conn->peerAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
	}
	else {
		int32_t num = numConnected_.decrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "网关服[" << conn->localAddress().toIpPort() << "] -> 大厅服["
			<< conn->peerAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
	}
}

//网关服[C]端 -> 大厅服[S]端
void Gateway::onHallMessage(const muduo::net::TcpConnectionPtr& conn,
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
					&Gateway::asyncHallHandler,
					this,
					conn, buffer, receiveTime));
#else
			//////////////////////////////////////////////////////////////////////////
			//session -> conn -> entryContext -> index
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
						&Gateway::asyncHallHandler,
						this,
						conn,
						weakConn, buffer, receiveTime));
			}
#endif
		}
	}
}

//网关服[C]端 -> 大厅服[S]端
void Gateway::asyncHallHandler(
	const muduo::net::TcpConnectionPtr& conn,
	muduo::net::WeakTcpConnectionPtr const& weakConn,
	BufferPtr& buf,
	muduo::Timestamp receiveTime) {
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
		//命令消息头header_t
		packet::header_t /*const*/* header = (packet::header_t /*const*/*)(buf->peek() + packet::kPrevHeaderLen);
			
		//校验CRC header->len = packet::kHeaderLen + len
		uint16_t crc = packet::getCheckSum((uint8_t const*)&header->ver, header->len - 4);
		assert(header->crc == crc);

		TraceMessageID(header->mainID, header->subID);
		
		if (
			header->mainID == ::Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_HALL &&
			header->subID == ::Game::Common::MESSAGE_CLIENT_TO_HALL_SUBID::CLIENT_TO_HALL_LOGIN_MESSAGE_RES &&
			pre_header->ok == 1) {
			//////////////////////////////////////////////////////////////////////////
			//登陆成功，指定userid
			//////////////////////////////////////////////////////////////////////////
			entryContext->setUserID(userid);
			//////////////////////////////////////////////////////////////////////////
			//登陆成功，指定玩家大厅
			//////////////////////////////////////////////////////////////////////////
			assert(conn);
			std::vector<std::string> vec;
			boost::algorithm::split(vec, conn->name(), boost::is_any_of(":"));
			std::string const& name = vec[0] + ":" + vec[1];
			ClientConn clientConn(name, conn);
			LOG_WARN << "登陆成功，大厅节点 >>> " << name;
			entryContext->setClientConn(servTyE::kHallTy, clientConn);
			//////////////////////////////////////////////////////////////////////////
			//顶号处理 userid -> conn
			//////////////////////////////////////////////////////////////////////////
			muduo::net::TcpConnectionPtr peer_(sessions_.add(userid, muduo::net::WeakTcpConnectionPtr(peer)).lock());
			if (peer_) {
				assert(peer_ != peer);
				ContextPtr entryContext_(boost::any_cast<ContextPtr>(peer_->getContext()));
				assert(entryContext_);
				std::string const& session_ = entryContext_->getSession();
				assert(session_.size() == packet::kSessionSZ);
				assert(session_ != session);
				BufferPtr buffer = packClientShutdownMsg(userid, 0); assert(buffer);
				muduo::net::websocket::send(peer_, buffer->peek(), buffer->readableBytes());
#if 0
				peer_->getLoop()->runAfter(0.2f, [&]() {
					entry_.reset();
					});
#else
				peer_->forceCloseWithDelay(0.2);
#endif
			}
		}
		else if (
			header->mainID == ::Game::Common::MAINID::MAIN_MESSAGE_CLIENT_TO_HALL &&
			header->subID == ::Game::Common::MESSAGE_CLIENT_TO_HALL_SUBID::CLIENT_TO_HALL_GET_GAME_SERVER_MESSAGE_RES &&
			pre_header->ok == 1) {
			//////////////////////////////////////////////////////////////////////////
			//校验userid
			//////////////////////////////////////////////////////////////////////////
			assert(userid == entryContext->getUserID());

			ClientConn const& clientConn = entryContext->getClientConn(servTyE::kGameTy);
			muduo::net::TcpConnectionPtr gameConn(clientConn.second.lock());
			if (!gameConn) {
				LOG_ERROR << __FUNCTION__ << " --- *** " << "用户游戏服无效，重新分配";
				//用户游戏服无效，重新分配
				std::string gameIp;
				if (REDISCLIENT.GetUserOnlineInfoIP(userid, gameIp)) {
					//分配用户游戏服
					ClientConn clientConn;
					//异步获取指定游戏服连接
					clients_[servTyE::kGameTy].clients_->get(gameIp, clientConn);
					muduo::net::TcpConnectionPtr gameConn(clientConn.second.lock());
					if (gameConn) {
						entryContext->setClientConn(servTyE::kGameTy, clientConn);
					}
					else {

					}
				}
			}
		}
		muduo::net::websocket::send(peer, (uint8_t const*)header, header->len);
	}
}

#if 0
//网关服[C]端 -> 大厅服[S]端
void Gateway::sendHallMessage(
	ContextPtr const& entryContext,
	BufferPtr& buf, int64_t userid) {
	//printf("%s %s(%d)\n", __FUNCTION__, __FILE__, __LINE__);
	if (entryContext) {
		//printf("%s %s(%d)\n", __FUNCTION__, __FILE__, __LINE__);
		ClientConn const& clientConn = entryContext->getClientConn(servTyE::kHallTy);
		muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
		if (hallConn) {
			assert(hallConn->connected());
#ifndef NDEBUG
#if 0
			assert(
				std::find(
					std::begin(clients_[servTyE::kHallTy].names_),
					std::end(clients_[servTyE::kHallTy].names_),
					clientConn.first) != clients_[servTyE::kHallTy].names_.end());
#endif
			clients_[servTyE::kHallTy].clients_->check(clientConn.first, true);
#endif
			if (buf) {
				//printf("len = %d\n", buf->readableBytes());
				hallConn->send(buf.get());
			}
		}
		else {
			LOG_ERROR << __FUNCTION__ << " --- *** " << "用户大厅服失效，重新分配";
			//用户大厅服失效，重新分配
			ClientConnList clients;
			//异步获取全部有效大厅连接
			clients_[servTyE::kHallTy].clients_->getAll(clients);
			if (clients.size() > 0) {
				int index = randomHall_.betweenInt(0, clients.size() - 1).randInt_mt();
				assert(index >= 0 && index < clients.size());
				ClientConn const& clientConn = clients[index];
				muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
				if (hallConn) {
					if (entryContext->getUserID() > 0) {
						//账号已经登陆，但登陆大厅失效了，重新指定账号登陆大厅
						entryContext->setClientConn(servTyE::kHallTy, clientConn);
					}
					if (buf) {
						//printf("len = %d\n", buf->readableBytes());
						hallConn->send(buf.get());
					}
				}
				else {

				}
			}
		}
	}
}
#else
//网关服[C]端 -> 大厅服[S]端
void Gateway::sendHallMessage(
	ContextPtr const& entryContext,
	BufferPtr& buf, int64_t userid) {
	//printf("%s %s(%d)\n", __FUNCTION__, __FILE__, __LINE__);
	if (entryContext) {
		//printf("%s %s(%d)\n", __FUNCTION__, __FILE__, __LINE__);
		ClientConn const& clientConn = entryContext->getClientConn(servTyE::kHallTy);
		muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
		if (hallConn) {
			assert(hallConn->connected());
			assert(entryContext->getUserID() > 0);
			//判断节点是否维护中
			if (!clients_[servTyE::kHallTy].isRepairing(clientConn.first)) {
#ifndef NDEBUG
#if 0
				assert(
					std::find(
						std::begin(clients_[servTyE::kHallTy].clients_),
						std::end(clients_[servTyE::kHallTy].clients_),
						clientConn.first) != clients_[servTyE::kHallTy].clients_.end());
#endif
				clients_[servTyE::kHallTy].clients_->check(clientConn.first, true);
#endif
				if (buf) {
					//printf("len = %d\n", buf->readableBytes());
					hallConn->send(buf.get());
				}
			}
			else {
				LOG_ERROR << __FUNCTION__ << " --- *** " << "用户大厅服维护，重新分配";
				//用户大厅服维护，重新分配
				ClientConnList clients;
				//异步获取全部有效大厅连接
				clients_[servTyE::kHallTy].clients_->getAll(clients);
				if (clients.size() > 0) {
					bool bok = false;
					std::map<std::string, bool> repairs;
					do {
						int index = randomHall_.betweenInt(0, clients.size() - 1).randInt_mt();
						assert(index >= 0 && index < clients.size());
						ClientConn const& clientConn = clients[index];
						muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
						if (hallConn) {
							//判断节点是否维护中
							if (bok = !clients_[servTyE::kHallTy].isRepairing(clientConn.first)) {
								//账号已经登陆，但登陆大厅维护中，重新指定账号登陆大厅
								entryContext->setClientConn(servTyE::kHallTy, clientConn);
								if (buf) {
									//printf("len = %d\n", buf->readableBytes());
									hallConn->send(buf.get());
								}
							}
							else {
								repairs[clientConn.first] = true;
							}
						}
					} while (!bok && repairs.size() != clients.size());
				}
			}
		}
		else {
			LOG_ERROR << __FUNCTION__ << " --- *** " << "用户大厅服失效，重新分配";
			//用户大厅服失效，重新分配
			ClientConnList clients;
			//异步获取全部有效大厅连接
			clients_[servTyE::kHallTy].clients_->getAll(clients);
			if (clients.size() > 0) {
				bool bok = false;
				std::map<std::string, bool> repairs;
				do {
					int index = randomHall_.betweenInt(0, clients.size() - 1).randInt_mt();
					assert(index >= 0 && index < clients.size());
					ClientConn const& clientConn = clients[index];
					muduo::net::TcpConnectionPtr hallConn(clientConn.second.lock());
					if (hallConn) {
						assert(hallConn->connected());
						//判断节点是否维护中
						if (bok = !clients_[servTyE::kHallTy].isRepairing(clientConn.first)) {
							if (entryContext->getUserID() > 0) {
								//账号已经登陆，但登陆大厅失效了，重新指定账号登陆大厅
								entryContext->setClientConn(servTyE::kHallTy, clientConn);
							}
							if (buf) {
								//printf("len = %d\n", buf->readableBytes());
								hallConn->send(buf.get());
							}
						}
						else {
							repairs[clientConn.first] = true;
						}
					}
				} while (!bok && repairs.size() != clients.size());
			}
		}
	}
}
#endif

//网关服[C]端 -> 大厅服[S]端，跨网关顶号处理(异地登陆)
void Gateway::onUserLoginNotify(std::string const& msg) {
	LOG_WARN << __FUNCTION__ << " " << msg;
	std::stringstream ss(msg);
	boost::property_tree::ptree root;
	boost::property_tree::read_json(ss, root);
	try {
		int64_t userid = root.get<int>("userId");
		//////////////////////////////////////////////////////////////////////////
		//账号最新登陆session
		//////////////////////////////////////////////////////////////////////////
		std::string const session = root.get<std::string>("session");
#if 0
		std::string const servid_ = root.get<std::string>("proxyip");
		//排除自己
		std::string const& servid = nodeValue_;
		if (servid == servid_) {
			return;
		}
#endif
		muduo::net::TcpConnectionPtr peer(entities_.get(session).lock());
		if (!peer) {
			//////////////////////////////////////////////////////////////////////////
			//顶号处理 userid -> conn -> session
			//////////////////////////////////////////////////////////////////////////
			muduo::net::TcpConnectionPtr peer_(sessions_.get(userid).lock());
			if (peer_) {
				ContextPtr entryContext_(boost::any_cast<ContextPtr>(peer_->getContext()));
				assert(entryContext_);
				assert(entryContext_->getUserID() == userid);
				//相同账号，不同session，不关心旧的session是多少，只要不是当前最新，关闭之
				if (entryContext_->getSession() != session) {
					BufferPtr buffer = packClientShutdownMsg(userid, 0); assert(buffer);
					muduo::net::websocket::send(peer_, buffer->peek(), buffer->readableBytes());
#if 0
					peer_->getLoop()->runAfter(0.2f, [&]() {
						entry_.reset();
						});
#else
					peer_->forceCloseWithDelay(0.2);
#endif
				}
			}
		}
		else {
#if 0
			{
				ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
				assert(entryContext);
				assert(entryContext->getUserID() == userid);
				assert(entryContext->getSession() == session);
			}
			{
				muduo::net::TcpConnectionPtr peer(sessions_.get(userid).lock());
				assert(peer);
				ContextPtr entryContext(boost::any_cast<ContextPtr>(peer->getContext()));
				assert(entryContext);
				assert(entryContext->getUserID() == userid);
				assert(entryContext->getSession() == session);
			}
#endif
		}
	}
	catch (boost::property_tree::ptree_error & e) {
		LOG_ERROR << __FUNCTION__ << " " << e.what();
	}
}

//网关服[C]端 -> 大厅服[S]端
void Gateway::onUserOfflineHall(ContextPtr const& entryContext) {
	MY_TRY()
	if (entryContext) {
		//userid
		int64_t userid = entryContext->getUserID();
		//clientip
		uint32_t clientip = entryContext->getFromIp();
		//session
		std::string const& session = entryContext->getSession();
		//aeskey
		std::string const& aeskey = entryContext->getAesKey();
		if (userid > 0 && !session.empty()) {
			//packMessage
			BufferPtr buffer = packet::packMessage(
				userid,
				session,
				aeskey,
				clientip,
				0,
				::Game::Common::MAIN_MESSAGE_PROXY_TO_HALL,
				::Game::Common::MESSAGE_PROXY_TO_HALL_SUBID::HALL_ON_USER_OFFLINE,
				NULL);
			if (buffer) {
				TraceMessageID(
					::Game::Common::MAIN_MESSAGE_PROXY_TO_HALL,
					::Game::Common::MESSAGE_PROXY_TO_HALL_SUBID::HALL_ON_USER_OFFLINE);
				assert(buffer->readableBytes() < packet::kMaxPacketSZ);
				sendHallMessage(entryContext, buffer, userid);
			}
		}
	}
	MY_CATCH()
}