#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <semaphore.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include <map>
#include <string>

#include "log.h"

#include "scalerouter.h"

class LockFreeQueue
{
public:
	std::string name;	//name of shm
	size_t size;   // size of one cell
	size_t length; // total num of cell in queue
	size_t total_size;
	int shmfd;
	void *pshm;	  // each cell has a set bool flag in it's tail
	volatile size_t *head; // pointer at the head position
	volatile size_t *tail; // pointer at the tail position
	volatile size_t *ready;
	LockFreeQueue(const char *name, size_t single_size, size_t total_length)
	{
		this->name = name;
		this->shmfd = -1;
		this->pshm = NULL;
		this->head = NULL;
		this->tail = NULL;
		this->size = single_size;
		this->length = total_length;
		this->total_size = size * length + 2 * sizeof(size_t);
	}
	bool open()
	{
		/* open shared memory segment */
		this->shmfd = shm_open(this->name.c_str(), O_CREAT | O_RDWR, 0666);
		
		/* set the size of shared memory segment */
		ftruncate(shmfd, this->total_size);
		
		/* now map the shared memory segment in the address space of the process */
		this->pshm = mmap(0, this->total_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, this->shmfd, 0);
		
		if (this->pshm == MAP_FAILED){
			LOG_ERROR("Error mapping shared memory " << this->name);
			return false;
		}
		this->head = (size_t *)(reinterpret_cast<size_t>(this->pshm) + (this->size + sizeof(bool)) * this->length);
		this->tail = (size_t *)(reinterpret_cast<size_t>(this->pshm) + (this->size + sizeof(bool)) * this->length + sizeof(size_t));
		return true;
	}

	~LockFreeQueue()
	{
		// delete the mapping relation
		if (close(this->shmfd))
		{
			LOG_ERROR("Error close shm_fd: " << this->shmfd);
		}
		else
		{
			LOG_TRACE("Success close shm_fd: " << this->shmfd);
		}
		size_t total = this->size * length + 2 * sizeof(size_t);
		if (munmap(this->pshm, total))
		{
			LOG_ERROR("Error munmap shm_ptr: " << this->pshm);
		}
		else
		{
			LOG_TRACE("Success munmap shm_ptr: " << this->pshm);
		}
	}

	void *Enqueue()
	{
		size_t cur_tail = *this->tail;
		size_t cur_next = (cur_tail == (this->length - 1)) ? 0 : (cur_tail + 1);
		while (!__sync_bool_compare_and_swap(this->tail, cur_tail, cur_next))
		{
			cur_tail = *this->tail;
			cur_next = (cur_tail == (this->length - 1)) ? 0 : (cur_tail + 1);
		}
		// memcpy((void *)(reinterpret_cast<size_t>(this->pshm) + cur_tail * (this->size + sizeof(bool))), p, this->size);
		// indicate the message copy is finish
		// *(bool *)(reinterpret_cast<size_t>(this->pshm) + cur_tail * (this->size + sizeof(bool)) + this->size) = true;
		return (void *)(reinterpret_cast<size_t>(this->pshm) + cur_tail * this->size);
	}

	void *Dequeue()
	{
		// wait until the write operation is finished
		while (((struct CtrlShmPiece *)(reinterpret_cast<size_t>(this->pshm) + *this->head * this->size))->state!=REQ_DONE);
		size_t cur_head = *this->head;
		// while (*(bool *)(reinterpret_cast<size_t>(this->pshm) + *this->head * (this->size + sizeof(bool)) + this->size) == false);
		// memcpy(p, (void *)(reinterpret_cast<size_t>(this->pshm) + *this->head * (this->size + sizeof(bool))), this->size);
		// *(bool *)(reinterpret_cast<size_t>(this->pshm) + *this->head * (this->size + sizeof(bool)) + this->size) = 0;
		*this->head = (*this->head == (this->length - 1)) ? 0 : (*this->head + 1);
		return (void *)(reinterpret_cast<size_t>(this->pshm) + cur_head * this->size);
	}

	bool Empty()
	{
		if (*this->head == *this->tail)
		{
			return true;
		}
		else
			return false;
	}
	bool Full()
	{
		if ((*this->tail - *this->head + 1) % this->length == 0)
		{
			return true;
		}
		else
			return false;
	}
};


LockFreeQueue* initCtrlShm_test(const char* tag)
{
    int len=32;

    std::stringstream ss;
    ss << "ctrlshm-" << tag;

    //create shmqueue and initiate
    LockFreeQueue *sp = new LockFreeQueue(ss.str().c_str(), sizeof(struct CtrlShmPiece), len);
    if (!sp->open())
    {
        sp = NULL;
    }

    if (!sp){
		LOG_ERROR("Failed to create control shm for tag  " << tag);
	}
	else{
		LOG_TRACE_PRINTF("Succeed to create shm %s\n", ss.str().c_str());
	}
    memset(sp->pshm, 0, sp->total_size);
    for (size_t i = 0; i < len; i++)
    {
        struct CtrlShmPiece *csp = (struct CtrlShmPiece *)(sp->pshm+i*sizeof(struct CtrlShmPiece));
        csp->state = IDLE;
    } 
    return sp;
}


