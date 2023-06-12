#include <unistd.h>
#include <sys/time.h>
#include "scalelib.h"
extern "C"{
    #include "get_clock.h"
}
static void usage(const char *argv0)
{
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, " %s start a server and wait for connection\n", argv0);
	fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
	fprintf(stdout, "\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
    fprintf(stdout, " -f, --free <1/0>  whether free physical QP resource (default true)\n");
}

struct config_t config = {
	NULL, /* server_name */
	PORT,    /* tcp_port */
    -1,          /* sock */
    1,           /* free */
	};

static int poll_completion(struct ConnIdx *conn_idx)
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
        LOG_INFO("POLL_CQ");
		poll_result = poll_cq(conn_idx, 1);
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
		LOG_TRACE("completion was found in CQ with wr_id: " << conn_idx->process_id);
	}
	return rc;
}

int main (int argc, char **argv) {

    cycles_t 	end_cycle, start_cycle, delta, end_cycle1, start_cycle1, delta1, end_cycle2, start_cycle2, delta2, middle_cycle, del;

    double cycles_to_units = get_cpu_mhz(0);
    /* parse the command line parameters */
	while (1) {
		int c;
		struct option long_options[] = {
			{name : "port", has_arg : 1, flag : NULL, val : 'p'},
            {name : "free", has_arg : 0, flag : NULL, val : 'f'},
			{name : NULL, has_arg : 0, flag : NULL, val : '\0'}
		};
		c = getopt_long(argc, argv, "p:f:", long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
			case 'p':
				config.tcp_port = strtoul(optarg, NULL, 0);
				break;
            case 'f':
				config.free = strtoul(optarg, NULL, 0);
				break;
			default:
				usage(argv[0]);
				return 1;
		}
	}
    /* parse the last parameter (if exists) as the server name */
	if (optind == argc - 1)
		config.server_name = argv[optind];
    if(config.server_name){
       LOG_INFO_PRINTF("servername=%s\n",config.server_name);
    }
	else if (optind < argc)
	{
		usage(argv[0]);
		return 1;
	}

    int rc = 0;
    char temp_char[2] = {};

    LOG_INFO("Outband TCP Connecting...");
    rc = sock_init(&config);
    if(rc){
        LOG_ERROR("failed to build outband TCP connection");
        return rc;
    }

    LOG_INFO("ScaleClient Starting...");
    /** abstract of QP connection */
    struct ConnInfo conn_info;
    /** qp & vqp index for datapath operation **/
    struct ConnIdx conn_idx;
    /** abstract of local&remote MR */
    struct LocalMem local_mem;
    struct RemoteMem remote_mem;

    /** init NIC res, gid, lid */
    LOG_INFO("INIT_RES");
    rc = init_res(&conn_info);
    if(rc){
        LOG_ERROR("failed to INIT_RES");
        return rc;
    }

    /** exchange meta data: gid, lid */
    rc = exchange_meta_nic(config.sock, &conn_info);
    if(rc){
        LOG_ERROR("failed to exchage_meta_nic");
        return rc;
    }
    print_conn_info(&conn_info);

    /** create QP res */
    LOG_INFO("CREATE_RES");
    rc = create_res(&conn_info.conn_id);
    if(rc){
        LOG_ERROR("failed to CREATE_RES");
        goto exit;
    }

    /** register MR item: conn_id, local_mem, size */
    LOG_INFO("REG_MR");
    rc = reg_mr(&conn_info.conn_id, &local_mem, 4096);
    if(rc){
        LOG_ERROR("failed to REG_MR");
        goto exit;
    }
    print_mem_item(&local_mem);

    /** exchange meta data: qpn, addr, rkey, etc.*/
    rc = exchange_meta_conn(config.sock, &conn_info, &local_mem, &remote_mem);
    if(rc){
        LOG_ERROR("failed to exchage_meta_conn");
        goto exit;
    }

    /** setup pQP conn if not ready, update vQP index for current process*/
    LOG_INFO("SETUP_CONN");
    rc = setup_conn(&conn_info, &conn_idx);    
    print_conn_id(&conn_info.conn_id);
    print_conn_idx(&conn_idx);

    /** two-sided communication */
    if(!config.server_name){
        // client post_recv in advance
        LOG_INFO("POST_RECV");
        rc = post_recv(&conn_idx, &local_mem, MSG_SIZE);
        if(rc){
            LOG_ERROR("POST_RECV failed at client side");
            goto exit;
        }
    }
    else{
        // server prepare buf
        strcpy((char *)local_mem.client_addr, MSG); 
    }
    // ensure post_recv is done!
    rc = sock_sync_data(config.sock, 1, (char *)"T", temp_char);
    if(rc){
        LOG_ERROR("SYNC failed between client-server: "<< temp_char);
    }
    else{
        LOG_TRACE_PRINTF("sync temp_char: %s\n", temp_char);
    }

    start_cycle = get_cycles();
    if(config.server_name){
        // server post_send
        LOG_INFO("POST_SEND");
        rc = post_send(&conn_idx, &local_mem, NULL, MSG_SIZE, IBV_WR_SEND);
        if(rc){
            LOG_ERROR("POST_SEND failed at client side");
            goto exit;
        }
    }

    // both side poll completion
    rc = poll_completion(&conn_idx);
    if(rc < 0){
        LOG_ERROR("POLL_CQ failed");
        goto exit;
    }
    end_cycle = get_cycles();
    delta = end_cycle - start_cycle;
    printf("post send takes %-7.2f usec \n",delta/cycles_to_units);
    if (config.server_name)
    {
        LOG_TRACE_PRINTF("[client] SEND message is: '%s'\n", (char *)local_mem.client_addr);
    }
	else
	{
        LOG_TRACE_PRINTF("[server] RECV message is: '%s'\n", (char *)local_mem.client_addr);
		/* setup server buffer with read message */
		strcpy((char *)local_mem.client_addr, RDMAMSGR);
	}

    // ensure server buffer is ready before client READ!
    rc = sock_sync_data(config.sock, 1, (char *)"R", temp_char);
    if(rc){
        LOG_ERROR("SYNC failed between client-server: "<< temp_char);
    }
    else{
        LOG_TRACE_PRINTF("sync temp_char: %s\n", &temp_char);
    }

    start_cycle1 = get_cycles();
    /** one-sided communication */
	if (config.server_name)
	{
		/* First we read contens of server's buffer */
        rc = post_send(&conn_idx, &local_mem, &remote_mem, MSG_SIZE, IBV_WR_RDMA_READ);
		if (rc)
		{
			LOG_ERROR("failed to post SR");
			rc = 1;
			goto exit;
		}
		if (poll_completion(&conn_idx))
		{
			LOG_ERROR("poll completion failed");
			rc = 1;
			goto exit;
		}
        end_cycle1 = get_cycles();
        delta1 = end_cycle1 - start_cycle1;
        printf("rdma read takes %-7.2f usec \n",delta1/cycles_to_units);
		LOG_TRACE_PRINTF("[client] READ contents of server's buffer: '%s'\n", local_mem.client_addr);
		/* Now we replace what's in the server's buffer */
		strcpy((char *)local_mem.client_addr, RDMAMSGW);
		LOG_TRACE_PRINTF("[client] Now replacing it with: '%s'\n", local_mem.client_addr);
        start_cycle2 = get_cycles();
        rc = post_send(&conn_idx, &local_mem, &remote_mem, MSG_SIZE, IBV_WR_RDMA_WRITE);
		if (rc)
		{
			LOG_ERROR("failed to post SR");
			rc = 1;
			goto exit;
		}
		if (poll_completion(&conn_idx))
		{
			LOG_ERROR("poll completion failed");
			rc = 1;
			goto exit;
		}
        end_cycle2 = get_cycles();
        delta2 = end_cycle2 - start_cycle2;
        printf("rdma write takes %-7.2f usec \n",delta2/cycles_to_units);
	}
	/* Sync so server will know that client is done mucking with its memory */
	if (sock_sync_data(config.sock, 1, (char *)"W", temp_char)) /* just send a dummy char back and forth */
	{
		fprintf(stderr, "sync error after RDMA ops\n");
		rc = 1;
		goto exit;
	}
    else{
        LOG_TRACE_PRINTF("sync temp_char: %s\n", temp_char);
    }

	if (!config.server_name){
        LOG_TRACE_PRINTF("[server] WRITE Contents of server buffer: '%s'\n", local_mem.client_addr);
    }

    if(LOG_LEVEL <= 3) fflush(stdout);

exit:    
    /** dereg MR item */
    rc = dereg_mr(&conn_info.conn_id, &local_mem);
    if(rc){
        LOG_ERROR("failed to REG_MR");
    }
    print_mem_item(&local_mem);

    /** destroy QP res */
    if(config.free){
        rc = destroy_res(&conn_info.conn_id, &conn_idx);
        if(rc){
            LOG_ERROR("failed to DESTROY_RES");
        }
    }
    print_conn_id(&conn_info.conn_id);

    /** close sock conn with remote process */
    if(config.sock > 0){
        rc = close(config.sock);
        if(rc){
            LOG_ERROR("faile to close sock");
        }
    }

    return rc;
}