/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <netinet/ether.h>
#include <linux/if.h>

#include "networkd.h"
#include "libudev-private.h"
#include "util.h"

int link_new(Manager *manager, struct udev_device *device, Link **ret) {
        _cleanup_link_free_ Link *link = NULL;
        const char *mac;
        struct ether_addr *mac_addr;
        const char *ifname;
        int r;

        assert(device);
        assert(ret);

        link = new0(Link, 1);
        if (!link)
                return -ENOMEM;

        link->manager = manager;
        link->state = _LINK_STATE_INVALID;

        link->ifindex = udev_device_get_ifindex(device);
        if (link->ifindex <= 0)
                return -EINVAL;

        mac = udev_device_get_sysattr_value(device, "address");
        if (mac) {
                mac_addr = ether_aton(mac);
                if (mac_addr)
                        memcpy(&link->mac, mac_addr, sizeof(struct ether_addr));
        }

        ifname = udev_device_get_sysname(device);
        link->ifname = strdup(ifname);

        r = hashmap_put(manager->links, &link->ifindex, link);
        if (r < 0)
                return r;

        *ret = link;
        link = NULL;

        return 0;
}

void link_free(Link *link) {
        if (!link)
                return;

        assert(link->manager);

        if (link->dhcp)
                sd_dhcp_client_free(link->dhcp);

        route_free(link->dhcp_route);
        link->dhcp_route = NULL;

        address_free(link->dhcp_address);
        link->dhcp_address = NULL;

        hashmap_remove(link->manager->links, &link->ifindex);

        free(link->ifname);

        free(link);
}

int link_add(Manager *m, struct udev_device *device) {
        Link *link;
        Network *network;
        int r;
        uint64_t ifindex;
        const char *devtype;

        assert(m);
        assert(device);

        ifindex = udev_device_get_ifindex(device);
        link = hashmap_get(m->links, &ifindex);
        if (link)
                return 0;

        r = link_new(m, device, &link);
        if (r < 0) {
                log_error("Could not create link: %s", strerror(-r));
                return r;
        }

        devtype = udev_device_get_devtype(device);
        if (streq_ptr(devtype, "bridge")) {
                r = bridge_set_link(m, link);
                if (r < 0)
                        return r == -ENOENT ? 0 : r;
        }

        r = network_get(m, device, &network);
        if (r < 0)
                return r == -ENOENT ? 0 : r;

        r = network_apply(m, network, link);
        if (r < 0)
                return r;

        return 0;
}

static int link_enter_configured(Link *link) {
        assert(link);
        assert(link->state == LINK_STATE_SETTING_ROUTES);

        log_info("Link '%s' configured", link->ifname);

        link->state = LINK_STATE_CONFIGURED;

        return 0;
}

static void link_enter_failed(Link *link) {
        assert(link);

        link->state = LINK_STATE_FAILED;
}

static int route_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->route_messages > 0);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES ||
               link->state == LINK_STATE_SETTING_ROUTES ||
               link->state == LINK_STATE_FAILED);

        link->route_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_warning("Could not set route on interface '%s': %s",
                            link->ifname, strerror(-r));

        /* we might have received an old reply after moving back to SETTING_ADDRESSES,
         * ignore it */
        if (link->route_messages == 0 && link->state == LINK_STATE_SETTING_ROUTES) {
                log_info("Routes set for link '%s'", link->ifname);
                link_enter_configured(link);
        }

        return 1;
}

static int link_enter_set_routes(Link *link) {
        Route *route;
        int r;

        assert(link);
        assert(link->network);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES);

        link->state = LINK_STATE_SETTING_ROUTES;

        if (!link->network->static_routes && !link->dhcp_route)
                return link_enter_configured(link);

        LIST_FOREACH(static_routes, route, link->network->static_routes) {
                r = route_configure(route, link, &route_handler);
                if (r < 0) {
                        log_warning("Could not set routes for link '%s'", link->ifname);
                        link_enter_failed(link);
                        return r;
                }

                link->route_messages ++;
        }

        if (link->dhcp_route) {
                r = route_configure(link->dhcp_route, link, &route_handler);
                if (r < 0) {
                        log_warning("Could not set routes for link '%s'", link->ifname);
                        link_enter_failed(link);
                        return r;
                }

                link->route_messages ++;
        }

        return 0;
}

static int address_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);
        assert(link->addr_messages > 0);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES || link->state == LINK_STATE_FAILED);

        link->addr_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_warning("Could not set address on interface '%s': %s",
                            link->ifname, strerror(-r));

        if (link->addr_messages == 0) {
                log_info("Addresses set for link '%s'", link->ifname);
                link_enter_set_routes(link);
        }

        return 1;
}

