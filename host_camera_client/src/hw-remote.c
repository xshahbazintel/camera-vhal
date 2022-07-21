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
#include <sys/socket.h>
#include "hw-remote.h"
#include "debug.h"
#include "misc.h"
#include "system.h"
#include "panic.h"

#define D(...) VERBOSE_PRINT(remote, __VA_ARGS__)
#define D_ACTIVE VERBOSE_CHECK(remote)

/* the T(...) macro is used to dump traffic */
#define T_ACTIVE 0

#if T_ACTIVE
#define T(...) VERBOSE_PRINT(remote, __VA_ARGS__)
#else
#define T(...) ((void)0)
#endif

/* max serial MTU. Don't change this without modifying
 * development/emulator/remote/remote.c as well.
 */
#define MAX_SERIAL_PAYLOAD 4000

/* max framed data payload. Must be < (1 << 16)
 */
#define MAX_FRAME_PAYLOAD 65535

/* Version number of snapshots code. Increment whenever the data saved
 * or the layout in which it is saved is changed.
 */
#define REMOTE_SAVE_VERSION 2

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* define SUPPORT_LEGACY_REMOTE to 1 if you want to support
 * talking to a legacy remote daemon. See docs/ANDROID-REMOTE.TXT
 * for details.
 */
#ifdef TARGET_ARM
#define SUPPORT_LEGACY_REMOTE 1
#endif
#ifdef TARGET_I386
#define SUPPORT_LEGACY_REMOTE 0 /* no legacy support */
#endif
#if SUPPORT_LEGACY_REMOTE
#include "telephony/android_modem.h"
#include "telephony/modem_driver.h"
#endif

/*
 *  This implements support for the 'remote' multiplexing communication
 *  channel between clients running in the virtual system and 'services'
 *  provided by the emulator.
 *
 *  For additional details, please read docs/ANDROID-REMOTE.TXT
 *
 */

/*
 * IMPLEMENTATION DETAILS:
 *
 * We use one charpipe to connect the virtual serial port to the 'RemoteSerial'
 * object. This object is used to receive data from the serial port, and
 * unframe messages (i.e. extract payload length + channel id from header,
 * then the payload itself), before sending them to a generic receiver.
 *
 * The RemoteSerial object can also be used to send messages to the daemon
 * through the serial port (see remote_serial_send())
 *
 * The multiplexer is connected to one or more 'service' objects.
 * are themselves connected through a charpipe to an virtual device or
 * control sub-module in the emulator.
 *
 *  tty <==charpipe==> RemoteSerial ---> RemoteMultiplexer ----> RemoteClient
 *                          ^                                      |
 *                          |                                      |
 *                          +--------------------------------------+
 *
 */

/** HANDLING INCOMING DATA FRAMES
 **/

/* A RemoteSink is just a handly data structure that is used to
 * read a fixed amount of bytes into a buffer
 */
typedef struct RemoteSink
{
    int used; /* number of bytes already used */
    int size; /* total number of bytes in buff */
    uint8_t *buff;
} RemoteSink;

/* reset a RemoteSink, i.e. provide a new destination buffer address
 * and its size in bytes.
 */
static void
remote_sink_reset(RemoteSink *ss, int size, uint8_t *buffer)
{
    ss->used = 0;
    ss->size = size;
    ss->buff = buffer;
}

/* try to fill the sink by reading bytes from the source buffer
 * '*pmsg' which contains '*plen' bytes
 *
 * this functions updates '*pmsg' and '*plen', and returns
 * 1 if the sink's destination buffer is full, or 0 otherwise.
 */
static int
remote_sink_fill(RemoteSink *ss, const uint8_t **pmsg, int *plen)
{
    int avail = ss->size - ss->used;

    if (avail <= 0)
        return 1;

    if (avail > *plen)
        avail = *plen;

    memcpy(ss->buff + ss->used, *pmsg, avail);
    *pmsg += avail;
    *plen -= avail;
    ss->used += avail;

    return (ss->used == ss->size);
}

/* returns the number of bytes needed to fill a sink's destination
 * buffer.
 */
