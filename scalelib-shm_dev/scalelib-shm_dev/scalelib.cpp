#include "scalelib.h"

ScaleLib scalelib;

ScaleLib::ScaleLib(){
    this->process_id = getpid();

	for (int i = 0; i < MAP_SIZE; i++)
    {
		this->conn_map[i] = NULL;
	}
	pthread_mutex_init(&this->conn_mutex, NULL);
}

ScaleLib::~ScaleLib(){
	pthread_mutex_destroy(&this->conn_mutex);
}

int ScaleLib::setup_res_conn(struct ConnIdx *conn_idx){
		// try pre-allocate conn_res
	int create_flag = 0; // will be true if host_key has no conn_res
	struct conn_resources *new_conn = (struct conn_resources *)calloc(1, sizeof(struct conn_resources));
	pthread_mutex_init(&new_conn->mutex, NULL);
	pthread_mutex_lock(&new_conn->mutex);
	new_conn->ref_cnt++; // update ref_cnt after created

	struct conn_resources *old_conn = NULL;
	// update host_map
	pthread_mutex_lock(&scalelib.conn_mutex);
	if(scalelib.conn_map[conn_idx->qp_index] != NULL){
		create_flag = 0;
		old_conn = scalelib.conn_map[conn_idx->qp_index]; 
		old_conn->ref_cnt++;
		if(old_conn->status !=4 || old_conn->qp_index != conn_idx->qp_index){
			LOG_ERROR("old_conn is created, however status: " << old_conn->status << ", or qp_index: " << old_conn->qp_index << " is not correct, sync error may happen!");
			pthread_mutex_unlock(&scalelib.conn_mutex);
			return -1;
		}
		else{
			LOG_TRACE_PRINTF("setup conn resource at lib side success, status: %u, qp_index: %u\n", old_conn->status.load(), old_conn->qp_index);
		}
	}
	else{
		// host_key has no conn_res, insert new_conn with host_key, update ref_count
		create_flag = 1;
		scalelib.conn_map[conn_idx->qp_index] = new_conn;
		LOG_TRACE("new conn pointer: " << new_conn << " update conn_map success");
	}
	pthread_mutex_unlock(&scalelib.conn_mutex);
	
	if(create_flag){
		new_conn->status = 4;
		new_conn->qp_index = conn_idx->qp_index;
		pthread_mutex_unlock(&new_conn->mutex);
	}
	else{
		pthread_mutex_unlock(&new_conn->mutex);
		pthread_mutex_destroy(&new_conn->mutex);
		free(new_conn); // release pre-allocate conn_res
	}	

	// /** still cause ref_cnt error in parallel updating conn_map */
	// if(scalelib.conn_map[conn_idx->qp_index] == NULL){
	// 	struct conn_resources *new_conn = (struct conn_resources *)calloc(1, sizeof(struct conn_resources));
	// 	pthread_mutex_init(&new_conn->mutex, NULL);
	// 	pthread_mutex_lock(&new_conn->mutex);
	// 	new_conn->ref_cnt++;
	// 	new_conn->status = 4;
	// 	new_conn->qp_index = conn_idx->qp_index;
	// 	pthread_mutex_unlock(&new_conn->mutex);
	// scalelib.conn_map[conn_idx->qp_index] = new_conn;
	// }
	// else{
	// 	struct conn_resources *old_conn = scalelib.conn_map[conn_idx->qp_index];
	// 	pthread_mutex_lock(&old_conn->mutex);
	// 	old_conn->ref_cnt++;
	// 	if(old_conn->status !=4 || old_conn->qp_index != conn_idx->qp_index){
	// 		LOG_ERROR("old_conn is created, however status: " << old_conn->status << ", or qp_index: " << old_conn->qp_index << " is not correct, sync error may happen!");
	// 		pthread_mutex_unlock(&old_conn->mutex);
	// 		return -1;
	// 	}
	// 	pthread_mutex_unlock(&old_conn->mutex);
	// }

	return 0;
}

int ScaleLib::release_res_conn(struct ConnIdx *conn_idx){
	struct conn_resources *conn = scalelib.conn_map[conn_idx->qp_index];
	int rc = 0, del_res = 0;

	/** try to delete conn_res in conn_map */
	pthread_mutex_lock(&scalelib.conn_mutex);
	if(conn != NULL){
		conn->ref_cnt--;
		if(conn->ref_cnt <= 0){
			del_res = 1;
			LOG_TRACE("delete conn_res from host_map with qp_index: " << conn_idx->qp_index);
		}
	}
	else{
		LOG_ERROR("conn_res is NULL, sync error may happen");
		rc = -1;
		return rc;
	}
	pthread_mutex_unlock(&scalelib.conn_mutex);
	
	/** release conn_resources */
	if(del_res){
		pthread_mutex_destroy(&conn->mutex);
		free(conn);
	}

	return rc;
}

