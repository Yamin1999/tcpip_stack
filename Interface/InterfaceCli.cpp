#include <stdio.h>
#include "../CLIBuilder/cmdtlv.h"
#include "../CLIBuilder/libcli.h"
#include "../cmdcodes.h"
#include "../utils.h"
#include "../tcpip_notif.h"
#include "../graph.h"
#include "InterfaceUApi.h"

extern graph_t *topo;
extern void gre_cli_config_tree (param_t *interface);
extern void tcp_ip_traceoptions_cli(
                                param_t *node_name_param, 
                                 param_t *intf_name_param);
extern void 
config_interface_build_transport_svc_cli_tree (
    param_t *node_name_param,
    param_t *param);

extern int validate_mask_value(Stack_t *tlv_stack, c_string mask_str);

static int
validate_vlan_id(Stack_t *tlv_stack, c_string vlan_value){

    uint32_t vlan = atoi((const char *)vlan_value);
    if(!vlan){
        cprintf("Error : Invalid Vlan Value\n");
        return LEAF_VALIDATION_FAILED;
    }
    if(vlan >= 1 && vlan <= 4095)
        return LEAF_VALIDATION_SUCCESS;

    return LEAF_VALIDATION_FAILED;
};

static int
validate_l2_mode_value(Stack_t *tlv_stack, c_string l2_mode_value){

    if((string_compare(l2_mode_value, "access", strlen("access")) == 0) || 
        (string_compare(l2_mode_value, "trunk", strlen("trunk")) == 0))
        return LEAF_VALIDATION_SUCCESS;
    return LEAF_VALIDATION_FAILED;
}

static int
validate_interface_metric_val(Stack_t *tlv_stack, c_string  value){

    uint32_t metric_val = atoi((const char *)value);
    if(metric_val > 0 && metric_val <= INTF_MAX_METRIC)
        return LEAF_VALIDATION_SUCCESS;
    return LEAF_VALIDATION_FAILED;
}

static int 
validate_if_up_down_status(Stack_t *tlv_stack, c_string value){

    if(string_compare(value, "up", strlen("up")) == 0 ) {
        return LEAF_VALIDATION_SUCCESS;
    }
    else if(string_compare(value, "down", strlen("down")) == 0) {
        return LEAF_VALIDATION_SUCCESS;
    }
    return LEAF_VALIDATION_FAILED;
}

/*Display Node Interfaces*/
void
display_node_interfaces (param_t *param, Stack_t *tlv_stack){

    node_t *node;
    c_string node_name = NULL;
    tlv_struct_t *tlv = NULL;

    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;

    }TLV_LOOP_END;

    node = node_get_node_by_name(topo, node_name);
    
    int i = 0;
    Interface *intf;

    for(; i < MAX_INTF_PER_NODE; i++){

        intf = node->intf[i];
        if(!intf) continue;

        printw (" %s\n", intf->if_name.c_str());
    }
}


