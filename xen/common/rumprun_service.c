/*
 * LibrettOS Network Server
 *
 * Copyright (c) 2018-2019 Mincheol Sung <mincheol@vt.edu>
 * Copyright (c) 2018-2019 Ruslan Nikolaev <rnikola@vt.edu>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <xen/lib.h>
#include <xen/spinlock.h>
#include <xen/guest_access.h>
#include <xen/rumprun_service.h>
#include <xen/sched.h>

#define RUMPRUN_SERVICE_UNREGISTERED	0
#define RUMPRUN_SERVICE_REGISTERED	1

typedef struct rumprun_service_data {
	backend_connect_t	data;
	uint32_t			state;
	frontend_connect_t	app_data[RUMPRUN_NUM_OF_APPS];
	spinlock_t			lock;
} rumprun_service_data_t;

#define TCP 6
#define UDP 17

#define tcp_portmap ((portmap_entry_t *)xen_netdom_map)
#define udp_portmap ((portmap_entry_t *)xen_netdom_map + 65536)

/* A pool of all available IP addresses; 0 if allocated */
static pool_entry_t ip_addrpool[RUMPRUN_SERVICE_IPS+1]; /* only for sysid = 0 */
/* Allocated IP addresses; 0 if shared (non-dynamic) */
static pool_entry_t ip_alloc[RUMPRUN_NUM_OF_APPS] = { { 0, 0, 0 } }; /* only for sysid = 0 */
/* The very last address is shared */
#define ip_shared_addr (ip_addrpool[RUMPRUN_SERVICE_IPS])

static unsigned int dom = 0;

static rumprun_service_data_t rumprun_service_data[RUMPRUN_SYSIDS] =
	{{ .state = RUMPRUN_SERVICE_UNREGISTERED, .lock = SPIN_LOCK_UNLOCKED},
	 { .state = RUMPRUN_SERVICE_UNREGISTERED, .lock = SPIN_LOCK_UNLOCKED},
	 { .state = RUMPRUN_SERVICE_UNREGISTERED, .lock = SPIN_LOCK_UNLOCKED}};

