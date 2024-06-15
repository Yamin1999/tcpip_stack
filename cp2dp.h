#ifndef __CP2DP__
#define __CP2DP__

#include <stdint.h>

typedef struct node_ node_t;

typedef enum DP_COMPONENT_TYPE_ {

    RT_TABLE_IPV4

} DP_COMPONENT_TYPE_T;

typedef enum DP_OPR_TYPE_ {

    DP_CREATE,
    DP_DEL,
    DP_UPDATE,
    DP_READ
    
} DP_OPR_TYPE_T;

typedef struct dp_msg_ {

    DP_COMPONENT_TYPE_T component_type;
    DP_OPR_TYPE_T opr_type;
    uint16_t flags;
    uint32_t data_size;
    uint8_t data[0];

} dp_msg_t;

void 
cp2dp_submit (node_t *node, dp_msg_t *dp_msg, bool async);

dp_msg_t *
cp2dp_msg_alloc (node_t *node, uint32_t data_size);

void
cp2dp_msg_free (node_t *node, dp_msg_t *dp_msg);

#endif 