static int
remote_sink_needed(RemoteSink *ss)
{
    return ss->size - ss->used;
}

/** HANDLING SERIAL PORT CONNECTION
 **/

/* The RemoteSerial object receives data from the serial port charpipe.
 * It parses the header to extract the channel id and payload length,
 * then the message itself.
 *
 * Incoming messages are sent to a generic receiver identified by
 * the 'recv_opaque' and 'recv_func' parameters to remote_serial_init()
 *
 * It also provides remote_serial_send() which can be used to send
 * messages back through the serial port.
 */

#define HEADER_SIZE 6

#define LENGTH_OFFSET 2
#define LENGTH_SIZE 4

#define CHANNEL_OFFSET 0
#define CHANNEL_SIZE 2

#if SUPPORT_LEGACY_REMOTE
typedef enum
{
    REMOTE_VERSION_UNKNOWN,
    REMOTE_VERSION_LEGACY,
    REMOTE_VERSION_NORMAL
} RemoteVersion;

#define LEGACY_LENGTH_OFFSET 0
#define LEGACY_CHANNEL_OFFSET 4
#endif

/* length of the framed header */
#define FRAME_HEADER_SIZE 4

#define BUFFER_SIZE MAX_SERIAL_PAYLOAD

/* out of convenience, the incoming message is zero-terminated
 * and can be modified by the receiver (e.g. for tokenization).
 */
typedef void (*RemoteSerialReceive)(void *opaque, int channel, uint8_t *msg, int msglen);

/** CLIENTS
 **/

/* Descriptor for a data buffer pending to be sent to a remote pipe client.
 *
 * When a service decides to send data to the client, there could be cases when
 * client is not ready to read them. In this case there is no GoldfishPipeBuffer
 * available to write service's data to, So, we need to cache that data into the
 * client descriptor, and "send" them over to the client in _remotePipe_recvBuffers
 * callback. Pending service data is stored in the client descriptor as a list
 * of RemotePipeMessage instances.
 */
typedef struct RemotePipeMessage RemotePipeMessage;
struct RemotePipeMessage
{
    /* Message to send. */
    uint8_t *message;
    /* Message size. */
    size_t size;
    /* Offset in the message buffer of the chunk, that has not been sent
     * to the pipe yet. */
    size_t offset;
    /* Links next message in the client. */
    RemotePipeMessage *next;
};

/* A RemoteClient models a single client as seen by the emulator.
 * Each client has its own channel id (for the serial remote), or pipe descriptor
 * (for the pipe based remote), and belongs to a given RemoteService (see below).
 *
 * There is a global list of serial clients used to multiplex incoming
 * messages from the channel id (see remote_multiplexer_serial_recv()). Pipe
 * clients don't need multiplexing, because they are communicated via remote pipes
 * that are unique for each client.
 *
 */

/* Defines type of the client: pipe, or serial.
 */
typedef enum RemoteProtocol
{
    /* Client is communicating via pipe. */
    REMOTE_PROTOCOL_PIPE,
    /* Client is communicating via serial port. */
    REMOTE_PROTOCOL_SERIAL
} RemoteProtocol;

struct RemoteClient
{
    /* Defines protocol, used by the client. */
    RemoteProtocol protocol;

    /* Fields that are common for all protocols. */
    char *param;
    void *clie_opaque;
    RemoteClient *next_serv; /* next in same service */
    RemoteClient *next;
    RemoteClient **pref;

    /* framing support */
    int framing;
    ABool need_header;
    ABool closing;
    RemoteSink header[1];
    uint8_t header0[FRAME_HEADER_SIZE];
    RemoteSink payload[1];
};

static ABool
_is_pipe_client(RemoteClient *client)
{
    return (client->protocol == REMOTE_PROTOCOL_PIPE) ? true : false;
}

static void remote_service_remove_client(RemoteService *service,
                                         RemoteClient *client);

/* remove a RemoteClient from global list */
static void
remote_client_remove(RemoteClient *c)
{
    c->pref[0] = c->next;
    if (c->next)
        c->next->pref = c->pref;

    c->next = NULL;
    c->pref = &c->next;
}

