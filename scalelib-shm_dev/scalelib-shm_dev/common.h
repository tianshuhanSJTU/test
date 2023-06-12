/** common data structure client&router */

// max number of hosts
#define MAX_HOST_NUM 8
#define MAP_SIZE MAX_HOST_NUM
#define VQ_MAP_SIZE 1024

#define CTRL_REQ_SIZE 1024 // 1 KB
#define CTRL_RSP_SIZE 1024 // 1 KB

// use page as deafult mem unit
#define PAGE_SIZE 4096 

// define default unix communicaiton domain
#define AGENT_DOMAIN "/tmp/scale.domain"

/* key of src---dst host (PF/VF) */
struct HostKey{
    ibv_gid src_gid;     /* RNIC src gid */
    ibv_gid dst_gid;     /* RNIC dst gid */
}__attribute__((packed));

/* local LID of IB port in src---dst host (PF/VF), maybe not considered in RoCEv2 */
struct HostLid{
    uint16_t src_lid;	/* LID of the IB port, src or local */
    uint16_t dst_lid;   /* LID of the IB prot, dst or remote, used for build QP connection */
}__attribute__((packed));

struct ConnKey{
    // uint8_t src_gid[16]; /* RNIC src gid */
    // uint8_t dst_gid[16]; /* RNIC dst gid */
    // ibv_gid src_gid;     /* RNIC src gid */
    // ibv_gid dst_gid;     /* RNIC dst gid */
    struct HostKey host_key; /* RNIC src---dst gid */
    uint32_t src_qpn;        /* RNIC src qpn */
    uint32_t dst_qpn;        /* RNIC dst qpn */
}__attribute__((packed));

struct ConnId{
    struct ConnKey conn_key; /* identifier of QP connection */
    pid_t process_id;        /* current process id */
}__attribute__((packed));

/* struct to fill conn_id and local/remote lid */
struct ConnInfo{
    struct ConnId conn_id;
    struct HostLid host_lid;
}__attribute__((packed));

/* qp and vqp index for datapath operations */
struct ConnIdx{
    uint16_t qp_index;       /* assign index of PHY QP */
    uint16_t vq_index;       /* assign index of VIR QP */
    pid_t process_id;        /* current process id */
}__attribute__((packed));

/* local memory item */
/* reg_memory (pointer & size), allocate as aligned page (client), export as shared memory (router), register at RNIC */
struct LocalMem{
    struct ConnId *conn_id;
    void *client_addr;      /* virutal address of local client */
    size_t			length; /* virtual address length of remote */  

	void *router_addr;      /* virtual address of local router */
    void *shm_piece;        /* pointer to shm_piece at  router */
    char shm_name[100];     /* shared mem name of local router */
	int shm_fd;             /* shared mem fd   of local router */

    struct ibv_mr *mr;      /* pointer to MR, valid router only*/
	uint32_t		lkey;
	uint32_t		rkey;
}__attribute__((packed));

/* remote memory item */
struct RemoteMem{
    struct ConnId *conn_id;
    void *router_addr;         /* virtual address of remote router */ 
    size_t          length;    /* virtual address length of remote */
	uint32_t		rkey;
}__attribute__((packed));

enum CtrlChannelState
{
        IDLE,
        REQ_DONE,
        RSP_DONE,
};

struct CtrlShmPiece
{
        volatile enum CtrlChannelState state;
        uint8_t req[CTRL_REQ_SIZE];
        uint8_t rsp[CTRL_RSP_SIZE];
};

typedef enum SCALE_FUNCTION_CALL{
    INIT_RES,
    // after exchange meta nic via socket
    CREATE_RES,
    REG_MR,

    // after exchange meta conn via socket
    SETUP_CONN,

    POST_SEND,
    POST_RECV,
    POLL_CQ,

    DEREG_MR,
    DESTROY_RES
} SCALE_FUNCTION_CALL;

struct ScaleReqHeader
{
    pid_t process_id;
    SCALE_FUNCTION_CALL func;
    uint32_t body_size;
};

struct ScaleRspHeader
{
    uint32_t body_size;
};

/* no request body is required for INIT_RES */
// struct INIT_RES_REQ{
// };

struct INIT_RES_RSP{
    struct ConnInfo conn_info;
};

struct CREATE_RES_REQ{
    struct HostKey host_key;
};

struct CREATE_RES_RSP{
    struct ConnId conn_id;
};

struct REG_MR_REQ{
    struct ConnId conn_id;
    struct LocalMem local_mem;
};

struct REG_MR_RSP{
    struct LocalMem local_mem;
};

struct SETUP_CONN_REQ{
    struct ConnInfo conn_info;
};

struct SETUP_CONN_RSP{
    pid_t process_id;
    std::atomic<uint8_t> status; 
    struct ConnIdx conn_idx;
};

struct POST_SEND_REQ{
    // struct ConnId conn_id;
    struct ConnIdx conn_idx;

    int wr_id;
    void *local_addr;
    size_t length;
    uint32_t lkey;

    int opcode;
    void *remote_addr;
    uint32_t rkey;
};

struct POST_SEND_RSP{
    int wr_id;
};

struct POST_RECV_REQ{
    // struct ConnId conn_id;
    struct ConnIdx conn_idx;

    int wr_id;
    void *local_addr;
    size_t length;
    uint32_t lkey;
};

struct POST_RECV_RSP{
    int wr_id;
};

struct POLL_CQ_REQ{
    // struct ConnId conn_id;
    struct ConnIdx conn_idx;
    int wr_id;
    int count;
};

struct POLL_CQ_RSP{
    int wr_id;
    int count;
};

struct DEREG_MR_REQ{
    struct ConnId conn_id;
    struct LocalMem local_mem;
};

struct DEREG_MR_RSP{
    struct LocalMem local_mem;
};

struct DESTROY_RES_REQ{
    struct ConnId conn_id;
    struct ConnIdx conn_idx;
};

struct DESTROY_RES_RSP{
    struct ConnId conn_id;
};