int ScaleLib::setup_shm_comm(struct ConnIdx *conn_idx){
	struct conn_resources *conn_res = scalelib.conn_map[conn_idx->qp_index];
	int cq_fd, qp_fd;
	void* cq_shm_p;
	void* qp_shm_p;

	// Lock is only required when multiple threads use one VQ!
	// pthread_mutex_lock(&conn_res->mutex);

	/** allocate vcq_shm_map **/
	LOG_TRACE("Create vcq_shm_map: qp_index=" << conn_idx->qp_index << " vq_index=" << conn_idx->vq_index);
	std::stringstream cq_ss;
	cq_ss << "ctrlshm-cq-" << conn_idx->qp_index << "-" << conn_idx->vq_index;
	const char *vcq_shm_name = cq_ss.str().c_str();	
	cq_fd = shm_open(vcq_shm_name, O_CREAT | O_RDWR, 0666);
	if (ftruncate(cq_fd, sizeof(struct CtrlShmPiece))) {
		LOG_ERROR_PRINTF("Fail to mount shm %s\n", vcq_shm_name);
	}
	cq_shm_p = mmap(0, sizeof(struct CtrlShmPiece), PROT_READ | PROT_WRITE, MAP_SHARED, cq_fd, 0);    
    if (!cq_shm_p){
		LOG_ERROR_PRINTF("Fail to mount shm %s\n", vcq_shm_name);
		return -1;
	}
    else{
		LOG_TRACE_PRINTF("Succeed to mount shm %s\n", vcq_shm_name);
	}
	// close(cq_fd); // close fd will not affect the mmaped link

	conn_res->vcq_shm_map[conn_idx->vq_index] = (struct CtrlShmPiece *)cq_shm_p; 
	pthread_mutex_init(&(conn_res->vcq_shm_mtx_map[conn_idx->vq_index]), NULL); 

	/** allocate vqp_shm_map **/
	LOG_TRACE("Create vqp_shm_map: qp_index=" << conn_idx->qp_index << " vq_index=" << conn_idx->vq_index);
	std::stringstream qp_ss;
	qp_ss << "ctrlshm-qp-" << conn_idx->qp_index << "-" << conn_idx->vq_index;
	const char *vqp_shm_name = qp_ss.str().c_str();	
	qp_fd = shm_open(vqp_shm_name, O_CREAT | O_RDWR, 0666);
	if (ftruncate(qp_fd, sizeof(struct CtrlShmPiece))) {
		LOG_ERROR_PRINTF("Fail to mount shm %s\n", vqp_shm_name);
	}
	qp_shm_p = mmap(0, sizeof(struct CtrlShmPiece), PROT_READ | PROT_WRITE, MAP_SHARED, qp_fd, 0);    
    if (!qp_shm_p){
		LOG_ERROR_PRINTF("Fail to mount shm %s\n", vqp_shm_name);
		return -1;
	}
    else{
		LOG_TRACE_PRINTF("Succeed to mount shm %s\n", vqp_shm_name);
	}
	// close(qp_fd); // close fd will not affect the mmaped link

	/** close mmaped in advance 
	 * The mmap() function adds an extra reference to the file associated with the file descriptor fildes 
	 * which is not removed by a subsequent close() on that file descriptor. 
	 * This reference is removed when there are no more mappings to the file.
	*/

	conn_res->vqp_shm_map[conn_idx->vq_index] = (struct CtrlShmPiece *)qp_shm_p; 
	pthread_mutex_init(&(conn_res->vqp_shm_mtx_map[conn_idx->vq_index]), NULL); 

	// pthread_mutex_unlock(&conn_res->mutex);

	return 0;
}

int ScaleLib::release_shm_comm(struct ConnIdx *conn_idx){
	struct conn_resources *conn = scalelib.conn_map[conn_idx->qp_index];
	int rc = 0;

	/** release vq resources: vcq_shm_map, vcq_shm_mtx_map, vqp_shm_map, vqp_shm_mtx_map */
	// Lock is only required when multiple threads use one VQ!
	// pthread_mutex_lock(&conn->mutex);
	CtrlShmPiece* cq_sp = conn->vcq_shm_map[conn_idx->vq_index];
	if (cq_sp){
		rc = munmap((void *) cq_sp, sizeof(struct CtrlShmPiece));
		if(rc){
			LOG_ERROR("munmap cq shm map failed, errno: " << rc);
			return rc;
		}
		else{
			LOG_TRACE("munmap cq shm map success!");
		}
	}

	CtrlShmPiece* qp_sp = conn->vqp_shm_map[conn_idx->vq_index];
	if (qp_sp){
		rc = munmap((void *) qp_sp, sizeof(struct CtrlShmPiece));
		if(rc){
			LOG_ERROR("munmap qp shm map failed, errno: " << rc);
			return rc;
		}
		else{
			LOG_TRACE("munmap qp shm map success!");
		}
	}
	conn->vcq_shm_map[conn_idx->vq_index] = NULL;
	conn->vqp_shm_map[conn_idx->vq_index] = NULL;
	pthread_mutex_destroy(&(conn->vcq_shm_mtx_map[conn_idx->vq_index]));
	pthread_mutex_destroy(&(conn->vqp_shm_mtx_map[conn_idx->vq_index]));
	// pthread_mutex_unlock(&conn->mutex); 

	return rc;
}

/******************************************************************************
Unix domain socket operations to communicate with router
******************************************************************************/
/** TODO? change sock to member of ScaleLib, maintain sock connections (with mutex lock until request finished) for a conn_id, requester randomly submmits to each connection?
 ** TODO? limit parallel size? for control verbs, one sock connection is enough; for data verbs, order of requests need to be ensured!
 ** TDOO? handleprocess cannot be released with long sock connection! 
 * 
 */
void init_unix_sock(){

}

struct sock_with_lock* get_unix_sock(SCALE_FUNCTION_CALL req)
{
    struct sock_with_lock* unix_sock = (struct sock_with_lock*)malloc(sizeof(struct sock_with_lock));
    unix_sock->sock = -1;

	/** disable lock for local unix_socket*/
    // pthread_mutex_init(&(unix_sock->mutex), NULL);
    /* int index = rand() % PARALLEL_SIZE;
    
    switch (req)
	{
		case CREATE_RES:
        case REG_MR:
        case SETUP_CONN:
        case DEREG_MR:
        case DESTROY_RES:
			unix_sock = &control_sock[index];
			break;
		case POST_SEND:
			unix_sock = &send_sock[index];
			break;

		case POST_RECV:
			unix_sock = &recv_sock[index];
			break;

		case POLL_CQ:
			unix_sock = &poll_sock[index];
			break;

		default:
			printf("unrecognized request type %d.", req);
			fflush(stdout);
			return NULL;
	}*/