int main (int argc, char **argv) {
    /* parse the command line parameters */
	int qp_index=22;
    int vq_index=33;


    //create vcq and vqp
    LOG_TRACE("Create vcq_shm_map: qp_index=" << qp_index << " vq_index=" << vq_index);
    std::stringstream cq_ss;
    cq_ss << "cq-" << qp_index << "-" << vq_index;
    LockFreeQueue* cq_sp = initCtrlShm_test(cq_ss.str().c_str());

    /** allocate vqp_shm_map **/
    LOG_TRACE("Create vqp_shm_map: qp_index=" << qp_index << " vq_index=" << vq_index);
    std::stringstream qp_ss;
    qp_ss << "qp-" << qp_index << "-" << vq_index;
    LockFreeQueue* qp_sp = initCtrlShm_test(qp_ss.str().c_str());

    //simulate the post send and poll
    LOG_TRACE("Start Loop of worker thread");

    unsigned int count = 0;
	struct conn_resources *conn = NULL;
    LockFreeQueue *vqp_sp = NULL;
    LockFreeQueue *vcq_sp = NULL;
    struct CtrlShmPiece *vqp_csp = NULL;
    struct CtrlShmPiece *vcq_csp = NULL;

    void *req_body, *rsp;
    struct ScaleReqHeader *req_header;
    // struct ScaleRspHeader *rsp_header;

    struct ibv_qp *qp = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_wc *wc_list = NULL;
    while(1){
		// LOG_TRACE("Start iterate all conn on qp_vec, size=" << (int)router->qp_vec.size());
		vqp_sp = qp_sp;
		if (vqp_sp->Empty())
		{
			continue;
		}
		
		//let shmqueue check the req state
		vqp_csp = (struct CtrlShmPiece*)vqp_sp->Dequeue();

		
		// if(!(vqp_csp->state == REQ_DONE)) continue;

		// LOG_TRACE("REQ_DONE on vqp_shm vq_index=" );

		req_header = (struct ScaleReqHeader *)vqp_csp->req;
		req_body = vqp_csp->req + sizeof(struct ScaleReqHeader);
		rsp = vqp_csp->rsp;
		// rsp_header = (struct ScaleRspHeader *)vqp_csp->rsp;
		// rsp = vqp_csp->rsp + sizeof(struct ScaleRspHeader);

		switch(req_header->func){
			case POST_SEND:
			{
				// LOG_TRACE("POST_SEND, client id = " << req_header->process_id << "; body_size = " << req_header->body_size);
				struct POST_SEND_REQ *send_req = (struct POST_SEND_REQ *)req_body;
				//simulate the post send
				((struct POST_SEND_RSP *)rsp)->wr_id = send_req->conn_idx.process_id;
			}
			break;
			case POST_RECV:
			{
				LOG_TRACE("POST_RECV, client id = " << req_header->process_id << "; body_size = " << req_header->body_size);
				struct POST_RECV_REQ *recv_req = (struct POST_RECV_REQ *)req_body;
				//simulate the post recv
				((struct POST_RECV_RSP *)rsp)->wr_id = recv_req->conn_idx.vq_index;
			}
			break;
			default:
			{
				LOG_ERROR("REQ_DONE hoever, FUNC_CALL type is not correct!");
			}
			break;
		}
		wmb();
		vqp_csp->state = RSP_DONE;
			
			// pthread_mutex_unlock(&conn->vq_mutex);

			/*********************************
			// need this judgement here; if all vqp/vcq are released, continue to next conn_res/qp
			// otherwise control path will hang/starve on qp_mutex lock (qp_vec delete)
			// DESTROY_RES hang at qp_mutex lock...
			**********************************/
			/** async poll_cq operations, poll first then update vcq */
			/** prepare cq_wc */
			// ibv_poll_cq return count of CQEs
		int poll_result = 0;
		//simulate the poll result
		poll_result = 1;

		for(int k = 0; k < poll_result; k++){
			/* CQE found */
			// LOG_TRACE_PRINTF("completion was found in CQ");
			vcq_sp = cq_sp;
			// LOG_TRACE("Begin to post CQE to vcq_shm_map with vq_index=");
			while (vcq_sp->Empty());

			vcq_csp = (struct CtrlShmPiece*)vcq_sp->Dequeue();
			//rmb();

			// wait unitl vcq_csp state is ready
			// easy to be hanged by client, how to resolve???
			// do{}while(!(vcq_csp->state == REQ_DONE));
			// if(!(vcq_csp->state == REQ_DONE)) continue;
			do{
				// wmb();
				// LOG_TRACE("Current vcq_csp->state = " << vcq_csp->state);
			}while(!(vcq_csp->state == REQ_DONE));

			// LOG_TRACE("REQ_DONE on vcq_shm with vq_index=");

			req_header = (struct ScaleReqHeader *)vcq_csp->req;
			req_body = vcq_csp->req + sizeof(struct ScaleReqHeader);
			rsp = vcq_csp->rsp;

			switch(req_header->func){
				case POLL_CQ:
				{
					struct POLL_CQ_REQ *poll_req = (struct POLL_CQ_REQ *)req_body;
					((struct POLL_CQ_RSP *)rsp)->wr_id = poll_req->conn_idx.process_id;
					((struct POLL_CQ_RSP *)rsp)->count = 1;
				}
				break;
				default:
				{
					LOG_ERROR("REQ_DONE, however, FUNC_CALL type is not correct!");
				}
				break;
			}
			wmb();
			vcq_csp->state = RSP_DONE;
		}
	}
}