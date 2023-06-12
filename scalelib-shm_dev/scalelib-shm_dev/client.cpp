#include <unistd.h>
#include <sys/time.h>
#include "scalelib.h"

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
		// wait until the copy is finished
		while (((struct CtrlShmPiece *)(reinterpret_cast<size_t>(this->pshm) + *this->head * (this->size)))->state!=REQ_DONE);
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


void request_router_shm_test(struct CtrlShmPiece *csp)
{
	//struct timespec st, et;
	//clock_gettime(CLOCK_REALTIME, &st);
    //int time_us = 0;

    for (;;)
	{
        //rmb();
		// LOG_TRACE("Current csp->state = " << csp->state);
		switch (csp->state)
		{
			case IDLE:
                wmb();
				csp->state = REQ_DONE;

                //wc_wmb();
                //mem_flush((void*)&csp->state, sizeof(enum CtrlChannelState));
				//clock_gettime(CLOCK_REALTIME, &st);
				break;

			case REQ_DONE:
				break;

			case RSP_DONE:
				//!!! Must reset the state to IDLE outside this function
                
				//wc_wmb();
                //mem_flush((void*)&csp->state, sizeof(enum CtrlChannelState));
				//clock_gettime(CLOCK_REALTIME, &et);
				//	printf("REQ_DONE tv_sec=%d, tv_nsec=%d\n", st.tv_sec, st.tv_nsec);
				//	fflush(stdout);
				//	printf("RSP_DONE tv_sec=%d, tv_nsec=%d\n", et.tv_sec, et.tv_nsec);
				//	fflush(stdout);
				//goto end;
				return;
		}

		/*clock_gettime(CLOCK_REALTIME, &et);
        	time_us = (et.tv_sec - st.tv_sec) * 1000000 + (et.tv_nsec - st.tv_nsec) / 1000;
		if (time_us > 1000 * 1000)
		{
			printf("request_router_shm time out\n");
			fflush(stdout);
			goto end;
		}*/
	}
//end:
//	pthread_mutex_unlock(csp_mtx);
}


int post_send_test(LockFreeQueue* qp_shm_p){
	int rc = 0;
	//set virtual test settigns
	int qp_index = 22;
	int vq_index = 33;
	struct ConnIdx conn_idx;
	conn_idx.qp_index = 22;
	conn_idx.vq_index = 33;
	conn_idx.process_id = 1111;
    int process_id=1111;
    while (qp_shm_p->Full());

    struct CtrlShmPiece       *csp = (struct CtrlShmPiece *)qp_shm_p->Enqueue();
    struct ScaleReqHeader     *header = (struct ScaleReqHeader*)(csp->req);
	struct POST_SEND_REQ      *req = (struct POST_SEND_REQ*)(csp->req + sizeof(struct ScaleReqHeader));
	struct POST_SEND_RSP      *rsp = (struct POST_SEND_RSP*)(csp->rsp);


	// enter critical section
    while (csp->state!=IDLE);

    
	// fill in REQ HEADER
	header->process_id = process_id;
	header->func = POST_SEND;
	header->body_size = sizeof(struct POST_SEND_REQ);
	// fill in REQ
	memcpy(&req->conn_idx, &conn_idx, sizeof(struct ConnIdx));
	// req->wr_id = conn_idx->process_id;
	req->wr_id = process_id;
	// req->local_addr = router_addr;
	// req->length = length;
	// req->lkey = lkey;
	// req->opcode = opcode;

	// fill in RSP size
	int rsp_size = sizeof(struct POST_SEND_RSP);

	// LOG_TRACE_PRINTF("Request by SHM: qp_indx=%u, vq_index=%u, process_id=%d\n", qp_index, vq_index, process_id);
	request_router_shm_test(csp);		

	if(req->wr_id != rsp->wr_id){
		LOG_ERROR("POST_SEND error may happen");
		rc = -1;
	}

	wmb();
	csp->state = IDLE;
	return rc;
}