	if (unix_sock->sock < 0)
	{
		connect_router(unix_sock);
	}

	return unix_sock;
}

void connect_router(struct sock_with_lock *unix_sock)
{
    register int len;
    struct sockaddr_un srv_addr;

    // pthread_mutex_lock(&(unix_sock->mutex));

    if ((unix_sock->sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        LOG_ERROR("client: socket");
        exit(1);
    }
    
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
	strcpy(srv_addr.sun_path, AGENT_DOMAIN);
    if (connect(unix_sock->sock, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
        LOG_ERROR("client: cannot connect server domain: " << AGENT_DOMAIN);
        unix_sock->sock = -1;
    }

    // pthread_mutex_unlock(&(unix_sock->mutex));
}

int request_router(SCALE_FUNCTION_CALL req, void* req_body, void *rsp, int *rsp_size)
{
	// /* for cq event */
	// int mapped_fd = -1;

	/* return code of request result */
	int rc = 0;
	struct sock_with_lock* unix_sock = NULL;

    unix_sock = get_unix_sock(req);

	if (unix_sock == NULL || unix_sock->sock < 0)
	{
		LOG_ERROR("invalid socket.");
		rc = -1;
		return rc;
	}

	struct ScaleReqHeader header;

	if (!scalelib.process_id)
	{
		LOG_ERROR("Failed to acquire current process ID!");
		rc = -1;
		return rc;
	}

	header.process_id = scalelib.process_id;
	header.func = req;
	header.body_size = 0;
	int fixed_size_rsp = 1;
    int close_sock = 1;

	int bytes = 0;
	
	switch (req)
	{
		case INIT_RES:
			*rsp_size = sizeof(struct INIT_RES_RSP);
			header.body_size = 0;
			break;

		case CREATE_RES:
			*rsp_size = sizeof(struct CREATE_RES_RSP);
			header.body_size = sizeof(struct CREATE_RES_REQ);
			break;

		case REG_MR:
			*rsp_size = sizeof(struct REG_MR_RSP);
			header.body_size = sizeof(struct REG_MR_REQ);
			break;
		
		case SETUP_CONN:
			*rsp_size = sizeof(struct SETUP_CONN_RSP);
			header.body_size = sizeof(struct SETUP_CONN_REQ);
			break;
		
		case POST_SEND:
			*rsp_size = sizeof(struct POST_SEND_RSP);
			header.body_size = sizeof(struct POST_SEND_REQ);
			break;

		case POST_RECV:
			*rsp_size = sizeof(struct POST_RECV_RSP);
			header.body_size = sizeof(struct POST_RECV_REQ);
			break;

		case POLL_CQ:
			// fixed_size_rsp = 0;
			*rsp_size = sizeof(struct POLL_CQ_RSP);
			header.body_size = sizeof(struct POLL_CQ_REQ);
			break;
		
		case DEREG_MR:
			*rsp_size = sizeof(struct DEREG_MR_RSP);
			header.body_size = sizeof(struct DEREG_MR_REQ);
			break;

		case DESTROY_RES:
			*rsp_size = sizeof(struct DESTROY_RES_RSP);
			header.body_size = sizeof(struct DESTROY_RES_REQ);
			break;

		default:
			rc = -1;
			goto end;
	}

	// if (!close_sock)
	// {
	// 	pthread_mutex_lock(&(unix_sock->mutex));
	// }

	int n;
	if ((n = write(unix_sock->sock, &header, sizeof(header))) < sizeof(header))
	{
		if (n < 0)
		{
			LOG_ERROR("router disconnected in writing req header.\n");
			fflush(stdout);
			unix_sock->sock = -1;
		}
		else
		{
			LOG_TRACE("partial write " << n << "bytes" << " vs. sizeof(header) = " << sizeof(header));
		}

		rc = -1;
		goto end;
	}

	if (header.body_size > 0)
	{
		if ((n = write(unix_sock->sock, req_body, header.body_size)) < header.body_size)
		{
			if (n < 0)
			{
				LOG_ERROR("router disconnected in writing req body.");
				fflush(stdout);
				unix_sock->sock = -1;
			}
			else
			{
				LOG_TRACE("partial write " << n << "bytes" << " vs. sizeof(body) = " << header.body_size);
			}

			rc = -1;
			goto end;
		}	
	}

	/* for cq event */
	// if (req == IBV_CREATE_COMP_CHANNEL)
	// {
	// 	mapped_fd = recv_fd(unix_sock);
	// }

	if (!fixed_size_rsp)
	{
		struct ScaleRspHeader rsp_hr;
		bytes = 0;
		while(bytes < sizeof(rsp_hr)) {
			n = read(unix_sock->sock, ((char *)&rsp_hr) + bytes, sizeof(rsp_hr) - bytes);
			if (n < 0)
			{
				LOG_ERROR("router disconnected when reading rsp.");
				fflush(stdout);
				unix_sock->sock = -1;

				rc = -1;
				goto end;
			}
			bytes = bytes + n;
		}

		*rsp_size = rsp_hr.body_size;
	}

	bytes = 0;
	while(bytes < *rsp_size)
	{
		n = read(unix_sock->sock, (char*)rsp + bytes, *rsp_size - bytes);
		if (n < 0)
		{
			LOG_ERROR("router disconnected when reading rsp.");
			fflush(stdout);
			unix_sock->sock = -1;

			rc = -1;
			goto end;
		}

		bytes = bytes + n;
	}

	/** event ibv_get_cq_event is not implementated yet */
	// if (req == IBV_CREATE_COMP_CHANNEL)
	// {
	// 	printf("[INFO Comp Channel] rsp.ec.fd = %d, mapped_fd = %d\n", ((struct IBV_CREATE_COMP_CHANNEL_RSP*)rsp)->fd, mapped_fd);
	// 	fflush(stdout);

	// 	comp_channel_map[mapped_fd] = ((struct IBV_CREATE_COMP_CHANNEL_RSP*)rsp)->fd;
	// 	((struct IBV_CREATE_COMP_CHANNEL_RSP*)rsp)->fd = mapped_fd;
	// }

end:
    if (close_sock)
	{
		close(unix_sock->sock);
        // pthread_mutex_destroy(&unix_sock->mutex);
		free(unix_sock);
	}
	// else 
	// {
	// 	/* for cq event */
	// 	pthread_mutex_unlock(&(unix_sock->mutex)); 
	// }

	return rc;
}
/******************************************************************************
End of unix domain socket operations
******************************************************************************/


/******************************************************************************
SHM operations to communicate with router
******************************************************************************/
/* Fast datapath: this function itself is not thread safe! */
void request_router_shm(struct CtrlShmPiece *csp)
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
/******************************************************************************
 * End of SHM operations
*******************************************************************************/

/******************************************************************************
Socket operations
For simplicity, the example program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this example
******************************************************************************/
int sock_connect(const char *servername, int port)
{
	struct addrinfo *resolved_addr = NULL;
	struct addrinfo *iterator;
	char service[6];
	int sockfd = -1;
	int listenfd = 0;
	int tmp;
	int reuse_addr = 1;
	struct addrinfo hints =
	{
		.ai_flags = AI_PASSIVE,
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM};
	if (sprintf(service, "%d", port) < 0)
		goto sock_connect_exit;
	/* Resolve DNS address, use sockfd as temp storage */
	sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
	if (sockfd < 0) {
		LOG_ERROR_PRINTF("%s for %s:%d \n", gai_strerror(sockfd), servername, port);
		goto sock_connect_exit;
	}
	/* Search through results and find the one we want */
	for (iterator = resolved_addr; iterator; iterator = iterator->ai_next) {
		sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
		if (sockfd >= 0) {
			if (servername) {
				/* Client mode. Initiate connection to remote */
				if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen))) {
					LOG_ERROR("failed connect");
					close(sockfd);
					sockfd = -1;
				}
			} else {
				/* Server mode. Set up listening socket an accept a connection */
				listenfd = sockfd;
				sockfd = -1;
				setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int));
				if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
					goto sock_connect_exit;
				listen(listenfd, 1);
				sockfd = accept(listenfd, NULL, 0);
			}
		}
	}
