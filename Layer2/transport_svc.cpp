#include <assert.h>
#include <stdint.h>
#include <unordered_map>
#include "../graph.h"
#include "transport_svc.h"
#include "../Interface/Interface.h"
#include "../CLIBuilder/libcli.h"
#include "../cmdcodes.h"

extern graph_t *topo;

static std::unordered_map<std::string , TransportService *> TransPortSvcDB;

TransportService::TransportService(std::string& svc_name) {

    this->trans_svc = svc_name;
}

TransportService::~TransportService() {

    /* Remove all Vlans from vlanSet if any*/
    this->vlanSet.clear();
    assert (this->ifSet.empty());
    assert (!this->ref_count);
}

bool
TransportService::AddVlan(int vlan_id) {

    auto it = this->vlanSet.find( vlan_id );

    if (it != this->vlanSet.end()) {
        return false;
    }

    this->vlanSet.insert ( vlan_id );
    return true;
}

bool
TransportService::RemoveVlan(int vlan_id) {

    auto it = this->vlanSet.find( vlan_id );

    if (it == this->vlanSet.end()) {
        return false;
    }

    this->vlanSet.erase ( vlan_id );
    return true;
}

bool 
TransportService::AttachInterface(Interface *intf) {

    if  (!intf->GetSwitchport() ) {
        cprintf ("\nError : Interface %s is not L2 interface", intf->if_name.c_str());
        return false;
    }

    if (intf->GetL2Mode() == LAN_ACCESS_MODE) {
        cprintf ("\nError : Cant attach TSP to interface in Access Mode");
        return false;
    }

    PhysicalInterface *phy_intf = dynamic_cast<PhysicalInterface *> (intf);
    assert (phy_intf);

    uint32_t ifindex = intf->ifindex;
    auto it = this->ifSet.find ( ifindex );

    if ( it != this->ifSet.end() ) {
        return true;
    }

    this->ifSet.insert (ifindex);
    intf->SetL2Mode (LAN_TRUNK_MODE);
    phy_intf->trans_svc = this;
    this->ref_count++;
    return true;
}

bool 
TransportService::DeAttachInterface (Interface *intf) {

    PhysicalInterface *phy_intf = dynamic_cast<PhysicalInterface *> (intf);
    assert (phy_intf);
    TransportService *trans_svc = phy_intf->trans_svc;
    if (!trans_svc) return true;
    if (this != trans_svc) return true;
    trans_svc->ifSet.erase (intf->ifindex);
    phy_intf->trans_svc = NULL;
    trans_svc->ref_count--;
    intf->SetL2Mode (LAN_MODE_NONE);
    return true;
}


bool 
TransportService::InUse() {

    return !(this->ref_count == 0);
}




/* Global Functions */

TransportService *
TransportServiceLookUp (std::unordered_map<std::string , TransportService *> *TransPortSvcDB, std::string& svc_name) {

    if (!TransPortSvcDB) return NULL;

    auto it = TransPortSvcDB->find ( svc_name );

    if (it == TransPortSvcDB->end()) {
        return NULL;
    }

    return it->second;
}

TransportService *
TransportServiceCreate (node_t *node, std::string& svc_name) {

    TransportService *trans_svc;

    trans_svc = TransportServiceLookUp (node->TransPortSvcDB, svc_name);
    if (trans_svc) return trans_svc;

    trans_svc = new TransportService (svc_name);

    if (!node->TransPortSvcDB) {

        node->TransPortSvcDB = new std::unordered_map<std::string , TransportService *>;
        assert (node->TransPortSvcDB);
    }

    node->TransPortSvcDB->insert (std::make_pair (svc_name, trans_svc));

    return trans_svc;
}

bool 
TransportServiceDelete (std::unordered_map<std::string , TransportService *> *TransPortSvcDB, 
std::string& svc_name) {

    auto it = TransPortSvcDB->find ( svc_name );

    if (it == TransPortSvcDB->end()) {
        return true;
    }     

    TransportService *trans_svc = it->second;

    /* Transport Svc must not be applied to any interface*/
    if (trans_svc->InUse()) {
        cprintf ("\nError : Transport Svc in Use, Cannot delete");
        return false;
    }

    delete trans_svc;   
    TransPortSvcDB->erase(it);
    return true;
}

