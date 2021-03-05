/*
 *  Interface to ovpn-win-dco networking code
 *
 *  Copyright (C) 2020 Arne Schwabe <arne@rfc2549.org>
 *  Copyright (C) 2020 OpenVPN Inc <sales@openvpn.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#if defined(_WIN32)

#include "syshead.h"

#include "dco.h"
#include "tun.h"
#include "crypto.h"
#include "ssl_common.h"


#include <winsock2.h>
#include <ws2tcpip.h>

#if defined(__MINGW32__)
const IN_ADDR in4addr_any = { 0 };
#endif

static struct tuntap
create_dco_handle(const char *devname, struct gc_arena *gc)
{
    struct tuntap tt = { .windows_driver = WINDOWS_DRIVER_WINDCO };
    const char *device_guid;

    tun_open_device(&tt, devname, &device_guid, gc);

    return tt;
}

bool
ovpn_dco_init(dco_context_t *dco)
{
    return true;
}

int
open_tun_dco(struct tuntap *tt, openvpn_net_ctx_t *ctx, const char *dev)
{
    ASSERT(0);
    return 0;
}

void
dco_start_tun(struct tuntap *tt)
{
    msg(D_DCO_DEBUG, "%s", __func__);

    /* reference the tt object inside the DCO context, because the latter will
     * be passed around
     */
    tt->dco.tt = tt;

    DWORD bytes_returned = 0;
    if (!DeviceIoControl(tt->hand, OVPN_IOCTL_START_VPN, NULL, 0, NULL, 0,
        &bytes_returned, NULL))
    {
        msg(M_ERR, "DeviceIoControl(OVPN_IOCTL_START_VPN) failed with code %lu", GetLastError());
    }
}

static int
dco_connect_wait(HANDLE handle, OVERLAPPED* ov, int timeout, volatile int* signal_received)
{
    DWORD timeout_msec = timeout * 1000;
    const int poll_interval_ms = 50;

    while (timeout_msec > 0)
    {
        timeout_msec -= poll_interval_ms;

        DWORD transferred;
        if (dco_get_overlapped_result(handle, ov, &transferred, poll_interval_ms, FALSE) != 0)
        {
            /* TCP connection established by dco */
            return 0;
        }

        DWORD err = GetLastError();
        if ((err != WAIT_TIMEOUT) && (err != ERROR_IO_INCOMPLETE))
        {
            /* dco reported connection error */
            struct gc_arena gc = gc_new();
            msg(M_NONFATAL, "%s: %s", __func__, strerror_win32(err, &gc));
            *signal_received = SIGUSR1;
            gc_free(&gc);
            return -1;
        }

        get_signal(signal_received);
        if (*signal_received)
        {
            return -1;
        }

        management_sleep(0);
    }

    /* we end up here when timeout occurs in userspace */
    msg(M_NONFATAL, "%s: dco connect timeout", __func__);
    *signal_received = SIGUSR1;

    return -1;
}

struct tuntap
dco_create_socket(struct addrinfo *remoteaddr, bool bind_local,
                  struct addrinfo *bind, const char* devname,
                  struct gc_arena *gc, int timeout, volatile int* signal_received)
{
    msg(D_DCO_DEBUG, "%s", __func__);

    OVPN_NEW_PEER peer = { 0 };

    struct sockaddr *local = NULL;
    struct sockaddr *remote = remoteaddr->ai_addr;

    if (remoteaddr->ai_protocol == IPPROTO_TCP
        || remoteaddr->ai_socktype == SOCK_STREAM)
    {
        peer.Proto = OVPN_PROTO_TCP;
    }
    else
    {
        peer.Proto = OVPN_PROTO_UDP;
    }

    if (bind_local)
    {
        /* Use first local address with correct address family */
        while(bind && !local)
        {
            if (bind->ai_family == remote->sa_family)
            {
                local = bind->ai_addr;
            }
            bind = bind->ai_next;
        }
    }

    if (bind_local && !local)
    {
        msg(M_FATAL, "DCO: Socket bind failed: Address to bind lacks %s record",
           addr_family_name(remote->sa_family));
    }

    if (remote->sa_family == AF_INET6)
    {
        peer.Remote.Addr6 = *((SOCKADDR_IN6 *)(remoteaddr->ai_addr));
        if (local)
        {
            peer.Local.Addr6 = *((SOCKADDR_IN6 *)local);
        }
        else
        {
            peer.Local.Addr6.sin6_addr = in6addr_any;
            peer.Local.Addr6.sin6_port = 0;
            peer.Local.Addr6.sin6_family = AF_INET6;
        }
    }
    else if (remote->sa_family == AF_INET)
    {
        peer.Remote.Addr4 = *((SOCKADDR_IN *)(remoteaddr->ai_addr));
        if (local)
        {
            peer.Local.Addr4 = *((SOCKADDR_IN *)local);
        }
        else
        {
            peer.Local.Addr4.sin_addr = in4addr_any;
            peer.Local.Addr4.sin_port = 0;
            peer.Local.Addr4.sin_family = AF_INET;
        }
    }
    else
    {
        ASSERT(0);
    }

    struct tuntap tt = create_dco_handle(devname, gc);

