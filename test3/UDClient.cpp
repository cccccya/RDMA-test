#include "rdma.h"
#include <chrono>

using std::cerr;
using std::endl;
using std::cout;

//10个qp分别给server发送
constexpr int qp_num = 10;

static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}

struct ClientContext {
    RdmaDeviceInfo  dev_info;     /* 设备信息 */
    ibv_pd *pd;                  /* 保护域 */
    ibv_cq *cq;
    ibv_qp *qp[qp_num];
    ibv_mr *mr;
    ibv_ah *ah;
    char *buf;                   /* 用于存储数据的缓冲区 */
    RdmaUDConnExchangeInfo remote_info;//主机字节序存放
    RdmaUDConnExchangeInfo local_info;//网络字节序存放

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

        /* 创建10个QP */
        for(int i = 0; i < qp_num; ++i) {
            qp[i] = CreateQP(pd, cq, cq, IBV_QPT_UD);
            if (qp == nullptr) {
                cerr << "Create qp failed" << endl;
                exit(0);
            }
        }
    }
    void DestroyContext() {
        for(int i = 0; i < qp_num; ++i) {
            if (qp[i] != nullptr) {
                ibv_destroy_qp(qp[i]);
                qp[i] = nullptr;
            }
        }
        ibv_destroy_cq(cq);
        ibv_destroy_ah(ah);
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(dev_info.ib_ctx);
    }

    void Client(const char* serverIP,int num) {
        sockaddr_in server_addr;

        int sockfd = -1;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0) {
            cerr << "socket error" << endl;
            exit(0);
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr(serverIP);
        server_addr.sin_port = htons(config.tcp_port+num);

        if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr))<0) {
            close(sockfd);
            cerr << "connect error" <<endl;
            exit(0);
        }

        RdmaUDConnExchangeInfo tmp_info;
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

        //本测试中 对侧不用发送过来 随意填写一个
        local_info.qpn = htonl(qp[0]->qp_num);

        local_info.lid = htons(dev_info.port_attr.lid);
        local_info.psn = htonl(lrand48() & 0xffffff);
        local_info.gid = my_gid;
        local_info.gid_idx = htonl(3);//magic
        /*cout<<"addr:"<< (uintptr_t)buf << endl;
        cout<<"qkey:"<< 0x11111111 << endl;
        cout<<"qpn:"<< qp->qp_num << endl;
        cout<<"lid:"<< dev_info.port_attr.lid << endl;
        cout<<"psn:"<< ntohl(local_info.psn) << endl;
        cout<<"client:"<<RdmaGid2Str(local_info.gid)<<endl;*/
        if(sock_sync_data(sockfd, sizeof(RdmaUDConnExchangeInfo), (char *)&local_info, (char *)&tmp_info) < 0) {
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
        cout<<"server:"<<RdmaGid2Str(remote_info.gid)<<endl;*/
    }
} s_ctx;

int main(int argc, char **argv) {
    char* servername = NULL;
    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        return -1;
    }

    s_ctx.CreateContext();
    s_ctx.Client(argv[1],0);
    for(int i = 0; i < qp_num; ++i) {
        if(modify_qp_to_init(s_ctx.qp[i], s_ctx.remote_info)) {
            exit(0);
        }
        if(modify_qp_to_rtr(s_ctx.qp[i], s_ctx.remote_info)) {
            exit(0);
        }
        if(modify_qp_to_rts(s_ctx.qp[i], s_ctx.remote_info)) {
            exit(0);
        }
    }

    /* 创建AH */
    s_ctx.ah = CreateAH(s_ctx.pd, config.ib_port, 0, s_ctx.remote_info, 3);
    if(s_ctx.ah == nullptr) {
        cerr << "Create ah failed" << endl;
        exit(0);
    }

    cout<<"finish connect"<<endl;
    int all = 0, num = 0;
    ibv_wc wc[cq_size]{};
    sleep(1);
    for(long long i = 0; i < 10; ++i) {
        for(int number = 0; number < qp_num; ++number) {
            string str = "QP"+std::to_string(number)+"Mes"+std::to_string(i);
            cout<<str<<endl;
            strcpy(s_ctx.buf+num*10,str.c_str());
            if(post_send(s_ctx.buf+num*10, PACKET_SIZE, s_ctx.mr->lkey, s_ctx.qp[number], 2, IBV_WR_SEND, s_ctx.ah, s_ctx.remote_info)) {
                cerr << "send失败" <<endl;
                exit(0);
            }
            num++;
            int wc_num = ibv_poll_cq(s_ctx.cq, cq_size, wc);
            all += wc_num;
        }
    }
    while(all < 10*qp_num) all+=ibv_poll_cq(s_ctx.cq, cq_size, wc);
    cout<<"finish send"<<endl;
    s_ctx.DestroyContext();
    return 0;
}