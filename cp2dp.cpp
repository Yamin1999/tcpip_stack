#include "graph.h"
#include "cp2dp.h"
#include "EventDispatcher/event_dispatcher.h"
#include "Layer3/rt_table/np_rt_table.h"
#include "LinuxMemoryManager/uapi_mm.h"

static void 
cp2dp_task_handler  (event_dispatcher_t *ev_dis,  void *arg, uint32_t arg_size) {

    // This function is the task handler for the task submitted to the Data Path (DP)
    // The task handler is called when the task is executed by the DP's event dispatcher
    // The task handler is called with the task data as arg

    dp_msg_t *dp_msg = (dp_msg_t *)arg;
    node_t *node = (node_t *)ev_dis->app_data;

    switch (dp_msg->component_type) {

        case RT_TABLE_IPV4:
            np_rt_table_process_msg (node, dp_msg);
            break;
        default:
            break;
    }
}

void 
cp2dp_submit (node_t *node, dp_msg_t *dp_msg, bool async) {

    // This function is used to submit a task to the DP
    // The task is submitted to the DP's event dispatcher
    // The task is submitted with the highest priority
    // The task is submitted as a one-shot task
    // The task is submitted with the task data as arg
    // The task data size is arg_size

    // Get the event dispatcher of the DP
    if (async) {
            task_create_new_job(EV_DP(node), (void *)dp_msg,
                cp2dp_task_handler, 
                TASK_ONE_SHOT, 
                TASK_PRIORITY_CP_TO_DP);
    }
    else {
        task_create_new_job_synchronous(EV_DP(node), (void *)dp_msg,
                cp2dp_task_handler, 
                TASK_ONE_SHOT, 
                TASK_PRIORITY_CP_TO_DP);
    }
}

dp_msg_t *
cp2dp_msg_alloc (node_t *node, uint32_t data_size) {

    return (dp_msg_t *)XCALLOC_BUFF (NULL, (sizeof(dp_msg_t) + data_size));
}

void
cp2dp_msg_free (node_t *node, dp_msg_t *dp_msg) {
    
        XFREE (dp_msg);
}