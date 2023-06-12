#ifndef SCALEROUTER_H
#define SCALEROUTER_H

#include <string>
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <sstream>
#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <malloc.h>
#include <errno.h>
#include <byteswap.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/un.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <rdma/rdma_cma.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <getopt.h>
#include <infiniband/verbs.h>

#include "log.h"
#include "common.h"
#include "types.h"
#include "shared_memory.h"
#include "arch.h"

/* structure of config parameters */
struct config_t
{
	const char *dev_name; /* IB device name */
	char *server_name;	  /* router host name */
	u_int32_t tcp_port;   /* router TCP port */
	int ib_port;		  /* local IB port to work with */
	int gid_idx;		  /* gid index to use */
    int disable_rdma;     /* disable shared memory communication*/
};

/* structure of global ibv resources */
struct global_resources
{
	/* --- global res for router --- */
	struct ibv_device_attr device_attr;		/* Device attributes */
	struct ibv_port_attr port_attr;     	/* IB port attributes */
	struct ibv_context *ib_ctx;     		/* Device handle */
	struct ibv_pd *pd;          			/* PD handle */
	int epoll_fd;								/* Create epoll fd for Completion Events */
	struct epoll_event ev_list[MAX_HOST_NUM];	/* List to store all returned events from epoll */

    ibv_gid src_gid;    /* local gid */
    uint16_t src_lid;   /* local lid */
};

/* structure of QP resources */
struct conn_resources
{
    /* --- connection res of each QP --- */
	// struct ConnKey *conn_key;       /* identify QP connection, src_gid, dst_gid, src_qpn, dst_qpn */
    // struct global_resources *global_res; /* pointer to global resource */
	struct ibv_cq *cq;          /* CQ handle */
	struct ibv_qp *qp;          /* QP handle */
	struct ibv_comp_channel *ev_channel;        /* channel for Completion Events */
	struct ibv_cq *ev_cq;						/* CQ that got the Completion Events */
	void *ev_ctx;								/* CQ context of the CQ that got the Completion Events */
    
    std::atomic<uint32_t> ref_cnt;      /* reference counting of virtual conn_ids */
    std::atomic<uint8_t> status;        /* status of QP; 0 is created; 1 is inited; 2 is rtr; 4 is rts */
    uint16_t qp_index;                 /* record global index of current qp */

    pthread_mutex_t mutex;             /* control path lock, mutex to handle control operations */
    pthread_mutex_t data_mutex;        /* use a seperate lock for data path, mutex to handle data operations, handle_process only */

    ShmPiece* vqp_shm_map[VQ_MAP_SIZE];    /* VQP shm */
    ShmPiece* vcq_shm_map[VQ_MAP_SIZE];    /* VCQ shm */
    uint16_t vq_index = 0;                 /* global vqp_index sharing current qp, always increase with new VQ until overflow, [0, VQ_MAP_SIZE) */
    std::vector<uint16_t> vq_shm_vec;      /* record vq_index */
    pthread_mutex_t vq_mutex;              /* mutex for for vq_shm_vec update & query only */
};

struct WorkerArgs {
    struct ScaleRouter *router;
};

struct HandlerArgs {
    struct ScaleRouter *router;
    int client_sock;
    int count;
};

struct ConnlistLock
{
    std::list<void *> conn_list; /* list <*conn_res> */
    pthread_mutex_t mutex; /* global read/write mutex */
};

struct HostmapLock
{
    std::unordered_map<struct HostKey, void *> host_map; /* map <host_key, conn_res> */
	pthread_mutex_t mutex; /* global read/write mutex */
};

struct ShmmapLock
{
    // process_id --> shared memmory piece vector
    std::map<pid_t, std::vector<ShmPiece*>> shm_map;
    // std::map<std::string, ShmPiece* > shm_map;
    pthread_mutex_t mutex;
};

struct ConnmapLock
{
    std::unordered_map<struct ConnId, void *> conn_map; /* map <conn_id, conn_res> */
    pthread_mutex_t mutex; /* global read/write mutex */
};

class ScaleRouter{
public:
    /* global configurations */
    struct config_t config;

    /* RDMA resources */
    struct global_resources global_res;
    // list of pointer to conn_res, is conn_res list required? for the ease of free resource?
    struct ConnlistLock connlist_lock;      
    // index QP created for src---dst host (PF/VF); need to be thread safe
    struct HostmapLock hostmap_lock; 
    // index MR by client process_id---shared mem; need to be thread safe
    struct ShmmapLock shmmap_lock; 
    // TODO! concurrent hash map to index conn_id: <conn_id, &conn_res>; global read/write lock with low-level contentions
    struct ConnmapLock connmap_lock;
    // TODO! sychronization between host_map&conn_map

    /* Lockfree index of RDMA resources */
    struct conn_resources* conn_map[MAP_SIZE];
    // record qp_index
    std::vector<uint16_t> qp_vec;
    // global qp_index, always increase with new QP until overflow, [0, MAP_SIZE)
    uint16_t qp_index = 0;         
    // mutex for qp_index
    pthread_mutex_t qp_mutex; 

    /* queue resources */
    // TODO! shared SQ/RQ for each host_key/conn_key
    // TODO! CQ for each conn_id

    // worker thread id, work_process
    pthread_t worker_id;
    // dispatcher thread id, listen_local_process
    pthread_t dispatcher_id;

    ScaleRouter(struct config_t config);
    ~ScaleRouter();    
    
    // start main loop, dispatcher
    void start();
    
    void print_config();
    
    int init_global_res();
    int destroy_global_res();

    int init_global_mutex();
    int destroy_global_mutex();

    /* create phy QP */
    int resources_create(struct conn_resources *res);
    int resources_destroy(struct conn_resources *res);

    /* modify QP status */
    int modify_qp_to_ready(struct ConnInfo *conn_info, struct conn_resources *res);
    int modify_qp_to_init(struct ibv_qp *qp);
    int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid);
    int modify_qp_to_rts(struct ibv_qp *qp);

    /* allocate/deallocate shared mem piece */
    ShmPiece* addShmPiece(int process_id, int mem_size);
    void freeShmPiece();

    /* allocate/deallocate ctrl shm piece */
    ShmPiece* initCtrlShm(const char* tag); 
};



/* handle client request */
void *work_process(void *args);
void *handle_process(void *args);
void *listen_local_process(void *args);

/* print info */
void print_conn_info(struct ConnInfo *val);
void print_conn_id(struct ConnId *val);

#endif