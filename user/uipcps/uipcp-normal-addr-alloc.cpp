#include <algorithm>
#include <sstream>
#include <string>
#include <unistd.h>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <iomanip>

#include "uipcp-normal.hpp"
#include "uipcp-normal-ceft.hpp"

using namespace std;

namespace rlite {

class DistributedAddrAllocator : public AddrAllocator {
    /* Table used to carry on distributed address allocation.
     * It maps (address allocated) --> (requestor name). */
    std::unordered_map<rlm_addr_t, gpb::AddrAllocRequest> addr_alloc_table;
    std::unordered_set<rlm_addr_t> addr_pending;

public:
    RL_NODEFAULT_NONCOPIABLE(DistributedAddrAllocator);
    DistributedAddrAllocator(UipcpRib *_ur) : AddrAllocator(_ur) {}
    ~DistributedAddrAllocator() {}

    void dump(std::stringstream &ss) const override;
    int allocate(const std::string &ipcp_name, rlm_addr_t *addr) override;
    int rib_handler(const CDAPMessage *rm, const MsgSrcInfo &src) override;
    int sync_neigh(const std::shared_ptr<NeighFlow> &nf,
                   unsigned int limit) const override;

    static std::string ReqObjClass;

    /* Default value for the NACK timer before considering the address
     * allocation successful. */
    static constexpr int kAddrAllocDistrNackWaitSecs = 4;
};

std::string DistributedAddrAllocator::ReqObjClass = "aareq";

void
DistributedAddrAllocator::dump(std::stringstream &ss) const
{
    ss << "Address Allocation Table:" << endl;
    for (const auto &kva : addr_alloc_table) {
        ss << "    Address: " << kva.first
           << ", Requestor: " << kva.second.requestor();
        if (addr_pending.count(kva.second.address())) {
            ss << " [pending]";
        }
        ss << endl;
    }

    ss << endl;
}

int
DistributedAddrAllocator::sync_neigh(const std::shared_ptr<NeighFlow> &nf,
                                     unsigned int limit) const
{
    int ret = 0;

    for (auto ati = addr_alloc_table.begin(); ati != addr_alloc_table.end();) {
        gpb::AddrAllocEntries l;

        while (l.entries_size() < static_cast<int>(limit) &&
               ati != addr_alloc_table.end()) {
            *l.add_entries() = ati->second;
            ati++;
        }

        ret |= nf->sync_obj(true, ObjClass, TableName, &l);
    }

    return ret;
}

int
DistributedAddrAllocator::allocate(const std::string &ipcp_name,
                                   rlm_addr_t *result)
{
    rlm_addr_t modulo = addr_alloc_table.size() + 1;
    const int inflate = 2;
    rlm_addr_t addr   = RL_ADDR_NULL;
    auto nack_wait =
        rib->get_param_value<Msecs>(AddrAllocator::Prefix, "nack-wait");

    if ((modulo << inflate) <= modulo) { /* overflow */
        modulo = ~((rlm_addr_t)0);
        UPD(rib->uipcp, "overflow\n");
    } else {
        modulo <<= inflate;
    }

    srand((unsigned int)rib->myaddr);

    for (;;) {
        /* Randomly pick an address in [0 .. modulo-1]. */
        addr = rand() % modulo;

        /* Discard the address if it is invalid, or it is already (or possibly)
         * in use by us or another IPCP in the DIF. */
        if (!addr || addr == rib->myaddr || addr_alloc_table.count(addr) > 0 ||
            rib->lookup_neighbor_by_address(addr) != string()) {
            continue;
        }

        UPD(rib->uipcp, "Trying with address %lu\n", (unsigned long)addr);
        {
            gpb::AddrAllocRequest aar;
            aar.set_address(addr);
            aar.set_requestor(rib->myname);
            addr_alloc_table[addr] = aar;
            addr_pending.insert(addr);
        }

        for (const auto &kvn : rib->neighbors) {
            if (kvn.second->enrollment_complete()) {
                gpb::AddrAllocRequest aar;
                CDAPMessage m;
                int ret;

                m.m_create(ReqObjClass, TableName);
                aar.set_requestor(rib->myname);
                aar.set_address(addr);
                ret = kvn.second->mgmt_conn()->send_to_port_id(&m, 0, &aar);
                if (ret) {
                    UPE(rib->uipcp, "Failed to send msg to neighbor [%s]\n",
                        strerror(errno));
                    return -1;
                } else {
                    UPD(rib->uipcp,
                        "Sent address allocation request to neigh %s, "
                        "(addr=%lu,requestor=%s)\n",
                        kvn.second->ipcp_name.c_str(),
                        (long unsigned)aar.address(), aar.requestor().c_str());
                }
            }
        }

        rib->unlock();
        /* Wait a bit for possible negative responses. */
        sleep(std::chrono::duration_cast<Secs>(nack_wait).count());
        rib->lock();

        /* If the request is still there, then we consider the allocation
         * complete. */
        auto mit = addr_alloc_table.find(addr);
        if (mit != addr_alloc_table.end() &&
            mit->second.requestor() == rib->myname) {
            addr_pending.erase(addr);
            UPD(rib->uipcp, "Address %lu allocated\n", (unsigned long)addr);
            break;
        }
    }

    *result = addr;
    return 0;
}

int
DistributedAddrAllocator::rib_handler(const CDAPMessage *rm,
                                      const MsgSrcInfo &src)
{
    bool create;
    const char *objbuf;
    size_t objlen;

    rm->get_obj_value(objbuf, objlen);
    if (!objbuf) {
        UPE(rib->uipcp, "No object value found\n");
        return 0;
    }

    switch (rm->op_code) {
    case gpb::M_CREATE:
        create = true;
        break;
    case gpb::M_DELETE:
        create = false;
        break;
    default:
        UPE(rib->uipcp, "M_CREATE/M_DELETE expected\n");
        return 0;
    }

    if (rm->obj_class == ReqObjClass) {
        /* This is an address allocation request or a negative
         * address allocation response. */
        bool propagate = false;
        bool cand_neigh_conflict;
        gpb::AddrAllocRequest aar;

        aar.ParseFromArray(objbuf, objlen);

        /* Lookup the address contained in the request into the allocation
         * table and among the neighbor candidates. Also check if the proposed
         * address conflicts with our own address. */
        auto mit = addr_alloc_table.find(aar.address());
        cand_neigh_conflict =
            aar.address() == rib->myaddr ||
            rib->lookup_neighbor_by_address(aar.address()) != string();

        switch (rm->op_code) {
        case gpb::M_CREATE:
            if (!cand_neigh_conflict && mit == addr_alloc_table.end()) {
                /* New address allocation request, no conflicts. */
                addr_alloc_table[aar.address()] = aar;
                UPD(rib->uipcp,
                    "Address allocation request ok, (addr=%lu,"
                    "requestor=%s)\n",
                    (long unsigned)aar.address(), aar.requestor().c_str());
                propagate = true;

            } else if (cand_neigh_conflict ||
                       mit->second.requestor() != aar.requestor()) {
                /* New address allocation request, but there is a conflict. */
                std::unique_ptr<CDAPMessage> m =
                    utils::make_unique<CDAPMessage>();
                int ret;

                UPI(rib->uipcp,
                    "Address allocation request conflicts, (addr=%lu,"
                    "requestor=%s)\n",
                    (long unsigned)aar.address(), aar.requestor().c_str());
                m->m_delete(ReqObjClass, TableName);
                ret =
                    rib->send_to_dst_node(std::move(m), aar.requestor(), &aar);
                if (ret) {
                    UPE(rib->uipcp, "Failed to send message to %s [%s]\n",
                        aar.requestor().c_str(), strerror(errno));
                }
            } else {
                /* We have already seen this request, don't propagate. */
            }
            break;

        case gpb::M_DELETE:
            if (mit != addr_alloc_table.end()) {
                if (addr_pending.count(aar.address())) {
                    /* Negative feedback on a flow allocation request. */
                    addr_alloc_table.erase(aar.address());
                    addr_pending.erase(aar.address());
                    propagate = true;
                    UPI(rib->uipcp,
                        "Address allocation request deleted, "
                        "(addr=%lu,requestor=%s)\n",
                        (long unsigned)aar.address(), aar.requestor().c_str());
                } else {
                    /* Late negative feedback. This is a serious problem
                     * that we don't manage for now. */
                    UPE(rib->uipcp,
                        "Conflict on a committed address! "
                        "Part of the network may be "
                        "unreachable "
                        "(addr=%lu,requestor=%s)\n",
                        (long unsigned)aar.address(), aar.requestor().c_str());
                }
            }
            break;

        default:
            assert(0);
        }

        if (propagate) {
            /* nf can be nullptr for M_DELETE messages */
            rib->neighs_sync_obj_excluding(src.neigh, create, rm->obj_class,
                                           rm->obj_name, &aar);
        }

    } else if (rm->obj_class == ObjClass) {
        /* This is a synchronization operation targeting our
         * address allocation table. */
        gpb::AddrAllocEntries aal;
        gpb::AddrAllocEntries prop_aal;

        aal.ParseFromArray(objbuf, objlen);
        for (const gpb::AddrAllocRequest &r : aal.entries()) {
            auto mit = addr_alloc_table.find(r.address());

            if (rm->op_code == gpb::M_CREATE) {
                if (mit == addr_alloc_table.end() ||
                    mit->second.requestor() != r.requestor()) {
                    addr_alloc_table[r.address()] = r; /* overwrite */
                    *prop_aal.add_entries()       = r;
                    UPD(rib->uipcp,
                        "Address allocation entry created (addr=%lu,"
                        "requestor=%s)\n",
                        (long unsigned)r.address(), r.requestor().c_str());
                }
            } else { /* M_DELETE */
                if (mit != addr_alloc_table.end() &&
                    mit->second.requestor() == r.requestor()) {
                    addr_alloc_table.erase(r.address());
                    *prop_aal.add_entries() = r;
                    UPD(rib->uipcp,
                        "Address allocation entry deleted (addr=%lu,"
                        "requestor=%s)\n",
                        (long unsigned)r.address(), r.requestor().c_str());
                }
            }
        }

        if (prop_aal.entries_size() > 0) {
            assert(src.nf);
            rib->neighs_sync_obj_excluding(src.neigh, create, rm->obj_class,
                                           rm->obj_name, &prop_aal);
        }

    } else {
        UPE(rib->uipcp, "Unexpected object class %s\n", rm->obj_class.c_str());
    }

    return 0;
}

class StaticAddrAllocator : public DistributedAddrAllocator {
public:
    RL_NODEFAULT_NONCOPIABLE(StaticAddrAllocator);
    StaticAddrAllocator(UipcpRib *_ur) : DistributedAddrAllocator(_ur) {}
    int allocate(const std::string &ipcp_name, rlm_addr_t *addr) override
    {
        *addr = RL_ADDR_NULL;
        return 0;
    }
};

class CentralizedFaultTolerantAddrAllocator : public AddrAllocator {
    /* An instance of this class can be a state machine replica or it can just
     * be a client that will redirect requests to one of the replicas. */

