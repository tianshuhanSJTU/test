#include "scalerouter.h"

ScaleRouter::ScaleRouter(struct config_t config){
    this->config = config;
    print_config();
}

void ScaleRouter::start(){
    LOG_INFO("ScaleRouter Starting... ");

    /* init global RDMA reousrce */
    init_global_res();

	/* init mutex for thread safe */
	init_global_mutex();

    /* Starting worker thread
	 * polling shared memory for each QP/CQ ---> VQP/VCQ 
	 * post_send, post_recv, poll_cq
	 * through shm communication */
	if(!this->config.disable_rdma){
		LOG_INFO("start worker_process...");
		struct WorkerArgs *worker_args = (struct WorkerArgs *) malloc(sizeof(struct WorkerArgs));
		worker_args->router = this;
		int ret = pthread_create(&worker_id, NULL, &work_process, worker_args);
		LOG_TRACE("result of create [worker_process] --> " << ret);
		pthread_detach(worker_id);
	}
	
	/* Starting dispatcher thread
     * provide services for local users, all processes (ScaleLib) can 
     * create QP, register shared memory, post/polling, deregitster, destory QP, etc. 
     * through socket communication */
	if (pthread_create(&dispatcher_id, NULL, &listen_local_process, (void *)this)) {
		LOG_ERROR("create thread [listen_local_process]");
		goto exit;
	}

    /* waiting for the listen_local_process thread to exit */
	pthread_join(dispatcher_id, NULL);
exit:
    return;
}

ScaleRouter::~ScaleRouter(){
    /* destroy global RDMA reousrce */
    destroy_global_res();

	/* destroy global mutex */
	destroy_global_mutex();

	/* deallocate shared mem pieces */
	freeShmPiece();
}

void ScaleRouter::print_config()
{
	fprintf(stdout, "------------------------------------------------\n");
	fprintf(stdout, "Device name : \"%s\"\n", config.dev_name);
	fprintf(stdout, "IB port : %u\n", config.ib_port);
	if (config.server_name)
		fprintf(stdout, "IP : %s\n", config.server_name);
	fprintf(stdout, "TCP port : %u\n", config.tcp_port);
	if (config.gid_idx >= 0)
		fprintf(stdout, "GID index : %u\n", config.gid_idx);
	fprintf(stdout, "------------------------------------------------\n\n");
}

int ScaleRouter::init_global_res()
{
    LOG_DEBUG("init global resource");

	struct ibv_device **dev_list = NULL;
	struct ibv_device *ib_dev = NULL;
	int rc = 0;
	int i;
	int num_devices;

    // fprintf(stdout, "------------------------------------------------\n");
	memset(&global_res, 0, sizeof(global_res));
    LOG_DEBUG("searching for IB devices in host");
	// fprintf(stdout, "searching for IB devices in host\n");
	/* get device names in the system */
	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
        LOG_ERROR("failed to get IB devices list");
		// fprintf(stderr, "failed to get IB devices list\n");
		rc = 1;
		goto resources_create_exit;
	}
	/* if there isn't any IB device in host */
	if (!num_devices) {
        LOG_ERROR("found " << num_devices << " available device(s)");
		// fprintf(stderr, "found %d device(s)\n", num_devices);
		rc = 1;
		goto resources_create_exit;
	}
	LOG_TRACE("found " << num_devices << " available device(s)");
	/* search for the specific device we want to work with */
	for (i = 0; i < num_devices; i++) {
		if (!config.dev_name) {
			config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            LOG_TRACE("device not specified, using first one found: " << config.dev_name);
			// fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
		}
		if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
			ib_dev = dev_list[i];
			break;
		}
	}
	/* if the device wasn't found in host */
	if (!ib_dev) {
        LOG_ERROR("IB device " << config.dev_name << " wasn't found");
		// fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}
	/* get device handle */
	global_res.ib_ctx = ibv_open_device(ib_dev);
	if (!global_res.ib_ctx) {
        LOG_ERROR("failed to open device: " << config.dev_name);
		// fprintf(stderr, "failed to open device %s\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}
	/* We are now done with device list, free it */
	ibv_free_device_list(dev_list);
	dev_list = NULL;
	ib_dev = NULL;
	/* query port properties */
	if (ibv_query_port(global_res.ib_ctx, config.ib_port, &global_res.port_attr)) {
        LOG_ERROR("failed ibv_query_port on port " << config.ib_port)
		// fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
		rc = 1;
		goto resources_create_exit;
	}
	global_res.src_lid = global_res.port_attr.lid;
	/* query gid properties */
	if (config.gid_idx >= 0)
	{
		rc = ibv_query_gid(global_res.ib_ctx, config.ib_port, config.gid_idx, &global_res.src_gid);
		if (rc)
		{
			LOG_ERROR_PRINTF("could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
			rc = 1;
			goto resources_create_exit;
		}
	}
	else
		memset(&global_res.src_gid, 0, sizeof(ibv_gid));
	/* allocate Protection Domain */
	global_res.pd = ibv_alloc_pd(global_res.ib_ctx);
	if (!global_res.pd) {
        LOG_ERROR("ibv_alloc_pd failed");
		// fprintf(stderr, "ibv_alloc_pd failed\n");
		rc = 1;
		goto resources_create_exit;
	}
	/* create epoll fd */
	global_res.epoll_fd = epoll_create(MAX_HOST_NUM);
	if (global_res.epoll_fd < 0){
        LOG_ERROR("epoll fd create failed");
		// fprintf(stderr, "epoll fd create failed\n");
		rc = 1;
		goto resources_create_exit;
	}
    // fprintf(stdout, "------------------------------------------------\n\n");
resources_create_exit:
	if (rc){
		/* Error encountered, cleanup */
		destroy_global_res();
		if (dev_list) {
			ibv_free_device_list(dev_list);
			dev_list = NULL;
		}
	}

	return rc;
}


int ScaleRouter::destroy_global_res(){
    LOG_DEBUG("destroy global resource");

	int rc = 0;
	if (global_res.pd)
		if (ibv_dealloc_pd(global_res.pd)) {
			fprintf(stderr, "failed to deallocate PD\n");
			rc = 1;
		}
	if (global_res.ib_ctx)
		if (ibv_close_device(global_res.ib_ctx)) {
			fprintf(stderr, "failed to close device context\n");
			rc = 1;
		}
	if (global_res.epoll_fd > 0)
		if (close(global_res.epoll_fd)){
			fprintf(stderr, "failed to close epoll fd\n");
			rc = 1;
		}
	return rc;
}

int ScaleRouter::init_global_mutex(){
	pthread_mutex_init(&hostmap_lock.mutex, NULL);
	pthread_mutex_init(&shmmap_lock.mutex, NULL);
	pthread_mutex_init(&connmap_lock.mutex, NULL);
	pthread_mutex_init(&qp_mutex, NULL);
	return 0;
}

int ScaleRouter::destroy_global_mutex(){
	pthread_mutex_destroy(&hostmap_lock.mutex);
	pthread_mutex_destroy(&shmmap_lock.mutex);
	pthread_mutex_destroy(&connmap_lock.mutex);
	pthread_mutex_destroy(&qp_mutex);
	return 0;
}


int ScaleRouter::resources_create(struct conn_resources *res)
{
	struct ibv_qp_init_attr qp_init_attr;
	// int mr_flags = 0;
	int cq_size = 0;
	int rc = 0;

	/* init all control shm pieces to NULL */
	for (int i = 0; i < VQ_MAP_SIZE; i++)
    {
		res->vcq_shm_map[i] = NULL;
        res->vqp_shm_map[i] = NULL;
    }

	/* init standalone datapath lock */
	pthread_mutex_init(&res->data_mutex, NULL);

	/* init standalone vq_shm_vec lock */
	pthread_mutex_init(&res->vq_mutex, NULL);

	// /** init mutex, move to outside hostmap_lock*/
	// pthread_mutex_init(&res->mutex, NULL);
	/** init vqp mutex, reuse the above mutex */
	// pthread_mutex_init(&res->vq_mutex, NULL);
	/** event interface is not implemented currently, need to pass fd between process*/
	// int ret;
	// int flags;
	// struct epoll_event ev;

	/* init the Completion Event Channel */
	// res->ev_channel = ibv_create_comp_channel(global_res.ib_ctx);
	// if(!res->ev_channel){
	// 	fprintf(stderr, "Error ibv_create_comp_channel failed\n");
	// 	rc = 1;
	// 	goto resources_create_exit;
	// } 
	/** init the Completion Queue */
	cq_size = MAX_CQ_SIZE;
	res->cq = ibv_create_cq(global_res.ib_ctx, cq_size, res->ev_ctx, res->ev_channel, 0); // create QP associated with Completion Event Channel
	if (!res->cq) {
		LOG_ERROR_PRINTF("failed to create CQ with %u entries\n", cq_size);
		// fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
		rc = 1;
		goto resources_create_exit;
	}
	LOG_TRACE_PRINTF("create CQ with %u entries\n", cq_size);
	/* Request notification before any completion can be created (to prevent races) */
	// ret = ibv_req_notify_cq(res->cq, 0);
	// if(ret){
	// 	fprintf(stderr, "Couldn't request CQ notification\n");
	// 	rc = 1;
	// 	goto resources_create_exit;
	// }
	/* Change the blocking mode of the completion channel */
	// flags = fcntl(res->ev_channel->fd, F_GETFL);
	// rc = fcntl(res->ev_channel->fd, F_SETFL, flags | O_NONBLOCK);
	// if (rc < 0){
	// 	fprintf(stderr, "Failed to change file descriptor of Completion Event Channel\n");
    //     rc = 1;
	// 	goto resources_create_exit;
	// }
	/* Init and register epoll events */
	// ev.data.fd = res->ev_channel->fd;
	// ev.data.ptr = res;   /* only one valid member in union epoll_event.epoll_data */
	// ev.events = EPOLLIN | EPOLLET; /* set events epollin and epollet(event triggered) */
	// rc = epoll_ctl(global_res.epoll_fd, EPOLL_CTL_ADD, res->ev_channel->fd, &ev);
	// if (rc < 0){
	// 	fprintf(stderr, "Failed to add Completion Channel FD to epoll list\n");
	// 	rc = 1;
	// 	goto resources_create_exit;
	// }
	/** register the control region, move to sperate interface*/
	// mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
	// res->ctrl_region.mr = ibv_reg_mr(global_res.pd, res->ctrl_region.ctrl_entrys, MAX_CTRL_ENTRY_NUM * sizeof(struct ctrl_entry_t), mr_flags);
	// if (!res->ctrl_region.mr) {
	// 	fprintf(stderr, "failed to allocate the control regions\n");
	// 	rc = 1;
	// 	goto resources_create_exit;
	// }
	// fprintf(stdout, "MR(control region) was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->ctrl_region.mr->addr, res->ctrl_region.mr->lkey, res->ctrl_region.mr->rkey, mr_flags);
	/* register the receive buffer */
	// res->controller_block.addr = malloc(CONTROLLER_BLOCK_SIZE);
	// mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
	// res->controller_block.mr = ibv_reg_mr(global_res.pd, res->controller_block.addr, CONTROLLER_BLOCK_SIZE, mr_flags);
	// if (!res->controller_block.mr) {
	// 	fprintf(stderr, "failed to allocate the receivce buffers\n");
	// 	rc = 1;
	// 	goto resources_create_exit;
	// }
	// fprintf(stdout, "MR(receive buffers) was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->controller_block.mr->addr, res->controller_block.mr->lkey, res->controller_block.mr->rkey, mr_flags);
	// res->controller_block.blk_size = CONTROLLER_BLOCK_SIZE;
	/** create the Queue Pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.sq_sig_all = 1;
	qp_init_attr.send_cq = res->cq;
	qp_init_attr.recv_cq = res->cq;
	qp_init_attr.cap.max_send_wr = MAX_SEND_WR;
	qp_init_attr.cap.max_recv_wr = MAX_RECV_WR;
	qp_init_attr.cap.max_send_sge = MAX_SEND_SGE;
	qp_init_attr.cap.max_recv_sge = MAX_RECV_SGE;
	res->qp = ibv_create_qp(global_res.pd, &qp_init_attr);
	if (!res->qp) {
		fprintf(stderr, "failed to create QP\n");
		rc = 1;
		goto resources_create_exit;
	}
	LOG_TRACE_PRINTF("QP was created, QP number=0x%x\n", res->qp->qp_num);
	// fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);
resources_create_exit:
	if (rc) {
		/* Error encountered, cleanup */
		resources_destroy(res);
	}
	return rc;
}

int ScaleRouter::resources_destroy(struct conn_resources *res)
{
	int rc = 0;

	/* destroy standalone datapath lock */
	pthread_mutex_destroy(&res->data_mutex);

	/* destroy standalone vq_shm_vec lock */
	pthread_mutex_destroy(&res->vq_mutex);

	// if (resources_dereg_mr(res)) {
	// 	fprintf(stderr, "failed to deregister MR\n");
	// 	rc = 1;
	// }
	// /** destroy mutex, move to outside hostmap_lock*/
	// if(&res->mutex){
	// 	pthread_mutex_destroy(&res->mutex);
	// }
	if (res->qp)
		if (ibv_destroy_qp(res->qp)) {
			fprintf(stderr, "failed to destroy QP\n");
			rc = 1;
		}
	if (res->cq)
		if (ibv_destroy_cq(res->cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			rc = 1;
		}
	if (res->ev_channel){
		if(ibv_destroy_comp_channel(res->ev_channel)){
			fprintf(stderr, "Error, ibv_destroy_comp_channel() failed in resources_create_exit\n");
		}
		else{
			res->ev_channel = NULL;
		}
	}
	return rc;
}

int ScaleRouter::modify_qp_to_ready(struct ConnInfo *conn_info, struct conn_resources *res)
{
	int rc = 0;
	/* modify the QP to init */
	rc = modify_qp_to_init(res->qp);
	if (rc) {
		fprintf(stderr, "change QP state to INIT failed\n");
		goto connect_qp_exit;
	}
	/* modify the QP to RTR */
	rc = modify_qp_to_rtr(res->qp, conn_info->conn_id.conn_key.dst_qpn, conn_info->host_lid.dst_lid, (uint8_t *)&conn_info->conn_id.conn_key.host_key.dst_gid);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTR\n");
		goto connect_qp_exit;
	}
	rc = modify_qp_to_rts(res->qp);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTR\n");
		goto connect_qp_exit;
	}

connect_qp_exit:
	return rc;
}


