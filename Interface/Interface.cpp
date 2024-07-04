/*
 * =====================================================================================
 *
 *       Filename:  InterfaceBase.cpp
 *
 *    Description:
 *
 *        Version:  1.0
 *        Created:  12/06/2022 11:39:18 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  ABHISHEK SAGAR (), sachinites@gmail.com
 *   Organization:  Juniper Networks
 *
 * =====================================================================================
 */

#include <assert.h>
#include <memory.h>
#include <pthread.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include "../tcpconst.h"
#include "../utils.h"
#include "../BitOp/bitsop.h"
#include "Interface.h"
#include "../FireWall/acl/acldb.h"
#include "../graph.h"
#include "../pkt_block.h"
#include "../EventDispatcher/event_dispatcher.h"
#include "../Layer2/layer2.h"
#include "../Layer3/layer3.h"
#include "../Layer3/gre-tunneling/gre.h"
#include "../CLIBuilder/libcli.h"
#include "../Layer2/transport_svc.h"


extern void
snp_flow_init_flow_tree_root(avltree_t *avl_root);
extern void 
tcp_ip_de_init_intf_log_info(Interface *intf);
extern int 
access_group_unconfig (node_t *node, 
                       Interface *intf, 
                       char *dirn, 
                       access_list_t *acc_lst) ;

/* A fn to send the pkt as it is (unchanged) out on the interface */
static int
send_xmit_out (Interface *interface, pkt_block_t *pkt_block)
{

    pkt_size_t pkt_size;
    ev_dis_pkt_data_t *ev_dis_pkt_data;
    node_t *sending_node = interface->att_node;
    node_t *nbr_node = interface->GetNbrNode();

    uint8_t *pkt = pkt_block_get_pkt(pkt_block, &pkt_size);

    if (!(interface->is_up))
    {
        interface->xmit_pkt_dropped++;
        return 0;
    }

    if (!nbr_node)
        return -1;

    if (pkt_size > MAX_PACKET_BUFFER_SIZE)
    {
        cprintf("Error : Node :%s, Pkt Size exceeded\n", sending_node->node_name);
        return -1;
    }

    /* Access List Evaluation at Layer 2 Exit point*/
    if (access_list_evaluate_ethernet_packet(
            interface->att_node, interface,
            pkt_block, false) == ACL_DENY)
    {
        return -1;
    }

    Interface *other_interface = interface->GetOtherInterface();

    ev_dis_pkt_data = (ev_dis_pkt_data_t *)XCALLOC(0, 1, ev_dis_pkt_data_t);

    ev_dis_pkt_data->recv_node = nbr_node;
    ev_dis_pkt_data->recv_intf = other_interface;
    ev_dis_pkt_data->pkt = tcp_ip_get_new_pkt_buffer(pkt_size);
    memcpy(ev_dis_pkt_data->pkt, pkt, pkt_size);
    ev_dis_pkt_data->pkt_size = pkt_size;

    tcp_dump_send_logger(sending_node, interface,
                         pkt_block, pkt_block_get_starting_hdr(pkt_block));

    if (!pkt_q_enqueue(EV_DP(nbr_node), DP_PKT_Q(nbr_node),
                       (char *)ev_dis_pkt_data, sizeof(ev_dis_pkt_data_t)))
    {

        cprintf("%s : Fatal : Ingress Pkt QueueExhausted\n", nbr_node->node_name);

        tcp_ip_free_pkt_buffer(ev_dis_pkt_data->pkt, ev_dis_pkt_data->pkt_size);
        XFREE(ev_dis_pkt_data);
    }

    interface->pkt_sent++;
    return pkt_size;
}

static int
SendPacketOutRaw(PhysicalInterface *Intf, pkt_block_t *pkt_block)
{

    return send_xmit_out(Intf, pkt_block);
}

static int
SendPacketOutLAN(PhysicalInterface *Intf, pkt_block_t *pkt_block)
{

    pkt_size_t pkt_size;

    IntfL2Mode intf_l2_mode = Intf->GetL2Mode();

    if (intf_l2_mode == LAN_MODE_NONE)
    {
        return 0;
    }

    ethernet_hdr_t *ethernet_hdr =
        (ethernet_hdr_t *)pkt_block_get_pkt(pkt_block, &pkt_size);

    vlan_8021q_hdr_t *vlan_8021q_hdr = is_pkt_vlan_tagged(ethernet_hdr);

    switch (intf_l2_mode)
    {

    case LAN_ACCESS_MODE:
    {
        vlan_id_t intf_vlan_id = Intf->GetVlanId();

        /*Case 1 : If interface is operating in ACCESS mode, but
         not in any vlan, and pkt is also untagged, then simply
         forward it. This is default Vlan unaware case*/
        if (!intf_vlan_id && !vlan_8021q_hdr)
        {
            return send_xmit_out(Intf, pkt_block);
        }

        /*Case 2 : if oif is VLAN aware, but pkt is untagged, simply
         drop the packet. This is not an error, it is a L2 switching
         behavior*/
        if (intf_vlan_id && !vlan_8021q_hdr)
        {
            return 0;
        }

        /*Case 3 : If oif is VLAN AWARE, and pkt is also tagged,
          forward the frame only if vlan IDs matches after untagging
          the frame*/
        if (vlan_8021q_hdr &&
            (intf_vlan_id == GET_802_1Q_VLAN_ID(vlan_8021q_hdr)))
        {

            untag_pkt_with_vlan_id(pkt_block);
            return send_xmit_out(Intf, pkt_block);
        }

        /* case 4 : if vlan id in pkt do not matches with the vlan id of
            the interface*/
        if (vlan_8021q_hdr &&
            (intf_vlan_id != GET_802_1Q_VLAN_ID(vlan_8021q_hdr)))
        {
            return 0;
        }

        /*case 5 : if oif is vlan unaware but pkt is vlan tagged,
         simply drop the packet.*/
        if (!intf_vlan_id && vlan_8021q_hdr)
        {
            return 0;
        }
    }
    break;
    case LAN_TRUNK_MODE:
    {
        vlan_id_t pkt_vlan_id = 0;

        if (vlan_8021q_hdr)
        {
            pkt_vlan_id = GET_802_1Q_VLAN_ID(vlan_8021q_hdr);
        }

        if (pkt_vlan_id &&
            Intf->IsVlanTrunked(pkt_vlan_id))
        {
            return send_xmit_out(Intf, pkt_block);
        }

        /*Do not send the pkt in any other case*/
        return 0;
    }
    break;
    case LAN_MODE_NONE:
        break;
    default:;
    }
    return 0;
}