/* add a RemoteClient to global list */
static void
remote_client_prepend(RemoteClient *c, RemoteClient **plist)
{
    c->next = *plist;
    c->pref = plist;
    *plist = c;
    if (c->next)
        c->next->pref = &c->next;
}

/* receive a new message from a client, and dispatch it to
 * the real service implementation.
 */
static void
remote_client_recv(void *opaque, uint8_t *msg, int msglen)
{
}

/* Sends data to a pipe-based client.
 */
static void
_remote_pipe_send(RemoteClient *client, const uint8_t *msg, int msglen);

/* Frees memory allocated for the remote client.
 */
static void
_remote_client_free(RemoteClient *c)
{
}

/* disconnect a client. this automatically frees the RemoteClient.
 * note that this also removes the client from the global list
 * and from its service's list, if any.
 * Param:
 *  opaque - RemoteClient instance
 *  guest_close - For pipe clients control whether or not the disconnect is
 *      caused by guest closing the pipe handle (in which case 1 is passed in
 *      this parameter). For serial clients this parameter is ignored.
 */
static void
remote_client_disconnect(void *opaque, int guest_close)
{
}

/* allocate a new RemoteClient object
 * NOTE: channel_id valie is used as a selector between serial and pipe clients.
 * Since channel_id < 0 is an invalid value for a serial client, it would
 * indicate that creating client is a pipe client. */
static RemoteClient *
remote_client_alloc(int channel_id,
                    const char *client_param,
                    void *clie_opaque,
                    RemoteClientRecv clie_recv,
                    RemoteClientClose clie_close,
                    RemoteClient **pclients)
{
    RemoteClient *c;

    return c;
}

/* forward */
static RemoteService *remote_service_find(RemoteService *service_list,
                                          const char *service_name);
static RemoteClient *remote_service_connect_client(RemoteService *sv,
                                                   int channel_id,
                                                   const char *client_param);

/** SERVICES
 **/

/* A RemoteService models a _named_ service facility implemented
 * by the emulator, that clients in the virtual system can connect
 * to.
 *
 * Each service can have a limit on the number of clients they
 * accept (this number if unlimited if 'max_clients' is 0).
 *
 * Each service maintains a list of active RemoteClients and
 * can also be used to create new RemoteClient objects through
 * its 'serv_opaque' and 'serv_connect' fields.
 */
struct RemoteService
{
    const char *name;
    int max_clients;
    int num_clients;
    RemoteClient *clients;
    RemoteServiceConnect serv_connect;
    void *serv_opaque;
    RemoteService *next;
};

/* ask the service to create a new RemoteClient. Note that we
 * assume that this calls remote_client_new() which will add
 * the client to the service's list automatically.
 *
 * returns the client or NULL if an error occurred
 */
static RemoteClient *
remote_service_connect_client(RemoteService *sv,
                              int channel_id,
                              const char *client_param)
{
    RemoteClient *client =
        sv->serv_connect(sv->serv_opaque, sv, channel_id, client_param);
    if (client == NULL)
    {
        D("%s: registration failed for '%s' service",
          __FUNCTION__, sv->name);
        return NULL;
    }
    D("%s: registered client channel %d for '%s' service",
      __FUNCTION__, channel_id, sv->name);
    return client;
}

/* find a registered service by name.
 */
static RemoteService *
remote_service_find(RemoteService *service_list, const char *service_name)
{
    RemoteService *sv = NULL;
    for (sv = service_list; sv != NULL; sv = sv->next)
    {
        if (!strcmp(sv->name, service_name))
        {
            break;
        }
    }
    return sv;
}

/* this can be used by a service implementation to send an answer
 * or message to a specific client.
 */
