#ifndef RDMA_H
#define RDMA_H

#include <infiniband/verbs.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <byteswap.h>

constexpr int cq_size = 100;
constexpr int BUF_SIZE = 1024*1024;

struct config_t {
    const char *dev_name;   /* IB设备名称 */
    char *server_name;      /* 服务器主机名 */
    u_int32_t tcp_port;     /* 服务器TCP端口 */
    int ib_port;            /* 本地IB端口 */
    int gid_idx;            /* GID索引 */
};

constexpr config_t config = {
    "mlx5_0",   /* dev_name */
    nullptr,   /* server_name */
    19875,  /* tcp_port */
    1,      /* ib_port */
    3      /* gid_idx */
};

struct RdmaDeviceInfo {
    ibv_context *ib_ctx;         /* 设备句柄 */
    ibv_device_attr device_attr; /* 设备属性 */
    ibv_port_attr port_attr;     /* 设备端口属性 */
};

struct RdmaConnExchangeInfo {
    uint64_t addr;        /* 缓冲区地址 */
    uint32_t rkey;        /* 远程键 */
    uint16_t lid;         /* IB端口的LID */
    uint32_t qp_num;      /* QP号 */
    //TODO: 改为 union ibv_gid gid; 并添加gid_idx
    uint8_t gid[16];      /* GID */
};

RdmaDeviceInfo GetRdmaDeviceByName(const std::string &device_name);
ibv_qp *CreateQP(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq, ibv_qp_type qp_type);
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data);
int modify_qp_to_init(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info);
int modify_qp_to_rtr(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info);
int modify_qp_to_rts(struct ibv_qp *qp, RdmaConnExchangeInfo remote_info);
int post_recv(const void *buf, uint32_t	len, uint32_t lkey, ibv_qp *qp, int wr_id);
int post_send(const void *buf, uint32_t	len, uint32_t lkey, ibv_qp *qp, int wr_id, enum ibv_wr_opcode opcode, uint64_t remote_addr, uint32_t remote_rkey);
int poll_completion(ibv_cq *cq);
#endif // RDMA_H