    OVERLAPPED ov = { 0 };
    if (!DeviceIoControl(tt.hand, OVPN_IOCTL_NEW_PEER, &peer, sizeof(peer), NULL, 0, NULL, &ov))
    {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
        {
            msg(M_ERR, "DeviceIoControl(OVPN_IOCTL_NEW_PEER) failed with code %lu", err);
        }
        else
        {
            if (dco_connect_wait(tt.hand, &ov, timeout, signal_received))
            {
                close_tun_handle(&tt);
            }
        }
    }
    return tt;
}

int dco_new_peer(dco_context_t *dco, unsigned int peerid, int sd,
                 struct sockaddr *localaddr, struct sockaddr *remoteaddr,
                 struct in_addr *remote_in4, struct in6_addr *remote_in6)
{
    msg(D_DCO_DEBUG, "%s: peer-id %d, fd %d", __func__, peerid, sd);
    return 0;
}

int dco_del_peer(dco_context_t *dco, unsigned int peerid)
{
    msg(D_DCO_DEBUG, "%s: peer-id %d - not implemented", __func__, peerid);
    return 0;
}

int ovpn_set_peer(dco_context_t *dco, unsigned int peerid,
                  unsigned int keepalive_interval,
                  unsigned int keepalive_timeout)
{
    msg(D_DCO_DEBUG, "%s: peer-id %d, keepalive %d/%d", __func__, peerid,
        keepalive_interval, keepalive_timeout);

    OVPN_SET_PEER peer;

    peer.KeepaliveInterval =  keepalive_interval;
    peer.KeepaliveTimeout = keepalive_timeout;

    DWORD bytes_returned = 0;
    if (!DeviceIoControl(dco->tt->hand, OVPN_IOCTL_SET_PEER, &peer,
                         sizeof(peer), NULL, 0, &bytes_returned, NULL))
    {
        msg(M_WARN, "DeviceIoControl(OVPN_IOCTL_SET_PEER) failed with code %lu", GetLastError());
        return -1;
    }
    return 0;
}

int
dco_new_key(dco_context_t *dco, unsigned int peerid, int keyid,
            dco_key_slot_t slot,
            const uint8_t *encrypt_key, const uint8_t *encrypt_iv,
            const uint8_t *decrypt_key, const uint8_t *decrypt_iv,
            const char *ciphername)
{
    msg(D_DCO_DEBUG, "%s: slot %d, key-id %d, peer-id %d, cipher %s",
       __func__, slot, keyid, peerid, ciphername);

    const int nonce_len = 8;
    size_t key_len = cipher_kt_key_size(ciphername);

    OVPN_CRYPTO_DATA crypto_data;
    ZeroMemory(&crypto_data, sizeof(crypto_data));

    crypto_data.CipherAlg = dco_get_cipher(ciphername);
    crypto_data.KeyId = keyid;
    crypto_data.PeerId = peerid;
    crypto_data.KeySlot = slot;

    CopyMemory(crypto_data.Encrypt.Key, encrypt_key, key_len);
    crypto_data.Encrypt.KeyLen = (char)key_len;
    CopyMemory(crypto_data.Encrypt.NonceTail, encrypt_iv, nonce_len);

    CopyMemory(crypto_data.Decrypt.Key, decrypt_key, key_len);
    crypto_data.Decrypt.KeyLen = (char)key_len;
    CopyMemory(crypto_data.Decrypt.NonceTail, decrypt_iv, nonce_len);

    ASSERT(crypto_data.CipherAlg > 0);

    DWORD bytes_returned = 0;

    if (!DeviceIoControl(dco->tt->hand, OVPN_IOCTL_NEW_KEY, &crypto_data,
                         sizeof(crypto_data), NULL, 0, &bytes_returned, NULL))
    {
        msg(M_ERR, "DeviceIoControl(OVPN_IOCTL_NEW_KEY) failed with code %lu",
            GetLastError());
        return -1;
    }
    return 0;
}
int
dco_del_key(dco_context_t *dco, unsigned int peerid, dco_key_slot_t slot)
{
    msg(D_DCO, "%s: peer-id %d, slot %d called but ignored", __func__, peerid,
        slot);
    /* FIXME: Implement in driver first */
    return 0;
}

int dco_swap_keys(dco_context_t *dco, unsigned int peer_id)
{
    msg(D_DCO_DEBUG, "%s: peer-id %d", __func__, peer_id);

    DWORD bytes_returned = 0;
    if (!DeviceIoControl(dco->tt->hand, OVPN_IOCTL_SWAP_KEYS, NULL, 0, NULL, 0,
                         &bytes_returned, NULL))
    {
        msg(M_ERR, "DeviceIoControl(OVPN_IOCTL_SWAP_KEYS) failed with code %lu",
            GetLastError());
        return -1;
    }
    return 0;
}

bool
dco_available(int msglevel)
{
    return true;
}

int
dco_do_read(dco_context_t *dco)
{
    /* no-op on windows */
    return 0;
}

int
dco_do_write(dco_context_t *dco, int peer_id, struct buffer *buf)
{
    /* no-op on windows */
    return 0;
}

void
dco_event_set(dco_context_t *dco, struct event_set *es, void *arg)
{
    /* no-op on windows */
}

#endif /* defined(_WIN32) */