int do_rumprun_service_op(int op, int sysid, XEN_GUEST_HANDLE(void) ptr)
{
	uint32_t domid = current->domain->domain_id;
	int rc = 0;

	if ((unsigned int) sysid >= RUMPRUN_SYSIDS)
		return -EINVAL;

	spin_lock(&rumprun_service_data[sysid].lock);
	switch (op)
	{
		case RUMPRUN_SERVICE_CLEANUP:
		{
			printk("RUMPRUN_SERVICE_CLEANUP\n");
			rumprun_service_data[sysid].state = RUMPRUN_SERVICE_UNREGISTERED;

			for (unsigned int i = 0; i < RUMPRUN_NUM_OF_APPS; i++)
				rumprun_service_data[sysid].app_data[i].status = RUMPRUN_FRONTEND_DEAD;

			dom = 0;
			memset(xen_netdom_map, 0xFF, PAGE_SIZE * XEN_NETDOM_PAGES);
			memset(ip_addrpool, 0, (RUMPRUN_SERVICE_IPS + 1) * sizeof(ip_addrpool[0]));
			break;
		}

		case RUMPRUN_SERVICE_REGISTER:
		{
			if (sysid == 0)
				memset(ip_addrpool, 0, (RUMPRUN_SERVICE_IPS + 1) * sizeof(ip_addrpool[0]));

			if (copy_from_guest((void *) &rumprun_service_data[sysid].data, ptr, sizeof(backend_connect_t)) != 0) {
				rc = -EFAULT;
				goto error;
			}
			rumprun_service_data[sysid].data.domid = domid;
			rumprun_service_data[sysid].state = RUMPRUN_SERVICE_REGISTERED;
			break;
		}

		case RUMPRUN_SERVICE_REGISTER_IP:
		{
			if (copy_from_guest((void *) ip_addrpool, ptr, sizeof(ip_addrpool[0]) * (RUMPRUN_SERVICE_IPS + 1))) {
				rc = -EFAULT;
				goto error;
			}

			break;
		}

		case RUMPRUN_SERVICE_UNREGISTER:
		{
			if (rumprun_service_data[sysid].state != RUMPRUN_SERVICE_REGISTERED) {
				rc = -EAGAIN;
				goto error;
			}
			if (unlikely(domid != rumprun_service_data[sysid].data.domid)) {
				rc = -EPERM;
				goto error;
			}
			rumprun_service_data[sysid].state = RUMPRUN_SERVICE_UNREGISTERED;
			break;
		}

		case RUMPRUN_SERVICE_REGISTER_APP_DYNAMIC:
		case RUMPRUN_SERVICE_REGISTER_APP:
		{
			int idx_alloc = -1;
			unsigned int i, new_dom;

			if (dom >= RUMPRUN_NUM_OF_APPS) {
				printk("app_dom is full\n");
				rc = -EFAULT;
				goto error;
			}

			for (i = 0; i < RUMPRUN_NUM_OF_APPS; i++) {
				if (rumprun_service_data[sysid].app_data[i].domid == domid) {
					if (rumprun_service_data[sysid].app_data[i].status == RUMPRUN_FRONTEND_ACTIVE) {
						new_dom = i;
						goto register_app;
					}
				}
			}
			new_dom = dom;
register_app:
			if (op == RUMPRUN_SERVICE_REGISTER_APP) {
				if (ip_alloc[new_dom].ipaddr != 0) { /* release IP */
					for (i = 0; i < RUMPRUN_SERVICE_IPS; i++) {
						if (ip_addrpool[i].ipaddr == 0) {
							ip_addrpool[i] = ip_alloc[new_dom];
							break;
						}
					}
					ip_alloc[new_dom].ipaddr = 0;
					ip_alloc[new_dom].netmask = 0;
					ip_alloc[new_dom].mtu = 0;
				}
			} else { /* RUMPRUN_SERVICE_REGISTER_APP_DYNAMIC */
				if (ip_alloc[new_dom].ipaddr == 0) { /* allocate IP */
					for (i = 0; i < RUMPRUN_SERVICE_IPS; i++) {
						if (ip_addrpool[i].ipaddr != 0) {
							ip_alloc[new_dom] = ip_addrpool[i];
							ip_addrpool[i].ipaddr = 0;
							ip_addrpool[i].netmask = 0;
							ip_addrpool[i].mtu = 0;
							idx_alloc = i;
							goto register_app2;
						}
					}
					rc = -EADDRINUSE;
					goto error;
				}
			}
register_app2:
			if (copy_from_guest((void *) &rumprun_service_data[sysid].app_data[new_dom], ptr, sizeof(frontend_connect_t)) != 0) {
				if (idx_alloc != -1) {
					/* roll back */
					ip_addrpool[idx_alloc] = ip_alloc[new_dom];
					ip_alloc[new_dom].ipaddr = 0;
					ip_alloc[new_dom].netmask = 0;
					ip_alloc[new_dom].mtu = 0;
				}
				rc = -EFAULT;
				goto error;
			}
			dom++;
			rumprun_service_data[sysid].app_data[new_dom].domid = domid;

			break;
		}

		case RUMPRUN_SERVICE_FETCH_IP:
		{
			pool_entry_t ip;
			unsigned int i;
			for (i = 0; i < RUMPRUN_NUM_OF_APPS; i++) {
				if (rumprun_service_data[sysid].app_data[i].domid == domid) {
					if (rumprun_service_data[sysid].app_data[i].status == RUMPRUN_FRONTEND_ACTIVE) {
						ip = ip_alloc[i];
						if (ip.ipaddr == 0)
							ip = ip_shared_addr;
						goto copy_ip;
					}
				}
			}
			rc = -EINVAL;
			goto error;
copy_ip:
			if (copy_to_guest(ptr, (void *) &ip, sizeof(ip)) != 0) {
				rc = -EFAULT;
				goto error;
			}

			break;
		}

		case RUMPRUN_SERVICE_QUERY:
		{
			if (copy_to_guest(ptr, (void *) &rumprun_service_data[sysid].data, sizeof(backend_connect_t)) != 0) {
				rc = -EFAULT;
				goto error;
			}

			break;
		}

		case RUMPRUN_SERVICE_FETCH:
		case RUMPRUN_SERVICE_RECONNECT:
		{
			if (copy_to_guest(ptr, (void *) &rumprun_service_data[sysid].app_data, sizeof(frontend_connect_t) * RUMPRUN_NUM_OF_APPS) != 0)
			{
				rc = -EFAULT;
				goto error;
			}
			break;
		}

		default:
		{
			rc = -EINVAL;
			break;
		}
	}

error:
	spin_unlock(&rumprun_service_data[sysid].lock);
	return rc;
}