static int link_enter_set_addresses(Link *link) {
        Address *address;
        int r;

        assert(link);
        assert(link->network);
        assert(link->state != _LINK_STATE_INVALID);

        link->state = LINK_STATE_SETTING_ADDRESSES;

        if (!link->network->static_addresses && !link->dhcp_address)
                return link_enter_set_routes(link);

        LIST_FOREACH(static_addresses, address, link->network->static_addresses) {
                r = address_configure(address, link, &address_handler);
                if (r < 0) {
                        log_warning("Could not set addresses for link '%s'", link->ifname);
                        link_enter_failed(link);
                        return r;
                }

                link->addr_messages ++;
        }

        if (link->dhcp_address) {
                r = address_configure(link->dhcp_address, link, &address_handler);
                if (r < 0) {
                        log_warning("Could not set addresses for link '%s'", link->ifname);
                        link_enter_failed(link);
                        return r;
                }

                link->addr_messages ++;
        }

        return 0;
}

static int link_up_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0) {
                log_warning("Could not bring up interface '%s': %s",
                            link->ifname, strerror(-r));
                link_enter_failed(link);
        }

        return 1;
}

static int link_up(Link *link) {
        _cleanup_sd_rtnl_message_unref_ sd_rtnl_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        r = sd_rtnl_message_link_new(RTM_SETLINK, link->ifindex, &req);
        if (r < 0) {
                log_error("Could not allocate RTM_SETLINK message");
                return r;
        }

        r = sd_rtnl_message_link_set_flags(req, IFF_UP);
        if (r < 0) {
                log_error("Could not set link flags");
                return r;
        }

        r = sd_rtnl_call_async(link->manager->rtnl, req, link_up_handler, link, 0, NULL);
        if (r < 0) {
                log_error("Could not send rtnetlink message: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int link_bridge_joined(Link *link) {
        int r;

        assert(link);
        assert(link->state == LINK_STATE_JOINING_BRIDGE);
        assert(link->network);

        r = link_up(link);
        if (r < 0) {
                link_enter_failed(link);
                return r;
        }

        if (!link->network->dhcp) {
                r = link_enter_set_addresses(link);
                if (r < 0)
                        link_enter_failed(link);
                        return r;
        }

        return 0;
}

static int bridge_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->state == LINK_STATE_JOINING_BRIDGE || link->state == LINK_STATE_FAILED);
        assert(link->network);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0) {
                log_warning("Could not join interface '%s' to bridge '%s': %s",
                            link->ifname, link->network->bridge->name, strerror(-r));
                link_enter_failed(link);
                return 1;
        } else
                log_info("Join interface '%s' to bridge: %s",
                            link->ifname, link->network->bridge->name);

        link_bridge_joined(link);

        return 1;
}

static int link_enter_join_bridge(Link *link) {
        int r;

        assert(link);
        assert(link->network);
        assert(link->state == _LINK_STATE_INVALID);

        link->state = LINK_STATE_JOINING_BRIDGE;

        if (!link->network->bridge)
                return link_bridge_joined(link);

        r = bridge_join(link->network->bridge, link, &bridge_handler);
        if (r < 0) {
                log_warning("Could not join link '%s' to bridge '%s'", link->ifname,
                            link->network->bridge->name);
                link_enter_failed(link);
                return r;
        }

        return 0;
}

static int link_get_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0) {
                log_warning("Could not get state of interface '%s': %s",
                            link->ifname, strerror(-r));
                link_enter_failed(link);
        }

        link_update(link, m);

        return 1;
}

static int link_get(Link *link) {
        _cleanup_sd_rtnl_message_unref_ sd_rtnl_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        r = sd_rtnl_message_link_new(RTM_GETLINK, link->ifindex, &req);
        if (r < 0) {
                log_error("Could not allocate RTM_GETLINK message");
                return r;
        }

        r = sd_rtnl_call_async(link->manager->rtnl, req, link_get_handler, link, 0, NULL);
        if (r < 0) {
                log_error("Could not send rtnetlink message: %s", strerror(-r));
                return r;
        }

        return 0;
}

int link_configure(Link *link) {
        int r;

        assert(link);
        assert(link->network);
        assert(link->state == _LINK_STATE_INVALID);

        r = link_get(link);
        if (r < 0) {
                link_enter_failed(link);
                return r;
        }

        r = link_enter_join_bridge(link);
        if (r < 0)
                return r;

        return 0;
}

static int address_drop_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_warning("Could not drop address from interface '%s': %s",
                            link->ifname, strerror(-r));

        return 1;
}

