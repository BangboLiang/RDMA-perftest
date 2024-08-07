/*
* BUILD COMMAND:
* gcc -Wall -O2 -o MyPerfv5 MyPerfv5.c -libverbs
*/
/******************************************************************************
*Version 5: Support RDMA read and RDMA write
* RDMA Aware Networks Programming Perftest
*
*
*****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "SEND operation "
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE 4096
#define PRINT_LOG 1

/*Change host data structure 2 network data structure due to byte order*/
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return x;
}
static inline uint64_t ntohll(uint64_t x)
{
    return x;
}
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif


/* structure of test parameters */
struct config_t
{
    const char *dev_name; /* IB device name */
    char *server_name;    /* server host name */
    uint32_t tcp_port;    /* server TCP port */
    int ib_port;          /* local IB port to work with */
    int gid_idx;          /* gid index to use */
    int test_interval_us; /* test interval in us*/
    int test_opcode;      /*2-send 4-read 0-write*/
    int test_times;       /* test times*/
    int iter_nums;         /*Number of iterations per test round.*/
};

struct timeval t1;
struct timeval t2;
long timer;

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
    uint64_t addr;        /* Buffer address */
    uint32_t rkey;        /* Remote key */
    uint32_t qp_num;      /* QP number */
    uint16_t lid;         /* LID of the IB port */
    uint8_t gid[16];      /* gid */
} __attribute__((packed));

/* structure of IB system resources */
struct resources
{
    struct ibv_device_attr device_attr; /* Device attributes */
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct cm_con_data_t remote_props;  /* values to connect to remote side */
    struct ibv_context *ib_ctx;         /* device handle */
    struct ibv_pd *pd;                  /* PD handle */
    struct ibv_cq *cq;                  /* CQ handle */
    struct ibv_qp *qp;                  /* QP handle */
    struct ibv_mr *mr;                  /* MR handle for buf */
    char *buf;                          /* memory buffer pointer, used for RDMA and send ops */
    int sock;                           /* TCP socket file descriptor */
};

struct config_t config =
{
    NULL,  /* dev_name */
    NULL,  /* server_name */
    19875, /* tcp_port */
    1,     /* ib_port */
    -1,    /* gid_idx */
    1000,   /* test_interval_us */
    2,      /*test_opcode*/
    2,      /*test times*/
    50      /*iter_tim*/
};