Interface::Interface(std::string if_name, InterfaceType_t iftype)
{

    this->if_name = if_name;
    this->iftype = iftype;
    this->config_ref_count = 0;
    this->dynamic_ref_count = 0;
    this->att_node = NULL;
    memset(&this->log_info, 0, sizeof(this->log_info));
    this->link = NULL;
    this->is_up = true;
    this->ifindex = get_new_ifindex();
    this->cost = INTF_METRIC_DEFAULT;

    this->l2_egress_acc_lst = NULL;
    this->l2_ingress_acc_lst = NULL;

    this->l3_ingress_acc_lst2 = NULL;
    this->l3_egress_acc_lst2 = NULL;

    this->isis_intf_info = NULL;
}

Interface::~Interface()
{

    assert(this->config_ref_count == 0);
    assert(this->dynamic_ref_count == 0);
    cprintf ("%s : Interface %s deleted\n", this->att_node->node_name, this->if_name.c_str());
}

uint32_t
Interface::GetIntfCost()
{
    return this->cost;
}

void Interface::PrintInterfaceDetails()
{

    cprintf("%s   index = %u   Owning-Dev %s\n",
           this->if_name.c_str(), this->ifindex, this->att_node->node_name);

    cprintf("State : Administratively %s\n", this->is_up ? "Up" : "Down");

#if 0
    cprintf("L2 access Lists : Ingress - %s, Egress - %s\n",
           this->l2_ingress_acc_lst ? (const char *)this->l2_ingress_acc_lst->name : "None",
           this->l2_egress_acc_lst ? (const char *)this->l2_egress_acc_lst->name : "None");

    cprintf("L3 access Lists : Ingress - %s, Egress - %s\n",
           this->l3_ingress_acc_lst2 ? (const char *)this->l3_ingress_acc_lst2->name : "None",
           this->l3_egress_acc_lst2 ? (const char *)this->l3_egress_acc_lst2->name : "None");
#endif 

    if (this->isis_intf_info)
    {
        cprintf("ISIS Running\n");
    }

    cprintf("Metric = %u\n", this->GetIntfCost());
    cprintf ("config_ref_count  = %u, dynamic_ref_count = %u\n", 
        this->config_ref_count, this->dynamic_ref_count);
}

node_t *
Interface::GetNbrNode()
{

    Interface *interface = this;

    assert(this->att_node);
    assert(this->link);

    linkage_t *link = interface->link;
    if (link->Intf1 == interface)
        return link->Intf2->att_node;
    else
        return link->Intf1->att_node;
}

Interface *
Interface::GetOtherInterface()
{

    return this->link->Intf1 == this ? this->link->Intf2 : this->link->Intf1;
}

int Interface::SendPacketOut(pkt_block_t *pkt_block)
{

    cprintf ("Error : Operation %s not supported\n", __func__);
    return -1;
}

void Interface::SetMacAddr(mac_addr_t *mac_add)
{

    cprintf ("Error : Operation %s not supported\n", __func__);
}

mac_addr_t *
Interface::GetMacAddr()
{
    return NULL;
}

bool Interface::IsIpConfigured()
{

    return false;
}
void Interface::InterfaceSetIpAddressMask(uint32_t ip_addr, uint8_t mask)
{

    cprintf ("Error : Operation %s not supported\n", __func__);
}

void Interface::InterfaceGetIpAddressMask(uint32_t *ip_addr, uint8_t *mask)
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

vlan_id_t
Interface::GetVlanId()
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

bool Interface::IsVlanTrunked(vlan_id_t vlan_id)
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

void Interface::SetSwitchport(bool enable)
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

bool Interface::IntfConfigTransportSvc(std::string& trans_svc) 
{
   cprintf ("Error : Operation %s not supported\n", __func__);
}

bool Interface::IntfUnConfigTransportSvc(std::string& trans_svc) 
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

bool Interface::GetSwitchport()
{
    return false;
}

IntfL2Mode
Interface::GetL2Mode()
{

    return LAN_MODE_NONE;
}

void Interface::SetL2Mode(IntfL2Mode l2_mode)
{
    cprintf ("Error : Operation %s not supported\n", __func__);
}

bool Interface::IntfConfigVlan(vlan_id_t vlan_id, bool add)
{
    cprintf ("Error : Operation %s not supported\n", __func__);
    return false;
}

bool Interface::IsSameSubnet(uint32_t ip_addr)
{

    if (!this->IsIpConfigured())
        return false;
     cprintf ("Error : Operation %s not supported\n", __func__);
     return false;
}

