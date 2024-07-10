#include "../gluethread/glthread.h"
#include "../BitOp/bitsop.h"
#include "../Threads/refcount.h"
#include "../graph.h"
#include "../notif.h"
#include "rt_notif.h"
#include "rt_table/nexthop.h"
#include "layer3.h"
#include "../LinuxMemoryManager/uapi_mm.h"
#include "../Tracer/tracer.h"

void
rt_table_add_route_to_notify_list (
                rt_table_t *rt_table, 
                l3_route_t *l3route,
                uint8_t flag) {

    char buffer[48];
    uint8_t old_flags = l3route->rt_flags;

    l3_route_inc_ref_count (l3route);

    if (!IS_GLTHREAD_LIST_EMPTY(&l3route->notif_glue)) {
        remove_glthread(&l3route->notif_glue);
        l3_route_dec_ref_count(l3route);
    }

    UNSET_BIT8(l3route->rt_flags, RT_ADD_F);
    UNSET_BIT8(l3route->rt_flags, RT_DEL_F);
    UNSET_BIT8(l3route->rt_flags, RT_UPDATE_F);

    if (IS_BIT_SET(old_flags, RT_DEL_F) &&
         IS_BIT_SET(flag, RT_ADD_F)) {
            SET_BIT(l3route->rt_flags, RT_UPDATE_F);
    } else {
            SET_BIT(l3route->rt_flags, flag);
    }
    glthread_add_next(&rt_table->rt_notify_list_head, &l3route->notif_glue);
    tracer (rt_table->node->dptr, DIPC_DET, "Route %s/%d : Added to notify list with flags %s\n", 
        l3route->dest, l3route->mask, RT_FLAGS_STR(l3route->rt_flags, buffer, sizeof(buffer)));
    l3_route_inc_ref_count (l3route);
    l3_route_dec_ref_count(l3route);
}

static void
rt_table_notif_job_cb(event_dispatcher_t *ev_dis, void *arg, uint32_t arg_size) {

    glthread_t *curr;
    l3_route_t *l3route;
    rt_table_t *rt_table = (rt_table_t *)arg;
    
    rt_table->notif_job = NULL;

    rt_route_notif_data_t rt_route_notif_data;

    /* Start Sending Notifications Now */
    ITERATE_GLTHREAD_BEGIN_REVERSE(&rt_table->rt_notify_list_head, curr) {

        l3route = notif_glue_to_l3_route(curr);
        rt_route_notif_data.l3route = l3route;
        rt_route_notif_data.node = rt_table->node;

        tracer (rt_table->node->dptr, DIPC_DET, "Route %s/%d : Notification Invoked\n",
            l3route->dest, l3route->mask);

        nfc_invoke_notif_chain(NULL,
                                               &rt_table->nfc_rt_updates, 
                                               &rt_route_notif_data,
                                               sizeof(rt_route_notif_data), 0, 0,
                                               TASK_PRIORITY_COMPUTE);

        remove_glthread(&l3route->notif_glue);
        if (l3_route_dec_ref_count(l3route) == 0) {
           continue;
        }

        if ( IS_BIT_SET(l3route->rt_flags, RT_DEL_F) ) {
                continue;
        }

        UNSET_BIT8(l3route->rt_flags, RT_ADD_F);
        UNSET_BIT8(l3route->rt_flags, RT_UPDATE_F);

    } ITERATE_GLTHREAD_END_REVERSE(&rt_table->rt_notify_list_head, curr)
}

void
rt_table_kick_start_notif_job(rt_table_t *rt_table) {

    if (rt_table->notif_job) return;
    rt_table->notif_job = task_create_new_job(
                                        EV(rt_table->node),
                                        rt_table, 
                                        rt_table_notif_job_cb,
                                        TASK_ONE_SHOT,
                                        TASK_PRIORITY_COMPUTE);
                                        
    tracer (rt_table->node->dptr, DIPC, "Routing Table Notification Job Scheduled\n");
}

void
nfc_ipv4_rt_subscribe (node_t *node, nfc_app_cb cbk) {

    notif_chain_elem_t nfce_template;

    memset(&nfce_template, 0, sizeof(notif_chain_elem_t));
    nfce_template.app_cb = cbk;

    nfc_register_notif_chain(&NODE_RT_TABLE(node)->nfc_rt_updates,
                                            &nfce_template);
}

void
nfc_ipv4_rt_un_subscribe (node_t *node, nfc_app_cb cbk) {

    notif_chain_elem_t nfce_template;

    memset(&nfce_template, 0, sizeof(notif_chain_elem_t));
    nfce_template.app_cb = cbk;

    nfc_de_register_notif_chain(&node->node_nw_prop.rt_table->nfc_rt_updates,
                                                    &nfce_template);
}

/* Begin : Route flash on Application Demand */

typedef struct flash_data_ {

    rt_table_t *rt_table;
    nfc_app_cb cbk;
} flash_data_t;