static int
transport_svc_config_handler (int cmdcode, 
                                                  Stack_t *tlv_stack,
                                                  op_mode enable_or_disable) {

    bool rc;
    node_t *node;
    tlv_struct_t *tlv;
    uint16_t vlan_id;
    TransportService *tsp;
    c_string tsp_name = NULL;
    c_string node_name = NULL;
    std::string tsp_name_cplus_string;
    
    rc = true;

    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if  (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;
        else if (parser_match_leaf_id(tlv->leaf_id, "transport-service-profile"))
            tsp_name = tlv->value;
        else if (parser_match_leaf_id(tlv->leaf_id, "vlan-id"))
            vlan_id = atoi((const char *)tlv->value);

    } TLV_LOOP_END;
    
    node = node_get_node_by_name(topo, node_name);
    
    /* CLI prevent user from mentioning non-existing nodes*/
    assert(node);

    switch (cmdcode) {

        /* config node <node-name> transport-service-profile <transport-service-name> */
        case CMDCODE_CONFIG_NODE_TRANSPORT_SVC:
        
            tsp_name_cplus_string = std::string(reinterpret_cast<const char*>(tsp_name));

            switch (enable_or_disable) {
                case CONFIG_ENABLE:
                    TransportServiceCreate (node, tsp_name_cplus_string);
                    break;
                case CONFIG_DISABLE:
                    TransportServiceDelete (node->TransPortSvcDB, tsp_name_cplus_string);
                    break;
                default: ;
            }
        break;


        /* config node <node-name> transport-service-profile <transport-service-name> vlan add <vlan-id>*/
        case CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_ADD:

            tsp_name_cplus_string = std::string(reinterpret_cast<const char*>(tsp_name));
            tsp = TransportServiceLookUp (node->TransPortSvcDB, tsp_name_cplus_string);

            if (!tsp) {

                /* Silently discard the CLI*/
                if (enable_or_disable == CONFIG_DISABLE) return 0;

                if ( !(tsp = TransportServiceCreate (node, tsp_name_cplus_string))) {
                    cprintf ("\nError : Failed to Create Transport Service Profile %s", tsp_name);
                    return -1;
                }
            }

            switch (enable_or_disable) {
                case CONFIG_ENABLE:
                    rc = tsp->AddVlan(vlan_id);
                    if (!rc) {
                        printf ("\nError : Failed to Add Vlan %d to Transport Service Profile %s",
				vlan_id, tsp_name);
                        return -1;
                    }
                    break;
                case CONFIG_DISABLE:
                    tsp->RemoveVlan(vlan_id);
                    break;
                default: ;
            }
        break;


        case CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_DEL:

            tsp_name_cplus_string = std::string(reinterpret_cast<const char*>(tsp_name));
            tsp = TransportServiceLookUp (node->TransPortSvcDB, tsp_name_cplus_string);

            if (!tsp) {

                /* Silently discard the CLI*/
                if (enable_or_disable == CONFIG_DISABLE) return 0;

                if (!(tsp = TransportServiceCreate (node, tsp_name_cplus_string))) {
                    cprintf ("\nError : Failed to Create Transport Service Profile %s", tsp_name);
                    return -1;
                }
            }

            switch (enable_or_disable) {
                case CONFIG_ENABLE:
                    rc = tsp->RemoveVlan(vlan_id);
                    if (!rc) {
                        printf ("\nError : Failed to Delete Vlan %d from Transport Service Profile %s", vlan_id, tsp_name);
                        return -1;
                    }
                    break;
                case CONFIG_DISABLE:
                    /* Make no sense */
                    break;
                default: ;
            }                        

        break;


        case CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_DEL_ALL:
        break;

        default:    ;
    }

    return 0;
}

#if 0 

config node <node-name> transport-service-profile <transport-service-name>
config node <node-name> transport-service-profile <transport-service-name> vlan add <vlan-id>
config node <node-name> transport-service-profile <transport-service-name> vlan del <vlan-id>
config node <node-name> transport-service-profile <transport-service-name> vlan del all

#endif 

