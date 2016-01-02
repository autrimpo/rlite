#ifndef __RINA_CONF_MSG_H__
#define __RINA_CONF_MSG_H__

#include <stdint.h>

#include <rina/rina-ipcp-types.h>
#include <rina/rina-common.h>
#include <rina/rina-utils.h>


/* Message types. They **must** be listed alternating requests with
 * the corresponding responses. */
enum {
    RINA_CONF_IPCP_REGISTER = 1,  /* 1 */
    RINA_CONF_IPCP_ENROLL,        /* 2 */
    RINA_CONF_IPCP_DFT_SET,       /* 3 */
    RINA_CONF_BASE_RESP,          /* 4 */

    RINA_CONF_MSG_MAX,
};

/* Numtables for rina-config <==> ipcm messages exchange. */

extern struct rina_msg_layout rina_conf_numtables[RINA_CONF_MSG_MAX];

/* Application --> IPCM message to register an IPC process
 * to another IPC process */
struct rina_amsg_ipcp_register {
    rina_msg_t msg_type;
    uint32_t event_id;

    uint8_t reg;
    struct rina_name ipcp_name;
    struct rina_name dif_name;
} __attribute__((packed));

/* Application --> IPCM message to enroll an IPC process
 * to another IPC process */
struct rina_amsg_ipcp_enroll {
    rina_msg_t msg_type;
    uint32_t event_id;

    struct rina_name dif_name;
    struct rina_name ipcp_name;
    struct rina_name neigh_ipcp_name;
    struct rina_name supp_dif_name;
} __attribute__((packed));

/* Application --> IPCM message to set an IPC process DFT entry */
struct rina_amsg_ipcp_dft_set {
    rina_msg_t msg_type;
    uint32_t event_id;

    uint64_t remote_addr;
    struct rina_name ipcp_name;
    struct rina_name appl_name;
} __attribute__((packed));

#endif  /* __RINA_CONF_MSG_H__ */
