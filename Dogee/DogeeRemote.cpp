#include "DogeeRemote.h"
#include "DogeeEnv.h"
#include <thread>
#include <chrono>
#include "DogeeStorage.h"
#include <string.h>

#include <vector>
#include <queue>
#include <unordered_map>
#include "DogeeSocket.h"
#include <atomic>
#include "DogeeLocalSync.h"
#include "DogeeAPIWrapping.h"
#include <map>
#include <assert.h>
#include "DogeeUtil.h"

#ifdef _WIN32
int RcWinsockStartup()
{
	WORD sockVersion = MAKEWORD(2, 2);
	WSADATA wsaData;
	if (WSAStartup(sockVersion, &wsaData) != 0)
	{
		return 1;
	}
	return 0;
}
//called when initialized
int RcStartipRet = RcWinsockStartup();
#pragma comment(lib, "WS2_32")
#endif

#define RC_MAGIC_MASTER 0x12335edf
#define RC_MAGIC_SLAVE 0x33950f0e


enum RcCommand
{
	RcCmdClose = 1,
	RcCmdCreateThread,
	RcCmdSuspendThread,
	RcCmdStopThread,
	RcCmdTriggerGC,
	RcCmdDoGC,
	RcCmdDoneGC,
	RcCmdWakeSync,
	RcCmdEnterBarrier,
	RcCmdEnterSemaphore,
	RcCmdLeaveSemaphore,
	RcCmdWaitForEvent,
	RcCmdSetEvent,
	RcCmdResetEvent,
	RcCmdAlive,
	RcCmdRestart,
};



#pragma pack(push)
#pragma pack(4)

struct MasterInfo
{
	int32_t magic;
	uint32_t num_mem_server;
	uint32_t num_nodes;
	uint32_t node_id;
	int32_t localport;
	Dogee::BackendType backty;
	Dogee::CacheType cachety;
	int32_t checkpoint;
};

struct SlaveInfo
{
	int32_t magic;
};


#pragma pack(pop)

namespace Dogee
{
	extern void (*slave_init_proc)(uint32_t);
	extern void AcInit(SOCKET sock);
	extern void AcClose();
	extern bool AcSlaveInitDataConnections(std::vector<std::string>& hosts, std::vector<int>& ports, int node_id);
	extern void RestartCurrentProcess(std::vector<std::string>& excludes_ip, std::vector<int>& excludes_ports);
	extern void RestartCurrentProcess();

	class RemoteNodes
	{
	private:
		std::vector<SOCKET> connections;
	public:
		void PushConnection(SOCKET s)
		{
			connections.push_back(s);
		}
		SOCKET GetConnection(int node_id)
		{
			return connections[node_id];
		}
	};
	static SOCKET master_socket;
	static RemoteNodes remote_nodes;


	void RcSetRemoteEvent(int local_thread_id);
	/*
	Wake up a remote thread from sync. Called on master
	*/
	void RcWakeRemoteThread(int dest, int thread_id);

	//variables and types only avaiable on master node
	namespace MasterZone
	{
		std::vector<std::string> hosts;
		std::vector<int> ports;
		bool closing = false;
		std::atomic<int> restart_lock = { 0 };
		struct SyncNode
		{
			enum
			{
				Barrier,
				Semaphore,
				Event,
			}Kind;
			int val;
			int data;
			union
			{
				std::vector<SyncThreadNode>* waitlist;
				std::queue<SyncThreadNode>* waitqueue;
			};
		};

		class SyncManager
		{
		private:
			std::unordered_map<ObjectKey, SyncNode*> sync_data;
			std::mutex mutex;
		public:
			std::vector<std::chrono::system_clock::time_point> clocks;
			~SyncManager()
			{
				auto itr = sync_data.begin();
				for (; itr != sync_data.end(); itr++)
				{
					if (itr->second->Kind == SyncNode::Barrier)
					{
						delete itr->second->waitlist;
					}
					else if (itr->second->Kind == SyncNode::Semaphore || itr->second->Kind == SyncNode::Event)
					{
						delete itr->second->waitqueue;
					}
				}
			}
			SyncManager() :clocks(DogeeEnv::num_nodes, std::chrono::system_clock::now())
			{
			}

			void Alive(int src)
			{
				clocks[src] = std::chrono::system_clock::now();
			}