sock_connect_exit:
	if (listenfd)
		close(listenfd);
	if (resolved_addr)
		freeaddrinfo(resolved_addr);
	if (sockfd < 0) {
		if (servername){
			LOG_ERROR_PRINTF("Couldn't connect to %s:%d \n", servername, port);
		}
		else {
			perror("server accept");
			LOG_ERROR_PRINTF("accept() failed \n");
		}
	}
	return sockfd;
}

int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data)
{
	int rc;
	int read_bytes = 0;
	int total_read_bytes = 0;
	rc = write(sock, local_data, xfer_size);
	if (rc < xfer_size){
		LOG_ERROR_PRINTF("Failed writing data during sock_sync_data \n");
	}
	else
		rc = 0;
	while (!rc && total_read_bytes < xfer_size)
	{
		read_bytes = read(sock, remote_data, xfer_size);
		if (read_bytes > 0)
			total_read_bytes += read_bytes;
		else
			rc = read_bytes;
	}
	return rc;
}

int sock_init(struct config_t *val){
	int rc = 0;
	struct config_t config = *val;
	/* if client side */
	if (config.server_name)
	{
		val->sock = sock_connect(config.server_name, config.tcp_port);
		if (val->sock < 0)
		{
			LOG_ERROR_PRINTF("failed to establish TCP connection to server %s, port %d \n",
					config.server_name, config.tcp_port);
			rc = -1;
		}
	}
	else
	{
		LOG_INFO_PRINTF("waiting on port %d for TCP connection \n", config.tcp_port);
		val->sock = sock_connect(NULL, config.tcp_port);
		if (val->sock < 0)
		{
			LOG_ERROR_PRINTF("failed to establish TCP connection with client on port %d \n",
					config.tcp_port);
			rc = -1;
		}
	}
	LOG_INFO("TCP connection with rmeote process was established");	
	return rc;
}

int exchange_meta_nic(int sock, struct ConnInfo *conn_info){
	struct cm_con_data_t local_con_data;
	struct cm_con_data_t remote_con_data;
	struct cm_con_data_t tmp_con_data;
	int rc = 0;
	char temp_char;
	union ibv_gid src_gid;

	/* exchange using TCP sockets info required*/
	// local_con_data.addr = htonll((uintptr_t)buf);
	// local_con_data.rkey = htonl(rkey);
	// local_con_data.qp_num = htonl(qp_num);
	local_con_data.lid = htons(conn_info->host_lid.src_lid);
	memcpy(local_con_data.gid, &conn_info->conn_id.conn_key.host_key.src_gid, 16);
	if (sock_sync_data(sock, sizeof(struct cm_con_data_t), (char *)&local_con_data, (char *)&tmp_con_data) < 0)
	{
		LOG_ERROR("failed to exchange nic data between sides");
		rc = 1;
		return rc;
	}
	// remote_con_data.addr = ntohll(tmp_con_data.addr);
	// remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	// remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	remote_con_data.lid = ntohs(tmp_con_data.lid);
	memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
	
	/* save the remote side attributes, we will need it for the post SR */
	conn_info->host_lid.dst_lid = remote_con_data.lid; 
	memcpy(&conn_info->conn_id.conn_key.host_key.dst_gid, tmp_con_data.gid, 16);

	return rc;
}