int ScaleRouter::modify_qp_to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = config.ib_port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to INIT\n");
	return rc;
}

int ScaleRouter::modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_256;
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 16;
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dlid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config.ib_port;
	if (config.gid_idx >= 0) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.port_num = 1;
		memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 0xff;
		attr.ah_attr.grh.sgid_index = config.gid_idx;
		attr.ah_attr.grh.traffic_class = 0;
	}
	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
		IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTR\n");
	return rc;
}

int ScaleRouter::modify_qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 0x12;
	attr.retry_cnt = 6;
	attr.rnr_retry = 0;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 16; // number of outstanding wqe for RDMA read
	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTS\n");
	return rc;
}

ShmPiece* ScaleRouter::addShmPiece(int process_id, int mem_size)
{
    pthread_mutex_lock(&this->shmmap_lock.mutex);
    if (this->shmmap_lock.shm_map.find(process_id) == this->shmmap_lock.shm_map.end())
    {
        std::vector<ShmPiece*> v;
        this->shmmap_lock.shm_map[process_id] = v;
    }

    int count = this->shmmap_lock.shm_map[process_id].size();

    std::stringstream ss;
    ss << "client-" << process_id << "-memsize-" << mem_size << "-index-" << count;

    ShmPiece *sp = new ShmPiece(ss.str().c_str(), mem_size);
    this->shmmap_lock.shm_map[process_id].push_back(sp);
    if (!sp->open())
    {
        sp = NULL;
    }

    pthread_mutex_unlock(&this->shmmap_lock.mutex);
    return sp;
}

void ScaleRouter::freeShmPiece()
{
    for(auto it = this->shmmap_lock.shm_map.begin(); it != this->shmmap_lock.shm_map.end(); it++)
    {
        for (int i = 0; i < it->second.size(); i++)
        {
            delete it->second[i];
        }
    }  
}

ShmPiece* ScaleRouter::initCtrlShm(const char* tag)
{
    std::stringstream ss;
    ss << "ctrlshm-" << tag;

    ShmPiece *sp = new ShmPiece(ss.str().c_str(), sizeof(struct CtrlShmPiece));
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

    memset(sp->ptr, 0, sizeof(struct CtrlShmPiece));

    struct CtrlShmPiece *csp = (struct CtrlShmPiece *)(sp->ptr);
    csp->state = IDLE;
    return sp;
}