bool 
Interface:: IsInterfaceUp(vlan_id_t vlan_id) {

        cprintf ("Error : Operation %s not supported\n", __func__);
        return false;
}

bool 
Interface::IsCrossReferenced() {

    return (this->config_ref_count > 1 );
}

void 
Interface::InterfaceReleaseAllResources() {

    tcp_ip_de_init_intf_log_info (this);

    if (this->link) {
        /* Nothing to do, we dont break topology !*/
    }

    if (this->l2_ingress_acc_lst) {
        assert(0); /* Not Supported Yet*/
    }

    if (this->l2_egress_acc_lst) {
        assert(0); /* Not Supported Yet*/
    }

    if (this->l3_ingress_acc_lst2) {
        access_group_unconfig (this->att_node, this, "in", this->l3_ingress_acc_lst2);
    }

    if (this->l3_egress_acc_lst2) {
        access_group_unconfig (this->att_node, this, "out", this->l3_egress_acc_lst2);
    }

    /* This is configuration, this fn call must not see it set*/
    assert (!this->isis_intf_info);
}


void 
Interface::InterfaceLockStatic() {

    this->config_ref_count++;
}
void 
Interface::InterfaceLockDynamic() {

    this->dynamic_ref_count++;
}

bool
Interface::InterfaceUnLockStatic() {

    assert (this->config_ref_count);
    this->config_ref_count--;

    if (this->config_ref_count == 0 &&
         this->dynamic_ref_count == 0 ) {

        /* Delete the interface and all its resources */
        this->InterfaceReleaseAllResources();

        if (this->iftype != INTF_TYPE_PHY) {
            delete this;
            return true;
        }
    }
    return false;
}

bool
Interface::InterfaceUnLockDynamic() {

    assert (this->dynamic_ref_count);
    this->dynamic_ref_count--;

    if (this->config_ref_count == 0 &&
         this->dynamic_ref_count == 0 ) {

        /* Delete the interface and all its resources */
        this->InterfaceReleaseAllResources();

        if (this->iftype != INTF_TYPE_PHY) {
            delete this;
            return true;
        }
    }
    return false;
}

uint16_t 
Interface::GetConfigRefCount() {

    return this->config_ref_count;
}

uint16_t 
Interface::GetDynamicRefCount() {

    return this->dynamic_ref_count;
}

bool 
Interface::IsSVI () {

    return false;
}

/* ************ PhysicalInterface ************ */
PhysicalInterface::PhysicalInterface(std::string ifname, InterfaceType_t iftype, mac_addr_t *mac_add)
    : Interface(ifname, iftype)
{

    this->switchport = false;
    if (mac_add)
    {
        memcpy(this->mac_add.mac, mac_add->mac, sizeof(*mac_add));
    }
    this->l2_mode = LAN_MODE_NONE;
    this->ip_addr = 0;
    this->mask = 0;
    this->cost = INTF_METRIC_DEFAULT;
}

PhysicalInterface::~PhysicalInterface()
{
}

void PhysicalInterface::SetMacAddr(mac_addr_t *mac_add)
{

    if (mac_add)
    {
        memcpy(this->mac_add.mac, mac_add->mac, sizeof(*mac_add));
    }
}

mac_addr_t *
PhysicalInterface::GetMacAddr()
{

    return &this->mac_add;
}

void PhysicalInterface::PrintInterfaceDetails()
{

    byte ip_addr[16];

    cprintf("MAC : %02x:%02x:%02x:%02x:%02x:%02x\n",
           this->mac_add.mac[0],
           this->mac_add.mac[1],
           this->mac_add.mac[2],
           this->mac_add.mac[3],
           this->mac_add.mac[4],
           this->mac_add.mac[5]);

    if (this->IsIpConfigured())
    {
        cprintf("IP Addr : %s/%d\n", 
            tcp_ip_covert_ip_n_to_p(this->ip_addr, ip_addr), this->mask);
    }
    else
    {
        cprintf("IP Addr : Not Configured\n");
    }

    cprintf("Vlan L2 Mode : %s\n",
        PhysicalInterface::L2ModeToString(this->l2_mode).c_str());

    this->Interface::PrintInterfaceDetails();
}

void PhysicalInterface::InterfaceSetIpAddressMask(uint32_t ip_addr, uint8_t mask)
{

    if (this->switchport && ip_addr)
    {
        cprintf("Error : Remove L2 Config first\n");
        return;
    }

    this->ip_addr = ip_addr;
    this->mask = mask;
}

void PhysicalInterface::InterfaceGetIpAddressMask(uint32_t *ip_addr, uint8_t *mask)
{

    *ip_addr = this->ip_addr;
    *mask = this->mask;
}

bool PhysicalInterface::IsIpConfigured()
{

    if (this->ip_addr && this->mask)
        return true;
    return false;
}

std::string
PhysicalInterface::L2ModeToString(IntfL2Mode l2_mode)
{

    switch (l2_mode)
    {

    case LAN_MODE_NONE:
        return std::string("None");
    case LAN_ACCESS_MODE:
        return std::string("Access");
    case LAN_TRUNK_MODE:
        return std::string("Trunk");
    default:;
    }
    return NULL;
}

bool PhysicalInterface::IsVlanTrunked(vlan_id_t vlan_id)
{
    TransportService *tsp = this->trans_svc;
    if (!tsp) return false;
    for (auto it = tsp->vlanSet.begin(); it != tsp->vlanSet.end(); ++it) {
        if (*it == vlan_id) return true;
    }
    return false;
}