void remote_client_send(int fd, const uint8_t *msg, int msglen)
{
    ssize_t ret = 0;
    ssize_t len = msglen;
    char *pointer = (char *)msg;
    while (len > 0)
    {
        do
        {
            ret = write(fd, pointer, len);
        } while (ret < 0 && errno == EINTR);
        if (ret == 0)
        {
            printf("%s:%d the camera client(%d) may close. Close this client.\n", __func__, __LINE__, fd);
            shutdown(fd, SHUT_RDWR);
            close(fd);
            fd = -1;
            break;
        }
        if (ret > 0)
        {
            pointer += ret;
            len -= ret;
        }
    }
    if (len == 0)
    {
        // printf("%s:%d Secess to send '%s' to camera socket server(%d)\n", __func__, __LINE__, msg, fd);
    }
    else
    {
        printf("%s:%d Fail to send '%s' to camera socket server(%d). ret = %zd\n", __func__, __LINE__, msg, fd, ret);
    }
}

/* enable framing for this client. When TRUE, this will
 * use internally a simple 4-hexchar header before each
 * message exchanged through the serial port.
 */
void remote_client_set_framing(RemoteClient *client, int framing)
{
    /* release dynamic buffer if we're disabling framing */
    if (client->framing)
    {
        if (!client->need_header)
        {
            AFREE(client->payload->buff);
            client->need_header = 1;
        }
    }
    client->framing = !!framing;
}

/* this can be used by a service implementation to close a
 * specific client connection.
 */
void remote_client_close(RemoteClient *client)
{
    remote_client_disconnect(client, 0);
}

extern void
android_remote_init(void)
{
    D("%s", __FUNCTION__);
    /* We don't know in advance whether the guest system supports remote pipes,
     * so we will initialize both remote machineries, the legacy (over serial
     * port), and the new one (over remote pipe). Then we let the guest to connect
     * via one, or the other. */
}

/* broadcast a given message to all clients of a given RemoteService
 */
extern void
remote_service_broadcast(RemoteService *sv,
                         const uint8_t *msg,
                         int msglen)
{
    RemoteClient *c;

    for (c = sv->clients; c; c = c->next_serv)
        remote_client_send(c, msg, msglen);
}

/*
 * The following code is used for backwards compatibility reasons.
 * It allows you to implement a given remote-based service through
 * a charpipe.
 *
 * In other words, this implements a RemoteService and corresponding
 * RemoteClient that connects a remote client running in the virtual
 * system, to a CharDriverState object implemented through a charpipe.
 *
 *   RemoteCharClient <===charpipe====> (char driver user)
 *
 * For example, this is used to implement the "gsm" service when the
 * modem emulation is provided through an external serial device.
 *
 * A RemoteCharService can have only one client by definition.
 * There is no RemoteCharClient object because we can store a single
 * CharDriverState handle in the 'opaque' field for simplicity.
 */

/* we don't expect clients of char. services to exit. Just
 * print an error to signal an unexpected situation. We should
 * be able to recover from these though, so don't panic.
 */
static void
_remote_char_client_close(void *opaque)

{
    RemoteClient *client = opaque;

    /* At this point modem driver still uses char pipe to communicate with
     * hw-remote, while communication with the guest is done over remote pipe.
     * So, when guest disconnects from the remote pipe, and emulator-side client
     * goes through the disconnection process, this routine is called, since it
     * has been set to called during service registration. Unless modem driver
     * is changed to drop char pipe communication, this routine will be called
     * due to guest disconnection. As long as the client was a remote pipe - based
     * client, it's fine, since we don't really need to do anything in this case.
     */
    if (!_is_pipe_client(client))
    {
        derror("unexpected remote char. channel close");
    }
}

/* called by the charpipe to know how much data can be read from
 * the user. Since we send everything directly to the serial port
 * we can return an arbitrary number.
 */
static int
_remote_char_service_can_read(void *opaque)
{
    return 8192; /* whatever */
}

/* called to read data from the charpipe and send it to the client.
 * used remote_service_broadcast() even if there is a single client
 * because we don't need a RemoteCharClient object this way.
 */
static void
_remote_char_service_read(void *opaque, const uint8_t *from, int len)
{
    RemoteService *sv = opaque;
    remote_service_broadcast(sv, from, len);
}