void *listen_local_process(void *args)
{
	int listen_fd;
	int conn_fd;
	int ret;
	struct sockaddr_un srv_addr;
	struct sockaddr_un clt_addr;
	socklen_t clt_addr_len = sizeof(clt_addr);
	pthread_t thread_id;

	listen_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOG_ERROR("cannot create socket");
		return NULL;
	}
	/* set server addr_param */
	srv_addr.sun_family = AF_UNIX;
	strncpy(srv_addr.sun_path, AGENT_DOMAIN, sizeof(srv_addr.sun_path)-1);
	unlink(AGENT_DOMAIN);
	/* bind sockfd & addr */
	ret = bind(listen_fd, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
	if(ret==-1) {
		LOG_ERROR("cannot bind server socket");
		close(listen_fd);
		unlink(AGENT_DOMAIN);
		return NULL;
	}
	/* listen sockfd */
	ret = listen(listen_fd, 10);
	if (ret == -1) {
		LOG_ERROR("cannot listen the client connect request");
		close(listen_fd);
		unlink(AGENT_DOMAIN);
		return NULL;
	}
    LOG_INFO("listening to the local users...");
	// fprintf(stdout, "listening to the local users...\n");
	while (1) {
		conn_fd = accept(listen_fd, (struct sockaddr*)&clt_addr, &clt_addr_len);
		if (conn_fd < 0) {
			LOG_ERROR("accept this time");
			continue;
		}
		if (conn_fd > 0) {
			LOG_TRACE("receive the local request, conn_fd = " << conn_fd);
            struct HandlerArgs *handler_args = (struct HandlerArgs *) malloc(sizeof(struct HandlerArgs));
            handler_args->router = (struct ScaleRouter *)args;
            handler_args->client_sock = conn_fd;
			int ret = pthread_create(&thread_id, NULL, &handle_process, handler_args);
			LOG_TRACE("result of pthread_create --> " << ret);
			pthread_detach(thread_id);
		}
	}

	close(listen_fd);

	return NULL;
}