vlan_id_t
PhysicalInterface::GetVlanId()
{
    if (this->l2_mode == LAN_MODE_NONE)
        return 0;

     if (this->l2_mode == LAN_ACCESS_MODE) {
        return this->access_vlan_intf->GetVlanId();
     }

     return 0;
}

void PhysicalInterface::SetSwitchport(bool enable)
{

    if (this->switchport == enable)
        return;

    if (this->used_as_underlying_tunnel_intf > 0)
    {
        cprintf("Error : Intf being used as underlying tunnel interface\n");
        return;
    }

    if (enable && this->IsIpConfigured()) {
        cprintf("Error : Remove L3 config first\n");
        return;
    }

    if (enable)
    {
        this->InterfaceSetIpAddressMask(0, 0);
        this->l2_mode = LAN_MODE_NONE;
        this->iftype = INTF_TYPE_VLAN;
    }
    else
    {
        if (this->access_vlan_intf || this->trans_svc) {
            cprintf("Error : Remove L2 Config first\n");
            this->switchport = true;
            return;
        }
        this->l2_mode = LAN_MODE_NONE;
        this->iftype = INTF_TYPE_PHY;
    }
    this->switchport = enable;
}

bool PhysicalInterface::GetSwitchport()
{

    return this->switchport;
}

int PhysicalInterface::SendPacketOut(pkt_block_t *pkt_block)
{

    if (this->switchport)
    {
        return SendPacketOutLAN(this, pkt_block);
    }
    else
    {
        return SendPacketOutRaw(this, pkt_block);
    }
}

IntfL2Mode
PhysicalInterface::GetL2Mode()
{

    return this->l2_mode;
}

void PhysicalInterface::SetL2Mode(IntfL2Mode l2_mode)
{

    if (this->IsIpConfigured())
    {
        cprintf("Error : Remove L3 config first\n");
        return;
    }

    if (this->used_as_underlying_tunnel_intf > 0)
    {
        cprintf("Error : Intf being used as underlying tunnel interface\n");
        return;
    }
    
    if (this->trans_svc) {

        cprintf("Error : Intf being used in Transport Service\n");
        return;
    }

    if (this->l2_mode == l2_mode)
        return;

    if (l2_mode != LAN_MODE_NONE &&
             this->l2_mode != LAN_MODE_NONE)
    {
        cprintf("Error : Remove configured L2 Mode first\n");
        return;
    }

    this->l2_mode = l2_mode;
}

bool
PhysicalInterface::IntfConfigTransportSvc(std::string& trans_svc_name) {

    if (!this->switchport) {
        printf ("Error : Interface %s is not L2 interface\n", this->if_name.c_str());
        return false;
    }

    TransportService *trans_svc_obj = TransportServiceLookUp (this->att_node->TransPortSvcDB, trans_svc_name);
    
    if (!trans_svc_obj) {
        printf ("Error : Transport Svc do not exist\n");
        return false;
    }

    if (this->trans_svc == trans_svc_obj) return true;

    /* Remove old Transport svc if any*/
    if (this->trans_svc) {
        this->trans_svc->DeAttachInterface(this);
    }

    trans_svc_obj->AttachInterface(this);
    return true;
}

bool 
PhysicalInterface::IntfUnConfigTransportSvc(std::string& trans_svc_name) {

    if (!this->trans_svc) return true;
    TransportService *trans_svc_obj = TransportServiceLookUp (this->att_node->TransPortSvcDB, trans_svc_name);
    if (!trans_svc_obj) return true;
    if (this->trans_svc != trans_svc_obj) return true;
    this->trans_svc->DeAttachInterface (this);
    this->trans_svc = NULL;
    return true;
}

bool 
PhysicalInterface::IntfConfigVlan(vlan_id_t vlan_id, bool add)
{

    int i;
    if (!this->switchport)
        return false;
    if (this->used_as_underlying_tunnel_intf > 0)
    {
        cprintf("Error : Intf being used as underlying tunnel interface");
        return false;
    }

    if (this->GetL2Mode() == LAN_TRUNK_MODE) {
        cprintf ("Error : Cannot (Un)configure Access Vlan to Interface in Trunk Mode");
        return false;
    }

    if (add)
    {
        if (this->access_vlan_intf && 
                this->access_vlan_intf->GetVlanId() == vlan_id) return true;

        if (this->access_vlan_intf)
        {
            cprintf("Error : Access Mode Interface already in vlan %u", this->access_vlan_intf->GetVlanId());
            return false;
        }

        this->access_vlan_intf = VlanInterface::VlanInterfaceLookUp(this->att_node, vlan_id);
        
        if (!this->access_vlan_intf)
        {
            cprintf("Error : Vlan Interface not found");
            return false;
        }
        this->access_vlan_intf->access_member_intf_lst.push_back(this);
        this->l2_mode = LAN_ACCESS_MODE;
        this->access_vlan_intf->InterfaceLockStatic();
        this->InterfaceLockStatic();
        return true;
    }
    else
    {
        if (this->access_vlan_intf->GetVlanId() == vlan_id)
            {
                this->access_vlan_intf->access_member_intf_lst.erase(
                    std::remove (this->access_vlan_intf->access_member_intf_lst.begin(), this->access_vlan_intf->access_member_intf_lst.end(), this),
                    this->access_vlan_intf->access_member_intf_lst.end());
                this->InterfaceUnLockStatic();
                this->access_vlan_intf->InterfaceUnLockStatic();
                this->access_vlan_intf = NULL;
                this->l2_mode = LAN_MODE_NONE;

                return true;
            }
            {
                cprintf("Error : Interface not in vlan %u", vlan_id);
                return false;
            }
    }
    return true;
}