			/*
			Process the barrier message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", 0 represents the master)
			b_id - barrier id
			*/
			void BarrierMsg(int src, ObjectKey b_id, int thread_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				auto itr = sync_data.find(b_id);
				SyncNode* node;
				if (itr == sync_data.end())
				{
					node = new SyncNode;
					node->data = 0;
					node->Kind = SyncNode::Barrier;
					node->data = (int)DogeeEnv::cache->get(b_id, 0);
					node->val = 0;
					node->waitlist = new std::vector<SyncThreadNode>;
					sync_data[b_id] = node;
				}
				else
				{
					node = itr->second;
				}
				node->val++;
				if (node->val >= node->data)
				{
					node->val = 0;
					RcWakeRemoteThread(src, thread_id);
					for (unsigned i = 0; i<node->waitlist->size(); i++)
					{
						SyncThreadNode& th = (*node->waitlist)[i];
						RcWakeRemoteThread(th.machine, th.thread_id);
					}
					node->waitlist->clear();
				}
				else
				{
					SyncThreadNode th = { src, thread_id };
					node->waitlist->push_back(th);
				}
			}

			/*
			Process the semaphore message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", represents the master)
			b_id - semaphore id
			*/
			void SemaphoreMsg(int src, ObjectKey b_id, int thread_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				auto itr = sync_data.find(b_id);
				SyncNode* node;
				if (itr == sync_data.end())
				{
					node = new SyncNode;
					node->data = 0;
					node->Kind = SyncNode::Semaphore;
					uint64_t val = DogeeEnv::cache->get(b_id, 0);
					node->data = *(int*)&val;
					node->val = node->data;
					node->waitqueue = new std::queue<SyncThreadNode>;
					sync_data[b_id] = node;
				}
				else
				{
					node = itr->second;
				}
				node->val--;
				if (node->val >= 0)
				{
					RcWakeRemoteThread(src, thread_id);
				}
				else
				{
					SyncThreadNode th = { src, thread_id };
					node->waitqueue->push(th);
				}
			}

			/*
			Process the semaphore release message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", 0 represents the master)
			b_id - semaphore id
			*/
			void SemaphoreLeaveMsg(int src, ObjectKey b_id, int thread_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				auto itr = sync_data.find(b_id);
				SyncNode* node;
				if (itr == sync_data.end())
				{
					node = new SyncNode;
					node->data = 0;
					node->Kind = SyncNode::Semaphore;
					uint64_t val = DogeeEnv::cache->get(b_id, 0);
					node->data = *(int*)val;
					node->val = node->data;
					node->waitqueue = new std::queue<SyncThreadNode>;
					sync_data[b_id] = node;
				}
				else
				{
					node = itr->second;
				}
				node->val++;
				if (node->val >= 0)
				{
					if (!node->waitqueue->empty())
					{
						SyncThreadNode& th = node->waitqueue->front();
						RcWakeRemoteThread(th.machine, th.thread_id);
						node->waitqueue->pop();
					}
				}
			}


			SyncNode*  CreateEventNode(ObjectKey b_id)
			{
				SyncNode* node;
				auto itr = sync_data.find(b_id);
				if (itr == sync_data.end())
				{
					node = new SyncNode;
					node->data = DogeeEnv::cache->get(b_id, 0); //->data=auto_reset
					node->Kind = SyncNode::Event;
					node->val = DogeeEnv::cache->get(b_id, 2); //->val=is_signal
					node->waitqueue = new std::queue<SyncThreadNode>;
					sync_data[b_id] = node;
				}
				else
				{
					node = itr->second;
				}
				return node;
			}
			/*
			Process the event-wait message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", represents the master)
			b_id - event id
			*/
			void EventWaitMsg(int src, ObjectKey b_id, int thread_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				SyncNode* node = CreateEventNode(b_id);
				if (node->val)//if is signaled
				{
					RcWakeRemoteThread(src, thread_id);
					if (node->data)//if is autoreset
					{
						node->val = 0; //reset the event
					}
				}
				else
				{
					SyncThreadNode th = { src, thread_id };
					node->waitqueue->push(th);
				}
			}


			/*
			Process the event-set message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", represents the master)
			b_id - event id
			*/
			void SetEventMsg(int src, ObjectKey b_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				SyncNode* node = CreateEventNode(b_id);
				node->val = 1;

				if (node->data)//if is autoreset
				{
					if (node->waitqueue->empty())
					{
						node->val = 1;
					}
					else
					{
						node->val = 0;
						SyncThreadNode th = node->waitqueue->front();
						node->waitqueue->pop();
						RcWakeRemoteThread(th.machine, th.thread_id);
					}
				}
				else//if not, release all threads
				{
					while (!node->waitqueue->empty())
					{
						SyncThreadNode th = node->waitqueue->front();
						node->waitqueue->pop();
						RcWakeRemoteThread(th.machine, th.thread_id);
					}
				}
			}