void *handle_process(void *args){
    struct HandlerArgs *handler_args = (struct HandlerArgs *)args;
    LOG_INFO("Start to handle the request from client sock " << handler_args->client_sock << ".");

    ScaleRouter *router = handler_args->router;
    int client_sock = handler_args->client_sock;

    // Speed up by pre malloc?
    char *req_body = NULL;
    char *rsp = NULL;

	// disable_rdma or shared memory communication by default
	// need to verify req/rsq pre-allocated size
    if (router->config.disable_rdma) {
        req_body = (char*)malloc(0xff);
        rsp = (char*)malloc(0xff);
		memset(req_body, 0, 0xff);
		memset(rsp, 0, 0xff);
    }
    else {
        req_body = (char*)malloc(0xfffff);
        rsp = (char*)malloc(0xfffff);
    }

    while(1)
    {
        int n = 0, size = 0, count = 0, i = 0, ret = 0, host_fd = -1;
        //void *req_body = NULL;
        //void *rsp = NULL;
		
		/** verbs object may be used in the future, decoupled to worker thread*/
        // void *context = NULL;
        // struct ibv_cq *cq = NULL;
        // struct ibv_qp *qp = NULL;
        // struct ibv_pd *pd = NULL;
        // struct ibv_mr *mr = NULL;
        // struct ibv_ah *ah = NULL;
        // struct ibv_srq *srq = NULL;
        // struct ibv_comp_channel *channel = NULL;
        // struct rdma_event_channel *event_channel = NULL;
        // struct rdma_cm_id *cm_id = NULL;
        // struct ibv_wc *wc_list = NULL;
        // TokenBucket *tb = NULL;

        struct ScaleReqHeader header;
    
		LOG_INFO("Start to read from sock " << client_sock);
        
        if ((n = read(client_sock, &header, sizeof(header))) < sizeof(header))
        {
            if (n < 0)
                LOG_ERROR("Failed to read the request header. Read bytes: " << n << " Size of Header: " << sizeof(header));

            goto kill;
        }
		else
		{
			LOG_TRACE("Get request cmd " << header.func);
		}

        switch(header.func)
        {
			case INIT_RES:
			{
				LOG_TRACE("INIT_RES, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** req is null */

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct INIT_RES_RSP));
				// fill rsp with src_gid
				((struct INIT_RES_RSP *)rsp)->conn_info.conn_id.conn_key.host_key.src_gid = router->global_res.src_gid;
				// fill rsp with src_lid
				((struct INIT_RES_RSP *)rsp)->conn_info.host_lid.src_lid = router->global_res.src_lid;
				size = sizeof(struct INIT_RES_RSP);

				// verify results after filling
				print_conn_info(&((struct INIT_RES_RSP *)rsp)->conn_info);
			}
			break;

			case CREATE_RES:
            {
                LOG_TRACE("CREATE_RES, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = malloc(sizeof(struct CREATE_RES_REQ));
                if (read(client_sock, req_body, sizeof(struct CREATE_RES_REQ)) < sizeof(struct CREATE_RES_REQ))
                {
                    LOG_ERROR("CREATE_RES: Failed to read the request body."); 
                    goto kill;
                }
				struct CREATE_RES_REQ *res_req = (struct CREATE_RES_REQ *)req_body;

				/** create conn resources and update host_key---conn_res */
				// try pre-allocate conn_res
				int create_flag = 0; // will be true if host_key has no conn_res
				int qpn = 0;         // 0 means create_res failed  
				struct conn_resources *new_conn = (struct conn_resources *)calloc(1, sizeof(struct conn_resources));
				pthread_mutex_init(&new_conn->mutex, NULL);
				pthread_mutex_lock(&new_conn->mutex);
				new_conn->ref_cnt++; // update ref_cnt after created

				struct conn_resources *old_conn = NULL;
				// update host_map
				pthread_mutex_lock(&router->hostmap_lock.mutex);
				auto iter = router->hostmap_lock.host_map.find(res_req->host_key);
				if(iter != router->hostmap_lock.host_map.end())
				{
					// host_key has conn_res
					if(iter->second != NULL){
						create_flag = 0;
						old_conn = (struct conn_resources *)iter->second; 
						old_conn->ref_cnt++;
						LOG_TRACE("old conn pointer: " << old_conn << " query hostmap success");
					}
					// host_key has no conn_res
					// if host_key exists, conn_res must be not NULL, or synchronization error may happens
					if(iter->second == NULL){
						create_flag = 1;
						iter->second = new_conn;
						LOG_ERROR("host_key exists, however conn_res is NULL, sync error may happen");
						LOG_TRACE("new conn pointer: " << new_conn << " insert hostmap success, however sysnc error may happen");
					}
				}
				else{
					// host_key has no conn_res, insert new_conn with host_key, update ref_count
					create_flag = 1;
					router->hostmap_lock.host_map[res_req->host_key] = (void *)new_conn;
					LOG_TRACE("new conn pointer: " << new_conn << " insert hostmap success");
				}
				pthread_mutex_unlock(&router->hostmap_lock.mutex);

				/* QP create may be put inside hostmap_lock, to ensure QP is created before modification */
				/* another implemeantaion is lock new_conn->mutex before hostmap_lock->mutex */
				// create QP res
				if(create_flag){
					if(router->resources_create(new_conn)){
						LOG_ERROR("Create new QP res failed");
					}
					qpn = new_conn->qp->qp_num;
					pthread_mutex_unlock(&new_conn->mutex);

					/* update qp_vec & qp_index & conn_map */
					pthread_mutex_lock(&router->qp_mutex);
					router->qp_vec.push_back(router->qp_index);  // record valid qp_index in qp_vec
					new_conn->qp_index = router->qp_index;
					router->conn_map[new_conn->qp_index] = new_conn;
					LOG_TRACE_PRINTF("Update qp_vec & conn_map with qp_index=%u success!\n", router->qp_index);
					router->qp_index++;
					pthread_mutex_unlock(&router->qp_mutex);
				}
				else{
					if(router->resources_destroy(new_conn)){
						LOG_ERROR("Destroy new QP res failed");
					}
					pthread_mutex_unlock(&new_conn->mutex);
					pthread_mutex_destroy(&new_conn->mutex);
					free(new_conn); // release pre-allocate conn_res
					qpn = old_conn->qp->qp_num;
				}
				
				/** fill rsp */
                // rsp = (char *)malloc(sizeof(struct CREATE_RES_RSP));
				// fill rsp with src_qpn 
				((struct CREATE_RES_RSP *)rsp)->conn_id.conn_key.src_qpn = qpn;
                size = sizeof(struct CREATE_RES_RSP);

				// verify results after filling
				print_conn_id(&((struct CREATE_RES_RSP *)rsp)->conn_id);
            }
            break;

			case REG_MR:
			{
				LOG_TRACE("REG_MR, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** fill req */
                // req_body = malloc(sizeof(struct REG_MR_REQ));
                if (read(client_sock, req_body, sizeof(struct REG_MR_REQ)) < sizeof(struct REG_MR_REQ))
                {
                    LOG_ERROR("REG_MR: Failed to read the request body."); 
                    goto kill;
                }

				struct REG_MR_REQ *mr_req = (struct REG_MR_REQ *)req_body;
				struct ConnId conn_id = mr_req->conn_id;		// conn_id is not used, using a global PD/ctx currently
				struct LocalMem local_mem = mr_req->local_mem;

				/** allocate shared memory */
				ShmPiece* sp = NULL;
				struct ibv_mr *mr = NULL;
				int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE; // enable read/write access by default

				// create a shm buffer
                LOG_TRACE("Create a shared memory piece for client-" << header.process_id << " with size " << local_mem.length);
                if (local_mem.shm_name[0] == '\0')
                {
                    LOG_TRACE("create shm from client id and count.");
                    sp = router->addShmPiece(header.process_id, local_mem.length);
                }

				if (sp == NULL)
                {
                    LOG_ERROR("Failed to the shared memory piece.");
                    goto kill;
                }

				/** register at RNIC, currently a global PD is used; (TODO! host_key ---> PD/QP) */
				LOG_TRACE("Registering a MR ptr="<< sp->ptr << ", size="  << sp->size);            
                mr = ibv_reg_mr(router->global_res.pd, sp->ptr, sp->size, mr_flags);
                if (mr == NULL)
                {
                    LOG_ERROR("Failed to regiester the MR. Current shared memory size: " << sp->size);
                    goto kill;
                }

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct REG_MR_RSP));
				// fill rsp with pointer of router_addr, shm_piece, shm_name, shm_fd 
				local_mem.router_addr = sp->ptr;
				local_mem.shm_piece = (void *)sp;
				strcpy(local_mem.shm_name, sp->name.c_str());
				// local_mem.shm_fd = sp->shm_fd; // shm_fd is not used for mmap, fill shm_fd in scalelib
				// fill rsp with pointer of ibv_mr, lkey, rkey
				local_mem.mr = mr;
				local_mem.lkey = mr->lkey;
				local_mem.rkey = mr->rkey;
				// fill rsp: copy local_mem
				((struct REG_MR_RSP *)rsp)->local_mem = local_mem;
                size = sizeof(struct REG_MR_RSP);
			}
			break;

			case SETUP_CONN:
			{
				LOG_TRACE("SETUP_CONN, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = malloc(sizeof(struct SETUP_CONN_REQ));
                if (read(client_sock, req_body, sizeof(struct SETUP_CONN_REQ)) < sizeof(struct SETUP_CONN_REQ))
                {
                    LOG_ERROR("SETUP_CONN: Failed to read the request body."); 
                    goto kill;
                }
				struct SETUP_CONN_REQ *conn_req = (struct SETUP_CONN_REQ *)req_body;

				int ready_flag = 0;  // will be true if QP is RTS  
				struct conn_resources *conn = NULL;
				/** query QP res */
				pthread_mutex_lock(&router->hostmap_lock.mutex);
				auto iter = router->hostmap_lock.host_map.find(conn_req->conn_info.conn_id.conn_key.host_key);
				if(iter != router->hostmap_lock.host_map.end())
				{
					// host_key has conn_res
					if(iter->second != NULL){
						conn = (struct conn_resources *)iter->second; 
						LOG_TRACE("conn pointer: " << conn << " query hostmap success ");
					}
					// host_key has no conn_res
					// if host_key exists, conn_res must be not NULL, or synchronization error may happens
					if(iter->second == NULL){
						LOG_ERROR("host_key exists, however conn_res is NULL, sync error may happen");
						goto kill;
					}
				}
				else{
					LOG_ERROR("host_key cannot found when SETUP_CONN");
					goto kill;
				}
				pthread_mutex_unlock(&router->hostmap_lock.mutex);

				/** modify QP res */
				pthread_mutex_lock(&conn->mutex);
				// verify QP status is 0
				if(conn->status == 0){
					if(router->modify_qp_to_ready(&conn_req->conn_info, conn)){
						LOG_ERROR("failed to modify qp to ready");
					}
					else{
						conn->status = 4;
						LOG_TRACE("conn pointer: " << conn << " modify success ");
					}
				}

				if(conn->status == 4){
					ready_flag = 1;
				}

				if(ready_flag){
					/** allocate vcq_shm_map **/
					/** TODO! separate mutex lock for conn_res, vq_shm management */
					LOG_TRACE("Create vcq_shm_map: qp_index=" << conn->qp_index << " vq_index=" << conn->vq_index);
					std::stringstream cq_ss;
					cq_ss << "cq-" << conn->qp_index << "-" << conn->vq_index;
					ShmPiece* cq_sp = router->initCtrlShm(cq_ss.str().c_str());
					conn->vcq_shm_map[conn->vq_index] = cq_sp;

					/** allocate vqp_shm_map **/
					LOG_TRACE("Create vqp_shm_map: qp_index=" << conn->qp_index << " vq_index=" << conn->vq_index);
					std::stringstream qp_ss;
					qp_ss << "qp-" << conn->qp_index << "-" << conn->vq_index;
					ShmPiece* qp_sp = router->initCtrlShm(qp_ss.str().c_str());
					conn->vqp_shm_map[conn->vq_index] = qp_sp;

					/** udpate vq_shm_vec **/
					pthread_mutex_lock(&conn->vq_mutex);
					conn->vq_shm_vec.push_back(conn->vq_index);
					pthread_mutex_unlock(&conn->vq_mutex);

					/** update vq_index **/
					conn->vq_index++;
				}

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct SETUP_CONN_RSP));
				// fill rsp with process_id, QP status
				((struct SETUP_CONN_RSP *)rsp)->process_id = header.process_id;
				if(ready_flag){
					((struct SETUP_CONN_RSP *)rsp)->status = 4;
					((struct SETUP_CONN_RSP *)rsp)->conn_idx.qp_index = conn->qp_index;
					((struct SETUP_CONN_RSP *)rsp)->conn_idx.vq_index = conn->vq_index - 1;
					((struct SETUP_CONN_RSP *)rsp)->conn_idx.process_id = header.process_id;
				}
				size = sizeof(struct SETUP_CONN_RSP);

				pthread_mutex_unlock(&conn->mutex);

				// /** use conn_idx instead of conn_id to index QP and vQP/CQ, to eliminate all locks at data path! */
				// /** fill conn_id with process_id, otherwise conn_map will not match */
				// conn_req->conn_info.conn_id.process_id = header.process_id; 
				// /** update conn_map, index conn_id(vQP) --- conn_res */
				// pthread_mutex_lock(&router->connmap_lock.mutex);
				// auto connmap_iter = router->connmap_lock.conn_map.find(conn_req->conn_info.conn_id);
				// if(connmap_iter != router->connmap_lock.conn_map.end()){
				// 	LOG_ERROR("conn_id exists, virtual conn already inserted or sync error may happen");
				// 	goto kill;
				// }
				// else{
				// 	router->connmap_lock.conn_map[conn_req->conn_info.conn_id] = (void *)conn;
				// 	LOG_TRACE("conn pointer: " << conn << " insert connmap success ");
				// 	// print_conn_id(&conn_req->conn_info.conn_id);
				// }
				// pthread_mutex_unlock(&router->connmap_lock.mutex);
			}
			break;

			case POST_SEND:
			{
				LOG_TRACE("POST_SEND, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = (char *) malloc(sizeof(struct POST_SEND_REQ));
                if (read(client_sock, req_body, sizeof(struct POST_SEND_REQ)) < sizeof(struct POST_SEND_REQ))
                {
                    LOG_ERROR("POST_SEND: Failed to read the request body."); 
                    goto kill;
                }
				struct POST_SEND_REQ *send_req = (struct POST_SEND_REQ *)req_body;

				/** construct send_wr according to send_req */
				int rc = 0;
				struct ibv_send_wr sr;
				struct ibv_sge sge;
				struct ibv_send_wr *bad_wr = NULL;

				// prepare the scatter/gather entry */
				memset(&sge, 0, sizeof(sge));
				sge.addr = (uintptr_t)send_req->local_addr;
				sge.length = send_req->length;
				sge.lkey = send_req->lkey;
				/* prepare the send work request */
				memset(&sr, 0, sizeof(sr));
				sr.next = NULL;
				sr.wr_id = send_req->wr_id;
				sr.sg_list = &sge;
				sr.num_sge = 1;
				sr.opcode = static_cast<ibv_wr_opcode>(send_req->opcode);
				sr.send_flags = IBV_SEND_SIGNALED;
				if (sr.opcode != IBV_WR_SEND)
				{
					sr.wr.rdma.remote_addr = (uintptr_t)send_req->remote_addr;
					sr.wr.rdma.rkey = send_req->rkey;
				}

				// if (sr.wr_id != header.process_id){
				// 	LOG_ERROR("reqeust header process_id is not equal to wr_id: " << header.process_id << " vs. " << sr.wr_id);
				// 	goto kill;
				// }

				if (sr.wr_id != send_req->conn_idx.vq_index){
					LOG_ERROR("conn_idx.vq_index is not equal to wr_id: " << send_req->conn_idx.vq_index << " vs. " << sr.wr_id);
					goto kill;
				}

				/** acquire physical QP by qp_index, without locks */
				struct conn_resources *conn = NULL;
				if(router->conn_map[send_req->conn_idx.qp_index] != NULL){
					conn = router->conn_map[send_req->conn_idx.qp_index];
					LOG_TRACE("conn pointer: " << conn << " get QP by qp_index success ");
				}
				else{
					LOG_ERROR("conn_idx exists, conn_res is null, sync error may happen");
				}
				// /** query physical QP by conn_id */
				// struct conn_resources *conn = NULL;
				// pthread_mutex_lock(&router->connmap_lock.mutex);
				// auto connmap_iter = router->connmap_lock.conn_map.find(send_req->conn_id);
				// if(connmap_iter != router->connmap_lock.conn_map.end()){
				// 	if(connmap_iter->second != NULL){
				// 		conn = (conn_resources *)connmap_iter->second;
				// 		LOG_TRACE("conn pointer: " << conn << " query connmap success ");					
				// 	}
				// 	else{
				// 		LOG_ERROR("conn_id exists, conn_res is null, sync error may happen");
				// 		goto kill;
				// 	}
				// }
				// else{
				// 	LOG_ERROR("conn_id not exists, virtual conn maynot inserted or sync error may happen");
				// 	goto kill;
				// }
				// pthread_mutex_unlock(&router->connmap_lock.mutex);

				/** perform post send */
				// this lock maybe not required when using single worker thread
				pthread_mutex_lock(&conn->data_mutex);
				rc = ibv_post_send(conn->qp, &sr, &bad_wr);
				if (rc){
					LOG_ERROR("failed to post SR");
					goto kill;
				}
				else
				{
					switch (sr.opcode)
					{
					case IBV_WR_SEND:
						LOG_TRACE("Send Request was posted");
						break;
					case IBV_WR_RDMA_READ:
						LOG_TRACE("RDMA Read Request was posted");
						break;
					case IBV_WR_RDMA_WRITE:
						LOG_TRACE("RDMA Write Request was posted");
						break;
					default:
						LOG_TRACE("Unknown Request was posted");
						break;
					}
				}
				pthread_mutex_unlock(&conn->data_mutex);

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct POST_SEND_RSP));
				// fill rsp with wr_id
				// ((struct POST_SEND_RSP *)rsp)->wr_id = header.process_id;
				((struct POST_SEND_RSP *)rsp)->wr_id = send_req->conn_idx.vq_index;
				size = sizeof(struct POST_SEND_RSP);
			}
			break;

			case POST_RECV:
			{
				LOG_TRACE("POST_RECV, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = (char *) malloc(sizeof(struct POST_RECV_REQ));
                if (read(client_sock, req_body, sizeof(struct POST_RECV_REQ)) < sizeof(struct POST_RECV_REQ))
                {
                    LOG_ERROR("POST_RECV: Failed to read the request body."); 
                    goto kill;
                }
				struct POST_RECV_REQ *recv_req = (struct POST_RECV_REQ *)req_body;
				
				/** construct recv_wr according to recv_req */
				int rc = 0;
				struct ibv_recv_wr rr;
				struct ibv_sge sge;
				struct ibv_recv_wr *bad_wr = NULL;

				// prepare the scatter/gather entry */
				memset(&sge, 0, sizeof(sge));
				sge.addr = (uintptr_t)recv_req->local_addr;
				sge.length = recv_req->length;
				sge.lkey = recv_req->lkey;
				/* prepare the send work request */
				memset(&rr, 0, sizeof(rr));
				rr.next = NULL;
				rr.wr_id = recv_req->wr_id;
				rr.sg_list = &sge;
				rr.num_sge = 1;

				// if (rr.wr_id != header.process_id){
				// 	LOG_ERROR("reqeust header process_id is not equal to wr_id: " << header.process_id << " vs. " << rr.wr_id);
				// 	goto kill;
				// }

				if (rr.wr_id != recv_req->conn_idx.vq_index){
					LOG_ERROR("conn_idx.vq_index is not equal to wr_id: " << recv_req->conn_idx.vq_index << " vs. " << rr.wr_id);
					goto kill;
				}

				/** acquire physical QP by qp_index, without locks */
				struct conn_resources *conn = NULL;
				if(router->conn_map[recv_req->conn_idx.qp_index] != NULL){
					conn = router->conn_map[recv_req->conn_idx.qp_index];
					LOG_TRACE("conn pointer: " << conn << " get QP by qp_index success ");
				}
				else{
					LOG_ERROR("conn_idx exists, conn_res is null, sync error may happen");
				}
				/** query physical QP by conn_id */
				// struct conn_resources *conn = NULL;
				// // print_conn_id(&recv_req->conn_id);
				// pthread_mutex_lock(&router->connmap_lock.mutex);
				// auto connmap_iter = router->connmap_lock.conn_map.find(recv_req->conn_id);
				// if(connmap_iter != router->connmap_lock.conn_map.end()){
				// 	if(connmap_iter->second != NULL){
				// 		conn = (conn_resources *)connmap_iter->second;
				// 		LOG_TRACE("conn pointer: " << conn << " query connmap success ");
				// 	}
				// 	else{
				// 		LOG_ERROR("conn_id exists, conn_res is null, sync error may happen");
				// 		goto kill;
				// 	}
				// }
				// else{
				// 	LOG_ERROR("conn_id not exists, virtual conn maynot inserted or sync error may happen");
				// 	goto kill;
				// }
				// pthread_mutex_unlock(&router->connmap_lock.mutex);

				/** perform post recv */
				// this lock maybe not required when using single worker thread
				pthread_mutex_lock(&conn->data_mutex);
				rc = ibv_post_recv(conn->qp, &rr, &bad_wr);
				if (rc){
					LOG_ERROR("failed to post RR");
					goto kill;
				}
				else
				{
					LOG_TRACE("Receive Request was posted");
				}
				pthread_mutex_unlock(&conn->data_mutex);

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct POST_SEND_RSP));
				// fill rsp with wr_id
				// ((struct POST_RECV_RSP *)rsp)->wr_id = header.process_id;
				((struct POST_RECV_RSP *)rsp)->wr_id = recv_req->conn_idx.vq_index;
				size = sizeof(struct POST_RECV_RSP);
			}
			break;

			case POLL_CQ:
			{
				LOG_TRACE("POLL_CQ, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = (char *) malloc(sizeof(struct POLL_CQ_REQ));
                if (read(client_sock, req_body, sizeof(struct POLL_CQ_REQ)) < sizeof(struct POLL_CQ_REQ))
                {
                    LOG_ERROR("POLL_CQ: Failed to read the request body."); 
                    goto kill;
                }
				struct POLL_CQ_REQ *poll_req = (struct POLL_CQ_REQ *)req_body;

				/** prepare cq_wc */
				// ibv_poll_cq return count of CQEs
				int poll_result = 0;
				// expected number of CQEs
				int exp_cnt = poll_req->count; 
				// pre-allocate cq_wc
				struct ibv_wc *wc;
				wc = (struct ibv_wc *)calloc(exp_cnt, sizeof(struct ibv_wc));

				/** acquire physical QP by qp_index, without locks */
				struct conn_resources *conn = NULL;
				if(router->conn_map[poll_req->conn_idx.qp_index] != NULL){
					conn = router->conn_map[poll_req->conn_idx.qp_index];
					LOG_TRACE("conn pointer: " << conn << " get QP by qp_index success ");
				}
				else{
					LOG_ERROR("conn_idx exists, conn_res is null, sync error may happen");
				}				
				// /** query physical QP by conn_id */
				// struct conn_resources *conn = NULL;
				// pthread_mutex_lock(&router->connmap_lock.mutex);
				// auto connmap_iter = router->connmap_lock.conn_map.find(poll_req->conn_id);
				// if(connmap_iter != router->connmap_lock.conn_map.end()){
				// 	if(connmap_iter->second != NULL){
				// 		conn = (conn_resources *)connmap_iter->second;
				// 		LOG_TRACE("conn pointer: " << conn << " query connmap success ");
				// 	}
				// 	else{
				// 		LOG_ERROR("conn_id exists, conn_res is null, sync error may happen");
				// 		goto kill;
				// 	}
				// }
				// else{
				// 	LOG_ERROR("conn_id not exists, virtual conn maynot inserted or sync error may happen");
				// 	goto kill;
				// }
				// pthread_mutex_unlock(&router->connmap_lock.mutex);

				/** perform poll cq */
				// this lock maybe not required when using single worker thread
				pthread_mutex_lock(&conn->data_mutex);
				poll_result = ibv_poll_cq(conn->cq, exp_cnt, wc);
				pthread_mutex_unlock(&conn->data_mutex);

				if(poll_result < 0){
					LOG_ERROR("failed in poll CQ");
				}

				for(int i = 0; i < poll_result; i++){
					/* CQE found */
					LOG_TRACE_PRINTF("completion was found in CQ with status 0x%x, wr_id %d\n", wc[i].status, wc[i].wr_id);
					/* check the completion status (here we don't care about the completion opcode */
					if (wc[i].status != IBV_WC_SUCCESS){
						LOG_ERROR_PRINTF("got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc[i].status,
								wc[i].vendor_err);
						goto kill;
					}

					// if (wc[i].wr_id != header.process_id){
					// 	LOG_ERROR("reqeust header process_id is not equal to wr_id");
					// 	goto kill;
					// }

					if (wc[i].wr_id != poll_req->conn_idx.vq_index){
						LOG_ERROR("conn_idx.vq_index is not equal to wr_id");
						goto kill;
					}
				}
				free(wc);

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct POLL_CQ_RSP));
				// fill rsp with wr_id
				// ((struct POLL_CQ_RSP *)rsp)->wr_id = header.process_id;
				((struct POLL_CQ_RSP *)rsp)->wr_id = poll_req->conn_idx.vq_index;
				((struct POLL_CQ_RSP *)rsp)->count = poll_result;
				size = sizeof(struct POLL_CQ_RSP);
			}
			break;

			case DEREG_MR:
			{
				LOG_TRACE("DEREG_MR, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** fill req */
                // req_body = malloc(sizeof(struct DEREG_MR_REQ));
                if (read(client_sock, req_body, sizeof(struct DEREG_MR_REQ)) < sizeof(struct DEREG_MR_REQ))
                {
                    LOG_ERROR("DEREG_MR: Failed to read the request body."); 
                    goto kill;
                }

				struct DEREG_MR_REQ *mr_req = (struct DEREG_MR_REQ *)req_body;
				struct ConnId conn_id = mr_req->conn_id;		// conn_id is not used, using a global PD/ctx currently
				struct LocalMem local_mem = mr_req->local_mem;
				int rc = 0;

				/** register at RNIC, currently a global PD is used; (TODO! host_key ---> PD/QP) */
				LOG_TRACE("Deregistering a MR ptr="<< local_mem.router_addr << ", size="  << local_mem.length);            
                rc = ibv_dereg_mr(local_mem.mr);
                if (rc)
                {
                    LOG_ERROR("Failed to deregiester MR for shm_name: " << local_mem.shm_name);
                    goto kill;
                }

				/** free router side memory */
				// whether close shm_fd, munmap router_addr, shm_unlink shm_name at router side? yes!
				// whether free router_addr again after munmap? NO! shm_ptr is not malloced/calloced, free will cause segement fault!
				// pointer to sp is free in router's destructor
				ShmPiece *sp = (ShmPiece *)local_mem.shm_piece;
				sp->remove();

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct DEREG_MR_RSP));
				// fill rsp, clean conn_id, client_addr, length 
				local_mem.conn_id = NULL;
				local_mem.client_addr = NULL;
				local_mem.length = 0;
				// fill rsp, clean router_addr, shm_piece, shm_name, shm_fd 
				local_mem.router_addr = NULL;
				local_mem.shm_piece = NULL;
				memset(local_mem.shm_name, 0, sizeof(local_mem.shm_name));
				local_mem.shm_fd = 0; // shm_fd is not used for mmap, fill shm_fd in scalelib
				// fill rsp, clean pointer of ibv_mr, lkey, rkey
				local_mem.mr = NULL;
				local_mem.lkey = 0;
				local_mem.rkey = 0;
				// fill rsp: copy local_mem
				((struct REG_MR_RSP *)rsp)->local_mem = local_mem;
                size = sizeof(struct REG_MR_RSP);
			}
			break;

			case DESTROY_RES:
			{
                LOG_TRACE("DESTROY_RES, client id = " << header.process_id << "; body_size = " << header.body_size);
				/** read req */
                // req_body = (char *)malloc(sizeof(struct DESTROY_RES_REQ));
                if (read(client_sock, req_body, sizeof(struct DESTROY_RES_REQ)) < sizeof(struct DESTROY_RES_REQ))
                {
                    LOG_ERROR("DESTROY_RES: Failed to read the request body."); 
                    goto kill;
                }
				struct DESTROY_RES_REQ *res_req = (struct DESTROY_RES_REQ *)req_body;

				struct ConnId conn_id = res_req->conn_id;
				struct ConnIdx conn_idx = res_req->conn_idx;
				int del_res = 0;
				struct conn_resources *conn = NULL;

				// /** try to delete conn_id in conn_map */
				// pthread_mutex_lock(&router->connmap_lock.mutex);
				// auto connmap_iter = router->connmap_lock.conn_map.find(conn_id);
				// if(connmap_iter != router->connmap_lock.conn_map.end()){
				// 	router->connmap_lock.conn_map.erase(connmap_iter);
				// 	LOG_TRACE("delete conn_id from conn_map for process_id " << header.process_id);
				// }
				// else{
				// 	LOG_TRACE("found no conn_id from conn_map for process_id " << header.process_id);
				// }
				// pthread_mutex_unlock(&router->connmap_lock.mutex);

				/** try to query & delete host_key in host_map */
				pthread_mutex_lock(&router->hostmap_lock.mutex);
				auto iter = router->hostmap_lock.host_map.find(conn_id.conn_key.host_key);
				if(iter != router->hostmap_lock.host_map.end()){
					if(iter->second != NULL){
						conn = (struct conn_resources *)iter->second;
						conn->ref_cnt--;
						if(conn->ref_cnt <= 0){
							del_res = 1;
							router->hostmap_lock.host_map.erase(iter);
							LOG_TRACE("delete host_key from host_map for process_id " << header.process_id);
						}
					}
					else{
						LOG_ERROR("host_key exists, however conn_res is NULL, sync error may happen");
						goto kill;
					}
				}
				else{
					LOG_TRACE("found no host_key from host_map for process_id " << header.process_id);
				}
				pthread_mutex_unlock(&router->hostmap_lock.mutex);

				/** release vq resources: vq_shm_vec, vcq_shm_map, vqp_shm_map */
				pthread_mutex_lock(&conn->mutex);
				ShmPiece* cq_sp = conn->vcq_shm_map[conn_idx.vq_index];
				conn->vcq_shm_map[conn_idx.vq_index] = NULL;
                if (cq_sp)
                    delete cq_sp;
				ShmPiece* qp_sp = conn->vqp_shm_map[conn_idx.vq_index];
				conn->vqp_shm_map[conn_idx.vq_index] = NULL;
				if (qp_sp)
					delete qp_sp;
				pthread_mutex_lock(&conn->vq_mutex);
				std::vector<uint16_t>::iterator position = std::find(conn->vq_shm_vec.begin(), conn->vq_shm_vec.end(), conn_idx.vq_index);
                if (position != conn->vq_shm_vec.end()) // == myVector.end() means the element was not found
                    conn->vq_shm_vec.erase(position);
				pthread_mutex_unlock(&conn->vq_mutex);
				pthread_mutex_unlock(&conn->mutex); 

				/** release conn_resources */
				if(del_res){
					// this lock is not required?
					pthread_mutex_lock(&conn->mutex);
					if(router->resources_destroy(conn)){
						LOG_ERROR("Destroy QP res failed for process_id " << header.process_id);
					}
					else{
						LOG_TRACE("Destory QP res success for process_id " << header.process_id);
					}
					pthread_mutex_unlock(&conn->mutex);
					pthread_mutex_destroy(&conn->mutex);
					free(conn);

					/** delete conn_res in qp_vec & conn_map with qp_index */
					pthread_mutex_lock(&router->qp_mutex);
					std::vector<uint16_t>::iterator position = find(router->qp_vec.begin(), router->qp_vec.end(), conn_idx.qp_index);
					if(position != router->qp_vec.end()) // == myVector.end() means the element was not found
						router->qp_vec.erase(position);
					router->conn_map[conn_idx.qp_index] = NULL;
					LOG_TRACE_PRINTF("Delete qp_vec & conn_map with qp_index=%u success!\n", conn_idx.qp_index);
					pthread_mutex_unlock(&router->qp_mutex);
				}

				/** fill rsp */
				// rsp = (char *)malloc(sizeof(struct DESTROY_RES_RSP));
				conn_id.conn_key.src_qpn = 0;
				conn_id.conn_key.dst_qpn = 0;
				((struct DESTROY_RES_RSP *)rsp)->conn_id = conn_id;
                size = sizeof(struct DESTROY_RES_RSP);
			}
			break;
			
			default:
                break;

		}

		LOG_TRACE("write rsp " << size << " bytes to sock " << client_sock);
		if(LOG_LEVEL <= 1) fflush(stdout);
        
		if ((n = write(client_sock, rsp, size)) < size)
        {
            LOG_ERROR("Error in writing bytes" << n);
            /*if (req_body != NULL)
                free(req_body);
    
            if(rsp != NULL)
                free(rsp);*/

            goto kill;
        }


    //memset(rsp, 0, 0xfffff);
    