static void
 rt_table_purge_flash_route_queue(rt_table_t *rt_table) {

     glthread_t *curr;
     l3_route_t *l3route;

     ITERATE_GLTHREAD_BEGIN(&rt_table->rt_flash_list_head, curr) {

            l3route = flash_glue_to_l3_route(curr);
            UNSET_BIT8(l3route->rt_flags, RT_FLASH_REQ_F);
            remove_glthread(&l3route->flash_glue);
            l3_route_dec_ref_count(l3route);

     } ITERATE_GLTHREAD_END(&rt_table->rt_flash_list_head, curr)

    tracer (rt_table->node->dptr, DIPC, "Flash Route Queue of RIB is Purged\n");
 }

static void
rt_table_process_one_flash_client (rt_table_t *rt_table,  nfc_app_cb cbk) {

    glthread_t *curr;
    l3_route_t *l3route;
    rt_route_notif_data_t route_notif_data;

    ITERATE_GLTHREAD_BEGIN_REVERSE (&rt_table->rt_flash_list_head, curr) {

        l3route = flash_glue_to_l3_route(curr);
        route_notif_data.l3route = l3route;
        route_notif_data.node = rt_table->node;
        cbk(NULL, &route_notif_data, sizeof(route_notif_data));

    }ITERATE_GLTHREAD_END_REVERSE (&rt_table->rt_flash_list_head, curr) 
}

static void
rt_table_flash_job (event_dispatcher_t *ev_dis, void *arg, uint32_t arg_size) {

    glthread_t *curr;
    l3_route_t *l3route;
    rt_route_flash_request_t *flash_req;

    rt_table_t *rt_table = (rt_table_t *)arg;

    rt_table->flash_job = NULL;

    curr = dequeue_glthread_first(&rt_table->flash_request_list_head);

    flash_req = glue_to_route_flash_request(curr);
    rt_table_process_one_flash_client (rt_table, flash_req->cbk);
    XFREE (flash_req);

    if (!IS_GLTHREAD_LIST_EMPTY(&rt_table->flash_request_list_head)) {
        rt_table->flash_job = task_create_new_job(EV(rt_table->node), 
                                                  rt_table,
                                                  rt_table_flash_job, TASK_ONE_SHOT,
                                                  TASK_PRIORITY_COMPUTE);
        tracer (rt_table->node->dptr, DIPC, "Flash Route Queue Job of RIB is Triggered\n");
    }
    else {
        rt_table_purge_flash_route_queue(rt_table);
    }
}

static void
rt_table_add_route_to_flash_list (rt_table_t *rt_table,
                                                      l3_route_t *l3route) {
    
    l3_route_inc_ref_count(l3route);

    if (!IS_GLTHREAD_LIST_EMPTY(&l3route->flash_glue)) {
        remove_glthread (&l3route->flash_glue);
        l3_route_dec_ref_count(l3route);
    }

    SET_BIT(l3route->rt_flags, RT_FLASH_REQ_F);
    glthread_add_next(&rt_table->rt_flash_list_head, &l3route->flash_glue);
    tracer (rt_table->node->dptr, DIPC_DET, "Route %s/%d : Route added to Flash Route Queue of RIB\n",
        l3route->dest, l3route->mask);
    l3_route_inc_ref_count(l3route);
    l3_route_dec_ref_count(l3route);
}

static void
 rt_table_kick_start_flash_job(rt_table_t *rt_table) {

    glthread_t *curr;
    l3_route_t *l3route;
    mtrie_node_t *mnode;
    
     if (rt_table->flash_job) return;

     ITERATE_GLTHREAD_BEGIN(&rt_table->route_list.list_head, curr) {

        mnode = list_glue_to_mtrie_node(curr);
        l3route = (l3_route_t *)mnode->data;
        rt_table_add_route_to_flash_list (rt_table, l3route);

    } ITERATE_GLTHREAD_END(&rt_table->route_list, curr)

    if (! rt_table->flash_job) {
        rt_table->flash_job = task_create_new_job( 
                                                EV(rt_table->node) ,
                                                rt_table,
                                                rt_table_flash_job,
                                                TASK_ONE_SHOT,
                                                TASK_PRIORITY_COMPUTE);
        tracer (rt_table->node->dptr, DIPC, "Flash Route Queue Job of RIB is Scheduled\n");
    }
 }

void
nfc_ipv4_rt_request_flash (node_t *node, nfc_app_cb cbk) {

    rt_route_flash_request_t *flash_req =
        (rt_route_flash_request_t *)XCALLOC(0, 1, rt_route_flash_request_t);

    flash_req->cbk = cbk;
    init_glthread(&flash_req->glue);

    glthread_add_last(&(NODE_RT_TABLE(node)->flash_request_list_head), &flash_req->glue);
    tracer (node->dptr, DIPC, "IPV4 route request flash is recvd\n");
    rt_table_kick_start_flash_job(NODE_RT_TABLE(node));
}

/* End : Route flash on Application Demand */

void
nfc_ipv4_rt_subscribe_per_route (node_t *node, uint32_t ip, uint8_t mask) {

}

void
nfc_ipv4_rt_un_subscribe_per_route (node_t *node, uint32_t ip, uint8_t mask) {

}