			/*
			Process the event-reset message, may call the nodes to
			continue, called on master
			param: src - the source of the message (index of
			"slavenodes", represents the master)
			b_id - event id
			*/
			void ResetEventMsg(int src, ObjectKey b_id)
			{
				std::unique_lock<std::mutex> lock(mutex);
				SyncNode* node = CreateEventNode(b_id);
				node->val = 0;
			}

		};
		SyncManager* syncmanager;
		std::thread masterlisten;
		std::thread masteralive;
	}


	namespace Socket
	{
		SOCKET RcCreateListen(int port)
		{
			SOCKET slisten = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			int reuse = 1;
			if (setsockopt(slisten, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
				perror("setsockopt(SO_REUSEADDR) failed");

#if defined(SO_REUSEPORT) & defined(DOGEE_REUSE_PORT)
			reuse = 1;
			if (setsockopt(slisten, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse)) < 0)
				perror("setsockopt(SO_REUSEPORT) failed");
#endif
			if (slisten == INVALID_SOCKET)
			{
				printf("socket error ! \n");
				return 0;
			}

			//绑定IP和端口
			sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_port = htons(port);
#ifdef _WIN32
			sin.sin_addr.S_un.S_addr = INADDR_ANY;
#else
			sin.sin_addr.s_addr = INADDR_ANY;
#endif
			if (bind(slisten, (LPSOCKADDR)&sin, sizeof(sin)) == SOCKET_ERROR)
			{
				printf("bind error !");
				return 0;
			}

			//开始监听
			if (listen(slisten, 5) == SOCKET_ERROR)
			{
				printf("listen error !");
				return 0;
			}
			return (SOCKET)slisten;
		}
		void RcSetTCPNoDelay(SOCKET fd)
		{
			int enable = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&enable, sizeof(enable));
		}

		SOCKET RcConnect(char* ip, int port)
		{
			SOCKET sclient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (sclient == INVALID_SOCKET)
			{
				return (SOCKET)NULL;
			}

			sockaddr_in serAddr;
			memset(&serAddr, 0, sizeof(serAddr));
			serAddr.sin_family = AF_INET;
			serAddr.sin_port = htons(port);
#ifdef _WIN32
			serAddr.sin_addr.S_un.S_addr = inet_addr(ip);
#else
			serAddr.sin_addr.s_addr = inet_addr(ip);
#endif
			if (connect(sclient, (sockaddr *)&serAddr, sizeof(serAddr)) == SOCKET_ERROR)
			{
				closesocket(sclient);
				return (SOCKET)NULL;
			}
			return (SOCKET)sclient;
		}

