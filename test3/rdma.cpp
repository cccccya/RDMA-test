#include "rdma.h"
#include <string>

using std::string;
using std::cerr;
using std::endl;
using std::cout;

string RdmaGid2Str(ibv_gid gid) {
  string res;
  for (unsigned char i : gid.raw) {
    char s[3];
    sprintf(s, "%02X", i);
    res += string(s);
  }
  return res;
}
char get_xdigit(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}
ibv_gid RdmaStr2Gid(string s) {
  union ibv_gid gid;
  for (int32_t i = 0; i < 16; i++) {
    unsigned char x;
    x = get_xdigit(s[i * 2]);
    gid.raw[i] = x << 4;
    x = get_xdigit(s[i * 2 + 1]);
    gid.raw[i] |= x;
  }
  return gid;
}

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

ibv_ah *CreateAH(ibv_pd *pd, int port, int sl, RdmaUDConnExchangeInfo dest, int sgid_idx) {
    ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.dlid = dest.lid;
    ah_attr.sl = sl;
    ah_attr.is_global = 0;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = port;
    if (dest.gid.global.interface_id) {
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = dest.gid;
        ah_attr.grh.sgid_index = sgid_idx;
    }
    return ibv_create_ah(pd, &ah_attr);
}

    ibv_qp_init_attr qp_init_attr;
ibv_qp *CreateQP(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq, ibv_qp_type qp_type) {
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = qp_type;
    qp_init_attr.sq_sig_all = 1; // 向所有 WR 发送信号
    qp_init_attr.send_cq = send_cq; // 发送完成队列
    qp_init_attr.recv_cq = recv_cq; // 接收完成队列
    qp_init_attr.cap.max_send_wr = cq_size; // 最大发送 WR 数量
    qp_init_attr.cap.max_recv_wr = cq_size; // 最大接收 WR 数量
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

int modify_qp_to_init(struct ibv_qp *qp, RdmaUDConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(ibv_qp_attr));

    attr.qp_state = IBV_QPS_INIT;
    attr.qkey = 0x11111111;
    attr.pkey_index = 0;
    attr.port_num = config.ib_port;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret != 0) {
        printf("ibv_modify_qp to INIT failed %d", ret);
    }
    return ret;
}
int modify_qp_to_rtr(struct ibv_qp *qp, RdmaUDConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(ibv_qp_attr));
    attr.qp_state = IBV_QPS_RTR;
    flags = IBV_QP_STATE;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc) {
        cerr << "无法修改 QP 状态为 RTR"<<strerror(errno) <<endl;
    }
    return rc;
}
int modify_qp_to_rts(struct ibv_qp *qp, RdmaUDConnExchangeInfo remote_info) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = remote_info.psn;
    flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
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

int post_send(const void *buf, uint32_t	len, uint32_t lkey, ibv_qp *qp, int wr_id, enum ibv_wr_opcode opcode, ibv_ah* local_ah, RdmaUDConnExchangeInfo remote_info) {
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
    wr.send_flags = IBV_WR_SEND;
    wr.wr.ud.ah = local_ah; //本地ah存放了对侧信息
    wr.wr.ud.remote_qkey = remote_info.qkey;
    wr.wr.ud.remote_qpn = remote_info.qpn;
    ret = ibv_post_send(qp, &wr, &bad_wr);
    if(ret) cout << ret <<endl;
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