void
config_node_build_transport_svc_cli_tree (param_t *param) {

    {
        /* transport-service-profile <ransport-service-profile-name>*/
        static param_t transport_svc;
        init_param(&transport_svc, CMD, "transport-service-profile", 0, 0, INVALID, 0, "transport-service-profile");
        libcli_register_param(param, &transport_svc);
        {
            static param_t transport_svc_name;
            init_param(&transport_svc_name,
                                LEAF,
                                0, transport_svc_config_handler, 0, STRING, "transport-service-profile", "Transport Svc Profile Name");
            libcli_register_param(&transport_svc, &transport_svc_name);
            libcli_set_param_cmd_code(&transport_svc_name, CMDCODE_CONFIG_NODE_TRANSPORT_SVC);
            {
                /* vlan add <vlan-id>*/
                static param_t vlan;
                init_param(&vlan, CMD, "vlan", 0, 0, INVALID, 0, "vlan");
                libcli_register_param(&transport_svc_name, &vlan);
                {
                    static param_t add;
                    init_param(&add, CMD, "add", 0, 0, INVALID, 0, "add vlan to Transport Service profile");
                    libcli_register_param(&vlan, &add);
                    {
                        static param_t vlan_id;
                        init_param(&vlan_id, LEAF, 0, transport_svc_config_handler, 0, INT, "vlan-id", "vlan id");
                        libcli_register_param(&add, &vlan_id);
                        libcli_set_param_cmd_code(&vlan_id, CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_ADD);
                        libcli_set_tail_config_batch_processing(&vlan_id);
                    }
                }

                {
                    static param_t del;
                    init_param(&del, CMD, "del", 0, 0, INVALID, 0, "del vlan from Transport Service profile");
                    libcli_register_param(&vlan, &del);
                    {
                        static param_t vlan_id;
                        init_param(&vlan_id, LEAF, 0, transport_svc_config_handler, 0, INT, "vlan-id", "vlan id");
                        libcli_register_param(&del, &vlan_id);
                        libcli_set_param_cmd_code(&vlan_id, CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_DEL);
                        libcli_set_tail_config_batch_processing(&vlan_id);
                    }

                    {
                        static param_t all;
                        init_param(&all, CMD, "all", transport_svc_config_handler, 0, INVALID, 0, "Del all Vlans from Transport Svc Profile");
                        libcli_register_param(&del, &all);
                        libcli_set_param_cmd_code(&all, CMDCODE_CONFIG_NODE_TRANSPORT_SVC_VLAN_DEL_ALL);
                        libcli_set_tail_config_batch_processing(&all);
                    }                    
                }
            }
        }
    }
}


#if 0
config node <node-name> interface ethernet <if-name>  transport-service-profile <transport-service-name>
#endif

static int
transport_svc_intf_config_handler (int cmdcode, 
                                                         Stack_t *tlv_stack,
                                                         op_mode enable_or_disable) {
    
    node_t *node;
    tlv_struct_t *tlv;
    c_string tsp_name = NULL;
    c_string intf_name = NULL;
    c_string node_name = NULL;
    std::string  tsp_name_cplus_string;

    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if  (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;
        else if (parser_match_leaf_id(tlv->leaf_id, "transport-service-profile"))
            tsp_name = tlv->value;
        else if (parser_match_leaf_id(tlv->leaf_id, "if-name"))
            intf_name = tlv->value;

    } TLV_LOOP_END;

    node = node_get_node_by_name(topo, node_name);
    Interface *intf = node_get_intf_by_name(node, (const char *)intf_name);

    if (!intf) {
        cprintf("\n" "Error : Non Existing Interface Specified");
        return -1;
    }

    tsp_name_cplus_string = std::string(reinterpret_cast<const char*>(tsp_name));
    TransportService *tsp = TransportServiceLookUp (node->TransPortSvcDB, tsp_name_cplus_string);

    if (!tsp) {
        cprintf("\n" "Error : Non Existing Transport Service Profile Specified");
        return -1;
    }

    switch (cmdcode) {

        case CMDCODE_CONFIG_INTF_TRANSPORT_SVC:

            switch (enable_or_disable) {
                case CONFIG_ENABLE:
                    if (!tsp->AttachInterface(intf)) {
                        cprintf("\nError : Failed to Attach Interface %s to Transport Service Profile %s", 
                            intf->if_name.c_str(), tsp_name);
                        return -1;
                    }
                    break;
                case CONFIG_DISABLE:
                    if (!tsp->DeAttachInterface(intf)) {
                        cprintf("\nError : Failed to DeAttach Interface %s from Transport Service Profile %s",      
                            intf->if_name.c_str(), tsp_name);
                        return -1;
                    }
                    break;
                default: ;
            }
        break;

        default: ;
    }

    return 0;
}