static unsigned int get_dom(int sysid, unsigned int domid)
{
	unsigned int i;
	for (i = 0; i < RUMPRUN_NUM_OF_APPS; i++) {
		if (rumprun_service_data[sysid].app_data[i].domid == domid)
			return i;
	}
	return -1;
}

static uint16_t assign_random_port(portmap_entry_t *portmap, portmap_entry_t new)
{
	uint16_t hport;
	for (hport = 49152; hport != 0; hport++) { /* 49152 .. 65535 */
		/* TODO: assume a little endian host for now */
		uint16_t port = (hport << 8 | hport >> 8);
		portmap_entry_t old;
		old.full = __atomic_load_n(&portmap[port].full,
				__ATOMIC_ACQUIRE);
		if (old.dom == -1 && __atomic_compare_exchange_n(&portmap[port].full, &old.full, new.full, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return port;
	}
	return 0;
}

static uint16_t assign_port(portmap_entry_t *portmap, int dom, uint32_t ipaddr, uint16_t port)
{
	portmap_entry_t new = { .dom = dom, .ipaddr = ipaddr };
	if (port == 0) {
		port = assign_random_port(portmap, new);
		if (unlikely(port == 0)) {
			printk("Cannot find an available port\n");
			return 0;
		}
	} else {
		portmap_entry_t old;
		old.full = __atomic_load_n(&portmap[port].full,
				__ATOMIC_ACQUIRE);
		if (old.dom == dom) // already bound
			return port;
		if (unlikely(old.dom != -1 || !__atomic_compare_exchange_n(&portmap[port].full, &old.full, new.full, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)))
			return 0;
	}
	return port;
}

static void unassign_port(portmap_entry_t *portmap, uint16_t port)
{
	portmap_entry_t new = { .dom = -1, .ipaddr = 0 };
	__atomic_store_n(&portmap[port].full, new.full, __ATOMIC_RELEASE);
}

int do_rumprun_port_bind(int sysid, XEN_GUEST_HANDLE_PARAM(uint16) port, uint8_t protocol)
{
	unsigned int _dom;
	uint16_t _port;
	uint32_t _ip;
	portmap_entry_t *portmap;
	uint32_t domid = current->domain->domain_id;

	if (unlikely(sysid != 0))
		return -EINVAL;

	if (copy_from_guest(&_port, port, 1))
		return -EFAULT;

	// _port is in the big endian byte order
	printk("do_rumprun_port_bind, port: %hx\n", _port);

	spin_lock(&rumprun_service_data[sysid].lock);
	_dom = get_dom(sysid, domid);
	spin_unlock(&rumprun_service_data[sysid].lock);
	if (unlikely(_dom == -1)) {
		return -ENOENT;
	}
	_ip = __atomic_load_n(&ip_alloc[_dom].ipaddr, __ATOMIC_ACQUIRE);
	if (_ip == 0)
		_ip = ip_shared_addr.ipaddr;

	switch (protocol)
	{
		case TCP:
			portmap = tcp_portmap;
			break;

		case UDP:
			portmap = udp_portmap;
			break;

		default:
			return -EINVAL;
	}

	_port = assign_port(portmap, _dom, _ip, _port);
	if (unlikely(_port == 0))
		return -EADDRINUSE;

	if (copy_to_guest(port, &_port, 1)) {
		unassign_port(portmap, _port);
		return -EFAULT;
	}

	return 0;
}