static int
intf_config_handler(int cmdcode, Stack_t *tlv_stack,
                    op_mode enable_or_disable){

   node_t *node;
   c_string intf_name = NULL;
   c_string node_name = NULL;
   vlan_id_t vlan_id;
   uint8_t mask;
   uint8_t lono;
   c_string l2_mode_option;
   c_string if_up_down;
   tlv_struct_t *tlv = NULL;
   c_string intf_ip_addr = NULL;
   Interface *interface = NULL;
   uint32_t intf_new_matric_val;
   intf_prop_changed_t intf_prop_changed;
   
    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if     (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;
        else if(parser_match_leaf_id(tlv->leaf_id, "if-name"))
            intf_name = tlv->value;
        else if(parser_match_leaf_id(tlv->leaf_id, "vlan-id"))
            vlan_id = atoi((const char *)tlv->value);
        else if(parser_match_leaf_id(tlv->leaf_id, "l2-mode-val"))
            l2_mode_option = tlv->value;
        else if(parser_match_leaf_id(tlv->leaf_id, "if-up-down"))
             if_up_down = tlv->value; 
        else if(parser_match_leaf_id(tlv->leaf_id, "metric-val"))
             intf_new_matric_val = atoi((const char *)tlv->value);      
        else if(parser_match_leaf_id(tlv->leaf_id, "intf-ip-address"))
             intf_ip_addr = tlv->value;     
        else if(parser_match_leaf_id(tlv->leaf_id, "mask"))
             mask = atoi((const char *)tlv->value);  
        else if(parser_match_leaf_id(tlv->leaf_id, "lono"))
             lono = atoi((const char *)tlv->value);  
        else if(parser_match_leaf_id(tlv->leaf_id, ""))
             lono = atoi((const char *)tlv->value);               
    } TLV_LOOP_END;

    node = node_get_node_by_name(topo, node_name);
    if (intf_name) {
        interface = node_get_intf_by_name(node, (const char *)intf_name);
    }

    uint32_t if_change_flags = 0;

    switch(cmdcode){

        case CMDCODE_INTF_CONFIG_METRIC:
        {
            uint32_t intf_existing_metric = interface->GetIntfCost();

            if(intf_existing_metric != intf_new_matric_val){
                SET_BIT(if_change_flags, IF_METRIC_CHANGE_F); 
                intf_prop_changed.intf_metric = intf_existing_metric;
            }

            switch(enable_or_disable){
                case CONFIG_ENABLE:
                    interface->cost = intf_new_matric_val;        
                break;
                case CONFIG_DISABLE:
                    interface->cost = INTF_METRIC_DEFAULT;
                break;
                default: ;
            }
            if(IS_BIT_SET(if_change_flags, IF_METRIC_CHANGE_F)){
				nfc_intf_invoke_notification_to_sbscribers(
					interface, &intf_prop_changed, if_change_flags);
            }
        }    
        break;
        case CMDCODE_CONF_INTF_UP_DOWN:
            if(string_compare(if_up_down, "up", strlen("up")) == 0){
                if(interface->is_up == false){
                    SET_BIT(if_change_flags, IF_UP_DOWN_CHANGE_F); 
                     intf_prop_changed.up_status = false;
                }
                interface->is_up = true;
            }
            else{
                if(interface->is_up){
                    SET_BIT(if_change_flags, IF_UP_DOWN_CHANGE_F);
                     intf_prop_changed.up_status = true;
                }
                interface->is_up = false;
            }
            if(IS_BIT_SET(if_change_flags, IF_UP_DOWN_CHANGE_F)){
				nfc_intf_invoke_notification_to_sbscribers(
					interface, &intf_prop_changed, if_change_flags);
            }
            break;
        case CMDCODE_INTF_CONFIG_SWITCHPORT:
            intf_prop_changed.is_switchport = interface->GetSwitchport();
            switch (enable_or_disable)
            {
                case CONFIG_ENABLE:
                    interface->SetSwitchport(true);
                    break;
                case CONFIG_DISABLE:
                    interface->SetSwitchport(false);
                    break;
                default:;
            }
            if (intf_prop_changed.is_switchport != interface->GetSwitchport())
            {
                SET_BIT(if_change_flags, IF_OPER_MODE_CHANGE_F);
                nfc_intf_invoke_notification_to_sbscribers(
					interface, &intf_prop_changed, if_change_flags);
            }
            break;
        case CMDCODE_INTF_CONFIG_VLAN:
            intf_prop_changed.access_vlan = interface->GetVlanId();
            switch(enable_or_disable){
                case CONFIG_ENABLE:
                    interface->IntfConfigVlan(vlan_id, true);
                    break;
                case CONFIG_DISABLE:
                    interface->IntfConfigVlan(vlan_id, false);
                    break;
                default:
                    ;
            }
            if (intf_prop_changed.access_vlan  !=
                     interface->GetVlanId()) {
                SET_BIT(if_change_flags, IF_VLAN_MEMBERSHIP_CHANGE_F);
                nfc_intf_invoke_notification_to_sbscribers(
					interface, &intf_prop_changed, if_change_flags);
            }
            break;
        case CMDCODE_INTF_CONFIG_IP_ADDR:
             switch(enable_or_disable){
                case CONFIG_ENABLE:
                    interface_set_ip_addr(node, interface, intf_ip_addr, mask);
                    break;
                case CONFIG_DISABLE:
                    interface_unset_ip_addr(node, interface);
                    break;
                default:
                    ;
            }
            break;
        case CMDCODE_INTF_CONFIG_LOOPBACK:
            switch(enable_or_disable){
                case CONFIG_ENABLE:
                    break;
                case CONFIG_DISABLE:
                    break;
                default:
                    ;
            }
        break;

        case CMDCODE_CONFIG_INTF_VLAN_CREATE:
            switch (enable_or_disable)
            {
            case CONFIG_ENABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (vlan_intf)
                    return 0;
                vlan_intf = new VlanInterface(vlan_id);
                vlan_intf->att_node = node;
                if (!node->vlan_intf_db) {
                    node->vlan_intf_db = new std::unordered_map<uint16_t, VlanInterface *>;
                }
                node->vlan_intf_db->insert(std::make_pair(vlan_id, vlan_intf));
            }
            break;
            case CONFIG_DISABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (!vlan_intf)
                    return 0;
                if (!vlan_intf->access_member_intf_lst.empty())
                {
                    cprintf("Error : Vlan is in use\n");
                    return -1;
                }
                node->vlan_intf_db->erase(vlan_id);
                delete vlan_intf;
            }
            break;
            default:;
            }
            break;

        case CMDCODE_CONFIG_INTF_VLAN_IP_ADDR:
            switch (enable_or_disable)
            {
            case CONFIG_ENABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (!vlan_intf)
                {
                    cprintf("Error : Vlan Interface not created\n");
                    return -1;
                }
                vlan_intf->InterfaceSetIpAddressMask(tcp_ip_covert_ip_p_to_n(intf_ip_addr), mask);
            }
            break;
            case CONFIG_DISABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (!vlan_intf)
                {
                    cprintf("Error : Vlan Interface not created\n");
                    return -1;
                }
                vlan_intf->InterfaceSetIpAddressMask(0, 0);
            }
            break;
            default:;
            }
            break;
        case CMDCODE_CONFIG_INTF_VLAN_UP_DOWN:
            switch (enable_or_disable)
            {
            case CONFIG_ENABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (!vlan_intf)
                {
                    cprintf("Error : Vlan Interface not created\n");
                    return -1;
                }
                vlan_intf->is_up = true;
            }
            break;
            case CONFIG_DISABLE:
            {
                VlanInterface *vlan_intf =
                    static_cast<VlanInterface *>(VlanInterface::VlanInterfaceLookUp(node, vlan_id));
                if (!vlan_intf)
                {
                    cprintf("Error : Vlan Interface not created\n");
                    return -1;
                }
                vlan_intf->is_up = false;
            }
            break;
            default:;
            }
            break;
        default:;
        }
        return 0;
}