    /* In case of state machine replica, a pointer to a Raft state
     * machine. */
    class Replica : public CeftReplica {
        /* The structure of an address allocation command (i.e. a log entry
         * for the Raft SM). The command contains the name of the IPCP for
         * which we want to allocate or deallocate an address. Raft is only
         * needed to achieve consensus on the order of allocation and
         * deallocation operations. */
        struct Command {
            char ipcp_name[31];
            uint8_t opcode;
            static constexpr uint8_t OpcodeSet = 1;
            static constexpr uint8_t OpcodeDel = 2;
        } __attribute__((packed));
        static_assert(sizeof(Command) ==
                          sizeof(Command::ipcp_name) + sizeof(Command::opcode),
                      "Invalid memory layout for class Replica::Command");

        /* State machine implementation: a simple table mapping IPCP names
         * into addresses, plus a simple counter to keep the next address
         * to allocate. */
        std::map<string, rlm_addr_t> table;
        rlm_addr_t next_unused_address = 1;

    public:
        Replica(CentralizedFaultTolerantAddrAllocator *aa,
                std::list<raft::ReplicaId> peers)
            : CeftReplica(aa->rib, std::string("ceft-aa-") + aa->rib->myname,
                          aa->rib->myname,
                          std::string("/tmp/ceft-aa-") +
                              std::to_string(aa->rib->uipcp->id) +
                              std::string("-") + aa->rib->myname,
                          sizeof(Command), AddrAllocator::TableName)
        {
            /* Allocate addresses for the replicas in advance. */
            std::vector<raft::ReplicaId> peersv(peers.begin(), peers.end());
            std::sort(peersv.begin(), peersv.end());
            for (const auto &peer : peersv) {
                table[peer] = next_unused_address++;
            }
        };
        int apply(const char *const serbuf, CDAPMessage *const rm) override;
        virtual int replica_process_rib_msg(
            const CDAPMessage *rm, rlm_addr_t src_addr,
            std::vector<CommandToSubmit> *commands) override;
        void dump(std::stringstream &ss) const;

