#ifndef PARTYBOARD_PORT_NETPLAY_H
#define PARTYBOARD_PORT_NETPLAY_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL partyboard_netplay_initialize(int argc, char **argv);
BOOL partyboard_netplay_requested(int argc, char **argv);
BOOL partyboard_netplay_host(u16 port, u32 inputDelay);
BOOL partyboard_netplay_join(const char *address, u16 port);
void partyboard_netplay_cancel_pending(void);
const char *partyboard_netplay_last_error(void);
void partyboard_netplay_shutdown(void);
BOOL partyboard_netplay_active(void);
BOOL partyboard_netplay_sync_frame(u32 frame);
u32 partyboard_netplay_random_seed(void);

#ifdef __cplusplus
}
#endif

#endif