		SOCKET RcListen(int port)
		{
			SOCKET slisten = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			int reuse = 1;
			if (setsockopt(slisten, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
				perror("setsockopt(SO_REUSEADDR) failed");

#if defined(SO_REUSEPORT) & defined(DOGEE_REUSE_PORT)
			reuse = 1;
			if (setsockopt(slisten, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse)) < 0)
				perror("setsockopt(SO_REUSEPORT) failed");
#endif
			if (slisten == INVALID_SOCKET)
			{
				printf("socket error ! \n");
				return 0;
			}

			//绑定IP和端口
			sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_port = htons(port);
#ifdef _WIN32
			sin.sin_addr.S_un.S_addr = INADDR_ANY;
#else
			sin.sin_addr.s_addr = INADDR_ANY;
#endif
			if (bind(slisten, (LPSOCKADDR)&sin, sizeof(sin)) == SOCKET_ERROR)
			{
				printf("bind error !");
				return 0;
			}

			//开始监听
			if (listen(slisten, 5) == SOCKET_ERROR)
			{
				printf("listen error !");
				return 0;
			}

			//循环接收数据
			sockaddr_in remoteAddr;
#ifdef _WIN32
			int nAddrlen = sizeof(remoteAddr);
#else
			unsigned int nAddrlen = sizeof(remoteAddr);
#endif
			printf("port %d waiting for connections...\n", port);
			SOCKET sClient = accept(slisten, (LPSOCKADDR)&remoteAddr, &nAddrlen);
			if (sClient == INVALID_SOCKET)
			{
				printf("accept error !");
				return 0;
			}
			printf("port %d accepted ：%s \n", port, inet_ntoa(remoteAddr.sin_addr));
			closesocket(slisten);
			return (SOCKET)sClient;
		}

		SOCKET RcAccept(SOCKET slisten)
		{
			sockaddr_in remoteAddr;
#ifdef _WIN32
			int nAddrlen = sizeof(remoteAddr);
#else
			unsigned int nAddrlen = sizeof(remoteAddr);
#endif
			SOCKET sClient = accept(slisten, (LPSOCKADDR)&remoteAddr, &nAddrlen);
			if (sClient == INVALID_SOCKET)
			{
				printf("accept error !");
				return 0;
			}
			return (SOCKET)sClient;
		}

		void get_peer_ip_port(SOCKET fd, std::string& ip, int& port)
		{

			// discovery client information
			struct sockaddr_in addr;
#ifdef _WIN32
			int addrlen = sizeof(addr);
#else
			socklen_t addrlen = sizeof(addr);
#endif
			if (getpeername((SOCKET)fd, (struct sockaddr*)&addr, &addrlen) == -1){
				fprintf(stderr, "discovery client information failed, fd=%d, errno=%d(%#x).\n", fd, errno, errno);
				return;
			}
			port = ntohs(addr.sin_port);
			ip = inet_ntoa(addr.sin_addr);

			return;
		}

		int RcSendCmd(SOCKET s, RcCommandPack* cmd)
		{
			int ret = RcSend(s, cmd, sizeof(RcCommandPack));
			if (ret == SOCKET_ERROR)
			{
				return RcSocketLastError();
			}
			return 0;
		}

	}

	std::map<int, Event*> ThreadEventMap;
	extern std::atomic<int> tid_count;
	extern THREAD_LOCAL int current_thread_id;
	BD_RWLOCK thread_lock;
	int chk = 0;
	bool RcWaitForRemoteEvent(int timeout)
	{
		UaEnterReadRWLock(&thread_lock);
		bool ret= ThreadEventMap[current_thread_id]->WaitForEvent(timeout);
		UaLeaveReadRWLock(&thread_lock);
		return ret;
	}
	void RcSetRemoteEvent(int local_thread_id)
	{
		UaEnterReadRWLock(&thread_lock);
		ThreadEventMap[local_thread_id]->SetEvent();
		UaLeaveReadRWLock(&thread_lock);
	}

	void RcResetRemoteEvent()
	{
		UaEnterReadRWLock(&thread_lock);
		ThreadEventMap[current_thread_id]->ResetEvent();
		UaLeaveReadRWLock(&thread_lock);
	}

	void RcPrepareNewThread()
	{
		UaEnterWriteRWLock(&thread_lock);
		ThreadEventMap[current_thread_id]=new Event(false);
		UaLeaveWriteRWLock(&thread_lock);
	}

	void RcDeleteThread()
	{
		UaEnterWriteRWLock(&thread_lock);
		delete ThreadEventMap[current_thread_id];
		ThreadEventMap.erase(current_thread_id);
		UaLeaveWriteRWLock(&thread_lock);
	}