        rlm_addr_t lookup(const std::string &ipcp_name) const
        {
            const auto mit = table.find(ipcp_name);
            if (mit == table.end()) {
                return RL_ADDR_NULL;
            }
            return mit->second;
        }
    };
    std::unique_ptr<Replica> raft;

    /* In case of client, a pointer to client-side data structures. */
    class Client : public CeftClient {
        struct Synchronizer {
            std::condition_variable allocation_complete;
            bool allocated     = false;
            rlm_addr_t address = RL_ADDR_NULL;
        };
        struct PendingReq : public CeftClient::PendingReq {
            /* The IPCP to allocate the address for. */
            std::string ipcp_name;
            /* Synchronization variables for the client RIB handler to inform
             * the enroller thread about the allocated address. */
            std::shared_ptr<Synchronizer> synchro;
            PendingReq() = default;
            PendingReq(gpb::OpCode op_code, Msecs timeout,
                       const std::string &ipcp_name,
                       const std::shared_ptr<Synchronizer> &synchro)
                : CeftClient::PendingReq(op_code, timeout),
                  ipcp_name(ipcp_name),
                  synchro(synchro)
            {
            }
            std::unique_ptr<CeftClient::PendingReq> clone() const override
            {
                return utils::make_unique<PendingReq>(*this);
            }
        };