/******************************************************************************
Socket operations:
For simplicity, the perf program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this perftest.
******************************************************************************/
/******************************************************************************
* Function: sock_connect
* Input:
* servername: URL of server to connect to (NULL for server mode)
* port: port of service
*
* Output:none
*
* Returns: socket (fd) on success, negative error code on failure
*
* Description:
* Connect a socket. If servername is specified a client connection will be
* initiated to the indicated server and port. Otherwise listen on the
* indicated port for an incoming connection.
*
******************************************************************************/
static int sock_connect(const char *servername, int port)
{
    struct addrinfo *resolved_addr = NULL;
    struct addrinfo *iterator;
    char service[6];
    int sockfd = -1;
    int listenfd = 0;
    int tmp;
    struct addrinfo hints =
    {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };

    if(sprintf(service, "%d", port) < 0)
    {
        goto sock_connect_exit;
    }

    /* Resolve DNS address, use sockfd as temp storage */
    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
    if(sockfd < 0)
    {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
        goto sock_connect_exit;
    }

    /* Search through results and find the one we want */
    for(iterator = resolved_addr; iterator ; iterator = iterator->ai_next)
    {
        sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
        if(sockfd >= 0)
        {
            if(servername)
			{
                /* Client mode. Initiate connection to remote */
                if((tmp=connect(sockfd, iterator->ai_addr, iterator->ai_addrlen)))
                {
                    fprintf(stderr, "failed connect \n");
                    close(sockfd);
                    sockfd = -1;
                }
			}
            else
            {
                /* Server mode. Set up listening socket an accept a connection */
                listenfd = sockfd;
                sockfd = -1;
                if(bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
                {
                    goto sock_connect_exit;
                }
                listen(listenfd, 1);
                sockfd = accept(listenfd, NULL, 0);
            }
        }
    }

sock_connect_exit:
    if(listenfd)
    {
        close(listenfd);
    }

    if(resolved_addr)
    {
        freeaddrinfo(resolved_addr);
    }

    if(sockfd < 0)
    {
        if(servername)
        {
            fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        }
        else
        {
            perror("server accept");
            fprintf(stderr, "accept() failed\n");
        }
    }

    return sockfd;
}

/******************************************************************************
* Function: sock_sync_data
* Input:
* sock: socket to transfer data on
* xfer_size: size of data to transfer
* local_data: pointer to data to be sent to remote
*
* Output: remote_data pointer to buffer to receive remote data
*
* Returns: 0 on success, negative error code on failure
*
* Description:
* Sync data across a socket. The indicated local data will be sent to the
* remote. It will then wait for the remote to send its data back. It is
* assumed that the two sides are in sync and call this function in the proper
* order. Chaos will ensue if they are not. :)
*
* Also note this is a blocking function and will wait for the full data to be
* received from the remote.
*
******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data)
{
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;
    rc = write(sock, local_data, xfer_size);

    if(rc < xfer_size)
    {
        fprintf(stderr, "Failed writing data during sock_sync_data\n");
    }
    else
    {
        rc = 0;
    }

    while(!rc && total_read_bytes < xfer_size)
    {
        read_bytes = read(sock, remote_data, xfer_size);
        if(read_bytes > 0)
        {
            total_read_bytes += read_bytes;
        }
        else
        {
            rc = read_bytes;
        }
    }
    return rc;
}

/******************************************************************************
End of socket operations
******************************************************************************/

/* poll_completion */
/******************************************************************************
* Function: poll_completion
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description:
* Poll the completion queue for a single event. This function will continue to
* poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
*
******************************************************************************/
static int poll_completion(struct resources *res)
{
    struct ibv_wc wc;
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
        poll_result = ibv_poll_cq(res->cq, 1, &wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    }
    while((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT ));

    if(poll_result < 0)
    {
        /* poll CQ failed */
        fprintf(stderr, "poll CQ failed\n");
        rc = 1;
    }
    else if(poll_result == 0)
    {
        /* the CQ is empty */
        fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
        rc = 1;
    }
    else
    {
        /* CQE found */
        if(PRINT_LOG)
            fprintf(stdout, "completion was found in CQ with status 0x%x\n", wc.status);
        /* check the completion status (here we don't care about the completion opcode */
        if(wc.status != IBV_WC_SUCCESS)
        {
            fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", 
					wc.status, wc.vendor_err);
            rc = 1;
        }
        // else
        // {   
        //     printf("shm:%ld contents:%s\n",res->mr->shm_ptr,res->mr->shm_ptr);
        //     /* code */
        // }
        
    }
    return rc;
}

/******************************************************************************
* Function: post_send
*
* Input:
* res: pointer to resources structure
* opcode: IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: This function will create and post a send work request
******************************************************************************/
static int post_send(struct resources *res, int opcode)
{
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;

    /* prepare the send work request */
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    sr.send_flags = IBV_SEND_SIGNALED;
    if(opcode != IBV_WR_SEND)
    {
        sr.wr.rdma.remote_addr = res->remote_props.addr;
        sr.wr.rdma.rkey = res->remote_props.rkey;
    }

    /* there is a Receive Request in the responder side, so we won't get any into RNR flow */
    rc = ibv_post_send(res->qp, &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post SR\n");
    }
    else
    {
        switch(opcode)
        {
        case IBV_WR_SEND:
            fprintf(stdout, "Send Request was posted\n");
            break;
        case IBV_WR_RDMA_READ:
            fprintf(stdout, "RDMA Read Request was posted\n");
            break;
        case IBV_WR_RDMA_WRITE:
            fprintf(stdout, "RDMA Write Request was posted\n");
            break;
        default:
            fprintf(stdout, "Unknown Request was posted\n");
            break;
        }
    }
    return rc;
}