int poll_cq_test(int count,LockFreeQueue* cq_shm_p){
	int rc = 0;
	//set virtual test settigns
    int process_id=1111;
    struct ConnIdx conn_idx;
	conn_idx.qp_index = 22;
	conn_idx.vq_index = 33;
	conn_idx.process_id = 1111;
    while (cq_shm_p->Full());

	// set SHM
	struct CtrlShmPiece       *csp = (struct CtrlShmPiece *)cq_shm_p->Enqueue();
	struct ScaleReqHeader     *header = (struct ScaleReqHeader*)(csp->req);
	struct POLL_CQ_REQ      *req = (struct POLL_CQ_REQ*)(csp->req + sizeof(struct ScaleReqHeader));
	struct POLL_CQ_RSP      *rsp = (struct POLL_CQ_RSP*)(csp->rsp);

	/* enter critical section */
    while (csp->state!=IDLE);

	// fill in REQ HEADER
	header->process_id = process_id;
	header->func = POLL_CQ;
	header->body_size = sizeof(struct POLL_CQ_REQ);
	// fill in REQ BODY
	memcpy(&req->conn_idx, &conn_idx, sizeof(struct ConnIdx));
	// req->wr_id = conn_idx->process_id;
	req->wr_id = process_id;
	req->count = count;

	// fill in RSP size
	int rsp_size = sizeof(POLL_CQ_RSP);


	// LOG_TRACE_PRINTF("Request by SHM: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx.qp_index, conn_idx.vq_index, conn_idx.process_id);
	request_router_shm_test(csp);

	if(req->wr_id != rsp->wr_id || rsp->count < 0){
		LOG_ERROR("POLL_CQ error may happen");
	}

	rc = rsp->count;

	wmb();
	csp->state = IDLE;
	/* exit critical section */
	
	return rc;
}

static int poll_completion_test(LockFreeQueue* cq_shm_p)
{
	unsigned long start_time_msec;
	unsigned long cur_time_msec;
	struct timeval cur_time;
	int poll_result;
	int rc = 0;
	/* poll the completion for a while before giving up of doing it .. */
	gettimeofday(&cur_time, NULL);
	start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	do
	{
        // LOG_INFO("POLL_CQ");
		poll_result = poll_cq_test(1,cq_shm_p);
		gettimeofday(&cur_time, NULL);
		cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	} while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
	if (poll_result < 0)
	{
		/* poll CQ failed */
		LOG_ERROR("poll_cq failed\n");
		rc = 1;
	}
	else if (poll_result == 0)
	{ /* the CQ is empty */
		LOG_ERROR("completion wasn't found in the CQ after timeout\n");
		rc = 1;
	}
	else
	{
		/* CQE found */
		// LOG_TRACE("completion was found in CQ");
	}
	return rc;
}

int main()
{
    int qp_index=22;
    int vq_index=33;
    int len=32;
    int rc=0;
	struct timeval t1,t2;
    //client post send
    LOG_INFO("POST_SEND");
    //create shmqueue and initiate
    LOG_TRACE("Create vqp_shm_map: qp_index=" << qp_index << " vq_index=" << vq_index);
	std::stringstream qp_ss;
	qp_ss << "ctrlshm-qp-" << qp_index << "-" << vq_index;
    LockFreeQueue *qp_shm_p = new LockFreeQueue(qp_ss.str().c_str(), sizeof(struct CtrlShmPiece), len);
    if (!qp_shm_p->open())
    {
        qp_shm_p = NULL;
    }

    if (!qp_shm_p){
		LOG_ERROR_PRINTF("Failed to mount shm %s\n", qp_ss.str().c_str());
	}
	else{
		LOG_TRACE_PRINTF("Succeed to mount shm %s\n", qp_ss.str().c_str());
	}

	LOG_TRACE("Create vcp_shm_map: qp_index=" << qp_index << " vq_index=" << vq_index);
	std::stringstream cq_ss;
	cq_ss << "ctrlshm-cq-" << qp_index << "-" << vq_index;
    LockFreeQueue *cq_shm_p = new LockFreeQueue(cq_ss.str().c_str(), sizeof(struct CtrlShmPiece), len);
    if (!cq_shm_p->open())
    {
        cq_shm_p = NULL;
    }

    if (!cq_shm_p){
		LOG_ERROR_PRINTF("Failed to mount shm %s\n", cq_ss.str().c_str());
	}
	else{
		LOG_TRACE_PRINTF("Succeed to mount shm %s\n", cq_ss.str().c_str());
	}
	gettimeofday(&t1, NULL);
    rc = post_send_test(qp_shm_p);
    if(rc){
        LOG_ERROR("POST_SEND failed at client side");
        // goto exit;
    }

    // both side poll completion
    rc = poll_completion_test(cq_shm_p);
    if(rc < 0){
        LOG_ERROR("POLL_CQ failed");
        // goto exit;
    }
	gettimeofday(&t2, NULL);
    // end_cycle = get_cycles();
    // delta = end_cycle - start_cycle;
	printf("t1 %d usec \n",t1.tv_usec);
    printf("t2 %d usec \n",t2.tv_usec);
    

    return rc;
}