bool PhysicalInterface::IsSameSubnet(uint32_t ip_addr)
{

    uint8_t mask;
    uint32_t intf_ip_addr;
    uint32_t subnet_mask = ~0;

    if (!this->IsIpConfigured())
        return false;

    this->InterfaceGetIpAddressMask(&intf_ip_addr, &mask);

    if (mask != 32)
    {
        subnet_mask = subnet_mask << (32 - mask);
    }

    return ((intf_ip_addr & subnet_mask) == (ip_addr & subnet_mask));
}

bool 
PhysicalInterface:: IsInterfaceUp(vlan_id_t vlan_id) {

    if (!this->is_up) return false;

    if (this->IsIpConfigured()) return this->is_up;
    
    if (this->switchport && vlan_id) {

        if (this->access_vlan_intf) {
            return this->access_vlan_intf->IsInterfaceUp(vlan_id);
        }
        else {
            VlanInterface *vlan_intf = VlanInterface::VlanInterfaceLookUp(this->att_node, vlan_id);
            if (vlan_intf) {
                return vlan_intf->IsInterfaceUp(vlan_id);
            }
        }
    }
    return true;
}

void 
PhysicalInterface::InterfaceReleaseAllResources() {

    assert (this->used_as_underlying_tunnel_intf == 0);

    /* Handling attached TSP*/
    if (this->trans_svc) {
        this->IntfUnConfigTransportSvc (this->trans_svc->trans_svc);
    }
    
    /* Handling access Vlan Interface*/
    if (this->access_vlan_intf) {
        this->IntfConfigVlan (this->access_vlan_intf->GetVlanId(), false);
    }

    this->SetSwitchport (false);

    this->Interface::InterfaceReleaseAllResources();
}


/* ************ Virtual Interface ************ */
VirtualInterface::VirtualInterface(std::string ifname, InterfaceType_t iftype)
    : Interface(ifname, iftype)
{
    this->pkt_recv = 0;
    this->pkt_sent = 0;
    this->xmit_pkt_dropped = 0;
    this->recvd_pkt_dropped = 0;
}

VirtualInterface::~VirtualInterface()
{
}

void VirtualInterface::PrintInterfaceDetails()
{

    cprintf("pkt recvd : %u   pkt sent : %u   xmit pkt dropped : %u    recvd pkt dropped : %u\n",
           this->pkt_recv, this->pkt_sent, 
           this->xmit_pkt_dropped,
           this->recvd_pkt_dropped);

    this->Interface::PrintInterfaceDetails();
}

bool 
VirtualInterface::IsInterfaceUp(vlan_id_t vlan_id) {
    cprintf ("Error : Operation %s not supported\n", __func__);
}


void 
VirtualInterface::InterfaceReleaseAllResources() {

    /* Nothing to release */
    this->Interface::InterfaceReleaseAllResources();
}



/* ************ GRETunnelInterface ************ */
GRETunnelInterface::GRETunnelInterface(uint32_t tunnel_id)

    : VirtualInterface(std::string("tunnel") + std::to_string(tunnel_id), INTF_TYPE_GRE_TUNNEL)
{

    this->tunnel_id = tunnel_id;
    this->config_flags = 0;
    this->config_flags |= GRE_TUNNEL_TUNNEL_ID_SET;
}

GRETunnelInterface::~GRETunnelInterface() {

    assert (!this->tunnel_src_intf);
}

uint32_t
GRETunnelInterface::GetTunnelId()
{

    return this->tunnel_id;
}

bool GRETunnelInterface::IsGRETunnelActive()
{
    bool rc = false;

    if ((this->config_flags & GRE_TUNNEL_TUNNEL_ID_SET) &&
         (this->config_flags & GRE_TUNNEL_SRC_ADDR_SET || 
                this->config_flags & GRE_TUNNEL_SRC_INTF_SET) &&
        (this->config_flags & GRE_TUNNEL_DST_ADDR_SET) &&
            (this->config_flags & GRE_TUNNEL_OVLAY_IP_SET))
    {
        rc = true;
    }

    if ( this->tunnel_src_intf) {

        if ( !this->tunnel_src_intf->IsInterfaceUp(0)  || 
                !this->tunnel_src_intf->IsIpConfigured()) {
            rc = false;
        }
    }

    return rc;
}

bool 
GRETunnelInterface::SetTunnelSource(PhysicalInterface *interface)
{

    uint32_t ip_addr;
    uint8_t mask;

    if (interface)
    {
        if (this->tunnel_src_intf == interface)
        {
            return;
        }
	if (this->tunnel_src_intf &&
		this->tunnel_src_intf != interface) {
		cprintf ("Error : Tunnel Src Interface %s already set\n", this->tunnel_src_intf->if_name.c_str());
		return false;
	}
        this->tunnel_src_intf = interface;
        interface->used_as_underlying_tunnel_intf++;
        this->config_flags |= GRE_TUNNEL_SRC_INTF_SET;
    }
    else {

	if (this->tunnel_src_intf == NULL) return true;
        PhysicalInterface *tunnel_src_intf = dynamic_cast<PhysicalInterface *>(this->tunnel_src_intf );
        tunnel_src_intf->used_as_underlying_tunnel_intf--;
        this->tunnel_src_intf = NULL;
        this->config_flags &= ~GRE_TUNNEL_SRC_INTF_SET;
    }
    return true;
}

void 
GRETunnelInterface::SetTunnelDestination(uint32_t ip_addr)
{

    this->tunnel_dst_ip = ip_addr;
    if (ip_addr) {
        this->config_flags |= GRE_TUNNEL_DST_ADDR_SET;
    }
    else {
        this->config_flags &= ~GRE_TUNNEL_DST_ADDR_SET;
    }
}