    public:
        Client(CentralizedFaultTolerantAddrAllocator *aa,
               std::list<raft::ReplicaId> names)
            : CeftClient(aa->rib, std::move(names))
        {
        }
        int allocate(const std::string &ipcp_name, rlm_addr_t *addr);
        int client_process_rib_msg(const CDAPMessage *rm,
                                   CeftClient::PendingReq *const bpr,
                                   rlm_addr_t src_addr) override;
    };
    std::unique_ptr<Client> client;

public:
    CentralizedFaultTolerantAddrAllocator(UipcpRib *rib) : AddrAllocator(rib) {}
    ~CentralizedFaultTolerantAddrAllocator() {}
    int reconfigure() override;

    void dump(std::stringstream &ss) const override
    {
        if (raft) {
            raft->dump(ss);
        } else {
            ss << "Address allocation table: not available locally" << endl
               << endl;
        }
    }

    int allocate(const std::string &ipcp_name, rlm_addr_t *addr) override
    {
        if (raft) {
            /* Addresses in the cluster are pre-allocated to bootstrap
             * the address allocation infrastructure. */
            *addr = raft->lookup(ipcp_name);
            if (*addr != RL_ADDR_NULL) {
                return 0;
            }
            /* This happens if 'ipcp_name' is not a replica. Fallback on
             * regular client allocation. We also set the leader, since we
             * know it. */
            client->set_leader_id(raft->leader_name());
        }
        return client->allocate(ipcp_name, addr);
    }