	void RcInitThreadSystem()
	{
		UaInitRWLock(&thread_lock);
	}
	void RcFinalizeThreadSystem()
	{
		UaKillRWLock(&thread_lock);
	}
	using namespace Socket;
	extern void ThThreadEntryObject(int thread_id, int index, uint32_t param, ObjectKey okey,char* data);
	extern void ThThreadEntry(int thread_id,int index, uint32_t param,ObjectKey okey );
	extern void InitSharedConst();
	extern void DoRestart();
	extern int checkpoint_cnt;
	extern void InitDThreadPool();
	extern void DeleteDThreadPool();
	void RcSlaveMainLoop(char* path, SOCKET s, std::vector<std::string>& hosts, std::vector<int>& ports,
		std::vector<std::string>& mem_hosts, std::vector<int>& mem_ports, int node_id,
		BackendType backty, CacheType cachety,int checkpoint)
	{

		DogeeEnv::InitCurrentThread();
		if (slave_init_proc)
			slave_init_proc(node_id);
		if (DogeeEnv::InitCheckpoint)
			DogeeEnv::InitCheckpoint();
		if (checkpoint >= 0)
		{
			checkpoint_cnt = checkpoint;
			DoRestart();
		}
		for (;;)
		{
			RcCommandPack cmd;
			int ret = RcRecv(s, &cmd, sizeof(cmd));
			if (ret != sizeof(cmd))
			{
				printf("Socket error!\n");
				break;
			}
			InitSharedConst();
			switch (cmd.cmd)
			{
			case RcCmdClose:
				printf("Closing!\n");
				goto CLOSE;
				break;
			case RcCmdCreateThread:
				char pbuf[2048];
				if (cmd.param4)
				{
					if (cmd.param4 > 2048)
					{
						printf("function object too large\n");
						break;
					}
					int ret2 = RcRecv(s, pbuf, cmd.param4);
					if (ret2 != cmd.param4)
					{
						printf("Socket error!\n");
						break;
					}
					char* data = new char[cmd.param4];
					memcpy(data, pbuf, cmd.param4);
					std::thread(ThThreadEntryObject, 0, cmd.param, cmd.param2, cmd.param3,data).detach();
				}
				else
				{
					std::thread(ThThreadEntry, 0, cmd.param, cmd.param2, cmd.param3).detach();
				}
				break;
			case RcCmdWakeSync:
				RcSetRemoteEvent(cmd.param);
				break;
			case RcCmdAlive:
				RcSend(s, &cmd, sizeof(cmd));
				break;
			case RcCmdRestart:
				RestartCurrentProcess();
				break;
			default:
				printf("Unknown command %d\n", cmd.cmd);
			}
		}
		CLOSE:
		return;
	}

	extern bool AcWaitForReady();

	void RcSlave(int port)
	{
		printf("port %d waiting for connections...\n", port);
		SOCKET slisten = RcCreateListen(port);
		SOCKET s = RcAccept(slisten);
		printf("Waiting for hand shaking...\n");
		MasterInfo mi;
		SlaveInfo si;
		si.magic = RC_MAGIC_SLAVE;
		RcSend(s, &si, sizeof(si));
		int cnt = RcRecv(s, &mi, sizeof(mi));
		int err = 0, err2 = 0;
		if (cnt == sizeof(mi) && mi.magic == RC_MAGIC_MASTER)
		{
			char mainmod[255];

			DogeeEnv::self_node_id = mi.node_id;
			DogeeEnv::num_nodes = mi.num_nodes;

			char buf[255];
			

			//printf("Host list :\n");
			std::vector<std::string> hosts;
			std::vector<int> ports;

			std::string master;
			int masterport;
			get_peer_ip_port(s, master, masterport);

			hosts.push_back(master);
			ports.push_back(mi.localport);
			//printf("Master = %s:%d\n",master.c_str(),mi.localport);

			for (unsigned i = 1; i<mi.num_nodes; i++)
			{
				uint32_t len, port;
				err = 5;
				if (RcRecv(s, &len, sizeof(len)) != sizeof(len))
					goto ERR;
				if (len>255)
				{
					err = 6;
					goto ERR;
				}
				if (RcRecv(s, buf, len) != len)
				{
					err = 7;
					goto ERR;
				}
				if (RcRecv(s, &port, sizeof(port)) != sizeof(port))
				{
					err = 8;
					goto ERR;
				}
				hosts.push_back(std::string(buf));
				ports.push_back(port);
				//printf("%s:%d\n",buf,port);
			}

			//printf("Memory server list :\n");
			std::vector<std::string> memhosts;
			std::vector<int> memports;
			for (unsigned i = 0; i<mi.num_mem_server; i++)
			{
				uint32_t len, port;
				err = 5;
				if (RcRecv(s, &len, sizeof(len)) != sizeof(len))
					goto ERR;
				if (len>255)
				{
					err = 6;
					goto ERR;
				}
				if (RcRecv(s, buf, len) != len)
				{
					err = 7;
					goto ERR;
				}
				if (RcRecv(s, &port, sizeof(port)) != sizeof(port))
				{
					err = 8;
					goto ERR;
				}
				memhosts.push_back(std::string(buf));
				memports.push_back(port);
				//printf("%s:%d\n",buf,port);
			}
			DogeeEnv::InitStorage(mi.backty, mi.cachety, hosts, ports, memhosts, memports, mi.node_id);
			AcInit(slisten);
			if (!AcSlaveInitDataConnections(hosts, ports, mi.node_id))
				goto ERR;
			
			if (!AcWaitForReady())
			{
				printf("Wait for data socket timeout\n");
				goto ERR;
			}
			
			master_socket = s;
			DogeeEnv::SetIsMaster(false);
			
			RcSlaveMainLoop(mainmod, s, hosts, ports, memhosts, memports, mi.node_id,mi.backty,mi.cachety,mi.checkpoint);
			
		}
		else
		{
		ERR:
			printf("Hand shaking error! %d %d\n", err, err2);

		}
		DogeeEnv::CloseStorage(); //*/
		RcCloseSocket(s);
		AcClose();
		DeleteDThreadPool();
		//fix-me : kill control listen thread
	}



