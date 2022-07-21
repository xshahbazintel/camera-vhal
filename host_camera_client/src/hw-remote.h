/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#ifndef _android_remote_h
#define _android_remote_h

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <signal.h>

#ifdef _WIN32
#include "sysemu/os-win32.h"
#endif

#ifdef CONFIG_POSIX
#include "sysemu/os-posix.h"
#endif

/* Support for the remote-based 'services' in the emulator.
 * Please read docs/ANDROID-REMOTE.TXT to understand what this is about.
 */

/* initialize the remote support code in the emulator
 */

extern void android_remote_init(void);

/* list of known remote channel names */
#define ANDROID_REMOTE_GSM "gsm"
#define ANDROID_REMOTE_GPS "gps"
#define ANDROID_REMOTE_CONTROL "control"
#define ANDROID_REMOTE_SENSORS "sensors"

/* A RemoteService service is used to connect one or more clients to
 * a given emulator facility. Only one client can be connected at any
 * given time, but the connection can be closed periodically.
 */

typedef struct RemoteClient RemoteClient;
typedef struct RemoteService RemoteService;

/* A function that will be called when the client running in the virtual
 * system has closed its connection to remote.
 */
typedef void (*RemoteClientClose)(void *opaque);

/* A function that will be called when the client sends a message to the
 * service through remote.
 */
typedef void (*RemoteClientRecv)(void *opaque, uint8_t *msg, int msglen, RemoteClient *client);

/* Enable framing on a given client channel.
 */
extern void remote_client_set_framing(RemoteClient *client, int enabled);

/* Send a message to a given remote client
 */
extern void remote_client_send(int fd, const uint8_t *msg, int msglen);

/* Force-close the connection to a given remote client.
 */
extern void remote_client_close(RemoteClient *client);

/* A function that will be called each time a new client in the virtual
 * system tries to connect to a given remote service. This should typically
 * call remote_client_new() to register a new client.
 */
typedef RemoteClient *(*RemoteServiceConnect)(void *opaque,
                                            RemoteService *service,
                                            int channel,
                                            const char *client_param);

/* Sends a message to all clients of a given service.
 */
extern void remote_service_broadcast(RemoteService *sv,
                                    const uint8_t *msg,
                                    int msglen);

#endif /* _android_remote_h */
