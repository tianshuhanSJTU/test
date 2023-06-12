#ifndef SCALELIB_H
#define SCALELIB_H

#include <string>
#include <iostream>
#include <list>
#include <map>
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

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

/* structure of config parameters */
struct config_t
{
	char *server_name;	  /* remote host name */
	u_int32_t tcp_port;   /* remote TCP port */
    int sock;             /* remote sock */
    int free;             /* whether free PHY QP */
};

/* structure of remote host info*/
struct host_t {
    char ip[20];
    int port;
};

/* connect to router via unix socket */
struct sock_with_lock
{
	int sock;
	pthread_mutex_t mutex;
};

/* record conn_res */
struct conn_resources
{
    std::atomic<uint32_t> ref_cnt;      /* reference counting of virtual conn_ids */
    std::atomic<uint8_t> status;        /* status of QP; 0 is created; 1 is inited; 2 is rtr; 4 is rts */

    uint16_t qp_index;                 /* record global index of current qp */
    pthread_mutex_t mutex;             /* control path lock, mutex to handle control operations */

    /* record shm_map */
    CtrlShmPiece* vqp_shm_map[VQ_MAP_SIZE];    /* VQP shm */
    CtrlShmPiece* vcq_shm_map[VQ_MAP_SIZE];    /* VCQ shm */
    pthread_mutex_t vqp_shm_mtx_map[VQ_MAP_SIZE];  /* VQP shm mutex */
    pthread_mutex_t vcq_shm_mtx_map[VQ_MAP_SIZE];  /* VCQ shm mutex */
};

class ScaleLib {
public:
    // current process id
    pid_t process_id;

    /* map of virtual connections, assuming one process --- use one virtual conn for one physcial conn (shared)  */ 
    /** not used yet! not thread safe yet! */ 
    std::map<struct host_t, struct ConnId> conn_list; 
    
    /* map of registered share memory, app_mem vs. router_mem, all physical QP can access mem one registered under the same PD*/
    /** not used yet! not thread safe yet! */ 
    std::map<void *, void *> mem_list;

    ScaleLib();
    ~ScaleLib(); 

    /** Manage QP resources at lib, mirror of router */
    struct conn_resources* conn_map[MAP_SIZE];
    /* mutex for conn_map update */
    pthread_mutex_t conn_mutex; 

    int setup_res_conn(struct ConnIdx *conn_idx);
    int release_res_conn(struct ConnIdx *conn_idx);

    /** Setup SHM communication with router 
     *  input: ConnIdx, output: mmaped memory for virtual Queue: vqp, vcq */ 
    int setup_shm_comm(struct ConnIdx *conn_idx);
    int release_shm_comm(struct ConnIdx *conn_idx);
};

extern ScaleLib scalelib;

/** UDS communication with router 
 *  not thread safe yet! order of requests needs to ensured! 
 */ 
void init_unix_sock();
struct sock_with_lock* get_unix_sock(SCALE_FUNCTION_CALL req);
void connect_router(struct sock_with_lock *unix_sock);
int request_router(SCALE_FUNCTION_CALL req, void* req_body, void *rsp, int *rsp_size);

/** ScaleLib interface */ 
// int init_scale(); // init ScaleLib class, must be initialized before others

int init_res(struct ConnInfo *conn_info);     // init host_key, host_lid; fill src_gid, src_lid
int create_res(struct ConnId *conn_id);     // create QP index by host_key; fill src_qpn 
int reg_mr(struct ConnId *conn_id, struct LocalMem *local_mem, size_t length);       // reg mr; fill memory mapping (client---router), length, lkey/rkey
int setup_conn(struct ConnInfo *conn_info, struct ConnIdx *conn_idx);     // connect QP index by conn_id; fill process_id

// demo implementation, assuming one wr & one sge, remote_mem is be NULL for two-side OP
int post_send(struct ConnIdx *conn_idx, struct LocalMem *local_mem, struct RemoteMem *remote_mem, size_t length, int opcode);    
// demo implementation, assuming one wr & one sge, RXs need to post recv before TXs post send
int post_recv(struct ConnIdx *conn_idx, struct LocalMem *local_mem, size_t length);    
// demo implementation, assuming one wr & one sge
int poll_cq(struct ConnIdx *conn_idx, int count);     

int dereg_mr(struct ConnId *conn_id, struct LocalMem *local_mem);      // derege mr, free client memory
int destroy_res(struct ConnId *conn_id, struct ConnIdx *conn_idx);   // destroy virtual conn, update ref_cnt

/** Socket communication with remote process */
int sock_connect(const char *servername, int port);
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data);
int sock_init(struct config_t *val);
int exchange_meta_nic(int sock, struct ConnInfo *conn_info);  // fill dst_gid, dst_lic
int exchange_meta_conn(int sock, struct ConnInfo *conn_info, struct LocalMem *local_mem, struct RemoteMem *remote_mem); // fill dst_qpn, remote_mem

/* print info */
void print_conn_info(struct ConnInfo *val);
void print_conn_id(struct ConnId *val);
void print_conn_idx(struct ConnIdx *val);
void print_mem_item(struct LocalMem *val);

#endif