int exchange_meta_conn(int sock, struct ConnInfo *conn_info, struct LocalMem *local_mem, struct RemoteMem *remote_mem){
	struct cm_con_data_t local_con_data;
	struct cm_con_data_t remote_con_data;
	struct cm_con_data_t tmp_con_data;
	int rc = 0;
	char temp_char;
	union ibv_gid src_gid;

	/* exchange using TCP sockets info required*/
	local_con_data.addr = htonll((uintptr_t)local_mem->router_addr);
	local_con_data.rkey = htonl(local_mem->rkey);
	local_con_data.qp_num = htonl(conn_info->conn_id.conn_key.src_qpn);
	// local_con_data.lid = htons(conn_info->host_lid.src_lid);
	// memcpy(local_con_data.gid, &conn_info->conn_id.conn_key.host_key.src_gid, 16);
	if (sock_sync_data(sock, sizeof(struct cm_con_data_t), (char *)&local_con_data, (char *)&tmp_con_data) < 0)
	{
		LOG_ERROR("failed to exchange nic data between sides");
		rc = 1;
		return rc;
	}
	remote_con_data.addr = ntohll(tmp_con_data.addr);
	remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	// remote_con_data.lid = ntohs(tmp_con_data.lid);
	// memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
	
	/* save the remote side attributes, we will need it for the post SR */
	remote_mem->conn_id = &conn_info->conn_id;

	remote_mem->router_addr = (void *)remote_con_data.addr;
	remote_mem->length = local_mem->length;
	remote_mem->rkey = remote_con_data.rkey;
	// conn_info->host_lid.dst_lid = remote_con_data.lid; 
	// memcpy(&conn_info->conn_id.conn_key.host_key.dst_gid, tmp_con_data.gid, 16);

	conn_info->conn_id.conn_key.dst_qpn = remote_con_data.qp_num;

	return rc;
}
/******************************************************************************
End of socket operations
******************************************************************************/

/******************************************************************************
User interface of scalelib
******************************************************************************/
int init_res(struct ConnInfo *conn_info){
	// req body is NULL
	void* req_body = NULL;
	// zero init conn_info
	memset(conn_info, 0, sizeof(struct ConnInfo));

	// rep body is ConnInfo
	struct INIT_RES_RSP res_rsp;
	int rsp_size = sizeof(struct INIT_RES_RSP);

	int rc = request_router(INIT_RES, req_body, (void *)&res_rsp, &rsp_size);

	// fill src_gid, src_lid
	*conn_info = res_rsp.conn_info;

	return rc;
}

int create_res(struct ConnId *conn_id){
	// req body is HostKey
	struct CREATE_RES_REQ res_req;
	res_req.host_key = conn_id->conn_key.host_key;

	// rsp body is ConnId
	struct CREATE_RES_RSP res_rsp;
	int rsp_size = sizeof(struct CREATE_RES_RSP);

	int rc = request_router(CREATE_RES, &res_req, (void *)&res_rsp, &rsp_size);

	// fill src_qpn
	conn_id->conn_key.src_qpn = res_rsp.conn_id.conn_key.src_qpn;

	return rc;
}

int reg_mr(struct ConnId *conn_id, struct LocalMem *local_mem, size_t length){
	/* fill LocalMem */
	// zero init local_mem
	memset(local_mem, 0, sizeof(struct LocalMem));
	local_mem->conn_id = conn_id;
	local_mem->client_addr = memalign(PAGE_SIZE, length);  // allocate page aligned memory for client 
	local_mem->length = length;

	// req body is ConnId and LocalMem
	struct REG_MR_REQ mr_req;
	mr_req.conn_id = *conn_id;
	mr_req.local_mem = *local_mem;

	// rsp body is LocalMem
	struct REG_MR_RSP mr_rsp;
	int rsp_size = sizeof(struct REG_MR_RSP);

	int rc = request_router(REG_MR, &mr_req, (void *)&mr_rsp, &rsp_size);

	/** mounting shared memory from router */
	int shm_fd, ret;
	void *shm_ptr;
	shm_fd = shm_open(mr_rsp.local_mem.shm_name, O_CREAT | O_RDWR, 0666);
	ret = ftruncate(shm_fd, length);

	char *membuff = (char *)malloc(length);
	memcpy(membuff, local_mem->client_addr, length);

	// verify client addr is page aligned or not
    int is_align = (long)local_mem->client_addr % (4 * 1024) == 0 ? 1 : 0;

    if (is_align)
		shm_ptr = mmap(local_mem->client_addr, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_LOCKED, shm_fd, 0); 
	else
	{
		LOG_ERROR("client_addr is not page aligned");
		shm_ptr = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, shm_fd, 0); 
	}

	if (shm_ptr == MAP_FAILED || ret > 0){
		LOG_TRACE("mmap failed in REG_MR.");
		// if(LOG_LEVEL <= 1) fflush(stdout);
	}
	else
	{
		memcpy(local_mem->client_addr, membuff, length);
		LOG_TRACE("mmap succeed in reg mr.");

		// LOG_TRACE_PRINTF("lkey=%u, addr=%lu, shm_prt=%lu\n", mr_rsp.local_mem.lkey, (uint64_t)local_mem->client_addr, (uint64_t)shm_ptr);
		LOG_TRACE("client_addr: " << local_mem->client_addr << ", mmaped_addr: " << shm_ptr);
		LOG_TRACE("lkey | rkey: " << mr_rsp.local_mem.lkey << " | " << mr_rsp.local_mem.rkey);
		// if(LOG_LEVEL <= 1) fflush(stdout);
	}
	free(membuff);

	/* fill LocalMem with pointer to router_addr, shm_name, shm_fd, pointer to ibv_mr, lkey, rkey */
	local_mem->router_addr = mr_rsp.local_mem.router_addr;
	local_mem->shm_piece = mr_rsp.local_mem.shm_piece;
	strcpy(local_mem->shm_name, mr_rsp.local_mem.shm_name);
	local_mem->shm_fd = shm_fd;
	local_mem->mr = mr_rsp.local_mem.mr;
	local_mem->lkey = mr_rsp.local_mem.lkey;
	local_mem->rkey = mr_rsp.local_mem.rkey;

	return rc;
}

