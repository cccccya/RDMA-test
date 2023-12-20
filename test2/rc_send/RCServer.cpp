#include "rdma.h"

using std::cerr;
using std::endl;
using std::cout;

static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}

struct ServerContext {
    RdmaDeviceInfo dev_info;     /* 设备信息 */
    ibv_pd *pd;                  /* 保护域 */
    ibv_cq *cq;                  /* 完成队列 */
    ibv_qp *qp;                  /* 队列对 */
    ibv_mr *mr;                  /* 内存区域 */
    char *buf;                   /* 用于存储数据的缓冲区 */
    RdmaConnExchangeInfo remote_info;

    void CreateContext() {
        /* 获取设备信息 */
        dev_info = GetRdmaDeviceByName(config.dev_name);
        if(dev_info.ib_ctx == nullptr) {
            cerr << "Failed to get RDMA device " << config.dev_name << endl;
            exit(0);
        }
        /* 分配保护域 */
        pd = ibv_alloc_pd(dev_info.ib_ctx);
        if(pd == nullptr) {
            cerr << "Failed to allocate protection domain" << endl;
            exit(0);
        }
        /* 创建完成队列cq */
        cq = ibv_create_cq(dev_info.ib_ctx, cq_size, nullptr, nullptr, 0);
        if(cq == nullptr) {
            cerr << "Failed to create completion queue" << endl;
            exit(0);
        }

       /* 创建缓冲区 */
        buf = (char *) aligned_alloc(4096, BUF_SIZE);
        if(buf == nullptr) {
            cerr << "Failed to allocate buffer" << endl;
            exit(0);
        }
        memset(buf, 0, BUF_SIZE);

        /* 注册mr */
        int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        mr = ibv_reg_mr(pd, buf, BUF_SIZE, mr_flags);
        if(mr == nullptr) {
            cerr << "Failed to register memory region" << endl;
            return;
        }

        /* 创建QP */
        qp = CreateQP(pd, cq, cq, IBV_QPT_RC);
        if (qp == nullptr) {
            cerr << "Create qp failed" << endl;
            exit(0);
        }
    }
    void DestroyContext() {
        if (qp != nullptr) {
            ibv_destroy_qp(qp);
            qp = nullptr;
        }
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(dev_info.ib_ctx);
    }

    void Server(int num) {
        sockaddr_in server_addr;

        int sockfd = -1, connfd = -1;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0) {
            cerr << "socket error" << endl;
            exit(0);
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(config.tcp_port+num);

        bind(sockfd, (sockaddr*)&server_addr, sizeof(server_addr));
        if(listen(sockfd, 1) < 0) {
            cerr << "accept error" << endl;
            exit(0);
        }

        connfd = accept(sockfd, (sockaddr*)NULL, NULL);
        if(connfd < 0) {
            cerr << "accept error" << endl;
            exit(0);
        }
        close(sockfd);

        RdmaConnExchangeInfo local_info, tmp_info;
        union ibv_gid my_gid;
        if(config.gid_idx >= 0) {
            int rc = ibv_query_gid(dev_info.ib_ctx, config.ib_port, config.gid_idx, &my_gid);
            if(rc) {
                cerr << "无法获取本地GID" << endl;
                exit(0);
            }
        } else {
            memset(&my_gid, 0, sizeof my_gid);
        }
        local_info.addr = htonll((uintptr_t)buf);
        local_info.rkey = htonl(mr->rkey);
        local_info.qp_num = htonl(qp->qp_num);
        local_info.lid = htons(dev_info.port_attr.lid);
        memcpy(local_info.gid, &my_gid, 16);
        if(sock_sync_data(connfd, sizeof(RdmaConnExchangeInfo), (char *)&local_info, (char *)&tmp_info) < 0) {
            cerr << "无法与远程主机同步数据" << endl;
            exit(0);
        }
        remote_info.addr = ntohll(tmp_info.addr);
        remote_info.rkey = ntohl(tmp_info.rkey);
        remote_info.qp_num = ntohl(tmp_info.qp_num);
        remote_info.lid = ntohs(tmp_info.lid);
        memcpy(remote_info.gid, tmp_info.gid, 16);
        // cout<<(uintptr_t)buf<<" "<<mr->rkey<<" "<<qp->qp_num<<" "
        // <<dev_info.port_attr.lid<<" "<<endl;
        // cout<<remote_info.addr<<" "<<remote_info.rkey<<" "<<remote_info.qp_num<<" "
        // <<remote_info.lid<<" "<<remote_info.gid<<endl;
    }

    int poll_completion(ibv_cq *cq, int& pos) {
        struct ibv_wc wc[1024];
        unsigned long start_time_msec;
        unsigned long cur_time_msec;
        struct timeval cur_time;
        int poll_result;
        /* 获取当前时间 */
        gettimeofday(&cur_time, NULL);
        start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
        do {
            poll_result = ibv_poll_cq(cq, 1024, wc);
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
            //cout<<"in rdma.cpp: poll suc " << poll_result <<endl;
            for(int i = 0; i < poll_result; i++){
                if(wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "完成的操作失败，错误码=0x%x，vendor_err=0x%x\n", wc[i].status, wc[i].vendor_err);
                    poll_result = i;
                }
                post_recv(buf + pos*pack_size, pack_size, mr->lkey, qp, 0);
                pos=(pos+1)%1024;
            }
        }
        return poll_result;
    }
} s_ctx;
int main() {
    for(int i=0;i<10;i++){
    s_ctx.CreateContext();
    s_ctx.Server(i);
    if(modify_qp_to_init(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    for(int i = 0; i < 10240; i++) {
        post_recv(s_ctx.buf + (i%1024)*pack_size, pack_size, s_ctx.mr->lkey, s_ctx.qp, 0);
    }
    if(modify_qp_to_rtr(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    if(modify_qp_to_rts(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    //memset(s_ctx.buf,0,BUF_SIZE);
    //strcpy(s_ctx.buf,"aaa");
    /*if(post_send(s_ctx.buf, 3, s_ctx.mr->lkey, s_ctx.qp, 0, IBV_WR_SEND, 0, 0)) {
        cerr << "send失败" <<endl;
        exit(0);
    }*/
    int sum = 0, pos = 0, tot_num = 1ll*1024*1024*100*100/pack_size;
    while(sum < tot_num) {
        int cnt = 0, tmp = 0;
        cnt = s_ctx.poll_completion(s_ctx.cq, pos);
        if(cnt <= 0) {
            cerr << "sum: "<<sum<<"   轮询失败 " <<cnt<<endl;
            exit(0);
        }
        sum += cnt;
    }
    //cout << s_ctx.buf << endl;
    cout<< sum << " " << tot_num << endl;
    s_ctx.DestroyContext();
    }
    return 0;
}