end:
		/* only used for fd deliver */
        if (host_fd >= 0) {
            close(host_fd);
        }

		/* clients build new connection for each request */
		goto kill;
    }

kill:
    close(client_sock);
    free(args);
    free(rsp);
    free(req_body);

    return NULL;
}

void *work_process(void *args){
	struct WorkerArgs *worker_args = (struct WorkerArgs *)args;

	LOG_INFO("Start to handle the request from client SHM.");
	ScaleRouter *router = worker_args->router;

    cpu_set_t cpuset; 

    //the CPU we want to use
    int cpu = 2;

    CPU_ZERO(&cpuset);       //clears the cpuset
    CPU_SET( cpu , &cpuset); //set CPU 2 on cpuset

    /*
     * cpu affinity for the calling thread 
     * first parameter is the pid, 0 = calling thread
     * second parameter is the size of your cpuset
     * third param is the cpuset in which your thread will be
     * placed. Each bit represents a CPU
     */
    sched_setaffinity(0, sizeof(cpuset), &cpuset);

    unsigned int count = 0;
	struct conn_resources *conn = NULL;
    ShmPiece *vqp_sp = NULL;
    ShmPiece *vcq_sp = NULL;
    struct CtrlShmPiece *vqp_csp = NULL;
    struct CtrlShmPiece *vcq_csp = NULL;

    void *req_body, *rsp;
    struct ScaleReqHeader *req_header;
    // struct ScaleRspHeader *rsp_header;

    struct ibv_qp *qp = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_wc *wc_list = NULL;

	int i=0, j=0; // qp_index & vq_index respectively 

	LOG_TRACE("Start Loop of worker thread");

	while(1){
		// LOG_TRACE("Start iterate all conn on qp_vec, size=" << (int)router->qp_vec.size());

		pthread_mutex_lock(&router->qp_mutex);
		// lock is required here (read qp_vec error without lock)
		// however, qp_mutex may starve control path 
		// iterate over all conn
		int qp_vec_len = (int)router->qp_vec.size();
		for(i = 0; i < qp_vec_len; i ++){
			conn = router->conn_map[router->qp_vec[i]];

			// LOG_TRACE("Begin to process operations on conn pointer: " << conn << " with qp_index=" << router->qp_vec[i]);

			// verify conn status before operation
			if(!conn) continue;

			pthread_mutex_lock(&conn->vq_mutex);
			// lock is required here (read vqp_shm_vec error without lock)
			// however, the data path may be affected
			// iterate all vqp, post_send & post_recv operations
			int vq_vec_len = (int)conn->vq_shm_vec.size();
			for(j = 0; j < vq_vec_len; j ++){
				vqp_sp = conn->vqp_shm_map[conn->vq_shm_vec[j]];

				// verify vqp_shm status before operation
				if(!vqp_sp) continue;

				// LOG_TRACE("Begin to process operations on vqp_shm_map with vq_index=" << conn->vq_shm_vec[j]);

				vqp_csp = (struct CtrlShmPiece*)vqp_sp->ptr;
				//rmb();

				if(!(vqp_csp->state == REQ_DONE)) continue;

				LOG_TRACE("REQ_DONE on vqp_shm vq_index=" << conn->vq_shm_vec[j]);

				req_header = (struct ScaleReqHeader *)vqp_csp->req;
				req_body = vqp_csp->req + sizeof(struct ScaleReqHeader);
				rsp = vqp_csp->rsp;
				// rsp_header = (struct ScaleRspHeader *)vqp_csp->rsp;
				// rsp = vqp_csp->rsp + sizeof(struct ScaleRspHeader);

				switch(req_header->func){
					case POST_SEND:
					{
						LOG_TRACE("POST_SEND, client id = " << req_header->process_id << "; body_size = " << req_header->body_size);
						struct POST_SEND_REQ *send_req = (struct POST_SEND_REQ *)req_body;

						/** construct send_wr according to send_req */
						int rc = 0;
						struct ibv_send_wr sr;
						struct ibv_sge sge;
						struct ibv_send_wr *bad_wr = NULL;

						// prepare the scatter/gather entry */
						memset(&sge, 0, sizeof(sge));
						sge.addr = (uintptr_t)send_req->local_addr;
						sge.length = send_req->length;
						sge.lkey = send_req->lkey;
						/* prepare the send work request */
						memset(&sr, 0, sizeof(sr));
						sr.next = NULL;
						sr.wr_id = send_req->wr_id;
						sr.sg_list = &sge;
						sr.num_sge = 1;
						sr.opcode = static_cast<ibv_wr_opcode>(send_req->opcode);
						sr.send_flags = IBV_SEND_SIGNALED;
						if (sr.opcode != IBV_WR_SEND)
						{
							sr.wr.rdma.remote_addr = (uintptr_t)send_req->remote_addr;
							sr.wr.rdma.rkey = send_req->rkey;
						}

						// if (sr.wr_id != req_header->process_id){
						// 	LOG_ERROR("reqeust header process_id is not equal to wr_id");
						// 	goto kill;
						// }

						if (sr.wr_id != send_req->conn_idx.vq_index){
							LOG_ERROR("conn_idx.vq_index is not equal to wr_id: " << send_req->conn_idx.vq_index << " vs. " << sr.wr_id);
							goto kill;
						}

						/** perform post send */
						// this lock maybe not required when using single worker thread
						// pthread_mutex_lock(&conn->data_mutex);
						rc = ibv_post_send(conn->qp, &sr, &bad_wr);
						if (rc){
							LOG_ERROR("failed to post SR");
							goto kill;
						}
						else
						{
							switch (sr.opcode)
							{
							case IBV_WR_SEND:
								LOG_TRACE("Send Request was posted");
								break;
							case IBV_WR_RDMA_READ:
								LOG_TRACE("RDMA Read Request was posted");
								break;
							case IBV_WR_RDMA_WRITE:
								LOG_TRACE("RDMA Write Request was posted");
								break;
							default:
								LOG_TRACE("Unknown Request was posted");
								break;
							}
						}
						// pthread_mutex_unlock(&conn->data_mutex);

						/** fill rsp */
						// rsp = (char *)malloc(sizeof(struct POST_SEND_RSP));
						// fill rsp with wr_id
						// ((struct POST_SEND_RSP *)rsp)->wr_id = req_header->process_id;
						((struct POST_SEND_RSP *)rsp)->wr_id = send_req->conn_idx.vq_index;
					}
					break;
					case POST_RECV:
					{
						LOG_TRACE("POST_RECV, client id = " << req_header->process_id << "; body_size = " << req_header->body_size);
						struct POST_RECV_REQ *recv_req = (struct POST_RECV_REQ *)req_body;
						
						/** construct recv_wr according to recv_req */
						int rc = 0;
						struct ibv_recv_wr rr;
						struct ibv_sge sge;
						struct ibv_recv_wr *bad_wr = NULL;

						// prepare the scatter/gather entry */
						memset(&sge, 0, sizeof(sge));
						sge.addr = (uintptr_t)recv_req->local_addr;
						sge.length = recv_req->length;
						sge.lkey = recv_req->lkey;
						/* prepare the send work request */
						memset(&rr, 0, sizeof(rr));
						rr.next = NULL;
						rr.wr_id = recv_req->wr_id;
						rr.sg_list = &sge;
						rr.num_sge = 1;

						// if (rr.wr_id != req_header->process_id){
						// 	LOG_ERROR("reqeust header process_id is not equal to wr_id");
						// 	goto kill;
						// }

						if (rr.wr_id != recv_req->conn_idx.vq_index){
							LOG_ERROR("conn_idx.vq_index is not equal to wr_id: " << recv_req->conn_idx.vq_index << " vs. " << rr.wr_id);
							goto kill;
						}

						/** perform post recv */
						// this lock maybe not required when using single worker thread
						// pthread_mutex_lock(&conn->data_mutex);
						rc = ibv_post_recv(conn->qp, &rr, &bad_wr);
						if (rc){
							LOG_ERROR("failed to post RR");
							goto kill;
						}
						else
						{
							LOG_TRACE("Receive Request was posted");
						}
						// pthread_mutex_unlock(&conn->data_mutex);

						/** fill rsp */
						// rsp = (char *)malloc(sizeof(struct POST_SEND_RSP));
						// fill rsp with wr_id
						// ((struct POST_RECV_RSP *)rsp)->wr_id = req_header->process_id;
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
			}
			// pthread_mutex_unlock(&conn->vq_mutex);

			/*********************************
			// need this judgement here; if all vqp/vcq are released, continue to next conn_res/qp
			// otherwise control path will hang/starve on qp_mutex lock (qp_vec delete)
			// DESTROY_RES hang at qp_mutex lock...
			**********************************/
			if(vq_vec_len <= 0) 
			{
				// remember to release vq_mutex before lock, otherwise control path will hang/starve on vq_mutex lock (vq_vec insert)
			 	// SETUP_CONN hang at vq_mutex lock...
				pthread_mutex_unlock(&conn->vq_mutex); 
				continue;
			}

			/** async poll_cq operations, poll first then update vcq */
			/** prepare cq_wc */
			// ibv_poll_cq return count of CQEs
			int poll_result = 0;
			// expected number of CQEs
			int exp_cnt = 128; 
			// pre-allocate cq_wc
			struct ibv_wc *wc;
			wc = (struct ibv_wc *)calloc(exp_cnt, sizeof(struct ibv_wc));

			if(!conn || !conn->cq || conn->ref_cnt < 0){
				// this trace no valid!
				LOG_TRACE("conn null or conn->cq null or conn->ref_cnt < 0, conn_res is already released!");
			}
			
			// poll current conn->cq
			poll_result = ibv_poll_cq(conn->cq, exp_cnt, wc);

			if(poll_result < 0){
				LOG_ERROR("failed in poll CQ");
			}

			for(int k = 0; k < poll_result; k++){
				/* CQE found */
				LOG_TRACE_PRINTF("completion was found in CQ with status 0x%x, wr_id %d\n", wc[k].status, wc[k].wr_id);
				/* check the completion status (here we don't care about the completion opcode */
				if (wc[k].status != IBV_WC_SUCCESS){
					LOG_ERROR_PRINTF("got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc[k].status,
							wc[k].vendor_err);
					goto kill;
				}

				vcq_sp = conn->vcq_shm_map[wc[k].wr_id];

				// verify vcq_shm status before operation
				if(!vcq_sp) 
				{
					LOG_ERROR("Polled CQE for vcq_shm with vq_index=" << wc[k].wr_id << " , however, vcq_shm is not ready!");
					continue;
				}

				LOG_TRACE("Begin to post CQE to vcq_shm_map with vq_index=" << wc[k].wr_id);

				vcq_csp = (struct CtrlShmPiece*)vcq_sp->ptr;
				//rmb();

				// wait unitl vcq_csp state is ready
				// easy to be hanged by client, how to resolve???
				// do{}while(!(vcq_csp->state == REQ_DONE));
				// if(!(vcq_csp->state == REQ_DONE)) continue;
				do{
					// wmb();
					// LOG_TRACE("Current vcq_csp->state = " << vcq_csp->state);
				}while(!(vcq_csp->state == REQ_DONE));

				LOG_TRACE("REQ_DONE on vcq_shm with vq_index=" << wc[k].wr_id);

				req_header = (struct ScaleReqHeader *)vcq_csp->req;
				req_body = vcq_csp->req + sizeof(struct ScaleReqHeader);
				rsp = vcq_csp->rsp;

				switch(req_header->func){
					case POLL_CQ:
					{
						struct POLL_CQ_REQ *poll_req = (struct POLL_CQ_REQ *)req_body;
						if (wc[k].wr_id != poll_req->conn_idx.vq_index){
							LOG_ERROR("conn_idx.vq_index is not equal to wr_id");
							goto kill;
						}
						((struct POLL_CQ_RSP *)rsp)->wr_id = poll_req->conn_idx.vq_index;
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
			free(wc);	
			pthread_mutex_unlock(&conn->vq_mutex);	

			/* sync poll cq */
			/******************************************************************************
			// release once then acquire lock again
			pthread_mutex_lock(&conn->vq_mutex);
			// lock is required here (read vqp_shm_vec error without lock)
			// however, the data path may be affected
			// iterate all vqp, poll_cq operations
			vq_vec_len = (int)conn->vq_shm_vec.size();
			/*****************************************************************************/
			/******************************************************************************
			for(j = 0; j < vq_vec_len; j ++){
				vcq_sp = conn->vcq_shm_map[conn->vq_shm_vec[j]];

				// verify vcq_shm status before operation
				if(!vcq_sp) continue;

				// LOG_TRACE("Begin to process operations on vcq_shm_map with vq_index=" << conn->vq_shm_vec[j]);

				vcq_csp = (struct CtrlShmPiece*)vcq_sp->ptr;
				//rmb();

				if(!(vcq_csp->state == REQ_DONE)) continue;

				LOG_TRACE("REQ_DONE on vcq_shm vq_index=" << conn->vq_shm_vec[j]);

				req_header = (struct ScaleReqHeader *)vcq_csp->req;
				req_body = vcq_csp->req + sizeof(struct ScaleReqHeader);
				rsp = vcq_csp->rsp;
				// rsp_header = (struct ScaleRspHeader *)vqp_csp->rsp;
				// rsp = vqp_csp->rsp + sizeof(struct ScaleRspHeader);

				switch(req_header->func){
					case POLL_CQ:
					{
						struct POLL_CQ_REQ *poll_req = (struct POLL_CQ_REQ *)req_body;
						
						// prepare cq_wc
						// ibv_poll_cq return count of CQEs
						int poll_result = 0;
						// expected number of CQEs
						int exp_cnt = poll_req->count; 
						// pre-allocate cq_wc
						struct ibv_wc *wc;
						wc = (struct ibv_wc *)calloc(exp_cnt, sizeof(struct ibv_wc));

						poll_result = ibv_poll_cq(conn->cq, exp_cnt, wc);

						if(poll_result < 0){
							LOG_ERROR("failed in poll CQ");
						}

						for(int k = 0; k < poll_result; k++){
							// CQE found 
							LOG_TRACE_PRINTF("completion was found in CQ with status 0x%x, wr_id %d\n", wc[k].status, wc[k].wr_id);
							// check the completion status (here we don't care about the completion opcode
							if (wc[k].status != IBV_WC_SUCCESS){
								LOG_ERROR_PRINTF("got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc[k].status,
										wc[k].vendor_err);
								goto kill;
							}

							// if (wc[i].wr_id != header.process_id){
							// 	LOG_ERROR("reqeust header process_id is not equal to wr_id");
							// 	goto kill;
							// }

							if (wc[i].wr_id != poll_req->conn_idx.vq_index){
								LOG_ERROR("conn_idx.vq_index is not equal to wr_id");
								goto kill;
							}
						}
						free(wc);

						((struct POLL_CQ_RSP *)rsp)->wr_id = poll_req->conn_idx.vq_index;
						((struct POLL_CQ_RSP *)rsp)->count = poll_result;	
					}
					break;
					default:
					{
						LOG_ERROR("REQ_DONE hoever, FUNC_CALL type is not correct!");
					}
                    break;
				}
				wmb();
               	vcq_csp->state = RSP_DONE;
			}
			pthread_mutex_unlock(&conn->vq_mutex);
			******************************************************************************/
		}
		pthread_mutex_unlock(&router->qp_mutex);
	}


kill:
	free(args);

	LOG_TRACE("Exit work thread!");
	return NULL;
}

/******************************************************************************
Wrapper of logs
******************************************************************************/
void print_conn_info(struct ConnInfo *val){
	struct ConnInfo conn_info = *val;
	uint8_t *p;
	p = (uint8_t *)&conn_info.conn_id.conn_key.host_key.src_gid;
	LOG_TRACE_PRINTF("SRC: GID =%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x | LID = %u \n",p[0],
				p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], conn_info.host_lid.src_lid);
	p = (uint8_t *)&conn_info.conn_id.conn_key.host_key.dst_gid;
	LOG_TRACE_PRINTF("DST: GID =%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x | LID = %u \n",p[0],
				p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], conn_info.host_lid.dst_lid);
	// LOG_INFO_PRINTF("SRC: gid.sub_pf=%#x, gid.itl_id=%#x, lid=%u \n",
    //     conn_info.conn_id.conn_key.host_key.src_gid.global.subnet_prefix,
    //     conn_info.conn_id.conn_key.host_key.src_gid.global.interface_id,
    //     conn_info.host_lid.src_lid);
	// LOG_INFO_PRINTF("DST: gid.sub_pf=%#x, gid.itl_id=%#x, lid=%u \n",
    //     conn_info.conn_id.conn_key.host_key.dst_gid.global.subnet_prefix,
    //     conn_info.conn_id.conn_key.host_key.dst_gid.global.interface_id,
    //     conn_info.host_lid.dst_lid);
}

void print_conn_id(struct ConnId *val){
	struct ConnId conn_id = *val;
	LOG_TRACE("process_id: " << conn_id.process_id);
	uint8_t *p;
	p = (uint8_t *)&conn_id.conn_key.host_key.src_gid;
	LOG_TRACE_PRINTF("SRC: GID =%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x | QPN = %u \n",p[0],
				p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], conn_id.conn_key.src_qpn);
	p = (uint8_t *)&conn_id.conn_key.host_key.dst_gid;
	LOG_TRACE_PRINTF("DST: GID =%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x | QPN = %u \n", p[0],
				p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], conn_id.conn_key.dst_qpn);
    // LOG_INFO_PRINTF("SRC: gid.sub_pf=%#x, gid.itl_id=%#x, qpn=%u \n",
    //     conn_id.conn_key.host_key.src_gid.global.subnet_prefix,
    //     conn_id.conn_key.host_key.src_gid.global.interface_id,
    //     conn_id.conn_key.src_qpn);
    // LOG_INFO_PRINTF("DST: gid.sub_pf=%#x, gid.itl_id=%#x, qpn=%u \n",
    //     conn_id.conn_key.host_key.dst_gid.global.subnet_prefix,
    //     conn_id.conn_key.host_key.dst_gid.global.interface_id,
    //     conn_id.conn_key.dst_qpn);
}