int setup_conn(struct ConnInfo *conn_info, struct ConnIdx *conn_idx){
	// req body is conn_info --- conn_id --- conn_key
	struct SETUP_CONN_REQ conn_req;
	conn_req.conn_info = *conn_info;

	// rsp body is QP stauts
	struct SETUP_CONN_RSP conn_rsp;
	int rsp_size = sizeof(struct SETUP_CONN_RSP);

	int rc = request_router(SETUP_CONN, &conn_req, (void *)&conn_rsp, &rsp_size);

	if(conn_rsp.status != 4){
		LOG_ERROR("The QP status of target conn_id is not RTS " << conn_rsp.status)
	}
	// LOG_TRACE("The QP status of target conn_id is  " << conn_rsp.status);

	/* fill ConnId with process_id */
	conn_info->conn_id.process_id = conn_rsp.process_id;
	/* fill conn_idx */
	*conn_idx = conn_rsp.conn_idx;

	/* setup conn resources */
	if(!scalelib.setup_res_conn(conn_idx)){
		LOG_TRACE("setup conn resources success!");
	}

	/* setup ctrl shm comm */
	if(!scalelib.setup_shm_comm(conn_idx)){
		LOG_TRACE("setup ctrl shm comm success!");
	}

	return rc;
}

/** Support both UDS and SHM */
int post_send(struct ConnIdx *conn_idx, struct LocalMem *local_mem, struct RemoteMem *remote_mem, size_t length, int opcode){
	int rc = 0;

	// set SHM
	struct conn_resources     *conn = scalelib.conn_map[conn_idx->qp_index];
	struct CtrlShmPiece       *csp = conn->vqp_shm_map[conn_idx->vq_index];
    pthread_mutex_t           *csp_mtx = &(conn->vqp_shm_mtx_map[conn_idx->vq_index]);
	struct ScaleReqHeader     *header = (struct ScaleReqHeader*)(csp->req);
	struct POST_SEND_REQ      *req = (struct POST_SEND_REQ*)(csp->req + sizeof(struct ScaleReqHeader));
	struct POST_SEND_RSP      *rsp = (struct POST_SEND_RSP*)(csp->rsp);

	// enter critical section
	// fill in REQ HEADER
	header->process_id = scalelib.process_id;
	header->func = POST_SEND;
	header->body_size = sizeof(struct POST_SEND_REQ);
	// fill in REQ
	pthread_mutex_lock(csp_mtx);
	memcpy(&req->conn_idx, conn_idx, sizeof(struct ConnIdx));
	// req->wr_id = conn_idx->process_id;
	req->wr_id = conn_idx->vq_index;
	req->local_addr = local_mem->router_addr;
	req->length = length;
	req->lkey = local_mem->lkey;
	req->opcode = opcode;

	// fill in RSP size
	int rsp_size = sizeof(struct POST_SEND_RSP);

	if(opcode != IBV_WR_SEND){
		if(!remote_mem){
			LOG_ERROR("One-sided RDMA OP, however remote mem is not filled");
		}
		req->remote_addr = remote_mem->router_addr;
		req->rkey = remote_mem->rkey;
	}

	uint8_t disable_fastpath = 0;
	if (!getenv("DISABLE_FASTPATH"))
	{
		disable_fastpath = 1; // default disable fastpath without ENV_VAR
	}
	else
	{
		disable_fastpath = atoi(getenv("DISABLE_FASTPATH"));
	}

	if (disable_fastpath)
	{
		LOG_TRACE_PRINTF("Request by UDS: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		rc = request_router(POST_SEND, (void *)req, (void *)rsp, &rsp_size);
	}
	else
	{
		LOG_TRACE_PRINTF("Request by SHM: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		request_router_shm(csp);
	}		

	if(req->wr_id != rsp->wr_id){
		LOG_ERROR("POST_SEND error may happen");
		rc = -1;
	}

	wmb();
	csp->state = IDLE;
	pthread_mutex_unlock(csp_mtx);

	return rc;
}

/** only support UDS */
/******************************************************************************
int post_send(struct ConnIdx *conn_idx, struct LocalMem *local_mem, struct RemoteMem *remote_mem, size_t length, int opcode){
	int rc;

	// req body is conn_id, local & remote (optional) addr info
	struct POST_SEND_REQ send_req;
	send_req.conn_idx = *conn_idx;
	send_req.wr_id = conn_idx->process_id;
	send_req.local_addr = local_mem->router_addr;
	send_req.length = length;
	send_req.lkey = local_mem->lkey;
	send_req.opcode = opcode;

	if(opcode != IBV_WR_SEND){
		if(!remote_mem){
			LOG_ERROR("One-sided RDMA OP, however remote mem is not filled");
		}
		send_req.remote_addr = remote_mem->router_addr;
		send_req.rkey = remote_mem->rkey;
	} 

	// rsp body is wr_id, set as process_id currently
	struct POST_SEND_RSP send_rsp;
	int rsp_size = sizeof(struct POST_SEND_RSP);

	rc = request_router(POST_SEND, &send_req, (void *)&send_rsp, &rsp_size);

	if(send_req.wr_id != send_rsp.wr_id){
		LOG_ERROR("POST_SEND error may happen");
	}

	return rc;
}
******************************************************************************/

/** Support both UDS and SHM */
int post_recv(struct ConnIdx *conn_idx, struct LocalMem *local_mem, size_t length){
	int rc = 0;

	// set SHM
	struct conn_resources     *conn = scalelib.conn_map[conn_idx->qp_index];
	struct CtrlShmPiece       *csp = conn->vqp_shm_map[conn_idx->vq_index];
    pthread_mutex_t           *csp_mtx = &(conn->vqp_shm_mtx_map[conn_idx->vq_index]);
	struct ScaleReqHeader     *header = (struct ScaleReqHeader*)(csp->req);
	struct POST_RECV_REQ      *req = (struct POST_RECV_REQ*)(csp->req + sizeof(struct ScaleReqHeader));
	struct POST_RECV_RSP      *rsp = (struct POST_RECV_RSP*)(csp->rsp);

	/* enter critical section */
	pthread_mutex_lock(csp_mtx);
	// fill in REQ HEADER
	header->process_id = scalelib.process_id;
	header->func = POST_RECV;
	header->body_size = sizeof(struct POST_RECV_REQ);
	// fill in REQ BODY
	memcpy(&req->conn_idx, conn_idx, sizeof(struct ConnIdx));
	// req->wr_id = conn_idx->process_id;
	req->wr_id = conn_idx->vq_index;
	req->local_addr = local_mem->router_addr;
	req->length = length;
	req->lkey = local_mem->lkey;

	// fill in RSP size
	int rsp_size = sizeof(POST_RECV_RSP);

	uint8_t disable_fastpath = 0;
	if (!getenv("DISABLE_FASTPATH"))
	{
		disable_fastpath = 1; // default disable fastpath without ENV_VAR
	}
	else
	{
		disable_fastpath = atoi(getenv("DISABLE_FASTPATH"));
	}

	if (disable_fastpath)
	{	
		LOG_TRACE_PRINTF("Request by UDS: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		rc = request_router(POST_RECV, (void *)req, (void *)rsp, &rsp_size);
	}
	else{
		LOG_TRACE_PRINTF("Request by SHM: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		request_router_shm(csp);
	}
		
	if(req->wr_id != rsp->wr_id){
		LOG_ERROR("POST_RECV error may happen");
		rc = -1;
	}

	wmb();
	csp->state = IDLE;
	pthread_mutex_unlock(csp_mtx);
	/* exit critical section */

	return rc;
}

/** only support UDS */
/******************************************************************************
int post_recv(struct ConnIdx *conn_idx, struct LocalMem *local_mem, size_t length){
	int rc;

	// req body is conn_id, local addr info
	struct POST_RECV_REQ recv_req;
	recv_req.conn_idx = *conn_idx;
	recv_req.wr_id = conn_idx->process_id;
	recv_req.local_addr = local_mem->router_addr;
	recv_req.length = length;
	recv_req.lkey = local_mem->lkey;

	// rsp body is wr_id, set as process_id currently
	struct POST_RECV_RSP recv_rsp;
	int rsp_size = sizeof(struct POST_RECV_RSP);

	rc = request_router(POST_RECV, &recv_req, (void *)&recv_rsp, &rsp_size);

	if(recv_req.wr_id != recv_rsp.wr_id){
		LOG_ERROR("POST_RECV error may happen");
	}

	return rc;
}
******************************************************************************/

/** Support both UDS and SHM */
int poll_cq(struct ConnIdx *conn_idx, int count){
	int rc = 0;

	// set SHM
	struct conn_resources     *conn = scalelib.conn_map[conn_idx->qp_index];
	struct CtrlShmPiece       *csp = conn->vcq_shm_map[conn_idx->vq_index];
    pthread_mutex_t           *csp_mtx = &(conn->vcq_shm_mtx_map[conn_idx->vq_index]);
	struct ScaleReqHeader     *header = (struct ScaleReqHeader*)(csp->req);
	struct POLL_CQ_REQ      *req = (struct POLL_CQ_REQ*)(csp->req + sizeof(struct ScaleReqHeader));
	struct POLL_CQ_RSP      *rsp = (struct POLL_CQ_RSP*)(csp->rsp);

	/* enter critical section */
	pthread_mutex_lock(csp_mtx);
	// fill in REQ HEADER
	header->process_id = scalelib.process_id;
	header->func = POLL_CQ;
	header->body_size = sizeof(struct POLL_CQ_REQ);
	// fill in REQ BODY
	memcpy(&req->conn_idx, conn_idx, sizeof(struct ConnIdx));
	// req->wr_id = conn_idx->process_id;
	req->wr_id = conn_idx->vq_index;
	req->count = count;

	// fill in RSP size
	int rsp_size = sizeof(POLL_CQ_RSP);

	uint8_t disable_fastpath = 0;
	if (!getenv("DISABLE_FASTPATH"))
	{
		disable_fastpath = 1; // default disable fastpath without ENV_VAR
	}
	else
	{
		disable_fastpath = atoi(getenv("DISABLE_FASTPATH"));
	}

	if (disable_fastpath)
	{	
		LOG_TRACE_PRINTF("Request by UDS: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		rc = request_router(POLL_CQ, (void *)req, (void *)rsp, &rsp_size);
	}
	else{
		LOG_TRACE_PRINTF("Request by SHM: qp_indx=%u, vq_index=%u, process_id=%d\n", conn_idx->qp_index, conn_idx->vq_index, conn_idx->process_id);
		request_router_shm(csp);
	}

	if(req->wr_id != rsp->wr_id || rsp->count < 0){
		LOG_ERROR("POLL_CQ error may happen");
	}

	rc = rsp->count;

	wmb();
	csp->state = IDLE;
	pthread_mutex_unlock(csp_mtx);
	/* exit critical section */
	
	return rc;
}

/** only support UDS */
/******************************************************************************
int poll_cq(struct ConnIdx *conn_idx, int count){
	int rc;

	// req body is conn_id
	struct POLL_CQ_REQ poll_req;
	poll_req.conn_idx = *conn_idx;
	poll_req.wr_id = conn_idx->process_id;
	poll_req.count = count;

	// rsp body is wr_id & cout of CQE
	struct POLL_CQ_RSP poll_rsp;
	int rsp_size = sizeof(struct POLL_CQ_RSP);

	rc = request_router(POLL_CQ, &poll_req, (void *)&poll_rsp, &rsp_size);

	if(poll_req.wr_id != poll_rsp.wr_id || poll_rsp.count < 0){
		LOG_ERROR("POLL_CQ error may happen");
	}

	rc = poll_rsp.count;

	return rc;
}
******************************************************************************/

int dereg_mr(struct ConnId *conn_id, struct LocalMem *local_mem){
	int rc;

	/* try to release client mem */
	// verify if client_addr mmaped and shm_fd opened
	if(local_mem->client_addr && local_mem->shm_fd != 0){
		// whether close shm_fd at client? yes!
		if(close(local_mem->shm_fd)){
			LOG_ERROR("Error close shm_fd: " << local_mem->shm_fd);
		}
		else{
			LOG_TRACE("Success close shm_fd: " << local_mem->shm_fd);
		}

		// whether munmap client_addr is required? yes!
		if(munmap(local_mem->client_addr, local_mem->length)){
			LOG_ERROR("Error munmap client_addr: " << local_mem->client_addr);
		}
		else{
			LOG_TRACE("munmap client_addr success");
		}

		/* whether unlink shm_name at client? */
		// unlink twice will cause error, move shm_unlink to router side
		// if(shm_unlink(local_mem->shm_name)){
		// 	LOG_ERROR("shm_unlink failed on shm_name: " << local_mem->shm_name);
		// }
		// else{
		// 	LOG_TRACE("shm_unlink success on shm_name: " << local_mem->shm_name);
		// }
	}

	// verify if client_addr allocated in advance
	if(local_mem->client_addr){
		// whether free again is required after munmap? yes!
		free(local_mem->client_addr);
		LOG_TRACE("free client_addr");
	}

	/* try to dereg_mr, release router_addr */
	// req body is conn_id, local_mem
	struct DEREG_MR_REQ mr_req;
	mr_req.conn_id = *conn_id;
	mr_req.local_mem = *local_mem;
	
	// rsp body is local_mem
	struct DEREG_MR_RSP mr_rsp;
	int rsp_size = sizeof(struct DEREG_MR_RSP);

	rc = request_router(DEREG_MR, (void *)&mr_req, (void *)&mr_rsp, &rsp_size);

	// fill rsp body
	*local_mem = mr_rsp.local_mem;

	return rc;
}

int destroy_res(struct ConnId *conn_id, struct ConnIdx *conn_idx){
	int rc;

	// req body is conn_id
	struct DESTROY_RES_REQ res_req;
	res_req.conn_id = *conn_id;
	res_req.conn_idx = *conn_idx;

	// rsp body is conn_id
	struct DESTROY_RES_RSP res_rsp;
	int rsp_size = sizeof(struct DESTROY_RES_RSP);

	rc = request_router(DESTROY_RES, (void *)&res_req, (void *)&res_rsp, &rsp_size);

	// fill rsp body
	*conn_id = res_rsp.conn_id;

	/* release ctrl shm comm */
	if(!scalelib.release_shm_comm(conn_idx)){
		LOG_TRACE("release ctrl shm comm success!");
	}

	/* release conn resources */
	if(!scalelib.release_res_conn(conn_idx)){
		LOG_TRACE("release conn resources success!");
	}

	return rc;
}
/******************************************************************************
End of scalelib
******************************************************************************/

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

	if(LOG_LEVEL <= 1) fflush(stdout);
}

void print_conn_id(struct ConnId *val){
	struct ConnId conn_id = *val;
	LOG_TRACE("------------------------------[conn_id]------------------------------");
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
	LOG_TRACE("------------------------------[conn_id]------------------------------");

	if(LOG_LEVEL <= 1) fflush(stdout);
}

void print_conn_idx(struct ConnIdx *val){
	struct ConnIdx conn_idx = *val;
	LOG_TRACE("------------------------------[conn_idx]------------------------------");
	LOG_TRACE("process_id: " << conn_idx.process_id);
	LOG_TRACE("QP index = " << conn_idx.qp_index << ", VQ index = " << conn_idx.vq_index);
	LOG_TRACE("------------------------------[conn_idx]------------------------------");
}

void print_mem_item(struct LocalMem *val){
	struct LocalMem local_mem = *val;

	LOG_TRACE("------------------[local_mem]------------------");
	LOG_TRACE("client_addr = " << local_mem.client_addr << ", router_addr = " << local_mem.router_addr << ", size = " << local_mem.length);
	LOG_TRACE("lkey = " << local_mem.lkey << ",  rkey = " << local_mem.rkey);
	// LOG_TRACE_PRINTF("client_addr=%lu, router_addr=%lu, size=%u \n", (uint64_t)local_mem.client_addr, (uint64_t)local_mem.router_addr, local_mem.length);
	// LOG_TRACE_PRINTF("lkey=%u, rkey=%u \n", local_mem.lkey, local_mem.rkey);
	LOG_TRACE("------------------[local_mem]------------------");

	if(LOG_LEVEL <= 1) fflush(stdout);
}