static void dhcp_handler(sd_dhcp_client *client, int event, void *userdata) {
        Link *link = userdata;
        struct in_addr address;
        struct in_addr netmask;
        struct in_addr gateway;
        int prefixlen;
        int r;

        if (link->state == LINK_STATE_FAILED)
                return;

        if (event < 0) {
                log_warning("DHCP error: %s", strerror(-event));
                link_enter_failed(link);
                return;
        }

        if (event == DHCP_EVENT_NO_LEASE)
                log_info("IP address in use.");

        if (event == DHCP_EVENT_IP_CHANGE || event == DHCP_EVENT_EXPIRED ||
            event == DHCP_EVENT_STOP) {
                address_drop(link->dhcp_address, link, address_drop_handler);

                address_free(link->dhcp_address);
                link->dhcp_address = NULL;

                route_free(link->dhcp_route);
                link->dhcp_route = NULL;
        }

        r = sd_dhcp_client_get_address(client, &address);
        if (r < 0) {
                log_warning("DHCP error: no address");
                link_enter_failed(link);
                return;
        }

        r = sd_dhcp_client_get_netmask(client, &netmask);
        if (r < 0) {
                log_warning("DHCP error: no netmask");
                link_enter_failed(link);
                return;
        }

        prefixlen = sd_dhcp_client_prefixlen(&netmask);
        if (prefixlen < 0) {
                log_warning("DHCP error: no prefixlen");
                link_enter_failed(link);
                return;
        }

        r = sd_dhcp_client_get_router(client, &gateway);
        if (r < 0) {
                log_warning("DHCP error: no router");
                link_enter_failed(link);
                return;
        }

        if (event == DHCP_EVENT_IP_CHANGE || event == DHCP_EVENT_IP_ACQUIRE) {
                _cleanup_address_free_ Address *addr = NULL;
                _cleanup_route_free_ Route *rt = NULL;

                log_info("Received config over DHCPv4");

                r = address_new_dynamic(&addr);
                if (r < 0) {
                        log_error("Could not allocate address");
                        link_enter_failed(link);
                        return;
                }

                addr->family = AF_INET;
                addr->in_addr.in = address;
                addr->prefixlen = prefixlen;
                addr->netmask = netmask;

                r = route_new_dynamic(&rt);
                if (r < 0) {
                        log_error("Could not allocate route");
                        link_enter_failed(link);
                        return;
                }

                rt->family = AF_INET;
                rt->in_addr.in = gateway;

                link->dhcp_address = addr;
                link->dhcp_route = rt;
                addr = NULL;
                rt = NULL;

                link_enter_set_addresses(link);
        }

        return;
}

static int link_acquire_conf(Link *link) {
        int r;

        assert(link);
        assert(link->network);
        assert(link->network->dhcp);
        assert(link->manager);
        assert(link->manager->event);

        if (!link->dhcp) {
                link->dhcp = sd_dhcp_client_new(link->manager->event);
                if (!link->dhcp)
                        return -ENOMEM;

                r = sd_dhcp_client_set_index(link->dhcp, link->ifindex);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_set_mac(link->dhcp, &link->mac);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_set_callback(link->dhcp, dhcp_handler, link);
                if (r < 0)
                        return r;
        }

        r = sd_dhcp_client_start(link->dhcp);
        if (r < 0)
                return r;

        return 0;
}

int link_update(Link *link, sd_rtnl_message *m) {
        unsigned flags;
        int r;

        assert(link);
        assert(m);

        r = sd_rtnl_message_link_get_flags(m, &flags);
        if (r < 0) {
                log_warning("Could not get link flags of '%s'", link->ifname);
                return r;
        }

        if (link->flags & IFF_UP && !(flags & IFF_UP))
                log_info("Interface '%s' is down", link->ifname);
        else if (!(link->flags & IFF_UP) && flags & IFF_UP)
                log_info("Interface '%s' is up", link->ifname);

        if (link->flags & IFF_LOWER_UP && !(flags & IFF_LOWER_UP)) {
                log_info("Interface '%s' is disconnected", link->ifname);

                if (link->network->dhcp) {
                        r = sd_dhcp_client_stop(link->dhcp);
                        if (r < 0) {
                                link_enter_failed(link);
                                return r;
                        }
                }
        } else if (!(link->flags & IFF_LOWER_UP) && flags & IFF_LOWER_UP) {
                log_info("Interface '%s' is connected", link->ifname);

                if (link->network->dhcp) {
                        r = link_acquire_conf(link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return r;
                        }
                }
        }

        link->flags = flags;

        return 0;
}