    int rib_handler(const CDAPMessage *rm, const MsgSrcInfo &src) override
    {
        if (!raft || (rm->obj_class == ObjClass && rm->is_response())) {
            /* We may be a replica (raft != nullptr), but if this is a response
             * to a request done by us with the role of simple clients we
             * forward it to the client handler. */
            return client->rib_handler(rm, src);
        }

        return raft->rib_handler(rm, src);
    }
};

int
CentralizedFaultTolerantAddrAllocator::reconfigure()
{
    list<raft::ReplicaId> peers;
    string replicas;

    if (client) {
        return 0; /* nothing to do */
    }

    replicas =
        rib->get_param_value<std::string>(AddrAllocator::Prefix, "replicas");
    if (replicas.empty()) {
        UPW(rib->uipcp, "replicas param not configured\n");
        return 0;
    }
    UPD(rib->uipcp, "replicas = %s\n", replicas.c_str());
    peers = utils::strsplit<std::list>(replicas, ',');

    /* Create the client anyway. */
    client = utils::make_unique<Client>(this, peers);
    UPI(rib->uipcp, "Client initialized\n");

    /* I'm one of the replicas. Create a Raft state machine and
     * initialize it. */
    auto it = std::find(peers.begin(), peers.end(), rib->myname);
    if (it != peers.end()) {
        raft = utils::make_unique<Replica>(this, peers);
        peers.erase(it); /* remove myself */

        rlm_addr_t myaddress = raft->lookup(rib->myname);
        assert(myaddress != RL_ADDR_NULL);
        rib->set_address(myaddress);

        auto election_timeout = rib->get_param_value<Msecs>(
            AddrAllocator::Prefix, "raft-election-timeout");
        auto heartbeat_timeout = rib->get_param_value<Msecs>(
            AddrAllocator::Prefix, "raft-heartbeat-timeout");
        auto rtx_timeout = rib->get_param_value<Msecs>(AddrAllocator::Prefix,
                                                       "raft-rtx-timeout");
        raft->set_election_timeout(election_timeout, election_timeout * 2);
        raft->set_heartbeat_timeout(heartbeat_timeout);
        raft->set_retransmission_timeout(rtx_timeout);

        return raft->init(peers);
    }

    return 0;
}

int
CentralizedFaultTolerantAddrAllocator::Client::allocate(
    const std::string &ipcp_name, rlm_addr_t *addr)
{
    auto m       = utils::make_unique<CDAPMessage>();
    auto synchro = std::make_shared<Synchronizer>();

    *addr = RL_ADDR_NULL;
    m->m_create(ObjClass, TableName + "/" + ipcp_name);

    auto timeout =
        rib->get_param_value<Msecs>(AddrAllocator::Prefix, "cli-timeout");
    auto pr =
        utils::make_unique<PendingReq>(m->op_code, timeout, ipcp_name, synchro);
    int ret = send_to_replicas(std::move(m), std::move(pr), OpSemantics::Put);
    if (ret) {
        return ret;
    }

    UPI(rib->uipcp, "Issued address allocation request for IPCP '%s'\n",
        ipcp_name.c_str());

    /* Wait for the address allocation to complete. We need to drop
     * the RIB lock before waiting. */
    rib->unlock();

    {
        std::unique_lock<std::mutex> lk(rib->mutex);

        while (!synchro->allocated) {
            if (synchro->allocation_complete.wait_for(lk, timeout) ==
                std::cv_status::timeout) {
                UPW(rib->uipcp, "Address allocation for IPCP '%s' timed out\n",
                    ipcp_name.c_str());
                return -1;
            }
        }

        UPD(rib->uipcp, "Address %lu successfully allocated for IPCP '%s'\n",
            (long unsigned)synchro->address, ipcp_name.c_str());
        *addr = synchro->address;
        /* The RIB lock gets released here, so we need to acquire it again
         * below. */
    }

    rib->lock();

    return 0;
}

int
CentralizedFaultTolerantAddrAllocator::Client::client_process_rib_msg(
    const CDAPMessage *rm, CeftClient::PendingReq *const bpr,
    rlm_addr_t src_addr)
{
    PendingReq const *pr = dynamic_cast<PendingReq *>(bpr);
    struct uipcp *uipcp  = rib->uipcp;

    switch (rm->op_code) {
    case gpb::M_READ_R:
    case gpb::M_CREATE_R: {
        rlm_addr_t address = RL_ADDR_NULL;

        if (rm->result) {
            UPD(uipcp, "Address %s for IPCP '%s' failed remotely [%s]\n",
                rm->op_code == gpb::M_CREATE_R ? "allocation" : "lookup",
                pr->ipcp_name.c_str(), rm->result_reason.c_str());
        } else {
            int64_t addr;

            rm->get_obj_value(addr);
            address = static_cast<rlm_addr_t>(addr);
            UPD(uipcp, "Address %llu %s for IPCP '%s'\n",
                (long long unsigned)address,
                rm->op_code == gpb::M_CREATE_R ? "allocated" : "looked up",
                pr->ipcp_name.c_str());
        }

        if (rm->op_code == gpb::M_CREATE_R) {
            pr->synchro->address   = address;
            pr->synchro->allocated = true;
            pr->synchro->allocation_complete.notify_one();
        }
        break;
    }
    default:
        assert(false);
    }

    return 0;
}

void
CentralizedFaultTolerantAddrAllocator::Replica::dump(
    std::stringstream &ss) const
{
    ss << "Address Allocation Table:" << std::endl;
    for (const auto &kv : table) {
        ss << "    " << std::setw(20) << kv.first;
        ss << ": " << kv.second << std::endl;
    }
}

/* Apply a command to the replicated state machine. We just need to update our
 * map. */
int
CentralizedFaultTolerantAddrAllocator::Replica::apply(const char *const serbuf,
                                                      CDAPMessage *const rm)
{
    auto c = reinterpret_cast<const Command *const>(serbuf);

    assert(c->opcode == Command::OpcodeSet || c->opcode == Command::OpcodeDel);
    if (c->opcode == Command::OpcodeSet) {
        /* Allocate an address (for now we use a trivial sequential strategy)
         * and update our table. */
        table[c->ipcp_name] = next_unused_address;
        if (rm) {
            /* Update the response if we have one. */
            rm->set_obj_value((static_cast<int64_t>(next_unused_address)));
        }
        UPD(rib->uipcp, "Commit %s <-- %lu\n", c->ipcp_name,
            (long unsigned)next_unused_address);
        next_unused_address++;
    } else {
        table.erase(c->ipcp_name);
    }

    return 0;
}

int
CentralizedFaultTolerantAddrAllocator::Replica::replica_process_rib_msg(
    const CDAPMessage *rm, rlm_addr_t src_addr,
    std::vector<CommandToSubmit> *commands)
{
    struct uipcp *uipcp = rib->uipcp;

    if (rm->obj_class != ObjClass) {
        UPE(uipcp, "Unexpected object class '%s'\n", rm->obj_class.c_str());
        return 0;
    }

    /* Recover the name of the IPCP for which we want to allocate or
     * lookup the address. */
    string ipcp_name = rm->obj_name.substr(rm->obj_name.rfind("/") + 1);
    const auto mit   = table.find(ipcp_name);

    /* Either we are the leader (so we can go ahead and serve the request),
     * or this is a request that does not require consensus (so we can serve it
     * because it's ok to be eventually consistent). */
    if (!(leader() || mit != table.end())) {
        /* We are not the leader and this is not a read request. We
         * need to deny the request to preserve consistency. */
        UPD(uipcp, "Ignoring request, let the leader answer\n");
        return 0;
    }

    if (rm->op_code == gpb::M_CREATE) {
        /* We received an M_CREATE sent by Client::allocate() and we are the
         * leader. */
        auto m = utils::make_unique<CDAPMessage>();

        m->op_code   = gpb::M_CREATE_R;
        m->obj_name  = rm->obj_name;
        m->obj_class = rm->obj_class;
        m->invoke_id = rm->invoke_id;

        if (mit != table.end()) {
            /* An address has already been allocated. Raft is not needed,
             * just return it. */
            m->set_obj_value((static_cast<int64_t>(mit->second)));
            rib->send_to_dst_addr(std::move(m), src_addr);
        } else {
            /* We are the leader here. Create a command and submit it to the
             * Raft state machine. */
            auto cbuf  = std::unique_ptr<char[]>(new char[sizeof(Command)]);
            Command *c = reinterpret_cast<Command *>(cbuf.get());

            /* Fill in the command struct (already serialized). */
            strncpy(c->ipcp_name, ipcp_name.c_str(), sizeof(c->ipcp_name) - 1);
            c->opcode = Command::OpcodeSet;

            /* Return the command to the caller. */
            commands->push_back(make_pair(std::move(cbuf), std::move(m)));
        }
    } else if (rm->op_code == gpb::M_READ) {
        /* We received an an M_READ. Look up the IPCP in the map. */
        auto m = utils::make_unique<CDAPMessage>();

        m->m_read_r(rm->obj_class, rm->obj_name, /*obj_inst=*/0,
                    /*result=*/mit == table.end() ? -1 : 0,
                    /*result_reason=*/mit == table.end() ? "No address found"
                                                         : string());
        m->invoke_id = rm->invoke_id;
        m->set_obj_value((static_cast<int64_t>(mit->second)));
        rib->send_to_dst_addr(std::move(m), src_addr);
    } else {
        UPE(uipcp, "M_CREATE(aa), M_READ(aa) or M_DELETE(aa) expected\n");
        return 0;
    }

    return 0;
}

void
UipcpRib::addra_lib_init()
{
    UipcpRib::policy_register(
        AddrAllocator::Prefix, "static", [](UipcpRib *rib) {
            return utils::make_unique<StaticAddrAllocator>(rib);
        });
    UipcpRib::policy_register(
        AddrAllocator::Prefix, "distributed",
        [](UipcpRib *rib) {
            return utils::make_unique<DistributedAddrAllocator>(rib);
        },
        {AddrAllocator::TableName},
        {{"nack-wait",
          PolicyParam(Secs(
              int(DistributedAddrAllocator::kAddrAllocDistrNackWaitSecs)))}});
    UipcpRib::policy_register(
        AddrAllocator::Prefix, "centralized-fault-tolerant",
        [](UipcpRib *rib) {
            return utils::make_unique<CentralizedFaultTolerantAddrAllocator>(
                rib);
        },
        {AddrAllocator::TableName},
        {{"replicas", PolicyParam(string())},
         {"cli-timeout", PolicyParam(Secs(int(CeftClient::kTimeoutSecs)))},
         {"raft-election-timeout", PolicyParam(Secs(1))},
         {"raft-heartbeat-timeout",
          PolicyParam(Msecs(int(CeftReplica::kHeartBeatTimeoutMsecs)))},
         {"raft-rtx-timeout",
          PolicyParam(Msecs(int(CeftReplica::kRtxTimeoutMsecs)))}});
}

} // namespace rlite