/******************************************************************************
* Function: send_bandwidth_test
*
* Input:
* res: pointer to resources structure
* opcode: IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
* times: try times of bandwidth test
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: This function will create and post send work requests, and test performance
******************************************************************************/
static int send_bandwidth_test(struct resources *res, int opcode, int times, int iter, int interval_us){
    // struct ibv_send_wr sr;
    // struct ibv_sge sge;
    // struct ibv_send_wr *bad_wr = NULL;

    
    double start_time_usec;
    double cur_time_usec;
    struct timeval cur_time;
    double poll_time_cost;
    int poll_result;
    int total_poll_result;

    double bits_sent;
    struct timeval tt1, tt2;
    double send_time_usec;
    double total_send_time_usec;

    int rc;
    int num_tested;
    int message_sent;
    double speed_results[times];
    double time_results[times];
    char temp_char;

    /* prepare the scatter/gather entry */
    // memset(&sge, 0, sizeof(sge));
    // sge.addr = (uintptr_t)res->buf;
    // sge.length = MSG_SIZE;
    // sge.lkey = res->mr->lkey;

    /* prepare the send work request */
    // memset(&sr, 0, sizeof(sr));
    // sr.next = NULL;
    // sr.wr_id = 0;
    // sr.sg_list = &sge;
    // sr.num_sge = 1;
    // sr.opcode = opcode;
    // sr.send_flags = IBV_SEND_SIGNALED;

    // if(opcode != IBV_WR_SEND)
    // {
    //     sr.wr.rdma.remote_addr = res->remote_props.addr;
    //     sr.wr.rdma.rkey = res->remote_props.rkey;
    // }

    /*Ready to post send now*/
    printf("-----------------speed test-------------------\n");
    for (num_tested = 0; num_tested < times; num_tested++)
    {  
        bits_sent = 0;
        total_send_time_usec = 0;
        send_time_usec = 0;
        while (total_send_time_usec < interval_us){
            total_poll_result = 0;
            message_sent = 0;
            //printf("Tring to sync data.\n");
            //sock_sync_data(res->sock, 1, "Q", &temp_char);
            struct ibv_send_wr sr[iter];
            struct ibv_sge sge[iter];
            struct ibv_send_wr *bad_wr[iter];
            
            for(int i = 0;i < iter; i++){
                memset(&sge[i], 0, sizeof(sge[i]));
                sge[i].addr = (uintptr_t)res->buf;
                sge[i].length = MSG_SIZE;
                sge[i].lkey = res->mr->lkey;

                memset(&sr[i], 0, sizeof(sr[i]));
                sr[i].next = NULL;
                sr[i].wr_id = 0;
                sr[i].sg_list = &sge[i];
                sr[i].num_sge = 1;
                sr[i].opcode = opcode;
                if(opcode != IBV_WR_SEND)
                {   //read or write need remote addr and remote key.
                    sr[i].wr.rdma.remote_addr = res->remote_props.addr;
                    sr[i].wr.rdma.rkey = res->remote_props.rkey;
                }
                sr[i].send_flags = IBV_SEND_SIGNALED;
                bad_wr[i] = NULL;
            }
            struct ibv_wc wc[iter];

            gettimeofday(&tt1, NULL);
            if (opcode == IBV_WR_SEND)
            {
                sock_sync_data(res->sock, 1, "Q", &temp_char);
            }
            
            for(int i = 0; i < iter; i++ ){
                rc = ibv_post_send(res->qp, &sr[i], &bad_wr[i]);
                if (!rc){
                    //fprintf(stdout, "ibv_post_send success!\n");
                    message_sent++;
                }
                else
                    fprintf(stderr, "ibv_post_send error: %s\n", strerror(errno));
            }

            do{
            //printf("Tring to poll cq.\n");
                poll_result = ibv_poll_cq(res->cq, iter, wc);
                //printf("Poll cq done.\n");
                total_poll_result = total_poll_result +poll_result;
            }while(total_poll_result != iter);
            gettimeofday(&tt2, NULL);

            if(total_poll_result != iter){
            fprintf(stderr, "Poll_result is %d, while iter is %d, error occured!\n", poll_result, iter);
            }

            //printf("Trying to calculate results.\n");
            send_time_usec = (tt2.tv_sec * 1000000 + tt2.tv_usec) - (tt1.tv_sec * 1000000 + tt1.tv_usec);
            total_send_time_usec += send_time_usec;
            bits_sent += message_sent * MSG_SIZE * 8.0;
        } 
        speed_results[num_tested] = bits_sent / total_send_time_usec;
        speed_results[num_tested] = speed_results[num_tested] / 1000.0; // change bit/us to Gbits/s
        time_results[num_tested] = total_send_time_usec;

        printf("Speed: %.2f Gbit/s, time: %.2f us\n", speed_results[num_tested], time_results[num_tested]);
    }
    return 0;
}

