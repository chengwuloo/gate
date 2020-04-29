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

Gateway::Gateway(muduo::net::EventLoop* loop,
	const muduo::net::InetAddress& listenAddr,
	const muduo::net::InetAddress& listenAddrInn,
	const muduo::net::InetAddress& listenAddrHttp,
	std::string const& cert_path, std::string const& private_key_path,
	std::string const& client_ca_cert_file_path,
	std::string const& client_ca_cert_dir_path)
    : server_(loop, listenAddr, "Gateway[client]")
	, innServer_(loop, listenAddrInn, "Gateway[inner]")
	, httpServer_(loop, listenAddrHttp, "Gateway[http]")
	, hallClients_(loop)
	, gameClients_(loop)
	, kTimeoutSeconds_(3)
	, kHttpTimeoutSeconds_(3)
	, kMaxConnections_(15000)
	, serverState_(ServiceStateE::kRunning)
	, threadTimer_(new muduo::net::EventLoopThread(muduo::net::EventLoopThread::ThreadInitCallback(), "EventLoopThreadTimer"))
	, isdebug_(false) {

	init();

	//网络I/O线程池，I/O收发读写 recv(read)/send(write)
	muduo::net::ReactorSingleton::inst(loop, "RWIOThreadPool");

	//网关服[S]端 <- 客户端[C]端，websocket
	server_.setConnectionCallback(
		std::bind(&Gateway::onConnection, this, std::placeholders::_1));
	server_.setMessageCallback(
		std::bind(&muduo::net::websocket::onMessage,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	
	//网关服[S]端 <- 推送服[C]端，推送通知服务
	innServer_.setConnectionCallback(
		std::bind(&Gateway::onInnConnection, this, std::placeholders::_1));
	innServer_.setMessageCallback(
		std::bind(&Gateway::onInnMessage, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	
	//网关服[S]端 <- HTTP客户端[C]端，WEB前端
	httpServer_.setConnectionCallback(
		std::bind(&Gateway::onHttpConnection, this, std::placeholders::_1));
	httpServer_.setMessageCallback(
		std::bind(&Gateway::onHttpMessage, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	httpServer_.setWriteCompleteCallback(
		std::bind(&Gateway::onHttpWriteComplete, this, std::placeholders::_1));

	//网关服[C]端 -> 大厅服[S]端，内部交互
	hallClients_.setConnectionCallback(
		std::bind(&Gateway::onHallConnection, this, std::placeholders::_1));
	hallClients_.setMessageCallback(
		std::bind(&Gateway::onHallMessage, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	clients_[servTyE::kHallTy].clients_ = &hallClients_;
	clients_[servTyE::kHallTy].ty_ = servTyE::kHallTy;

	//网关服[C]端 -> 游戏服[S]端，内部交互
	gameClients_.setConnectionCallback(
		std::bind(&Gateway::onGameConnection, this, std::placeholders::_1));
	gameClients_.setMessageCallback(
		std::bind(&Gateway::onGameMessage, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	clients_[servTyE::kGameTy].clients_ = &gameClients_;
	clients_[servTyE::kGameTy].ty_ = servTyE::kGameTy;

	//添加OpenSSL认证支持 httpServer_&server_ 共享证书
	muduo::net::ssl::SSL_CTX_Init(
		cert_path,
		private_key_path,
		client_ca_cert_file_path, client_ca_cert_dir_path);

	//指定SSL_CTX
	server_.set_SSL_CTX(muduo::net::ssl::SSL_CTX_Get());
	httpServer_.set_SSL_CTX(muduo::net::ssl::SSL_CTX_Get());
}

Gateway::~Gateway() {
}

void Gateway::init() {
	handlers_[packet::enword(
		::Game::Common::MAIN_MESSAGE_CLIENT_TO_PROXY,
		::Game::Common::CLIENT_TO_PROXY_GET_AES_KEY_MESSAGE_REQ)] =
		std::bind(&Gateway::onGetAesKey, this, std::placeholders::_1, std::placeholders::_2);
}

void Gateway::quit() {
	hallClients_.closeAll();
	gameClients_.closeAll();
	for (size_t i = 0; i < threadPool_.size(); ++i) {
		threadPool_[i]->stop();
	}
	if (zkclient_) {
		zkclient_->closeServer();
	}
	if (redisClient_) {
		redisClient_->unsubscribe();
	}
}

//zookeeper
bool Gateway::initZookeeper(std::string const& ipaddr) {
	zkclient_.reset(new ZookeeperClient(ipaddr));
	zkclient_->SetConnectedWatcherHandler(
		std::bind(&Gateway::ZookeeperConnectedHandler, this));
	if (!zkclient_->connectServer()) {
		abort();
		return false;
	}
	return true;
}

//RedisCluster
bool Gateway::initRedisCluster(std::string const& ipaddr, std::string const& passwd) {
	
	redisClient_.reset(new RedisClient());
	if (!redisClient_->initRedisCluster(ipaddr, passwd)) {
		return false;
	}
	redisIpaddr_ = ipaddr;
	redisPasswd_ = passwd;
	//跨网关顶号处理(异地登陆)
	redisClient_->subscribeUserLoginMessage(std::bind(&Gateway::onUserLoginNotify, this, std::placeholders::_1));
	//跑马灯通告消息
	redisClient_->subscribePublishMsg(1, CALLBACK_1(Gateway::onMarqueeNotify, this));
	//幸运转盘消息
	redisClient_->subscribePublishMsg(0, [&](std::string const& msg) {
		threadTimer_->getLoop()->runAfter(10, std::bind(&Gateway::onLuckPushNotify, this, msg));
		});
	redisClient_->startSubThread();
	return true;
}

//RedisCluster
bool Gateway::initRedisCluster() {

	if (!REDISCLIENT.initRedisCluster(redisIpaddr_, redisPasswd_)) {
		abort();
		return false;
	}
	return true;
}

//RedisLock
bool Gateway::initRedisLock() {
#if 0
	for (std::vector<std::string>::const_iterator it = redlockVec_.begin();
		it != redlockVec_.end(); ++it) {
		std::vector<std::string> vec;
		boost::algorithm::split(vec, *it, boost::is_any_of(":"));
		LOG_INFO << __FUNCTION__ << " --- *** " << "\nredisLock " << vec[0].c_str() << ":" << vec[1].c_str();
		REDISLOCK.AddServerUrl(vec[0].c_str(), atol(vec[1].c_str()));
	}
#endif
	return true;
}

//MongoDB
bool Gateway::initMongoDB(std::string url) {
#if 0
	//http://mongocxx.org/mongocxx-v3/tutorial/
	LOG_INFO << __FUNCTION__ << " --- *** " << url;
	mongocxx::instance instance{};
	mongoDBUrl_ = url;
#endif
	return true;
}

//MongoDB
bool Gateway::initMongoDB() {
#if 0
	//http://mongocxx.org/mongocxx-v3/tutorial/
	//mongodb://admin:6pd1SieBLfOAr5Po@192.168.0.171:37017,192.168.0.172:37017,192.168.0.173:37017/?connect=replicaSet;slaveOk=true&w=1&readpreference=secondaryPreferred&maxPoolSize=50000&waitQueueMultiple=5
	MongoDBClient::ThreadLocalSingleton::setUri(mongoDBUrl_);
	static __thread mongocxx::database db = MONGODBCLIENT["gamemain"];
	static __thread mongocxx::database* dbgamemain_;
	dbgamemain_ = &db;
#endif
	return true;
}

//MongoDB/RedisCluster/RedisLock
void Gateway::threadInit() {
	initRedisCluster();
	initMongoDB();
	initRedisLock();
}

void Gateway::ZookeeperConnectedHandler() {
	if (ZNONODE == zkclient_->existsNode("/GAME"))
		zkclient_->createNode("/GAME", "Landy");
	//网关自己
	if (ZNONODE == zkclient_->existsNode("/GAME/ProxyServers"))
		zkclient_->createNode("/GAME/ProxyServers", "ProxyServers");
	//大厅
	if (ZNONODE == zkclient_->existsNode("/GAME/HallServers"))
		zkclient_->createNode("/GAME/HallServers", "HallServers");
	//大厅维护
	//if (ZNONODE == zkclient_->existsNode("/GAME/HallServersInvalid"))
	//	zkclient_->createNode("/GAME/HallServersInvalid", "HallServersInvalid");
	//游戏
	if (ZNONODE == zkclient_->existsNode("/GAME/GameServers"))
		zkclient_->createNode("/GAME/GameServers", "GameServers");
	//游戏维护
	//if (ZNONODE == zkclient_->existsNode("/GAME/GameServersInvalid"))
	//	zkclient_->createNode("/GAME/GameServersInvalid", "GameServersInvalid");
	{
		//指定网卡ip:port
		std::vector<std::string> vec;
		//server_ ip:port
		boost::algorithm::split(vec, server_.ipPort(), boost::is_any_of(":"));
		nodeValue_ = strIpAddr_ + ":" + vec[1];
#if 1
		//innServer_ ip:port
		boost::algorithm::split(vec, innServer_.ipPort(), boost::is_any_of(":"));
		nodeValue_ += ":" + vec[1] + ":" + std::to_string(getpid());
#endif
		nodePath_ = "/GAME/ProxyServers/" + nodeValue_;
		//启动时自注册自身节点
		zkclient_->createNode(nodePath_, nodeValue_, true);
		//挂维护中的节点
		invalidNodePath_ = "/GAME/ProxyServersInvalid/" + nodeValue_;
	}
	{
		//大厅服 ip:port
		std::vector<std::string> names;
		if (ZOK == zkclient_->getClildren(
			"/GAME/HallServers",
			names,
			std::bind(
				&Gateway::onHallWatcherHandler, this,
				std::placeholders::_1, std::placeholders::_2,
				std::placeholders::_3, std::placeholders::_4,
				std::placeholders::_5),
			this)) {
			
			clients_[servTyE::kHallTy].add(names);
		}
	}
	{
		//游戏服 ip:port
		std::vector<std::string> names;
		if (ZOK == zkclient_->getClildren(
			"/GAME/GameServers",
			names,
			std::bind(
				&Gateway::onGameWatcherHandler, this,
				std::placeholders::_1, std::placeholders::_2,
				std::placeholders::_3, std::placeholders::_4,
				std::placeholders::_5),
			this)) {

			clients_[servTyE::kGameTy].add(names);
		}
	}
}

void Gateway::onHallWatcherHandler(
	int type, int state, const std::shared_ptr<ZookeeperClient>& zkClientPtr,
	const std::string& path, void* context)
{
	LOG_ERROR << __FUNCTION__;
	//大厅服 ip:port
	std::vector<std::string> names;
	if (ZOK == zkclient_->getClildren(
		"/GAME/HallServers",
		names,
		std::bind(
			&Gateway::onHallWatcherHandler, this,
			std::placeholders::_1, std::placeholders::_2,
			std::placeholders::_3, std::placeholders::_4,
			std::placeholders::_5),
		this)) {

		clients_[servTyE::kHallTy].process(names);
	}
}

void Gateway::onGameWatcherHandler(
	int type, int state, const std::shared_ptr<ZookeeperClient>& zkClientPtr,
	const std::string& path, void* context)
{
	LOG_ERROR << __FUNCTION__;
	//游戏服 roomid:ip:port
	std::vector<std::string> names;
	if (ZOK == zkclient_->getClildren(
		"/GAME/GameServers",
		names,
		std::bind(
			&Gateway::onGameWatcherHandler, this,
			std::placeholders::_1, std::placeholders::_2,
			std::placeholders::_3, std::placeholders::_4,
			std::placeholders::_5),
		this)) {

		clients_[servTyE::kGameTy].process(names);
	}
}

//启动worker线程
//启动TCP监听客户端，websocket
//启动TCP监听客户端，内部推送通知服务
//启动TCP监听客户端，HTTP
void Gateway::start(int numThreads, int numWorkerThreads, int maxSize)
{
	//网络I/O线程数
	numThreads_ = numThreads;
	muduo::net::ReactorSingleton::setThreadNum(numThreads);
	//启动网络I/O线程池，I/O收发读写 recv(read)/send(write)
	muduo::net::ReactorSingleton::start();

	//worker线程数，最好 numWorkerThreads = n * numThreads
	numWorkerThreads_ = numWorkerThreads;
	//创建若干worker线程，启动worker线程池
	for (int i = 0; i < numWorkerThreads; ++i) {
		std::shared_ptr<muduo::ThreadPool> threadPool = std::make_shared<muduo::ThreadPool>("ThreadPool:" + std::to_string(i));
		threadPool->setThreadInitCallback(std::bind(&Gateway::threadInit, this));
		threadPool->setMaxQueueSize(maxSize);
		threadPool->start(1);
		threadPool_.push_back(threadPool);
	}

	LOG_INFO << __FUNCTION__ << " --- *** "
		<< "\nGateway = " << server_.ipPort()
		<< " 网络I/O线程数 = " << numThreads
		<< " worker线程数 = " << numWorkerThreads;

	//Accept时候判断，socket底层控制，否则开启异步检查
	if (blackListControl_ == IpVisitCtrlE::kOpenAccept) {
		server_.setConditionCallback(std::bind(&Gateway::onCondition, this, std::placeholders::_1));
	}

	//Accept时候判断，socket底层控制，否则开启异步检查
	if (whiteListControl_ == IpVisitCtrlE::kOpenAccept) {
		httpServer_.setConditionCallback(std::bind(&Gateway::onHttpCondition, this, std::placeholders::_1));
	}

	//启动TCP监听客户端，websocket
	//使用ET模式accept/read/write
	server_.start(true);

	//启动TCP监听客户端，内部推送通知服务
	//使用ET模式accept/read/write
	innServer_.start(true);

	//启动TCP监听客户端，HTTP
	//使用ET模式accept/read/write
	httpServer_.start(true);

	//等server_所有的网络I/O线程都启动起来
	//sleep(2);

	//获取网络I/O模型EventLoop池
	std::shared_ptr<muduo::net::EventLoopThreadPool> threadPool = 
		/*server_.*/muduo::net::ReactorSingleton::threadPool();
	std::vector<muduo::net::EventLoop*> loops = threadPool->getAllLoops();
	
	//为各网络I/O线程绑定Bucket
	for (size_t index = 0; index < loops.size(); ++index) {
#if 0
		ConnBucketPtr bucket(new ConnBucket(
				loops[index], index, kTimeoutSeconds_));
		bucketsPool_.emplace_back(std::move(bucket));
#else
		bucketsPool_.emplace_back(
			ConnBucketPtr(new ConnBucket(
			loops[index], index, kTimeoutSeconds_)));
#endif
		loops[index]->setContext(EventLoopContextPtr(new EventLoopContext(index)));
	}
	//为每个网络I/O线程绑定若干worker线程(均匀分配)
	{
		int next = 0;
		for (size_t index = 0; index < threadPool_.size(); ++index) {
			EventLoopContextPtr context = boost::any_cast<EventLoopContextPtr>(loops[next]->getContext());
			assert(context);
			context->addWorkerIndex(index);
			if (++next >= loops.size()) {
				next = 0;
			}
		}
	}
	//启动连接超时定时器检查，间隔1s
	for (size_t index = 0; index < loops.size(); ++index) {
		{
			assert(bucketsPool_[index]->index_ == index);
			assert(bucketsPool_[index]->loop_ == loops[index]);
		}
		{
			EventLoopContextPtr context = boost::any_cast<EventLoopContextPtr>(loops[index]->getContext());
			assert(context->index_ == index);
		}
		loops[index]->runAfter(1.0f, std::bind(&ConnBucket::onTimer, bucketsPool_[index].get()));
	}

	//公共定时器
	threadTimer_->startLoop();
}

void Gateway::onGetAesKey(
	const muduo::net::TcpConnectionPtr& conn,
	muduo::net::Buffer* msg) {

}