void 
GRETunnelInterface::SetTunnelLclIpMask(uint32_t ip_addr, uint8_t mask)
{
    this->InterfaceSetIpAddressMask(ip_addr, mask);
}

 bool 
 GRETunnelInterface::IsIpConfigured() {

    return (this->config_flags & GRE_TUNNEL_OVLAY_IP_SET);
 }

void 
GRETunnelInterface::SetTunnelSrcIp(uint32_t src_addr)
{

    if (this->config_flags & GRE_TUNNEL_SRC_ADDR_SET)
    {
        cprintf("Error : Src Address Already Set\n");
        return;
    }

    this->tunnel_src_ip = src_addr;
    this->config_flags |= GRE_TUNNEL_SRC_ADDR_SET;
}

void
GRETunnelInterface::UnSetTunnelSrcIp()
{
    if (this->config_flags & GRE_TUNNEL_SRC_ADDR_SET)
    {
        this->tunnel_src_ip = 0;
        this->config_flags &= ~GRE_TUNNEL_SRC_ADDR_SET;
    }
}

void 
GRETunnelInterface::InterfaceSetIpAddressMask(uint32_t ip_addr, uint8_t mask) {

        this->lcl_ip = ip_addr;
        this->mask = mask;
        if (ip_addr == 0 ) {
            this->config_flags &= ~GRE_TUNNEL_OVLAY_IP_SET;
            return;
        }
        this->config_flags |= GRE_TUNNEL_OVLAY_IP_SET;
    }
    
void 
GRETunnelInterface::InterfaceGetIpAddressMask(uint32_t *ip_addr, uint8_t *mask) {

    if (this->config_flags & GRE_TUNNEL_OVLAY_IP_SET) {

        *ip_addr = this->lcl_ip;
        *mask = this->mask;
        return;
    }
    *ip_addr = 0;
    *mask = 0;
}

bool 
GRETunnelInterface::IsSameSubnet(uint32_t ip_addr)
{

    uint8_t mask;
    uint32_t intf_ip_addr;
    uint32_t subnet_mask = ~0;

    if (!this->IsIpConfigured())
        return false;

    this->InterfaceGetIpAddressMask(&intf_ip_addr, &mask);

    if (mask != 32)
    {
        subnet_mask = subnet_mask << (32 - mask);
    }

    return ((intf_ip_addr & subnet_mask) == (ip_addr & subnet_mask));
}

mac_addr_t *
GRETunnelInterface::GetMacAddr() {

    return &this->att_node->node_nw_prop.rmac;
}

void GRETunnelInterface::PrintInterfaceDetails()
{

    byte ip_str[16];

    cprintf("Tunnel Id : %u\n", this->tunnel_id);
    cprintf("Tunnel Src Intf  : %s\n",
           this->tunnel_src_intf ? this->tunnel_src_intf->if_name.c_str() : "Not Set");
    if (this->config_flags & GRE_TUNNEL_SRC_ADDR_SET) {
        cprintf("Tunnel Src Ip : %s\n", tcp_ip_covert_ip_n_to_p(this->tunnel_src_ip, ip_str));
    }
    else if (this->config_flags & GRE_TUNNEL_SRC_INTF_SET ){
        uint32_t ip_addr;
        uint8_t mask;
        this->tunnel_src_intf->InterfaceGetIpAddressMask(&ip_addr, &mask);
        cprintf("Tunnel Src Ip : %s\n", tcp_ip_covert_ip_n_to_p(ip_addr, ip_str));
    }
    else {
        cprintf("Tunnel Src Ip : Nil\n"); 
    }
    cprintf("Tunnel Dst Ip : %s\n", tcp_ip_covert_ip_n_to_p(this->tunnel_dst_ip, ip_str));
    cprintf("Tunnel Lcl Ip/Mask : %s/%d\n", tcp_ip_covert_ip_n_to_p(this->lcl_ip, ip_str), this->mask);
    cprintf("Is Tunnel Active : %s\n", this->IsGRETunnelActive() ? "Y" : "N");

    this->VirtualInterface::PrintInterfaceDetails();
}

int 
GRETunnelInterface::SendPacketOut(pkt_block_t *pkt_block)
{
    pkt_size_t pkt_size;
    bool no_modify = false;
    node_t *node = this->att_node;
    pkt_block_t *pkt_block_copy;

    if (!this->IsGRETunnelActive()) {
        return 0;
    }
    
    if (pkt_block->no_modify) {
        no_modify = pkt_block->no_modify;
        pkt_block_copy = pkt_block_dup (pkt_block);
        pkt_block = pkt_block_copy;
    }

    gre_encasulate (pkt_block);
    pkt_block_get_pkt (pkt_block, &pkt_size);
    pkt_block_set_exclude_oif (pkt_block, this);

    /* Now attach outer IP Hdr and send the pkt*/
    tcp_ip_send_ip_data (node, pkt_block, GRE_HDR,  this->tunnel_dst_ip);
    this->pkt_sent++;

    if (no_modify) {
        pkt_block_dereference(pkt_block);
    }

    return pkt_size;
}

bool 
GRETunnelInterface::IsInterfaceUp(vlan_id_t vlan_id) {

    return this->is_up;
}

void 
GRETunnelInterface::InterfaceReleaseAllResources() {

    if (this->tunnel_src_intf) {
        this->SetTunnelSource(NULL);
    }

    this->VirtualInterface::InterfaceReleaseAllResources();
}