static int recv_bandwidth_test(struct resources *res, int opcode, int times, int iter)
{
    // struct ibv_recv_wr rr;
    // struct ibv_sge sge;
    // struct ibv_recv_wr *bad_wr;
    int rc;
    char temp_char;

    // struct ibv_wc wc;
    int total_poll_result;
    int poll_result;

    // memset(&sge, 0, sizeof(sge));
    // sge.addr = (uintptr_t)res->buf;
    // sge.length = MSG_SIZE;
    // sge.lkey = res->mr->lkey;

    /* prepare the receive work request */
    // memset(&rr, 0, sizeof(rr));
    // rr.next = NULL;
    // rr.wr_id = 0;
    // rr.sg_list = &sge;
    // rr.num_sge = 1;
    while(1){
        if (opcode == IBV_WR_SEND)
        {
            struct ibv_recv_wr rr[iter];
            struct ibv_sge sge[iter];
            struct ibv_recv_wr *bad_wr[iter];
            struct ibv_wc wc[iter];
            for (int i = 0; i < iter; i++)
            {
                memset(&sge[i], 0, sizeof(sge[i]));
                sge[i].addr = (uintptr_t)res->buf;
                sge[i].length = MSG_SIZE;
                sge[i].lkey = res->mr->lkey;

                memset(&rr[i], 0, sizeof(rr[i]));
                rr[i].next = NULL;
                rr[i].wr_id = 0;
                rr[i].sg_list = &sge[i];
                rr[i].num_sge = 1;
            }
            total_poll_result = 0;
            for(int j = 0; j < iter; j++ ){
                rc = ibv_post_recv(res->qp, &rr[j], &bad_wr[j]);
                if (rc)
                    fprintf(stderr, "ibv_post_recv error: %s\n", strerror(errno));
                // else
                //     fprintf(stdout, "post_recv success.\n");
            }
            sock_sync_data(res->sock, 1, "Q", &temp_char);
            //printf("Sync data. done\n");
            do{
                //printf("Tring to poll cq.\n");
                poll_result = ibv_poll_cq(res->cq, iter, wc);
                //printf("Poll cq done.\n");
                total_poll_result = total_poll_result + poll_result;
            }while(total_poll_result != iter);

            if(total_poll_result != iter){
                fprintf(stderr, "Poll_result is %d, while iter_num is %d error occured!\n", total_poll_result, iter);
            }
        }
        else{
            //sock_sync_data(res->sock, 1, "Q", &temp_char);
        }   
    }
    return rc;
}

/******************************************************************************
* Function: read_bandwidth_test
*
* Input:
* res: pointer to resources structure
* opcode: IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
* times: try times of bandwidth test
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: This function will create and post send work requests, and test performance
******************************************************************************/





/******************************************************************************
* Function: post_receive
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: post RR to be prepared for incoming messages
*
******************************************************************************/
static int post_receive(struct resources *res)
{
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;

    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;

    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(res->qp, &rr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RR\n");
    }
    else
    {
        if(PRINT_LOG)
            fprintf(stdout, "Receive Request was posted\n");
    }
    return rc;
}

/******************************************************************************
* Function: resources_init
*
* Input:
* res: pointer to resources structure
*
* Output: res is initialized
*
* Returns: none
*
* Description: res is initialized to default values
******************************************************************************/
static void resources_init(struct resources *res)
{
    memset(res, 0, sizeof *res);
    res->sock = -1;
}

