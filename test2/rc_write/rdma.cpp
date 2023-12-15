#include "rdma.h"
#include <string>

using std::string;
using std::cerr;
using std::endl;
using std::cout;

RdmaDeviceInfo GetRdmaDeviceByName(const string &device_name) {
    int num_devices;
    ibv_device *ib_dev = nullptr;
    ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if(dev_list == nullptr) {
        printf("Failed to get RDMA device list.\n");
        return {};
    }
    RdmaDeviceInfo ret;
    for(int i = 0; i < num_devices; i++) {
        /* 找到指定设备 */
        if(!strcmp(ibv_get_device_name(dev_list[i]), device_name.c_str())) {
            ib_dev = dev_list[i];
            break;
        }
    }
    if(ib_dev == nullptr) {
        printf("RDMA device %s not found.\n", device_name.c_str());
        return {};
    }
    ret.ib_ctx = ibv_open_device(ib_dev);
    if(ret.ib_ctx == nullptr) {
        printf("Failed to open RDMA device %s.\n", device_name.c_str());
        return {};
    }
    /* 查询端口属性 */
    ibv_query_port(ret.ib_ctx, config.ib_port, &ret.port_attr);
    ibv_free_device_list(dev_list);
    dev_list = nullptr;
    ib_dev = nullptr;
    return ret;
}

ibv_qp *CreateQP(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq, ibv_qp_type qp_type) {
    ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = qp_type; // RC类型
    qp_init_attr.sq_sig_all = 1; // 向所有 WR 发送信号
    qp_init_attr.send_cq = send_cq; // 发送完成队列
    qp_init_attr.recv_cq = recv_cq; // 接收完成队列
    qp_init_attr.cap.max_send_wr = 10; // 最大发送 WR 数量
    qp_init_attr.cap.max_recv_wr = 10; // 最大接收 WR 数量
    qp_init_attr.cap.max_send_sge = 1; // 最大发送 SGE 数量
    qp_init_attr.cap.max_recv_sge = 1; // 最大接收 SGE 数量
    return ibv_create_qp(pd, &qp_init_attr);
}

int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data) {
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;
    rc = write(sock, local_data, xfer_size);
    if(rc < xfer_size) {
        fprintf(stderr, "无法发送本地数据\n");
        return -1;
    }
    while(total_read_bytes < xfer_size) {
        read_bytes = read(sock, remote_data, xfer_size);
        if(read_bytes > 0)
            total_read_bytes += read_bytes;
        else {
            fprintf(stderr, "无法读取远程数据\n");
            return -1;
        }
    }
    return 0;
}

int modify_qp_to_init(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(ibv_qp_attr));

    attr.qp_state = IBV_QPS_INIT;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                            IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_REMOTE_WRITE;
    int flags = flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    attr.pkey_index = 0;
    attr.port_num = config.ib_port;

    int ret = ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS);
    if (ret != 0) {
        printf("ibv_modify_qp to INIT failed %d", ret);
    }
    return ret;
}
int modify_qp_to_rtr(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(ibv_qp_attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_info.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote_info.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = config.ib_port;
    if(config.gid_idx >= 0) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, remote_info.gid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc) {
        cerr << "无法修改 QP 状态为 RTR"<<strerror(errno) <<endl;
    }
    return rc;
}
int modify_qp_to_rts(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(qp, &attr, flags); // 使用 ibv_modify_qp 函数修改 QP 的属性
    if(rc){
        cerr << "将 QP 状态修改为 RTS 失败" <<endl;
    }
    return rc;
}

int post_recv(const void *buf, uint32_t	len, uint32_t lkey, ibv_qp *qp, int wr_id) {
    int ret = 0;
    struct ibv_recv_wr *bad_wr;

    struct ibv_sge list;
    memset(&list, 0, sizeof(ibv_sge));
    list.addr = reinterpret_cast<uintptr_t>(buf);
    list.length = len;
    list.lkey = lkey;

    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(ibv_recv_wr));
    wr.wr_id = wr_id;
    wr.next = NULL;
    wr.sg_list = &list;
    wr.num_sge = 1;

    ret = ibv_post_recv(qp, &wr, &bad_wr);
    return ret;
}

int post_send(const void *buf, uint32_t	len, uint32_t lkey, ibv_qp *qp, int wr_id, enum ibv_wr_opcode opcode, uint64_t remote_addr, uint32_t remote_rkey) {
    int ret = 0;
    struct ibv_send_wr *bad_wr;

    struct ibv_sge list;
    memset(&list, 0, sizeof(ibv_sge));
    list.addr = reinterpret_cast<uintptr_t>(buf);
    list.length = len;
    list.lkey = lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(ibv_send_wr));
    wr.wr_id = wr_id;
    wr.next = NULL;
    wr.sg_list = &list;
    wr.num_sge = 1;
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    if(opcode != IBV_WR_SEND) {
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = remote_rkey;
    }
    ret = ibv_post_send(qp, &wr, &bad_wr);
    return ret;
}

int poll_completion(ibv_cq *cq) {
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    int rc = 0;
    /* 获取当前时间 */
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    do {
        poll_result = ibv_poll_cq(cq, 1, &wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while((poll_result == 0) && ((cur_time_msec - start_time_msec) < 5000));
    if(poll_result < 0) {
        fprintf(stderr, "无法完成完成队列\n");
        rc = 1;
    } else if(poll_result == 0) {
        fprintf(stderr, "完成队列超时\n");
        rc = 1;
    } else {
        //fprintf(stdout, "完成队列完成\n");
        /* 检查完成的操作是否成功 */
        if(wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "完成的操作失败，错误码=0x%x，vendor_err=0x%x\n", wc.status, wc.vendor_err);
            rc = 1;
        }
    }
    return rc;
}