static void
vlan_cli_config_tree(param_t *root)
{
        /*config node <node-name> interface vlan*/
        static param_t vlan;
        init_param(&vlan, CMD, "vlan", 0, 0, INVALID, 0, "\"vlan\" keyword");
        libcli_register_param(root, &vlan);
        {
            /*config node <node-name> interface <if-name> vlan <vlan-id>*/
            static param_t vlan_id;
            init_param(&vlan_id, LEAF, 0, intf_config_handler, validate_vlan_id, INT, "vlan-id", "vlan id(1-4096)");
            libcli_register_param(&vlan, &vlan_id);
            libcli_set_param_cmd_code(&vlan_id, CMDCODE_CONFIG_INTF_VLAN_CREATE);
            {
                /*config node <node-name> interface vlan <vlan-id> ip address <ip-addr> <mask>*/
                static param_t ip_addr;
                init_param(&ip_addr, CMD, "ip", 0, 0, INVALID, 0, "Interface IP Address");
                libcli_register_param(&vlan_id, &ip_addr);
                {
                    static param_t ip_addr_val;
                    init_param(&ip_addr_val, LEAF, 0, intf_config_handler, 0, IPV4, "intf-ip-address", "IPV4 address");
                    libcli_register_param(&ip_addr, &ip_addr_val);
                    {
                        static param_t mask;
                        init_param(&mask, LEAF, 0, intf_config_handler, validate_mask_value, INT, "mask", "mask [0-32]");
                        libcli_register_param(&ip_addr_val, &mask);
                        libcli_set_param_cmd_code(&mask, CMDCODE_INTF_CONFIG_IP_ADDR);
                    }
                }
            }
            {
                /*config node <node-name> interface vlan <vlan-id> <up|down>*/
                static param_t if_up_down_status;
                init_param(&if_up_down_status, LEAF, 0, intf_config_handler, validate_if_up_down_status, STRING, "if-up-down", "<up | down>");
                libcli_register_param(&vlan_id, &if_up_down_status);
                libcli_set_param_cmd_code(&if_up_down_status, CMDCODE_CONF_INTF_UP_DOWN);
            }
        }
}

