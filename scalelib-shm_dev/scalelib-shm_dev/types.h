#ifndef TYPES_H
#define TYPES_H

#include <infiniband/verbs.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_cma_abi.h>

#include <rdma/rdma_verbs.h>
// #include <infiniband/kern-abi.h>

#include <pthread.h>

/* types/constant for client only */
// #define MAX_HOST_NUM 1024

#define MAX_POLL_CQ_TIMEOUT 5000
#define NUM_REPEATS 10

#define MAX_RECV_SGE 1
#define MAX_SEND_SGE 1
#define MAX_RECV_WR 1024
#define MAX_SEND_WR 1024
#define MAX_CQ_SIZE 1024

// default port to communicate with remote
#define PORT 19875

// default MSG for communicate tesing
#define MSG      "SEND operation      "
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE (strlen(MSG) + 1)

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
	uint64_t addr;   /* Remote buffer address */
	uint32_t rkey;   /* Remote key */
	uint32_t qp_num; /* QP number */
	uint16_t lid;	 /* LID of the IB port */
	uint8_t gid[16]; /* gid */
} __attribute__((packed));

#endif /* TYPES_H */