/******************************************************************************
* Function: resources_create
*
* Input: res pointer to resources structure to be filled in
*
* Output: res filled in with resources
*
* Returns: 0 on success, 1 on failure
*
* Description:
* This function creates and allocates all necessary system resources. These
* are stored in res.
*****************************************************************************/
static int resources_create(struct resources *res)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int i;
    int mr_flags = 0;
    int cq_size = 0;
    int num_devices;
    int rc = 0;
    unsigned long open_device;
    unsigned long alloc_pd;
    unsigned long create_cq;
    unsigned long reg_mr;
    unsigned long create_qp;
    unsigned long get_device_list;

    /* get device names in the system */
    gettimeofday(&t1,NULL);
    dev_list = ibv_get_device_list(&num_devices);
    gettimeofday(&t2,NULL);
    get_device_list= 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    if(!dev_list)
    {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* if there isn't any IB device in host */
    if(!num_devices)
    {
        fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto resources_create_exit;
    }
    if(PRINT_LOG)
        fprintf(stdout, "found %d device(s)\n", num_devices);

    /* search for the specific device we want to work with */
    for(i = 0; i < num_devices; i ++)
    {
        if(!config.dev_name)
        {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            if(PRINT_LOG)
                fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
        }
		/* find the specific device */
        if(!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name))
        {
            ib_dev = dev_list[i];
            break;
        }
    }

    /* if the device wasn't found in host */
    if(!ib_dev)
    {
        fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }
    if(PRINT_LOG)
        printf("dev: name:%s\ndev_name:%s\ndev_path:%s\nibdev_path:%s\n",\
    ib_dev->name,ib_dev->dev_name,ib_dev->dev_path,ib_dev->ibdev_path);
    /* get device handle */
    //printf("start RDMA Control Path\n");
    gettimeofday(&t1,NULL);
    res->ib_ctx = ibv_open_device(ib_dev);
    gettimeofday(&t2,NULL);
    open_device = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    //printf("ibv_open_device timer = %ld us\n",timer);
    if(!res->ib_ctx)
    {
        fprintf(stderr, "failed to open device %s\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }

    /* We are now done with device list, free it */
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    ib_dev = NULL;

    /* query port properties */
    // printf("-------ibv_query_port\n");
    if(ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr))
    {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate Protection Domain */
    gettimeofday(&t1,NULL);
    res->pd = ibv_alloc_pd(res->ib_ctx);
    gettimeofday(&t2,NULL);
    alloc_pd = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    //printf("ibv_alloc_pd timer = %ld us\n",timer);
    if(!res->pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* each side will send only one WR, so Completion Queue with 1 entry is enough */
    cq_size = 1000;
    gettimeofday(&t1,NULL);
    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
    gettimeofday(&t2,NULL);
    create_cq = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    //printf("ibv_create_cq timer = %ld us\n",timer);
    if(!res->cq)
    {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate the memory buffer that will hold the data */
    size = MSG_SIZE;
    res->buf = (char *) valloc(size);
    //res->buf = (char *) memalign (4096, size);
   // printf("Buffer addr=0x%x\n",res->buf);
    if(!res->buf)
    {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
        rc = 1;
        goto resources_create_exit;
    }
   // memset(res->buf, 0 , size);
    /* only in the server side put the message in the memory buffer */

    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    gettimeofday(&t1,NULL);
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    gettimeofday(&t2,NULL);
    reg_mr = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    //printf("ibv_reg_mr timer = %ld us\n",timer);
    if(!res->mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
    if(PRINT_LOG){
        fprintf(stdout, "MR was registered with addr=0x%x, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            res->buf, res->mr->lkey, res->mr->rkey, mr_flags);
	fprintf(stdout, "MR was registered with addr=%p\n",
            res->buf);
    }

   if(!config.server_name)
    {
        strcpy(res->buf, MSG);
        if(PRINT_LOG)
            fprintf(stdout, "going to send the message: '%s'\n", res->buf);
    }
    else
    {
        memset(res->buf, 0, size);
    }
    
    /* create the Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.cap.max_send_wr = 1000;
    qp_init_attr.cap.max_recv_wr = 1000;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
   // printf("----before ibv_create_qp\n");
    gettimeofday(&t1,NULL);
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    gettimeofday(&t2,NULL);
    create_qp = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    //printf("ibv_create_qp timer = %ld us\n",timer);
    if(!res->qp)
    {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }
    if(PRINT_LOG)
        fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);
    printf("get_device_list time =%ld us\n",get_device_list);
    printf("ibv_open_device time =%ld us\n",open_device);
    printf("ibv_alloc_pd    time =%ld us\n",alloc_pd);
    printf("ibv_create_cq   time =%ld us\n",create_cq);
    printf("ibv_reg_mr      time =%ld us\n",reg_mr);
    printf("ibv_create_qp   time =%ld us\n",create_qp);

resources_create_exit:
    if(rc)
    {
        /* Error encountered, cleanup */
        if(res->qp)
        {
            ibv_destroy_qp(res->qp);
            res->qp = NULL;
        }
        if(res->mr)
        {
            ibv_dereg_mr(res->mr);
            res->mr = NULL;
        }
        if(res->buf)
        {
            free(res->buf);
            res->buf = NULL;
        }
        if(res->cq)
        {
            ibv_destroy_cq(res->cq);
            res->cq = NULL;
        }
        if(res->pd)
        {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL;
        }
        if(res->ib_ctx)
        {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL;
        }
        if(dev_list)
        {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
        if(res->sock >= 0)
        {
            if(close(res->sock))
            {
                fprintf(stderr, "failed to close socket\n");
            }
            res->sock = -1;
        }
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_init
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RESET to INIT state
******************************************************************************/
static int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    unsigned long qp2init;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    gettimeofday(&t1,NULL);
    rc = ibv_modify_qp(qp, &attr, flags);
    gettimeofday(&t2,NULL);
    qp2init = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    printf("ibv_modify_qp init  timer = %ld us\n",qp2init);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to INIT\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rtr
*
* Input:
* qp: QP to transition
* remote_qpn: remote QP number
* dlid: destination LID
* dgid: destination GID (mandatory for RoCEE)
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: 
* Transition a QP from the INIT to RTR state, using the specified QP number
******************************************************************************/
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    unsigned long qp2rtr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = config.ib_port;
    if(config.gid_idx >= 0)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }
    if(PRINT_LOG)
        printf("qp's dev:%s\nqp's devname:%s\nqp's devpath:%s\nqp's ibdevpath:%s\n",\
    qp->context->device->name,qp->context->device->dev_name,qp->context->device->dev_path,qp->context->device->ibdev_path);
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    gettimeofday(&t1,NULL);
    rc = ibv_modify_qp(qp, &attr, flags);
    gettimeofday(&t2,NULL);
    qp2rtr = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    printf("ibv_modify_qp RTR   timer = %ld us\n",qp2rtr);
    if(PRINT_LOG)
        printf("modify QP state to RTR =%d\n",rc);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rts
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RTR to RTS state
******************************************************************************/
static int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    unsigned long qp2rts;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    gettimeofday(&t1,NULL);
    rc = ibv_modify_qp(qp, &attr, flags);
    gettimeofday(&t2,NULL);
    qp2rts = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    printf("ibv_modify_qp RTS   timer = %ld us\n",qp2rts);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
    }
    return rc;
}

/******************************************************************************
* Function: connect_qp
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: 
* Connect the QP. Transition the server side to RTR, sender side to RTS
******************************************************************************/
static int connect_qp(struct resources *res)
{
    struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc = 0;
    char temp_char;
    union ibv_gid my_gid;
    unsigned long query_gid;
    if(config.gid_idx >= 0)
    {
        gettimeofday(&t1,NULL);
        rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
        gettimeofday(&t2,NULL);
        query_gid = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
        printf("ibv_query_gid   timer = %ld us\n",query_gid);
        if(rc)
        {
            fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
            return rc;
        }
    }
    else
    {
        memset(&my_gid, 0, sizeof my_gid);
    }

    /* modify the QP to init */
    //gettimeofday(&t1,NULL);
    rc = modify_qp_to_init(res->qp);
    if(rc)
    {
        fprintf(stderr, "change QP state to INIT failed\n");
        goto connect_qp_exit;
    }

    /* let the client post RR to be prepared for incoming messages */
    // if(config.server_name)
    // {
    //     rc = post_receive(res);
    //     if(rc)
    //     {
    //         fprintf(stderr, "failed to post RR\n");
    //         goto connect_qp_exit;
    //     }
    // }

    /* if client side */
    if(config.server_name)
    {
        res->sock = sock_connect(config.server_name, config.tcp_port);
        if(res->sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
                    config.server_name, config.tcp_port);
            rc = -1;
            goto connect_qp_exit;
        }
    }
    else
    {
        if(PRINT_LOG)
            fprintf(stdout, "waiting on port %d for TCP connection\n", config.tcp_port);
        res->sock = sock_connect(NULL, config.tcp_port);
        if(res->sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection with client on port %d\n",
                    config.tcp_port);
            rc = -1;
            goto connect_qp_exit;
        }
    }
    if(PRINT_LOG){
        fprintf(stdout, "TCP connection was established\n");
        fprintf(stdout, "searching for IB devices in host\n");
    }
    /* exchange using TCP sockets info required to connect QPs */
    local_con_data.addr = htonll((uintptr_t)res->buf);
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp->qp_num);
    local_con_data.lid = htons(res->port_attr.lid);//锟剿口的憋拷锟截憋拷识锟斤拷lid
    memcpy(local_con_data.gid, &my_gid, 16);
    //fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
    if(sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0)
    {
        fprintf(stderr, "failed to exchange connection data between sides\n");
        rc = 1;
        goto connect_qp_exit;
    }

    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

    /* save the remote side attributes, we will need it for the post SR */
    res->remote_props = remote_con_data;
    if(PRINT_LOG){
        fprintf(stdout, "Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
        fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
        fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
        fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    }
    if(config.gid_idx >= 0)
    {
        uint8_t *p = remote_con_data.gid;
        if(PRINT_LOG)
            fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }


    /* modify the QP to RTR */
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }

    /* modify the QP to RTS */
    rc = modify_qp_to_rts(res->qp);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
        goto connect_qp_exit;
    }
    if(PRINT_LOG)
        fprintf(stdout, "QP state was change to RTS\n");
    // gettimeofday(&t2,NULL);
    // qp_timer = 1000000 * (t2.tv_sec-t1.tv_sec)+ t2.tv_usec-t1.tv_usec;
    // printf("modify timer = %ld us\n",qp_timer);

    /* sync to make sure that both sides are in states that they can connect to prevent packet loose */
    if(sock_sync_data(res->sock, 1, "Q", &temp_char))  /* just send a dummy char back and forth */
    {
        fprintf(stderr, "sync error after QPs are were moved to RTS\n");
        rc = 1;
    }

connect_qp_exit:
    return rc;
}

