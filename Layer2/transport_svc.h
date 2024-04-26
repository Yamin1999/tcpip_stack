#ifndef __TRANSPORT_SVC__
#define __TRANSPORT_SVC__

#include <string>
#include <unordered_set>
#include <stdbool.h>
#include <vector>
#define DEFAULT_TSP "DEFAULT_TSP"


class Interface;
typedef struct node_ node_t;

class TransportService {

    private:

    public:
         std::string trans_svc;
        std::unordered_set<int> vlanSet;
        std::unordered_set<int> ifSet;
        int ref_count;  // how many L2 interface it is attached         
        TransportService(std::string& svc_name);
        ~TransportService();
        bool AddVlan(int vlan_id);
        bool RemoveVlan(int vlan_id);
        bool AttachInterface(Interface *intf);
        bool DeAttachInterface(Interface *intf);
        bool InUse();
};

TransportService *
TransportServiceLookUp (std::unordered_map<std::string , TransportService *> *TransPortSvcDB, std::string& svc_name);

TransportService *
TransportServiceCreate (node_t *node, std::string& svc_name);

bool 
TransportServiceDelete (std::unordered_map<std::string , TransportService *> *TransPortSvcDB, 
std::string& svc_name);

#endif 