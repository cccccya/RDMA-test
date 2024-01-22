#include "rdma.h"
#include <vector>
#include <csignal>

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
    ibv_cq_ex *cq_ex;
    ibv_qp *qp;                  /* 队列对 */
    ibv_mr *mr;                  /* 内存区域 */
    char *buf;                   /* 用于存储数据的缓冲区 */
    ibv_ah *ah;                  /* 地址句柄  */
    RdmaUDConnExchangeInfo remote_info;

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
        /*cq = ibv_create_cq(dev_info.ib_ctx, cq_size, nullptr, nullptr, 0);
        if(cq == nullptr) {
            cerr << "Failed to create completion queue" << endl;
            exit(0);
        }*/
        ibv_cq_init_attr_ex cq_attr_ex{};
        cq_attr_ex.cqe = cq_size;
        cq_attr_ex.cq_context = nullptr;
        cq_attr_ex.channel = nullptr;
        cq_attr_ex.comp_vector = 0;
        cq_attr_ex.wc_flags = IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK;
        cq_ex = ibv_create_cq_ex(dev_info.ib_ctx, &cq_attr_ex);
        cq = ibv_cq_ex_to_cq(cq_ex);
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
        qp = CreateQP(pd, cq, cq, IBV_QPT_UD);
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
        ibv_destroy_ah(ah);
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
        server_addr.sin_port = htons(config.tcp_port + num);

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

        RdmaUDConnExchangeInfo local_info, tmp_info;
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
        local_info.qkey = htonl(0x11111111);
        local_info.qpn = htonl(qp->qp_num);
        local_info.lid = htons(dev_info.port_attr.lid);
        local_info.psn = htonl(lrand48() & 0xffffff);
        local_info.gid = my_gid;
        local_info.gid_idx = htonl(3);//magic
        
        /*cout<<"addr:"<< (uintptr_t)buf << endl;
        cout<<"qkey:"<< 0x11111111 << endl;
        cout<<"qpn:"<< qp->qp_num << endl;
        cout<<"lid:"<< dev_info.port_attr.lid << endl;
        cout<<"psn:"<< ntohl(local_info.psn) << endl;
        cout<<"server:"<<RdmaGid2Str(local_info.gid)<<endl;*/
        if(sock_sync_data(connfd, sizeof(RdmaUDConnExchangeInfo), (char *)&local_info, (char *)&tmp_info) < 0) {
            cerr << "无法与远程主机同步数据" << endl;
            exit(0);
        }
        remote_info.addr = ntohll(tmp_info.addr);
        remote_info.qkey = ntohl(tmp_info.qkey);
        remote_info.qpn = ntohl(tmp_info.qpn);
        remote_info.lid = ntohs(tmp_info.lid);
        remote_info.psn = ntohl(tmp_info.psn);
        remote_info.gid = tmp_info.gid;
        remote_info.gid_idx = ntohl(tmp_info.gid_idx);
        /*cout<<"addr:"<< remote_info.addr << endl;
        cout<<"qkey:"<< remote_info.qkey << endl;
        cout<<"qpn:"<< remote_info.qpn << endl;
        cout<<"lid:"<< remote_info.lid << endl;
        cout<<"psn:"<< remote_info.psn << endl;
        cout<<"client:"<<RdmaGid2Str(remote_info.gid)<<endl;*/
    }
} s_ctx;
int all;
void signalHandler(int signum) {
    if (signum == SIGTSTP) {
        std::cout << "Received Ctrl+Z signal. Outputting values..." << std::endl;

        // 在这里添加你想要输出的值或执行的操作
        cout << all<<endl;
        // 退出程序
        exit(signum);
    }
}
int main(int argc, char **argv) {
    signal(SIGTSTP, signalHandler);
    double avg_dk = 0,avg_t = 0;
    //for(int i = 0;i<20;i++) {
    s_ctx.CreateContext();
    s_ctx.Server(0);

    if(modify_qp_to_init(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    for(int i=0;i<cq_size;i++) post_recv(s_ctx.buf + i*(PACKET_SIZE+40)%BUF_SIZE, PACKET_SIZE+40, s_ctx.mr->lkey, s_ctx.qp, 1);
    if(modify_qp_to_rtr(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    if(modify_qp_to_rts(s_ctx.qp, s_ctx.remote_info)) {
        exit(0);
    }
    /* 创建AH */
    s_ctx.ah = CreateAH(s_ctx.pd, config.ib_port, 0, s_ctx.remote_info, 3);
    if(s_ctx.ah == nullptr) {
        cerr << "Create ah failed" << endl;
        exit(0);
    }
    cout<<"finish connect"<<endl;
    ibv_wc wc[cq_size]{};
    uint64_t timer{}, start{};
    long long recv_Byte = 0;
    int recv_wait = 0, pos = 0, recv_cnt = 0, recvpos = 0, last_recv_cnt = 0;
    std::vector<double> T;
    char buf[PACKET_SIZE];
    int lastpos = 0,lastcnt = 0;
    all =0;
    while(recv_Byte < (1ll<<33)) {
        if(recv_wait < cq_size) post_recv(s_ctx.buf + pos, PACKET_SIZE+40, s_ctx.mr->lkey, s_ctx.qp, 1), pos = (pos + PACKET_SIZE + 40) % BUF_SIZE, recv_wait++;
        ibv_poll_cq_attr cq_attr{};
        int ret = ibv_start_poll(s_ctx.cq_ex, &cq_attr);
        if(ret == ENOENT)continue;
        int wc_num = 1;
        recv_wait -= wc_num;

        all += wc_num;

        if(wc_num>0) {
            if(!start) {
                start = timer = ibv_wc_read_completion_wallclock_ns(s_ctx.cq_ex);
            }
            else if(recv_cnt - last_recv_cnt >= 4){
                uint64_t now = ibv_wc_read_completion_wallclock_ns(s_ctx.cq_ex);
                T.push_back((now - timer)/1e3);
                last_recv_cnt = recv_cnt;
                timer = now;
            }
            if(ibv_wc_read_opcode(s_ctx.cq_ex)==IBV_WC_RECV){
                recv_Byte += PACKET_SIZE, recv_cnt++;
                memcpy(buf, s_ctx.buf+recvpos+40, PACKET_SIZE);
                recvpos=(recvpos+PACKET_SIZE+40)%BUF_SIZE;
            }
            ibv_end_poll(s_ctx.cq_ex);
        }
    }
    printf("带宽：%.2lf\n",recv_Byte/1024.0/1024/(timer-start)*1e9);//计算带宽
    avg_dk+=recv_Byte/1024.0/1024/(timer-start)*1e9;
    cout<<"T size:"<<T.size()<<"  recv_cnt:"<<recv_cnt<<endl;
    double sum = 0;
    for(auto k:T){
        sum += k;
    }
    double avg = sum/T.size();
    avg_t+=avg;
    printf("平均时延：%.4lf\n",avg);
    s_ctx.DestroyContext();
    //}
    //cout<<avg_dk/20<<" "<<avg_t/20<<endl;
    return 0;
}