	int RcMasterHello(SOCKET s, std::vector<std::string>& hosts, std::vector<int>& ports,
		std::vector<std::string>& memhosts, std::vector<int>& memports, uint32_t node_id,
		BackendType backty, CacheType cachety,int checkpoint)
	{
		SlaveInfo si;
		unsigned mem_cnt;
		mem_cnt = memhosts.size();
		unsigned host_cnt = hosts.size();
		if (RcRecv(s, &si, sizeof(si)) != sizeof(si))
		{
			return 1;
		}
		if (si.magic != RC_MAGIC_SLAVE)
		{
			return 2;
		}
		MasterInfo mi = { RC_MAGIC_MASTER, mem_cnt, host_cnt, node_id, ports[0] ,backty,  cachety,checkpoint };
		RcSend(s, &mi, sizeof(mi));

		for (unsigned i = 1; i<host_cnt; i++)
		{

			uint32_t sendl = hosts[i].size() + 1;
			if (sendl>255)
			{
				sendl = 255;
				hosts[i][254] = 0;
			}
			RcSend(s, &sendl, sizeof(sendl));
			RcSend(s, (char*)hosts[i].c_str(), sendl);
			sendl = ports[i];
			RcSend(s, &sendl, sizeof(sendl));
		}

		for (unsigned i = 0; i<mem_cnt; i++)
		{

			uint32_t sendl = memhosts[i].size() + 1;
			if (sendl>255)
			{
				sendl = 255;
				memhosts[i][254] = 0;
			}
			RcSend(s, &sendl, sizeof(sendl));
			RcSend(s, (char*)memhosts[i].c_str(), sendl);
			sendl = memports[i];
			RcSend(s, &sendl, sizeof(sendl));
		}
		return 0;
	}

	void RestartCluster(std::vector<std::string>& excludes_ip, std::vector<int>& excludes_ports);
	void RestartCluster();
	static void RcMasterAliveListen()
	{
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			std::vector<std::string> ex_ip;
			std::vector<int> ex_port;
			for (int i = 1; i < DogeeEnv::num_nodes; i++)
			{
				if (std::chrono::system_clock::now() - MasterZone::syncmanager->clocks[i]>std::chrono::seconds(5))
				{
					ex_ip.push_back(MasterZone::hosts[i]);
					ex_port.push_back(MasterZone::ports[i]);
				}
				RcCommandPack cmd;
				cmd.cmd = RcCmdAlive;
				RcSend(remote_nodes.GetConnection(i), &cmd, sizeof(cmd));
			}
			if (ex_ip.size() > 0 && !MasterZone::closing)
			{
				RestartCluster(ex_ip, ex_port);
			}
		}
	}

	static void RcMasterListen();
	extern void DeleteSharedConstInitializer();
	extern int MasterCheckCheckPoint();
	int RcMaster(std::vector<std::string>& hosts, std::vector<int>& ports,
		std::vector<std::string>& memhosts, std::vector<int>& memports,
		BackendType backty, CacheType cachety)
	{
		int checkpoint=MasterCheckCheckPoint();
		//push master node as node_id=0
		remote_nodes.PushConnection(0);
		for (unsigned i = 1; i < hosts.size(); i++)
		{
			SOCKET s = (SOCKET)RcConnect((char*)hosts[i].c_str(), ports[i]);
			if (s == 0 || RcMasterHello((SOCKET)s, hosts, ports, memhosts, memports, i, backty, cachety, checkpoint))
				return 1;
			remote_nodes.PushConnection(s);
		}

		MasterZone::hosts = hosts;
		MasterZone::ports = ports;
		DogeeEnv::SetIsMaster(true);
		DogeeEnv::num_nodes = hosts.size();
		DogeeEnv::self_node_id = 0;
		DogeeEnv::InitStorage(backty, cachety,hosts,ports, memhosts, memports,0);
		DogeeEnv::InitCurrentThread();
		MasterZone::masterlisten =std::move( std::thread(RcMasterListen));
		MasterZone::masterlisten.detach();
		MasterZone::syncmanager = new MasterZone::SyncManager;
		if (DogeeEnv::InitCheckpoint)
		{
			MasterZone::masteralive = std::move(std::thread(RcMasterAliveListen));
			MasterZone::masteralive.detach();
		}
		AcInit(Socket::RcCreateListen(ports[0]));
		printf("Master Listen port %d\n", ports[0]);
		if (!AcWaitForReady())
		{
			printf("Wait for data socket timeout\n");
			return 1;
		}
		if (DogeeEnv::InitCheckpoint)
			DogeeEnv::InitCheckpoint();
		DeleteSharedConstInitializer();
		if (checkpoint >= 0)
		{
			checkpoint_cnt = checkpoint;
			DoRestart();
			exit(0);
		}
		return 0;

	}