/******************************************************************************
* Function: resources_destroy
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Cleanup and deallocate all resources used
******************************************************************************/
static int resources_destroy(struct resources *res)
{
    int rc = 0;
    if(res->qp)
	{
        if(ibv_destroy_qp(res->qp))
        {
            fprintf(stderr, "failed to destroy QP\n");
            rc = 1;
        }
	}

    if(res->mr)
	{
        if(ibv_dereg_mr(res->mr))
        {
            fprintf(stderr, "failed to deregister MR\n");
            rc = 1;
        }
	}

    if(res->buf)
    {
        free(res->buf);
    }

    if(res->cq)
	{
        if(ibv_destroy_cq(res->cq))
        {
            fprintf(stderr, "failed to destroy CQ\n");
            rc = 1;
        }
	}

    if(res->pd)
	{
        if(ibv_dealloc_pd(res->pd))
        {
            fprintf(stderr, "failed to deallocate PD\n");
            rc = 1;
        }
	}

    if(res->ib_ctx)
	{
        if(ibv_close_device(res->ib_ctx))
        {
            fprintf(stderr, "failed to close device context\n");
            rc = 1;
        }
	}

    if(res->sock >= 0)
	{
        if(close(res->sock))
        {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }
	}
    return rc;
}

/******************************************************************************
* Function: print_config
*
* Input: none
*
* Output: none
*
* Returns: none
*
* Description: Print out config information
******************************************************************************/
static void print_config(void)
{
    fprintf(stdout, " ------------------------------------------------\n");
    fprintf(stdout, " Device name : \"%s\"\n", config.dev_name);
    fprintf(stdout, " IB port : %u\n", config.ib_port);
    if(config.server_name)
    {
        fprintf(stdout, " IP : %s\n", config.server_name);
    }
    fprintf(stdout, " TCP port : %u\n", config.tcp_port);
    if(config.gid_idx >= 0)
    {
        fprintf(stdout, " GID index : %u\n", config.gid_idx);
    }
    fprintf(stdout, " Test interval : %uus\n", config.test_interval_us);
    fprintf(stdout, " Test opcode : ");
    switch (config.test_opcode)
    {
    case 2:
        fprintf(stdout, "Send\n");
        break;
    case 4:
        fprintf(stdout, "Read\n");
        break;
    case 0:
        fprintf(stdout, "Write\n");
        break;
    default:
        fprintf(stderr, " Unkown!\n");
        exit(-1);
        break;
    }
    fprintf(stdout, " Test times: %d\n", config.test_times);
    fprintf(stdout, " Test iter num: %d\n", config.iter_nums);
    fprintf(stdout, " ------------------------------------------------\n\n");
}
/******************************************************************************
* Function: usage
*
* Input:
* argv0: command line arguments
*
* Output: none
*
* Returns: none
*
* Description: print a description of command line syntax
******************************************************************************/
static void usage(const char *argv0)
{
    fprintf(stdout, "Usage:\n");
    fprintf(stdout, " %s start a server and wait for connection\n", argv0);
    fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
    fprintf(stdout, "\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
    fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
    fprintf(stdout, " -i, --ib-port <port> use port <port> of IB device (default 1)\n");
    fprintf(stdout, " -g, --gid_idx <git index> gid index to be used in GRH (default not used)\n");
    fprintf(stdout, " -t, --test_interval <interval> perf test interval in us\n");
    fprintf(stdout, " -o, --test_opcode <opcode> 2 for send, 4 for read, 0 for write\n");
    fprintf(stdout, " -s, --test_times <num> number of test times\n");
    fprintf(stdout, " -e, --iter_nums <num> number of iterations per test round\n");
}

/******************************************************************************
* Function: main
*
* Input:
* argc: number of items in argv
* argv: command line parameters
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Main program code
******************************************************************************/
int main(int argc, char *argv[])
{
    struct resources res;
    int rc = 1;
    char temp_char;

    /* parse the command line parameters */
    while(1)
    {
        int c;
		/* Designated Initializer */
        static struct option long_options[] =
        {
            {.name = "port", .has_arg = 1, .val = 'p' },
            {.name = "ib-dev", .has_arg = 1, .val = 'd' },
            {.name = "ib-port", .has_arg = 1, .val = 'i' },
            {.name = "gid-idx", .has_arg = 1, .val = 'g' },
            {.name = "test_interval", .has_arg = 1, .val = 't' },
            {.name = "test_opcode", .has_arg = 1, .val = 'o' },
            {.name = "test_times", .has_arg = 1, .val = 's'},
            {.name = "iter_nums", .has_arg = 1, .val = 'e'},
            {.name = NULL, .has_arg = 0, .val = '\0'}
        };

        c = getopt_long(argc, argv, "p:d:i:g:t:o:s:e:", long_options, NULL);
        if(c == -1)
        {
            break;
        }
        switch(c)
        {
        case 'p':
            config.tcp_port = strtoul(optarg, NULL, 0);
            break;
        case 'd':
            config.dev_name = strdup(optarg);
            break;
        case 'i':
            config.ib_port = strtoul(optarg, NULL, 0);
            if(config.ib_port < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'g':
            config.gid_idx = strtoul(optarg, NULL, 0);
            if(config.gid_idx < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 't':
            config.test_interval_us = strtoul(optarg, NULL, 0);
            break;
        case 'o':
            config.test_opcode = strtoul(optarg, NULL, 0);
            if (config.test_opcode != 0 && config.test_opcode !=2 && config.test_opcode != 4)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 's':
            config.test_times = strtoul(optarg, NULL, 0);
            break;
        case 'e':
            config.iter_nums = strtoul(optarg, NULL, 0);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* parse the last parameter (if exists) as the server name */
	/* 
	 * server_name is null means this node is a server,
	 * otherwise this node is a client which need to connect to 
	 * the specific server
	 */
    if(optind == argc - 1)
    {
        config.server_name = argv[optind];
    }
    else if(optind < argc)
    {
        usage(argv[0]);
        return 1;
    }

    /* print the used parameters for info*/
    if(PRINT_LOG)
        print_config();
    /* init all of the resources, so cleanup will be easy */
    resources_init(&res);
    /* create resources before using them */
    if(resources_create(&res))
    {
        fprintf(stderr, "failed to create resources\n");
        goto main_exit;
    }
    /* connect the QPs */
    if(connect_qp(&res))
    {
        fprintf(stderr, "failed to connect QPs\n");
        goto main_exit;
    }

    if(config.server_name){
        rc = send_bandwidth_test(&res, config.test_opcode, config.test_times, config.iter_nums, config.test_interval_us);
    }
    else{
        rc = recv_bandwidth_test(&res, config.test_opcode, config.test_times, config.iter_nums);
    }


main_exit:
    if(resources_destroy(&res))
    {
        fprintf(stderr, "failed to destroy resources\n");
        rc = 1;
    }
    if(config.dev_name)
    {
        free((char *) config.dev_name);
    }
    fprintf(stdout, "\ntest result is %d\n", rc);
    return rc;
}
