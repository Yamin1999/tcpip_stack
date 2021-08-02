#ifndef  __ISIS_INTF__
#define __ISIS_INTF__


typedef struct isis_intf_info_ {

    uint16_t hello_interval;

    /*  Timer to retransmit hellos out of
        the interface */
    timer_event_handle *hello_xmit_timer;

    /* stats */
    uint32_t good_hello_pkt_recvd;
    uint32_t bad_hello_pkt_recvd;
    uint32_t good_lsps_pkt_recvd;
    uint32_t bad_lsps_pkt_recvd;
    uint32_t lsp_pkt_sent;
    uint32_t hello_pkt_sent;
    /* intf cost */
    uint32_t cost;

    /* Adj list on this interface */
    glthread_t adj_list_head;
    glthread_t purge_glue;
    glthread_t lsp_xmit_list_head;

    task_t *lsp_xmit_job;

} isis_intf_info_t;
GLTHREAD_TO_STRUCT(isis_purge_glue_to_isis_intf_info,
        isis_intf_info_t, purge_glue);


/* Some short-hand macros to make life easy */
#define ISIS_INTF_INFO(intf_ptr)    \
    ((isis_intf_info_t *)((intf_ptr)->intf_nw_props.isis_intf_info))
#define ISIS_INTF_HELLO_XMIT_TIMER(intf_ptr)  \
    (((isis_intf_info_t *)((intf_ptr)->intf_nw_props.isis_intf_info))->hello_xmit_timer)
#define ISIS_INTF_COST(intf_ptr) \
    (((isis_intf_info_t *)((intf_ptr)->intf_nw_props.isis_intf_info))->cost)
#define ISIS_INTF_HELLO_INTERVAL(intf_ptr) \
    (((isis_intf_info_t *)((intf_ptr)->intf_nw_props.isis_intf_info))->hello_interval)
#define ISIS_INTF_ADJ_LST_HEAD(intf_ptr) \
    (&(((isis_intf_info_t *)((intf_ptr)->intf_nw_props.isis_intf_info))->adj_list_head))
#define ISIS_INCREMENT_STATS(intf_ptr, pkt_type)  \
    (((ISIS_INTF_INFO(intf_ptr))->pkt_type)++)


bool
isis_node_intf_is_enable(interface_t *intf) ;

void
isis_enable_protocol_on_interface(interface_t *intf);

void
isis_disable_protocol_on_interface(interface_t *intf);

void
isis_start_sending_hellos(interface_t *intf) ;

void
isis_stop_sending_hellos(interface_t *intf);

void
isis_refresh_intf_hellos(interface_t *intf);

void
isis_show_interface_protocol_state(interface_t *intf);

void
isis_interface_updates(void *arg, size_t arg_size);

void 
isis_check_and_delete_intf_info(interface_t *intf);


#endif // ! __ISIS_INTF__