#ifndef __NP_RT_TABLE__
#define __NP_RT_TABLE__

typedef struct dp_msg_ dp_msg_t;
typedef struct node_ node_t;

#include <stdint.h>
class Interface;

typedef struct rt_update_msg_ {

    uint32_t prefix;
    uint8_t   mask;
    uint32_t gateway;
    Interface *oif;
    uint32_t metric;
    uint16_t proto_id;

} rt_update_msg_t;

void 
np_rt_table_process_msg(node_t *node, dp_msg_t *dp_msg);

#endif 