/* ******** VirtualPort **************** */

VirtualPort::VirtualPort(std::string ifname) 
    : VirtualInterface(ifname, INTF_TYPE_VIRTUAL_PORT)
{

}

VirtualPort::~VirtualPort()
{
    assert (!this->olay_tunnel_intf);
    assert (!this->trans_svc);
}

void
VirtualPort::PrintInterfaceDetails()
{
    cprintf("Overlay Tunnel : %s\n",
           this->olay_tunnel_intf ? this->olay_tunnel_intf->if_name.c_str() : "Not Set");

    if (this->trans_svc) {
        cprintf("Transport Service Profile : %s\n", this->trans_svc->trans_svc.c_str());
    }

    this->VirtualInterface::PrintInterfaceDetails();
}

int
VirtualPort::SendPacketOut(pkt_block_t *pkt_block) {

    pkt_size_t pkt_size;

    if (!this->olay_tunnel_intf) {
        return 0;
    }

    assert (pkt_block_get_starting_hdr(pkt_block) == ETH_HDR);

    ethernet_hdr_t *ethernet_hdr = 
        ( ethernet_hdr_t *)pkt_block_get_pkt(pkt_block, &pkt_size);

    vlan_8021q_hdr_t *vlan_8021q_hdr = 
        is_pkt_vlan_tagged(ethernet_hdr);
    
    assert (vlan_8021q_hdr );

    /* If vport is in trunk mode, then check if vlan id is part of trunk*/
    if (!this->IsVlanTrunked (GET_802_1Q_VLAN_ID(vlan_8021q_hdr))) return 0;

    this->pkt_sent++;
    
    return this->olay_tunnel_intf->SendPacketOut(pkt_block);
}


bool 
VirtualPort::IsInterfaceUp(vlan_id_t vlan_id) 
{
    if (!this->is_up) return false;
   
    if (vlan_id) {

        VlanInterface *vlan_intf = VlanInterface::VlanInterfaceLookUp(this->att_node, vlan_id);
        if (vlan_intf)
        {
            return vlan_intf->IsInterfaceUp(vlan_id);
        }
    }
    return true;
}

void 
VirtualPort::InterfaceReleaseAllResources()
{
    /* Handling attached TSP*/
    if (this->trans_svc) {
        this->IntfUnConfigTransportSvc (this->trans_svc->trans_svc);
    }
    
    /* Handling access Vlan Interface*/
    this->VirtualInterface::InterfaceReleaseAllResources();
}

bool 
VirtualPort::IsVlanTrunked (vlan_id_t vlan_id) {

    TransportService *tsp = this->trans_svc;
    if (!tsp) return false;
    for (auto it = tsp->vlanSet.begin(); it != tsp->vlanSet.end(); ++it) {
        if (*it == vlan_id) return true;
    }
    return false;
}

bool 
VirtualPort::GetSwitchport( ) {

    return true;
}

IntfL2Mode 
VirtualPort::GetL2Mode ( ) {

        return LAN_TRUNK_MODE;
}


bool 
VirtualPort::IntfConfigTransportSvc(std::string& trans_svc_name) 
{
    TransportService *trans_svc_obj = TransportServiceLookUp (this->att_node->TransPortSvcDB, trans_svc_name);
    
    if (!trans_svc_obj) {
        printf ("Error : Transport Svc do not exist\n");
        return false;
    }

    if (this->trans_svc == trans_svc_obj) return true;

    /* Remove old Transport svc if any*/
    if (this->trans_svc) {
        this->trans_svc->DeAttachInterface(this);
    }

    trans_svc_obj->AttachInterface(this);
    return true;
}

bool 
VirtualPort::IntfUnConfigTransportSvc(std::string& trans_svc_name) 
{
    if (!this->trans_svc) return true;
    TransportService *trans_svc_obj = TransportServiceLookUp (this->att_node->TransPortSvcDB, trans_svc_name);
    if (!trans_svc_obj) return true;
    if (this->trans_svc != trans_svc_obj) return true;
    this->trans_svc->DeAttachInterface (this);
    this->trans_svc = NULL;
    return true;
}

bool 
VirtualPort::BindOverlayTunnel(Interface *tunnel) {

    if (this->olay_tunnel_intf == tunnel) return true;

    if (this->olay_tunnel_intf) {
        cprintf ("Error : Overlay Tunnel already set\n");
        return false;
    }

    this->olay_tunnel_intf = tunnel;
    tunnel->InterfaceLockStatic();

    /* If tunnel is GRE Interface*/
    switch (tunnel->iftype) {
        case INTF_TYPE_GRE_TUNNEL:
            {
                GRETunnelInterface *gre_tunnel_intf = dynamic_cast<GRETunnelInterface *>(tunnel);
                gre_tunnel_intf->virtual_port_intf = this;
                this->InterfaceLockStatic();
            }
            break;
    }
    return true;
}


bool 
VirtualPort::UnBindOverlayTunnel(Interface *tunnel) {

    Interface *overlay_tunnel;
    
    if (!this->olay_tunnel_intf) return true;

    if (this->olay_tunnel_intf != tunnel) {
        cprintf ("Error : Could not unbind Tunnel\n");
        return false;
    }

    overlay_tunnel = this->olay_tunnel_intf;
    switch (overlay_tunnel->iftype) {
        case INTF_TYPE_GRE_TUNNEL:
            {
                GRETunnelInterface *gre_tunnel_intf = dynamic_cast<GRETunnelInterface *>(overlay_tunnel);
                assert (gre_tunnel_intf->virtual_port_intf == this);
                gre_tunnel_intf->virtual_port_intf = NULL;
                this->InterfaceUnLockStatic();
            }
            break;
    }
    this->olay_tunnel_intf->InterfaceUnLockStatic();
    this->olay_tunnel_intf = NULL;
    return true;
}