void
Interface_config_cli_tree (param_t *root) {

            /*config node <node-name> interface*/
            static param_t interface;
            init_param(&interface, CMD, "interface", 0, 0, INVALID, 0, "\"interface\" keyword");
            libcli_register_display_callback(&interface, display_node_interfaces);
            libcli_register_param(root, &interface);
            {
                /* CLI for GRE Tunneling are mounted here*/
                gre_cli_config_tree(&interface);
                /* CLI for vlan Interfaces are mounted here*/
                vlan_cli_config_tree(&interface);
            }

            {
                /*config node <node-name> interface <if-name>*/
                static param_t if_name;
                init_param(&if_name, LEAF, 0, 0, 0, STRING, "if-name", "Interface Name");
                libcli_register_param(&interface, &if_name);
	
                {
                    /*CLI for traceoptions at interface level are hooked up here in tree */
                    tcp_ip_traceoptions_cli (0, &if_name);
                    config_interface_build_transport_svc_cli_tree (0, &if_name);
                    {
                        /*config node <node-name> interface <if-name> switchport */
                        static param_t switchport;
                        init_param(&switchport, CMD, "switchport", intf_config_handler, 0, INVALID, 0, "\"switchport\" keyword");
                        libcli_register_param(&if_name, &switchport);
                        libcli_set_param_cmd_code(&switchport, CMDCODE_INTF_CONFIG_SWITCHPORT);
                        {
                            /*config node <node-name> interface <if-name> switchport access ...*/
                            static param_t access;
                             init_param(&access, CMD, "access", intf_config_handler, 0, INVALID, 0, "\"switchport\" keyword");
                             libcli_register_param(&switchport, &access);
                            {
                                /*config node <node-name> interface <if-name> switchport access vlan <vlan-id>*/
                                static param_t vlan;
                                init_param(&vlan, CMD, "vlan", 0, 0, INVALID, 0, "\"vlan\" keyword");
                                libcli_register_param(&access, &vlan);
                                {
                                    /*config node <node-name> interface <if-name> switchport access vlan <vlan-id>*/
                                    static param_t vlan_id;
                                    init_param(&vlan_id, LEAF, 0, intf_config_handler, validate_vlan_id, INT, "vlan-id", "vlan id(1-4095)");
                                    libcli_register_param(&vlan, &vlan_id);
                                    libcli_set_param_cmd_code(&vlan_id, CMDCODE_INTF_CONFIG_VLAN);
                                    libcli_set_tail_config_batch_processing(&vlan_id);
                                }
                            }
                        }
                    }
                    {
                        /*config node <node-name> interface <if-name> <up|down>*/
                        static param_t if_up_down_status;
                        init_param(&if_up_down_status, LEAF, 0, intf_config_handler, validate_if_up_down_status, STRING, "if-up-down", "<up | down>");
                        libcli_register_param(&if_name, &if_up_down_status);
                        libcli_set_param_cmd_code(&if_up_down_status, CMDCODE_CONF_INTF_UP_DOWN);
                    }
                }
                {
                    static param_t loopback;
                    init_param(&loopback, CMD, "loopback", 0, 0, INVALID, 0, "loopback");
                    libcli_register_param(&interface, &loopback);
                    {
                        static param_t lono;
                        init_param(&lono, LEAF, 0, intf_config_handler, NULL, INT, "lono", "Loopback ID");
                        libcli_register_param(&loopback, &lono);
                        libcli_set_param_cmd_code(&lono, CMDCODE_INTF_CONFIG_LOOPBACK);
                    }
                }
                {
                    static param_t metric;
                    init_param(&metric, CMD, "metric", 0, 0, INVALID, 0, "Interface Metric");
                    libcli_register_param(&if_name, &metric);
                    {
                        static param_t metric_val;
                        init_param(&metric_val, LEAF, 0, intf_config_handler, validate_interface_metric_val, INT, "metric-val", "Metric Value(1-16777215)");
                        libcli_register_param(&metric, &metric_val);
                        libcli_set_param_cmd_code(&metric_val, CMDCODE_INTF_CONFIG_METRIC);
                    }
                }
                {
                    /* config node <node-name> ineterface <if-name> ip-address <ip-addr> <mask>*/
                    static param_t ip_addr;
                    init_param(&ip_addr, CMD, "ip-address", 0, 0, INVALID, 0, "Interface IP Address");
                    libcli_register_param(&if_name, &ip_addr);
                    {
                        static param_t ip_addr_val;
                        init_param(&ip_addr_val, LEAF, 0, 0, 0, IPV4, "intf-ip-address", "IPV4 address");
                        libcli_register_param(&ip_addr, &ip_addr_val);
                        {
                            static param_t mask;
                            init_param(&mask, LEAF, 0, intf_config_handler, validate_mask_value, INT, "mask", "mask [0-32]");
                            libcli_register_param(&ip_addr_val, &mask);
                            libcli_set_param_cmd_code(&mask, CMDCODE_INTF_CONFIG_IP_ADDR);
                        }
                    }
                }
                {
                    /*config node <node-name> interface <if-name> vlan*/
                    static param_t vlan;
                    init_param(&vlan, CMD, "vlan", 0, 0, INVALID, 0, "\"vlan\" keyword");
                    libcli_register_param(&if_name, &vlan);
                    {
                        /*config node <node-name> interface <if-name> vlan <vlan-id>*/
                         static param_t vlan_id;
                         init_param(&vlan_id, LEAF, 0, intf_config_handler, validate_vlan_id, INT, "vlan-id", "vlan id(1-4096)");
                         libcli_register_param(&vlan, &vlan_id);
                         libcli_set_param_cmd_code(&vlan_id, CMDCODE_INTF_CONFIG_VLAN);
                    }
                }    
            }
            libcli_support_cmd_negation(&interface); 
}

