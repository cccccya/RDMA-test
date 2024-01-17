#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <endian.h>
#include <infiniband/verbs.h>

#include <sys/time.h>

enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

long getTimeNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);

    return ts.tv_sec*1000000000+ts.tv_nsec;
}

static int page_size;

struct pingpong_context {
    struct ibv_context  *context;
    struct ibv_pd       *pd;
    struct ibv_mr       *mr;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_ah       *ah;
    char         *buf;
    int          size;
    int          send_flags;
    int          rx_depth;
    int          pending;
    struct ibv_port_attr     portinfo;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

void
wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        tmp_gid[i] = be32toh(v32);
    }
    memcpy(gid, tmp_gid, sizeof(*gid));
}

void
gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

static struct pingpong_context *
init_ctx(struct ibv_device *ib_dev, int size, int port)
{
    struct pingpong_context *ctx = NULL;

    ctx = malloc(sizeof(*ctx));
    if(!ctx) {
        printf("malloc ctx error\n");
        return NULL;
    }

    ctx->size = size;
    ctx->send_flags = IBV_SEND_SIGNALED;
    //ctx->send_flags = IBV_SEND_INLINE;
    ctx->buf = memalign(page_size, size + 40);
    if(!ctx->buf) {
        printf("ctx->buf malloc error\n");
        goto clean_ctx;
    }

    memset(ctx->buf, 0, size + 40);

    ctx->context = ibv_open_device(ib_dev);
    if(!ctx->context) {
        printf("ctx->context error, %s\n", ibv_get_device_name(ib_dev));
        goto clean_buffer;
    }

    {
        /*查询端口 mtu大小*/
        /*struct ibv_port_attr port_info = {};
        int mtu;
        if(ibv_query_port(ctx->context, port, &port_info)) {
            printf("ibv_query_port error\n");
            goto clean_device;
        }
        mtu = 1 << (port_info.active_mtu + 7);
        if (size > mtu) {
            printf("Requested size larger than port MTU (%d)\n", mtu);
            goto clean_device;
        }*/
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if(!ctx->pd) {
        printf("ibv_alloc_pd error\n");
        goto clean_device;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size + 40, IBV_ACCESS_LOCAL_WRITE);
    if(!ctx->mr) {
        printf("ibv_reg_mr error\n");
        goto clean_pd;
    }

    ctx->cq = ibv_create_cq(ctx->context, 1024, NULL, NULL, 0);
    if(!ctx->cq) {
        printf("ibv_create_cq error\n");
        goto clean_mr;
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {
            .send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .cap     = {
                .max_send_wr  = 1024,
                .max_recv_wr  = 10240,
                .max_send_sge = 1,
                .max_recv_sge = 1
            },
            .qp_type = IBV_QPT_UD,
        };
        init_attr.sq_sig_all = 0;
        //init_attr.cap.max_inline_data = ctx->size;
        ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
        if (!ctx->qp)  {
            printf("Couldn't create QP\n");
            goto clean_cq;
        }
        ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
    }

    {
        struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,
            .pkey_index      = 0,
            .port_num        = port,
            .qkey            = 0x11111111
        };
        /*修改QP状态-->IBV_QPS_INIT*/
        if (ibv_modify_qp(ctx->qp, &attr,
                  IBV_QP_STATE              |
                  IBV_QP_PKEY_INDEX         |
                  IBV_QP_PORT               |
                  IBV_QP_QKEY)) {
            printf("Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }
    return ctx;
clean_qp:
    ibv_destroy_qp(ctx->qp);
clean_cq:
    ibv_destroy_cq(ctx->cq);
clean_mr:
    ibv_dereg_mr(ctx->mr);
clean_pd:
    ibv_dealloc_pd(ctx->pd);
clean_device:
    ibv_close_device(ctx->context);
clean_buffer:
    free(ctx->buf);
clean_ctx:
    free(ctx);
    return NULL;
}

static int 
close_ctx(struct pingpong_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ibv_destroy_ah(ctx->ah)) {
        fprintf(stderr, "Couldn't destroy AH\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }
    free(ctx->buf);
    free(ctx);
    return 0;
}

static int
post_recv(struct pingpong_context *ctx)
{
    struct ibv_sge list = {
        .addr   = (uintptr_t) ctx->buf,
        .length = ctx->size + 40,
        .lkey   = ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id      = PINGPONG_RECV_WRID,
        .sg_list    = &list,
        .num_sge    = 1,
        .next       = NULL,
    };
    struct ibv_recv_wr *bad_wr;
    ibv_post_recv(ctx->qp, &wr, &bad_wr);
    return 0;
}

static int
post_send(struct pingpong_context *ctx, uint32_t qpn)
{
    /*struct ibv_sge list = {
        .addr   = (uintptr_t) ctx->buf + 40,
        .length = 200,
        .lkey   = ctx->mr->lkey
    };
    struct ibv_send_wr wr = {
        .wr_id      = PINGPONG_SEND_WRID,
        .sg_list    = &list1,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = 0,
        .wr         = {
            .ud = {
                 .ah          = ctx->ah,
                 .remote_qpn  = qpn,
                 .remote_qkey = 0x11111111
             }
        }
    };*/
    struct ibv_sge list[64];
    struct ibv_send_wr wr[64];
    for(int i = 0; i < 64; i++) {
        list[i].addr = (uintptr_t) ctx->buf + 1024 * i;
        list[i].length = 1024;
        list[i].lkey = ctx->mr->lkey;

        wr[i].wr_id = PINGPONG_SEND_WRID;
        wr[i].sg_list = list + i,
        wr[i].num_sge = 1,
        wr[i].opcode  = IBV_WR_SEND,
        wr[i].send_flags = 0,
        wr[i].wr.ud.ah = ctx->ah;
        wr[i].wr.ud.remote_qpn = qpn;
        wr[i].wr.ud.remote_qkey = 0x11111111;
        wr[i].next = wr + i + 1;
    }
    wr[63].send_flags = IBV_SEND_SIGNALED;
    wr[63].next = NULL;

    struct ibv_send_wr *bad_wr;
    int ret = ibv_post_send(ctx->qp, wr, &bad_wr);
    return ret;
}

static int
connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
    int sl, struct pingpong_dest *dest, int sgid_idx)
{
    struct ibv_ah_attr ah_attr = {
        .is_global     = 0,
        .dlid          = dest->lid,
        .sl            = sl,
        .src_path_bits = 0,
        .port_num      = port
    };

    struct timeval start_qp, end_qp;
    gettimeofday(&start_qp, NULL);
    struct ibv_qp_attr attr = {
        .qp_state       = IBV_QPS_RTR
    };

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn   = my_psn;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    gettimeofday(&end_qp, NULL);
    long qp_time = (end_qp.tv_sec-start_qp.tv_sec)*1000000+(end_qp.tv_usec-start_qp.tv_usec);
    //printf("qp:%ld ",qp_time);

    if (dest->gid.global.interface_id) {
        printf("client in");
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = dest->gid;
        ah_attr.grh.sgid_index = sgid_idx;
    }

    ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ctx->ah) {
        fprintf(stderr, "Failed to create AH\n");
        return 1;
    }
    return 0;
}

static struct pingpong_dest *
client(const char *servername, int port, 
    const struct pingpong_dest *my_dest)
{
    int sockfd = -1;
    struct sockaddr_in serv_addr;
    char gid[33];
    struct pingpong_dest *rem_dest = NULL;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    if((sockfd = socket(AF_INET, SOCK_STREAM, 0))< 0) {
        printf("\n Error : Could not create socket \n");
        return NULL;
    }
    //printf("sockfd:%d\n",sockfd);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr(servername);
    if(connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0) {
        close(sockfd);
        printf("connect error\n");
        return NULL;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg ||
        write(sockfd, "done", sizeof "done") != sizeof "done") {
        perror("client read/write");
        fprintf(stderr, "Couldn't read/write remote address\n");
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);
out:
    close(sockfd);
    return rem_dest;
}

static int poll_completion(struct pingpong_context *res, uint32_t qpn, int* send_num, int tot_num) {
    struct ibv_wc wc[1024];
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    /* 获取当前时间 */
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    do {
        poll_result = ibv_poll_cq(res->cq, 1024, wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while((poll_result == 0) && ((cur_time_msec - start_time_msec) < 5000));
    if(poll_result < 0) {
        fprintf(stderr, "无法完成完成队列\n");
    } else if(poll_result == 0) {
        fprintf(stderr, "完成队列超时\n");
    } else {
        //fprintf(stdout, "完成队列完成\n");
        /* 检查完成的操作是否成功 */
        for(int i=0;i<poll_result;i++) {
            if(wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "完成的操作失败，错误码=0x%x，vendor_err=0x%x\n", wc[i].status, wc[i].vendor_err);
                return i;
            }
            if(wc[i].opcode & IBV_WC_RECV) {
                printf("IBV_WC_RECV ok, %s\n", res->buf + 40);
            }
            else if(wc[i].opcode == IBV_WC_SEND){
                //printf("IBV_WC_SEND ok\n");
                if((*send_num) < tot_num) {
                    post_send(res, qpn);
                    (*send_num)++;
                }
            }
        }
        
    }
    return poll_result;
}

int main(int argc, char **argv)
{    
    //freopen("T","w",stdout);
    char teststr1[5000];memset(teststr1,0,sizeof(teststr1));
    char teststr2[5000];memset(teststr2,0,sizeof(teststr2));
    char teststr3[5000];memset(teststr3,0,sizeof(teststr3));
    char teststr4[5000];memset(teststr4,0,sizeof(teststr4));
    for(int i=0;i<2000;i++)teststr1[i]='a',teststr2[i]='b',teststr3[i]='c',teststr4[i]='d';

    //struct timeval start;  // define 2 struct timeval variables
    //gettimeofday(&start, NULL); // get the beginning time

    struct ibv_device      **dev_list;
    struct ibv_device      *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest my_dest, *rem_dest;
    unsigned int             port = 19785;
    int                      ib_port = 1;
    int                      gidx = 1;
    int                      sl = 0;
    unsigned int             size = 100000;
    char                     gid[33];
    char                    *servername = NULL;
    srand48(getpid() * time(NULL));
    
    /*获取IP*/
    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        return -1;
    }

    page_size = sysconf(_SC_PAGESIZE);
    dev_list = ibv_get_device_list(NULL);
    if(!dev_list){
        printf("ibv_get_device_list error\n");
        return -1;
    } else {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    }

    ctx = init_ctx(ib_dev, size, ib_port);
    if(!ctx){
        printf("init_ctx error\n");
        return -1;
    }
    post_recv(ctx);

    if(ibv_req_notify_cq(ctx->cq, 0)){
        printf("ibv_req_notify_cq error\n");
        return -1;
    }
    /*获取lid信息*/
    if(ibv_query_port(ctx->context, ib_port, &ctx->portinfo)){
        printf("ibv_query_port error\n");
        return -1;
    }

    my_dest.lid = ctx->portinfo.lid;
    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;

    /*得到my_dest.gid的值*/
    if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
        printf("Could not get local gid for gid index %d\n", gidx);
        return -1;
    }

    /*将my_dest.gid的值转为点分十进制形式*/
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof(gid));
    //printf("local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
    //       my_dest.lid, my_dest.qpn, my_dest.psn, gid);


    struct timeval start_con, end_con;
    gettimeofday(&start_con, NULL);
    rem_dest = client(servername, port, &my_dest);
    if(!rem_dest){
        printf("client error\n");
        return -1;
    }
    gettimeofday(&end_con, NULL);


    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof(gid));
    //printf("remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
    //       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

    if(connect_ctx(ctx, ib_port, my_dest.psn, sl, rem_dest, gidx)) {
        printf("connect_ctx error\n");
        return -1;
    }


    sleep(1);
    memcpy(ctx->buf, teststr1, 1024);
    memcpy(ctx->buf + 1024, teststr2, 1024);
    memcpy(ctx->buf + 2048, teststr3, 1024);
    memcpy(ctx->buf + 3072, teststr4, 1024);
    //long work1 = getTimeNs();
    struct timeval start, end;  // define 2 struct timeval variables
    gettimeofday(&start, NULL);
    int sum = 0, cnt = 0, send_sum = 16, tot_num = 1ll*1024*1024*100*100/65536;
    for(int i = 0; i < 16; i++) {
        if (post_send(ctx, rem_dest->qpn)) {
            printf("Couldn't post send\n");
            return -1;
        }
    }
    //post_send(ctx, rem_dest->qpn);
    while(send_sum < tot_num || sum < tot_num) {
        cnt = poll_completion(ctx, rem_dest->qpn, &send_sum, tot_num);
        if(cnt <= 0) {
            printf("send_sum:%d sum:%d tot:%d\n",send_sum, sum, tot_num);
            printf("轮询失败\n");
            exit(0);
        }
        sum += cnt;
        //printf("post send suc:%d\n", sum);
    }
    //printf("post send %d suc:%d\n", send_sum, sum);
    gettimeofday(&end, NULL);
    //long work_time = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
    double dDuration = (end.tv_sec - start.tv_sec) + ((end.tv_usec - start.tv_usec) / 1000.0) / 1000.0;
    //long work2 = getTimeNs();
    //printf("%.6lf\n",1.0*(work2-work1)/1e9);
    //printf("work:%ld\n",work_time);
    printf("work %.6lf\n", dDuration);

    if(close_ctx(ctx)){
        printf("close_ctx error \n");
        return -1;
    }
    ibv_free_device_list(dev_list);
    free(rem_dest);

    sleep(2);
    return 0;
}