void
config_interface_build_transport_svc_cli_tree (param_t *node_name_param, param_t *param) {

    {
        /* transport-service-profile <ransport-service-profile-name>*/
        static param_t transport_svc;
        init_param(&transport_svc, CMD, "transport-service-profile", 0, 0, INVALID, 0, "transport-service-profile");
        libcli_register_param(param, &transport_svc);
        {
            static param_t transport_svc_name;
            init_param(&transport_svc_name,
                       LEAF,
                       0, transport_svc_intf_config_handler, 0, STRING, "transport-service-profile", "Transport Svc Profile Name");
            libcli_register_param(&transport_svc, &transport_svc_name);
            libcli_set_param_cmd_code(&transport_svc_name, CMDCODE_CONFIG_INTF_TRANSPORT_SVC);
        }
    }
}

static int
transport_svc_show_handler (int cmdcode, 
                                                  Stack_t *tlv_stack,
                                                  op_mode enable_or_disable) {

    node_t *node;
    tlv_struct_t *tlv;
    c_string node_name = NULL;
    std::unordered_map<std::string , TransportService *> *TransPortSvcDB;

    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if  (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;

    } TLV_LOOP_END;
    
    node = node_get_node_by_name(topo, node_name);

    TransPortSvcDB = node->TransPortSvcDB;

    /* Iterate over  TransPortSvcDB*/
    for (auto it = TransPortSvcDB->begin(); it != TransPortSvcDB->end(); ++it) {

        TransportService *tsp = it->second;
        cprintf("Transport Service Profile : %s\n", tsp->trans_svc.c_str());

        cprintf("  Vlans : ");
        for (auto it2 = tsp->vlanSet.begin(); it2 != tsp->vlanSet.end(); ++it2)
        {
            cprintf("%d ", *it2);
        }

        cprintf("\n  Applied Interfaces : ");
        for (auto it2 = tsp->ifSet.begin(); it2 != tsp->ifSet.end(); ++it2)
        {
            cprintf("%s ", node_get_intf_by_ifindex(node, *it2)->if_name.c_str());
        }
        cprintf("\n");
    }
    
    return 0;
}


static int
show_vlan_members (int cmdcode, 
                                    Stack_t *tlv_stack,
                                    op_mode enable_or_disable) {

    uint16_t i;
    node_t *node;
    tlv_struct_t *tlv;
    c_string node_name = NULL;
    std::unordered_map<std::string , TransportService *> *TransPortSvcDB;
    std::unordered_map<std::uint16_t , VlanInterface *> *vlan_intf_db;

    TLV_LOOP_STACK_BEGIN(tlv_stack, tlv){

        if  (parser_match_leaf_id(tlv->leaf_id, "node-name"))
            node_name = tlv->value;

    } TLV_LOOP_END;

    node = node_get_node_by_name(topo, node_name);
   
    vlan_intf_db = node->vlan_intf_db;

    if (!vlan_intf_db) return 0;

    TransPortSvcDB = node->TransPortSvcDB;

    do {
               
        /* Iterate over vlan interface DB*/
        for (auto it_vlan = vlan_intf_db->begin(); it_vlan != vlan_intf_db->end(); ++it_vlan) {

            i = it_vlan->first;
            cprintf (" vlan %d\n", i);

            if (!TransPortSvcDB) continue;

            VlanInterface *vlan_intf = it_vlan->second;
            Interface *member_intf;

            ITERATE_VLAN_MEMBER_PORTS_TRUNK_BEGIN(vlan_intf, member_intf) {

                cprintf("  %s (Trunk)\n", member_intf->if_name.c_str());
            
            } ITERATE_VLAN_MEMBER_PORTS_TRUNK_END;

            ITERATE_VLAN_MEMBER_PORTS_ACCESS_BEGIN(vlan_intf, member_intf) {

                cprintf("  %s (Access)\n", member_intf->if_name.c_str());

            } ITERATE_VLAN_MEMBER_PORTS_ACCESS_END;

        }

    } while (0);

    return 0;
}

void
show_node_transport_svc_cli_tree (param_t *param) {

    {
        /* transport-service-profile <ransport-service-profile-name>*/
        static param_t transport_svc;
        init_param(&transport_svc, CMD, "transport-service-profile", transport_svc_show_handler, 0, INVALID, 0, "transport-service-profile");
        libcli_register_param(param, &transport_svc);
        libcli_set_param_cmd_code(&transport_svc, CMDCODE_SHOW_TRANSPORT_SVC_PROFILE);
    }

    {
        /* vlan-members */
        static param_t vlan_members;
        init_param(&vlan_members, CMD, "vlan-members", show_vlan_members, 0, INVALID, 0, "show vlan members");
        libcli_register_param(param, &vlan_members);
        libcli_set_param_cmd_code(&vlan_members, CMDCODE_SHOW_VLAN_MEMBERS);
    }

}
