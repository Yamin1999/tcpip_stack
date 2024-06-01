#ifndef __INTERFACE_TYPES__
#define __INTERFACE_TYPES__

enum IntfL2Mode
{
    LAN_MODE_NONE,
    LAN_ACCESS_MODE,
    LAN_TRUNK_MODE
};

enum InterfaceType_t {

    INTF_TYPE_PHY,
    INTF_TYPE_VLAN,
    INTF_TYPE_GRE_TUNNEL,
    INTF_TYPE_LOOPBACK,
    INTF_TYPE_UNKNOWN
};

#endif 