	void CloseCluster()
	{
		MasterZone::closing = true;
		RcCommandPack cmd;
		cmd.cmd = RcCmdClose;
		for (int i = 1; i < DogeeEnv::num_nodes; i++)
		{
			RcSendCmd(remote_nodes.GetConnection(i), &cmd);
		}
		if (DogeeEnv::DeleteCheckpoint)
			DogeeEnv::DeleteCheckpoint();
		delete MasterZone::syncmanager;
		AcClose();
		DogeeEnv::CloseStorage();
		DeleteDThreadPool();
	}

	int RcCreateThread(int node_id,uint32_t idx,uint32_t param,ObjectKey okey)
	{
		assert(DogeeEnv::isMaster());
		int _idx = idx;
		int _param = param;
		RcCommandPack cmd = { RcCmdCreateThread, _idx, _param };
		cmd.param3 = okey;
		cmd.param4 = 0;
		int sret = RcSendCmd(remote_nodes.GetConnection(node_id), &cmd);
		return sret;
	}

	int RcCreateThread(int node_id, uint32_t idx, uint32_t param, ObjectKey okey,void* data,uint32_t len)
	{
		assert(DogeeEnv::isMaster());
		int _idx = idx;
		int _param = param;
		assert(len <= 2048);
		char pbuf[2048 + sizeof(RcCommandPack)];
		RcCommandPack* pcmd = (RcCommandPack*)pbuf;
		pcmd->cmd = RcCmdCreateThread;
		pcmd->param = _idx;
		pcmd->param2 = _param;
		pcmd->param3 = okey;
		pcmd->param4 = len;
		memcpy(pbuf + sizeof(RcCommandPack), data, len);
		int ret = RcSend(remote_nodes.GetConnection(node_id), pcmd, sizeof(RcCommandPack)+len);
		if (ret == SOCKET_ERROR)
		{
			return RcSocketLastError();
		}
		return 0;
	}

	void RcWakeRemoteThread(int dest, int thread_id)
	{
		//printf("Wake %d.%d\n",dest,thread_id);
		if (dest == 0)
		{
			RcSetRemoteEvent(thread_id);
			return;
		}
		RcCommandPack cmd;
		cmd.cmd = RcCmdWakeSync;
		cmd.param = thread_id;
		RcSend(remote_nodes.GetConnection(dest), &cmd, sizeof(cmd));
	}