/* ************ VlanInterface ************ */

VlanInterface::VlanInterface(vlan_id_t vlan_id)
    : VirtualInterface("null", INTF_TYPE_VLAN)
{

    this->vlan_id = vlan_id;
    this->ip_addr = 0;
    this->mask = 0;
    
    std::string if_name = "vlan" + std::to_string(vlan_id);
    this->if_name = if_name;
}

VlanInterface::~VlanInterface() {

    assert (this->access_member_intf_lst.empty());
}

void 
VlanInterface::PrintInterfaceDetails() {

    int i;
    int vec_size;
    byte ip_str[16];
    TransportService *tsp;
    Interface *member_ports;

    cprintf("Vlan Id : %u\n", this->vlan_id);

    if (this->IsIpConfigured()) {
        cprintf("  IP Addr : %s/%d\n", tcp_ip_covert_ip_n_to_p(this->ip_addr, ip_str), this->mask);
    }

    cprintf ("Trunk Member Ports: \n");

    ITERATE_VLAN_MEMBER_PORTS_TRUNK_BEGIN(this, member_ports) {

        cprintf ("  %s\n", member_ports->if_name.c_str());

    } ITERATE_VLAN_MEMBER_PORTS_TRUNK_END;

    cprintf("  Access Member Ports: \n");

    ITERATE_VLAN_MEMBER_PORTS_ACCESS_BEGIN(this, member_ports) {

        cprintf ("  %s\n", member_ports->if_name.c_str());

    } ITERATE_VLAN_MEMBER_PORTS_ACCESS_END;

    this->VirtualInterface::PrintInterfaceDetails();
}


void 
VlanInterface::InterfaceSetIpAddressMask(uint32_t ip_addr, uint8_t mask) {

    this->ip_addr = ip_addr;
    this->mask = mask;
}

void 
VlanInterface::InterfaceGetIpAddressMask(uint32_t *ip_addr, uint8_t *mask) {

    *ip_addr = this->ip_addr;
    *mask = this->mask;
}

bool
VlanInterface::IsIpConfigured() {

    return (this->ip_addr && this->mask);
}

mac_addr_t *
VlanInterface::GetMacAddr( ) {

    return &this->att_node->node_nw_prop.rmac;
}

bool
VlanInterface::IsSameSubnet(uint32_t ip_addr) {

    if (!this->IsIpConfigured()) return false;
    uint32_t subnet_mask = ~0;
    if (this->mask != 32) {
        subnet_mask = subnet_mask << (32 - this->mask);
    }
    return ((this->ip_addr & subnet_mask) == (ip_addr & subnet_mask));
}

vlan_id_t
VlanInterface::GetVlanId() {

    return (vlan_id_t)this->vlan_id;
}

VlanInterface *
VlanInterface::VlanInterfaceLookUp(node_t *node, vlan_id_t vlan_id) {

    VlanInterface *vlan_intf = NULL;

    if (!node->vlan_intf_db) return NULL;

    std::unordered_map<std::uint16_t , VlanInterface *>::iterator it;
    it = node->vlan_intf_db->find(vlan_id);
    if (it != node->vlan_intf_db->end()) {
        vlan_intf = it->second;
    }
    return vlan_intf;
}

/* Vlan interface can have member ports which are : 
    1. Physical ports 
        1.a access mode
          If Pkt is untagged, drop it
          If pkt is tagged but with different vlan id, drop it
          If pkt is tagged with same vlan id, untag it and send it out
        1.b Trunk mode    
           If pkt is tagged with vlan id, and vlan id is part of trunk, send it out
           Else drop the pkt
*/

int 
VlanInterface::SendPacketOut(pkt_block_t *pkt_block) {

    pkt_size_t pkt_size;
    Interface *member_intf;
    pkt_block_t *dup_pkt_block;

    ethernet_hdr_t *ethernet_hdr =
        (ethernet_hdr_t *)pkt_block_get_pkt(pkt_block, &pkt_size);

    vlan_8021q_hdr_t *vlan_8021q_hdr = is_pkt_vlan_tagged(ethernet_hdr);

   if (!vlan_8021q_hdr ||
                (GET_802_1Q_VLAN_ID(vlan_8021q_hdr) !=  this->GetVlanId())) return 0;

   dup_pkt_block = pkt_block_dup(pkt_block);

   untag_pkt_with_vlan_id(dup_pkt_block);

   ITERATE_VLAN_MEMBER_PORTS_ACCESS_BEGIN(this, member_intf)
   {
       send_xmit_out(member_intf, dup_pkt_block);
   }
   ITERATE_VLAN_MEMBER_PORTS_ACCESS_END;

   pkt_block_free(dup_pkt_block);

   ITERATE_VLAN_MEMBER_PORTS_TRUNK_BEGIN(this, member_intf)
   {
       send_xmit_out(member_intf, pkt_block);
    } 
    ITERATE_VLAN_MEMBER_PORTS_TRUNK_END;

    return 0;
}

bool 
VlanInterface::IsInterfaceUp(vlan_id_t vlan_id) {

    return this->is_up;
}

void 
VlanInterface::InterfaceReleaseAllResources() {

    assert (this->access_member_intf_lst.empty());
    VirtualInterface::InterfaceReleaseAllResources();
}

bool 
VlanInterface::IsSVI () {

    return ( this->ip_addr && this->mask ) ;
}