	void RestartCluster(std::vector<std::string>& excludes_ip, std::vector<int>& excludes_ports)
	{
		int old=MasterZone::restart_lock.exchange(1);//compete for the lock, if anyone has already locked it, we just do nothing
		if (old == 1)
			return;
		RcCommandPack cmd;
		cmd.cmd = RcCmdRestart;
#ifndef _WIN32
		signal(SIGPIPE, SIG_IGN);
#endif
		for (int i = 1; i<DogeeEnv::num_nodes; i++)
		{
			if (SOCKET_ERROR == RcSend(remote_nodes.GetConnection(i), &cmd, sizeof(cmd)))
			{
				excludes_ip.push_back(MasterZone::hosts[i]);
				excludes_ports.push_back(MasterZone::ports[i]);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		RestartCurrentProcess(excludes_ip, excludes_ports);
	}
	void RestartCluster()
	{
		std::vector<std::string> excludes_ip;
		std::vector<int> excludes_ports;
		RestartCluster(excludes_ip, excludes_ports);
	}
	static void RcMasterListen()
	{
		int n = DogeeEnv::num_nodes;
		int maxfd;
		fd_set readfds;
		RcCommandPack cmd;
		if (n > 1)
			maxfd = remote_nodes.GetConnection(1);
		else
			maxfd = 0;
#ifndef _WIN32
		for (int i = 2; i<n; i++)
		{
			auto tmp= remote_nodes.GetConnection(i);
			if (tmp>maxfd)
				maxfd = tmp;
		}
		maxfd++;
#endif
		void* memc = NULL;
		for (;;)
		{
			FD_ZERO(&readfds);
			for (int i = 1; i<n; i++)
			{
				FD_SET(remote_nodes.GetConnection(i), &readfds);
			}
			if (SOCKET_ERROR == select(maxfd, &readfds, NULL, NULL, NULL))
			{
				printf("Select Error!%d\n", RcSocketLastError());
				if (DogeeEnv::InitCheckpoint && !MasterZone::closing)
					RestartCluster();
				break;
			}
			DogeeEnv::InitCurrentThread();
			for (int i = 1; i<n; i++)
			{
				auto sock = remote_nodes.GetConnection(i);
				if (FD_ISSET(sock, &readfds))
				{
					if (RcRecv(sock, &cmd, sizeof(cmd)) != sizeof(cmd))
					{
						printf("Socket recv Error! %d\n", RcSocketLastError());
						if (DogeeEnv::InitCheckpoint && !MasterZone::closing)
							RestartCluster();
						return ;
					}
					switch (cmd.cmd)
					{
					case RcCmdEnterBarrier:
						MasterZone::syncmanager->BarrierMsg(i, cmd.param, cmd.param2);
						break;
					case RcCmdEnterSemaphore:
						MasterZone::syncmanager->SemaphoreMsg(i, cmd.param, cmd.param2);
						break;
					case RcCmdLeaveSemaphore:
						MasterZone::syncmanager->SemaphoreLeaveMsg(i, cmd.param, cmd.param2);
						break;
					case RcCmdWaitForEvent:
						MasterZone::syncmanager->EventWaitMsg(i, cmd.param, cmd.param2);
						break;
					case RcCmdSetEvent:
						MasterZone::syncmanager->SetEventMsg(i, cmd.param);
						break;
					case RcCmdResetEvent:
						MasterZone::syncmanager->ResetEventMsg(i, cmd.param);
						break;
					case RcCmdAlive:
						MasterZone::syncmanager->Alive(i);
						break;
					default:
						printf("Bad command %u!\n", cmd.cmd);
					}
				}
			NEXT:
				int dummy;
			}

		}
	}

	bool RcEnterBarrier(ObjectKey okey, int timeout)
	{
		ThreadEventMap[current_thread_id]->ResetEvent();
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->BarrierMsg(0, okey, current_thread_id);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdEnterBarrier;
			cmd.param = okey;
			cmd.param2 = current_thread_id;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
		return ThreadEventMap[current_thread_id]->WaitForEvent(timeout);
	}


	void RcSetEvent(ObjectKey okey)
	{
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->SetEventMsg(0, okey);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdSetEvent;
			cmd.param = okey;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
	}
	void RcResetEvent(ObjectKey okey)
	{
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->ResetEventMsg(0, okey);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdResetEvent;
			cmd.param = okey;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
	}
	bool RcWaitForEvent(ObjectKey okey, int timeout)
	{
		ThreadEventMap[current_thread_id]->ResetEvent();
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->EventWaitMsg(0, okey, current_thread_id);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdWaitForEvent;
			cmd.param = okey;
			cmd.param2 = current_thread_id;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
		return ThreadEventMap[current_thread_id]->WaitForEvent(timeout);
	}

	bool RcEnterSemaphore(ObjectKey okey, int timeout)
	{
		ThreadEventMap[current_thread_id]->ResetEvent();
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->SemaphoreMsg(0, okey, current_thread_id);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdEnterSemaphore;
			cmd.param = okey;
			cmd.param2 = current_thread_id;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
		return ThreadEventMap[current_thread_id]->WaitForEvent(timeout);
	}

	void RcLeaveSemaphore(ObjectKey okey)
	{
		if (DogeeEnv::isMaster())
		{
			MasterZone::syncmanager->SemaphoreLeaveMsg(0, okey, current_thread_id);
		}
		else
		{
			RcCommandPack cmd;
			cmd.cmd = RcCmdLeaveSemaphore;
			cmd.param = okey;
			cmd.param2 = current_thread_id;
			RcSend(master_socket, &cmd, sizeof(cmd));
		}
	}


}