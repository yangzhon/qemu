    /*
 * virtio-iommu device
 *
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/iov.h"
#include "qemu-common.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "sysemu/kvm.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "trace.h"

#include "standard-headers/linux/virtio_ids.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "hw/virtio/virtio-iommu.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci.h"

/* Max size */
#define VIOMMU_DEFAULT_QUEUE_SIZE 256
#define VIOMMU_PROBE_SIZE 512

typedef struct viommu_domain {
    uint32_t id;
    GTree *mappings;
    QLIST_HEAD(, viommu_endpoint) endpoint_list;
} viommu_domain;

typedef struct viommu_endpoint {
    uint32_t id;
    viommu_domain *domain;
    QLIST_ENTRY(viommu_endpoint) next;
} viommu_endpoint;

typedef struct viommu_interval {
    uint64_t low;
    uint64_t high;
} viommu_interval;

typedef struct viommu_mapping {
    uint64_t phys_addr;
    uint32_t flags;
} viommu_mapping;

static inline uint16_t virtio_iommu_get_sid(IOMMUDevice *dev)
{
    return PCI_BUILD_BDF(pci_bus_num(dev->bus), dev->devfn);
}

static gint interval_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
    viommu_interval *inta = (viommu_interval *)a;
    viommu_interval *intb = (viommu_interval *)b;

    if (inta->high < intb->low) {
        return -1;
    } else if (intb->high < inta->low) {
        return 1;
    } else {
        return 0;
    }
}

static void virtio_iommu_notify_map(IOMMUMemoryRegion *mr, hwaddr iova,
                                    hwaddr paddr, hwaddr size)
{
    IOMMUTLBEntry entry;

    trace_virtio_iommu_notify_map(mr->parent_obj.name, iova, paddr, size);

    entry.target_as = &address_space_memory;
    entry.addr_mask = size - 1;
    entry.iova = iova;
    entry.perm = IOMMU_RW;
    entry.translated_addr = paddr;

    memory_region_notify_iommu(mr, 0, entry);
}

static void virtio_iommu_notify_unmap(IOMMUMemoryRegion *mr, hwaddr iova,
                                      hwaddr size)
{
    IOMMUTLBEntry entry;

    trace_virtio_iommu_notify_unmap(mr->parent_obj.name, iova, size);

    entry.target_as = &address_space_memory;
    entry.addr_mask = size - 1;
    entry.iova = iova;
    entry.perm = IOMMU_NONE;
    entry.translated_addr = 0;

    memory_region_notify_iommu(mr, 0, entry);
}

static gboolean virtio_iommu_mapping_unmap(gpointer key, gpointer value,
                                           gpointer data)
{
    viommu_interval *interval = (viommu_interval *) key;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    virtio_iommu_notify_unmap(mr, interval->low, interval->high - interval->low + 1);

    return false;
}

static gboolean virtio_iommu_mapping_map(gpointer key, gpointer value,
                                         gpointer data)
{
    viommu_interval *interval = (viommu_interval *) key;
    viommu_mapping *mapping = (viommu_mapping *) value;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    virtio_iommu_notify_map(mr, interval->low, mapping->phys_addr,
                            interval->high - interval->low + 1);

    return false;
}

static void virtio_iommu_detach_endpoint_from_domain(viommu_endpoint *ep)
{
    VirtioIOMMUNotifierNode *node;
    VirtIOIOMMU *s = ep->viommu;
    viommu_domain *domain = ep->domain;
    uint32_t sid;

    QLIST_FOREACH(node, &s->notifiers_list, next) {
        sid = virtio_iommu_get_sid(node->iommu_dev);
        if (ep->id == sid) {
            g_tree_foreach(domain->mappings, virtio_iommu_mapping_unmap,
                           &node->iommu_dev->iommu_mr);
        }
    }

    QLIST_REMOVE(ep, next);
    g_tree_unref(ep->domain->mappings);
    ep->domain = NULL;
}

static viommu_endpoint *virtio_iommu_get_endpoint(VirtIOIOMMU *s,
                                                  uint32_t ep_id)
{
    viommu_endpoint *ep;

    ep = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(ep_id));
    if (ep) {
        return ep;
    }
    ep = g_malloc0(sizeof(*ep));
    ep->id = ep_id;
    trace_virtio_iommu_get_endpoint(ep_id);
    g_tree_insert(s->endpoints, GUINT_TO_POINTER(ep_id), ep);
    return ep;
}

static void virtio_iommu_put_endpoint(gpointer data)
{
    viommu_endpoint *ep = (viommu_endpoint *)data;

    if (ep->domain) {
        virtio_iommu_detach_endpoint_from_domain(ep);
    }

    trace_virtio_iommu_put_endpoint(ep->id);
    g_free(ep);
}

static viommu_domain *virtio_iommu_get_domain(VirtIOIOMMU *s,
                                              uint32_t domain_id)
{
    viommu_domain *domain;

    domain = g_tree_lookup(s->domains, GUINT_TO_POINTER(domain_id));
    if (domain) {
        return domain;
    }
    domain = g_malloc0(sizeof(*domain));
    domain->id = domain_id;
    domain->mappings = g_tree_new_full((GCompareDataFunc)interval_cmp,
                                   NULL, (GDestroyNotify)g_free,
                                   (GDestroyNotify)g_free);
    g_tree_insert(s->domains, GUINT_TO_POINTER(domain_id), domain);
    QLIST_INIT(&domain->endpoint_list);
    trace_virtio_iommu_get_domain(domain_id);
    return domain;
}

static void virtio_iommu_put_domain(gpointer data)
{
    viommu_domain *domain = (viommu_domain *)data;
    viommu_endpoint *iter, *tmp;

    QLIST_FOREACH_SAFE(iter, &domain->endpoint_list, next, tmp) {
        virtio_iommu_detach_endpoint_from_domain(iter);
    }
    trace_virtio_iommu_put_domain(domain->id);
    g_free(domain);
}

static AddressSpace *virtio_iommu_find_add_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    VirtIOIOMMU *s = opaque;
    IOMMUPciBus *sbus = g_hash_table_lookup(s->as_by_busptr, bus);
    static uint32_t mr_index;
    IOMMUDevice *sdev;

    if (!sbus) {
        sbus = g_malloc0(sizeof(IOMMUPciBus) +
                         sizeof(IOMMUDevice *) * IOMMU_PCI_DEVFN_MAX);
        sbus->bus = bus;
        g_hash_table_insert(s->as_by_busptr, bus, sbus);
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        char *name = g_strdup_printf("%s-%d-%d",
                                     TYPE_VIRTIO_IOMMU_MEMORY_REGION,
                                     mr_index++, devfn);
        sdev = sbus->pbdev[devfn] = g_malloc0(sizeof(IOMMUDevice));

        sdev->viommu = s;
        sdev->bus = bus;
        sdev->devfn = devfn;

        trace_virtio_iommu_init_iommu_mr(name);

        memory_region_init_iommu(&sdev->iommu_mr, sizeof(sdev->iommu_mr),
                                 TYPE_VIRTIO_IOMMU_MEMORY_REGION,
                                 OBJECT(s), name,
                                 UINT64_MAX);
        address_space_init(&sdev->as,
                           MEMORY_REGION(&sdev->iommu_mr), TYPE_VIRTIO_IOMMU);
        g_free(name);
    }
    return &sdev->as;
}

static int virtio_iommu_attach(VirtIOIOMMU *s,
                               struct virtio_iommu_req_attach *req)
{
    uint32_t domain_id = le32_to_cpu(req->domain);
    uint32_t ep_id = le32_to_cpu(req->endpoint);
    VirtioIOMMUNotifierNode *node;
    viommu_domain *domain;
    viommu_endpoint *ep;
    uint32_t sid;

    trace_virtio_iommu_attach(domain_id, ep_id);

    ep = virtio_iommu_get_endpoint(s, ep_id);
    if (ep->domain) {
        /*
         * the device is already attached to a domain,
         * detach it first
         */
        virtio_iommu_detach_endpoint_from_domain(ep);
    }

    domain = virtio_iommu_get_domain(s, domain_id);
    QLIST_INSERT_HEAD(&domain->endpoint_list, ep, next);

    ep->domain = domain;
    g_tree_ref(domain->mappings);

    /* replay existing address space mappings on the associated mr */
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        sid = virtio_iommu_get_sid(node->iommu_dev);
        if (ep->id == sid) {
            g_tree_foreach(domain->mappings, virtio_iommu_mapping_map,
                           &node->iommu_dev->iommu_mr);
        }
    }

    return VIRTIO_IOMMU_S_OK;
}

static int virtio_iommu_detach(VirtIOIOMMU *s,
                               struct virtio_iommu_req_detach *req)
{
    uint32_t domain_id = le32_to_cpu(req->domain);
    uint32_t ep_id = le32_to_cpu(req->endpoint);
    viommu_endpoint *ep;

    trace_virtio_iommu_detach(domain_id, ep_id);

    ep = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(ep_id));
    if (!ep) {
        return VIRTIO_IOMMU_S_NOENT;
    }

    if (!ep->domain) {
        return VIRTIO_IOMMU_S_INVAL;
    }

    virtio_iommu_detach_endpoint_from_domain(ep);
    return VIRTIO_IOMMU_S_OK;
}

static int virtio_iommu_map(VirtIOIOMMU *s,
                            struct virtio_iommu_req_map *req)
{
    uint32_t domain_id = le32_to_cpu(req->domain);
    uint64_t phys_start = le64_to_cpu(req->phys_start);
    uint64_t virt_start = le64_to_cpu(req->virt_start);
    uint64_t virt_end = le64_to_cpu(req->virt_end);
    uint32_t flags = le32_to_cpu(req->flags);
    VirtioIOMMUNotifierNode *node;
    viommu_endpoint *ep;
    viommu_domain *domain;
    viommu_interval *interval;
    viommu_mapping *mapping;
    uint32_t sid;

    interval = g_malloc0(sizeof(*interval));

    interval->low = virt_start;
    interval->high = virt_end;

    domain = g_tree_lookup(s->domains, GUINT_TO_POINTER(domain_id));
    if (!domain) {
        return VIRTIO_IOMMU_S_NOENT;
    }

    mapping = g_tree_lookup(domain->mappings, (gpointer)interval);
    if (mapping) {
        g_free(interval);
        return VIRTIO_IOMMU_S_INVAL;
    }

    trace_virtio_iommu_map(domain_id, virt_start, virt_end, phys_start, flags);

    mapping = g_malloc0(sizeof(*mapping));
    mapping->phys_addr = phys_start;
    mapping->flags = flags;

    g_tree_insert(domain->mappings, interval, mapping);

    /* All devices in an address-space share mapping */
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        QLIST_FOREACH(ep, &domain->endpoint_list, next) {
            sid = virtio_iommu_get_sid(node->iommu_dev);
            if (ep->id == sid) {
                virtio_iommu_notify_map(&node->iommu_dev->iommu_mr,
                                        virt_start, phys_start, virt_end - virt_start + 1);
            }
        }
    }

    return VIRTIO_IOMMU_S_OK;
}

static void virtio_iommu_remove_mapping(VirtIOIOMMU *s, viommu_domain *domain,
                                        viommu_interval *interval)
{
    VirtioIOMMUNotifierNode *node;
    viommu_endpoint *ep;
    uint32_t sid;

    g_tree_remove(domain->mappings, (gpointer)(interval));
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        QLIST_FOREACH(ep, &domain->endpoint_list, next) {
            sid = virtio_iommu_get_sid(node->iommu_dev);
            if (ep->id == sid) {
                virtio_iommu_notify_unmap(&node->iommu_dev->iommu_mr,
                                          interval->low,
                                          interval->high - interval->low + 1);
            }
        }
    }
}

static int virtio_iommu_unmap(VirtIOIOMMU *s,
                              struct virtio_iommu_req_unmap *req)
{
    uint32_t domain_id = le32_to_cpu(req->domain);
    uint64_t virt_start = le64_to_cpu(req->virt_start);
    uint64_t virt_end = le64_to_cpu(req->virt_end);
    viommu_mapping *iter_val;
    viommu_interval interval, *iter_key;
    viommu_domain *domain;
    int ret = VIRTIO_IOMMU_S_OK;

    trace_virtio_iommu_unmap(domain_id, virt_start, virt_end);

    domain = g_tree_lookup(s->domains, GUINT_TO_POINTER(domain_id));
    if (!domain) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no domain\n", __func__);
        return VIRTIO_IOMMU_S_NOENT;
    }
    interval.low = virt_start;
    interval.high = virt_end;

    while (g_tree_lookup_extended(domain->mappings, &interval,
                                  (void **)&iter_key, (void**)&iter_val)) {
        uint64_t current_low = iter_key->low;
        uint64_t current_high = iter_key->high;

        if (interval.low <= current_low && interval.high >= current_high) {
            virtio_iommu_remove_mapping(s, domain, iter_key);
            trace_virtio_iommu_unmap_done(domain_id, current_low, current_high);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "%s: domain= %d Unmap [0x%"PRIx64",0x%"PRIx64"] forbidden as "
                "it would split existing mapping [0x%"PRIx64", 0x%"PRIx64"]\n",
                __func__, domain_id, interval.low, interval.high,
                current_low, current_high);
            ret = VIRTIO_IOMMU_S_RANGE;
            break;
        }
    }
    return ret;
}

static ssize_t virtio_iommu_fill_resv_mem_prop(VirtIOIOMMU *s, uint32_t ep,
                                               uint8_t *buf, size_t free)
{
    struct virtio_iommu_probe_resv_mem prop = {};
    size_t size = sizeof(prop), length = size - sizeof(prop.head), total;
    int i;

    total = size * s->nb_reserved_regions;

    if (total > free) {
        return -ENOSPC;
    }

    for (i = 0; i < s->nb_reserved_regions; i++) {
        prop.head.type = VIRTIO_IOMMU_PROBE_T_RESV_MEM;
        prop.head.length = cpu_to_le64(length);
        prop.subtype = cpu_to_le64(s->reserved_regions[i].type);
        prop.start = cpu_to_le64(s->reserved_regions[i].low);
        prop.end = cpu_to_le64(s->reserved_regions[i].high);

        memcpy(buf, &prop, size);

        trace_virtio_iommu_fill_resv_property(ep, prop.subtype,
                                              prop.start, prop.end);
        buf += size;
    }
    return total;
}

/**
 * virtio_iommu_probe - Fill the probe request buffer with
 * the properties the device is able to return and add a NONE
 * property at the end.
 */
static int virtio_iommu_probe(VirtIOIOMMU *s,
                              struct virtio_iommu_req_probe *req,
                              uint8_t *buf)
{
    uint32_t ep_id = le32_to_cpu(req->endpoint);
    struct virtio_iommu_probe_property last = {};
    size_t free = VIOMMU_PROBE_SIZE - sizeof(last);
    ssize_t count;

    count = virtio_iommu_fill_resv_mem_prop(s, ep_id, buf, free);
    if (count < 0) {
            return VIRTIO_IOMMU_S_INVAL;
    }
    buf += count;
    free -= count;

    memcpy(buf, &last, sizeof(last));

    return VIRTIO_IOMMU_S_OK;
}

static int virtio_iommu_iov_to_req(struct iovec *iov,
                                   unsigned int iov_cnt,
                                   void *req, size_t req_sz)
{
    size_t sz, payload_sz = req_sz - sizeof(struct virtio_iommu_req_tail);

    sz = iov_to_buf(iov, iov_cnt, 0, req, payload_sz);
    if (unlikely(sz != payload_sz)) {
        return VIRTIO_IOMMU_S_INVAL;
    }
    return 0;
}

#define virtio_iommu_handle_req(__req)                                  \
static int virtio_iommu_handle_ ## __req(VirtIOIOMMU *s,                \
                                         struct iovec *iov,             \
                                         unsigned int iov_cnt)          \
{                                                                       \
    struct virtio_iommu_req_ ## __req req;                              \
    int ret = virtio_iommu_iov_to_req(iov, iov_cnt, &req, sizeof(req)); \
                                                                        \
    return ret ? ret : virtio_iommu_ ## __req(s, &req);                 \
}

virtio_iommu_handle_req(attach)
virtio_iommu_handle_req(detach)
virtio_iommu_handle_req(map)
virtio_iommu_handle_req(unmap)

static int virtio_iommu_handle_probe(VirtIOIOMMU *s,
                                     struct iovec *iov,
                                     unsigned int iov_cnt,
                                     uint8_t *buf)
{
    struct virtio_iommu_req_probe req;
    int ret = virtio_iommu_iov_to_req(iov, iov_cnt, &req, sizeof(req));

    return ret ? ret : virtio_iommu_probe(s, &req, buf);
}

static void virtio_iommu_handle_command(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOIOMMU *s = VIRTIO_IOMMU(vdev);
    struct virtio_iommu_req_head head;
    struct virtio_iommu_req_tail tail = {};
    VirtQueueElement *elem;
    unsigned int iov_cnt;
    struct iovec *iov;
    size_t sz;

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }

        if (iov_size(elem->in_sg, elem->in_num) < sizeof(tail) ||
            iov_size(elem->out_sg, elem->out_num) < sizeof(head)) {
            virtio_error(vdev, "virtio-iommu bad head/tail size");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        }

        iov_cnt = elem->out_num;
        iov = elem->out_sg;
        sz = iov_to_buf(iov, iov_cnt, 0, &head, sizeof(head));
        if (unlikely(sz != sizeof(head))) {
            tail.status = VIRTIO_IOMMU_S_DEVERR;
            goto out;
        }
        qemu_mutex_lock(&s->mutex);
        switch (head.type) {
        case VIRTIO_IOMMU_T_ATTACH:
            tail.status = virtio_iommu_handle_attach(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_DETACH:
            tail.status = virtio_iommu_handle_detach(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_MAP:
            tail.status = virtio_iommu_handle_map(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_UNMAP:
            tail.status = virtio_iommu_handle_unmap(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_PROBE:
        {
            struct virtio_iommu_req_tail *ptail;
            uint8_t *buf = g_malloc0(s->config.probe_size + sizeof(tail));

            ptail = (struct virtio_iommu_req_tail *)
                        (buf + s->config.probe_size);
            ptail->status = virtio_iommu_handle_probe(s, iov, iov_cnt, buf);

            sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                              buf, s->config.probe_size + sizeof(tail));
            g_free(buf);
            assert(sz == s->config.probe_size + sizeof(tail));
            goto push;
        }
        default:
            tail.status = VIRTIO_IOMMU_S_UNSUPP;
        }

out:
        sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                          &tail, sizeof(tail));
        assert(sz == sizeof(tail));

push:
        qemu_mutex_unlock(&s->mutex);
        virtqueue_push(vq, elem, sz);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_iommu_report_fault(VirtIOIOMMU *viommu, uint8_t reason,
                                      uint32_t flags, uint32_t endpoint,
                                      uint64_t address)
{
    VirtIODevice *vdev = &viommu->parent_obj;
    VirtQueue *vq = viommu->event_vq;
    struct virtio_iommu_fault fault;
    VirtQueueElement *elem;
    size_t sz;

    memset(&fault, 0, sizeof(fault));
    fault.reason = reason;
    fault.flags = flags;
    fault.endpoint = endpoint;
    fault.address = address;

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));

        if (!elem) {
            error_report_once(
                "no buffer available in event queue to report event");
            return;
        }

        if (iov_size(elem->in_sg, elem->in_num) < sizeof(fault)) {
            virtio_error(vdev, "error buffer of wrong size");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            continue;
        }
        break;
    }
    /* we have a buffer to fill in */
    sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                      &fault, sizeof(fault));
    assert(sz == sizeof(fault));

    trace_virtio_iommu_report_fault(reason, flags, endpoint, address);
    virtqueue_push(vq, elem, sz);
    virtio_notify(vdev, vq);
    g_free(elem);

}

static IOMMUTLBEntry virtio_iommu_translate(IOMMUMemoryRegion *mr, hwaddr addr,
                                            IOMMUAccessFlags flag,
                                            int iommu_idx)
{
    IOMMUDevice *sdev = container_of(mr, IOMMUDevice, iommu_mr);
    viommu_interval interval, *mapping_key;
    viommu_mapping *mapping_value;
    VirtIOIOMMU *s = sdev->viommu;
    bool read_fault, write_fault;
    viommu_endpoint *ep;
    uint32_t sid, flags;
    bool bypass_allowed;
    bool found;
    int i;

    interval.low = addr;
    interval.high = addr + 1;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = (1 << ctz32(s->config.page_size_mask)) - 1,
        .perm = IOMMU_NONE,
    };

    bypass_allowed = virtio_has_feature(s->acked_features,
                                        VIRTIO_IOMMU_F_BYPASS);

    sid = virtio_iommu_get_sid(sdev);

    trace_virtio_iommu_translate(mr->parent_obj.name, sid, addr, flag);
    qemu_mutex_lock(&s->mutex);

    ep = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(sid));
    if (!ep) {
        if (!bypass_allowed) {
            error_report_once("%s sid=%d is not known!!", __func__, sid);
            virtio_iommu_report_fault(s, VIRTIO_IOMMU_FAULT_R_UNKNOWN,
                                      0, sid, 0);
        } else {
            entry.perm = flag;
        }
        goto unlock;
    }

    for (i = 0; i < s->nb_reserved_regions; i++) {
        if (interval.low >= s->reserved_regions[i].low &&
            interval.low <= s->reserved_regions[i].high) {
            switch (s->reserved_regions[i].type) {
            case VIRTIO_IOMMU_RESV_MEM_T_MSI:
                entry.perm = flag;
                goto unlock;
            case VIRTIO_IOMMU_RESV_MEM_T_RESERVED:
            default:
                virtio_iommu_report_fault(s, VIRTIO_IOMMU_FAULT_R_MAPPING,
                                          0, sid, addr);
            goto unlock;
           }
        }
    }

    if (!ep->domain) {
        if (!bypass_allowed) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s %02x:%02x.%01x not attached to any domain\n",
                          __func__, PCI_BUS_NUM(sid),
                          PCI_SLOT(sid), PCI_FUNC(sid));
            virtio_iommu_report_fault(s, VIRTIO_IOMMU_FAULT_R_DOMAIN,
                                      0, sid, 0);
        } else {
            entry.perm = flag;
        }
        goto unlock;
    }

    found = g_tree_lookup_extended(ep->domain->mappings, (gpointer)(&interval),
                                   (void **)&mapping_key,
                                   (void **)&mapping_value);
    if (!found) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s no mapping for 0x%"PRIx64" for sid=%d\n",
                      __func__, addr, sid);
        virtio_iommu_report_fault(s, VIRTIO_IOMMU_FAULT_R_MAPPING,
                                  0, sid, addr);
        goto unlock;
    }

    read_fault = (flag & IOMMU_RO) &&
                    !(mapping_value->flags & VIRTIO_IOMMU_MAP_F_READ);
    write_fault = (flag & IOMMU_WO) &&
                    !(mapping_value->flags & VIRTIO_IOMMU_MAP_F_WRITE);

    flags = read_fault ? VIRTIO_IOMMU_FAULT_F_READ : 0;
    flags |= write_fault ? VIRTIO_IOMMU_FAULT_F_WRITE : 0;
    if (flags) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Permission error on 0x%"PRIx64"(%d): allowed=%d\n",
                      addr, flag, mapping_value->flags);
        flags |= VIRTIO_IOMMU_FAULT_F_ADDRESS;
        virtio_iommu_report_fault(s, VIRTIO_IOMMU_FAULT_R_MAPPING,
                                  flags, sid, addr);
        goto unlock;
    }
    entry.translated_addr = addr - mapping_key->low + mapping_value->phys_addr;
    entry.perm = flag;
    trace_virtio_iommu_translate_out(addr, entry.translated_addr, sid);

unlock:
    qemu_mutex_unlock(&s->mutex);
    return entry;
}

static void virtio_iommu_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOIOMMU *dev = VIRTIO_IOMMU(vdev);
    struct virtio_iommu_config *config = &dev->config;

    trace_virtio_iommu_get_config(config->page_size_mask,
                                  config->input_range.start,
                                  config->input_range.end,
                                  config->domain_range.end,
                                  config->probe_size);
    memcpy(config_data, &dev->config, sizeof(struct virtio_iommu_config));
}

static void virtio_iommu_set_config(VirtIODevice *vdev,
                                      const uint8_t *config_data)
{
    struct virtio_iommu_config config;

    memcpy(&config, config_data, sizeof(struct virtio_iommu_config));
    trace_virtio_iommu_set_config(config.page_size_mask,
                                  config.input_range.start,
                                  config.input_range.end,
                                  config.domain_range.end,
                                  config.probe_size);
}

static uint64_t virtio_iommu_get_features(VirtIODevice *vdev, uint64_t f,
                                          Error **errp)
{
    VirtIOIOMMU *dev = VIRTIO_IOMMU(vdev);

    f |= dev->features;
    trace_virtio_iommu_get_features(f);
    return f;
}

static void virtio_iommu_set_features(VirtIODevice *vdev, uint64_t val)
{
    VirtIOIOMMU *dev = VIRTIO_IOMMU(vdev);

    dev->acked_features = val;
    trace_virtio_iommu_set_features(dev->acked_features);
}

static gint int_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
    uint ua = GPOINTER_TO_UINT(a);
    uint ub = GPOINTER_TO_UINT(b);
    return (ua > ub) - (ua < ub);
}

static gboolean virtio_iommu_remap(gpointer key, gpointer value, gpointer data)
{
    viommu_interval *interval = (viommu_interval *) key;
    viommu_mapping *mapping = (viommu_mapping *) value;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    trace_virtio_iommu_remap(interval->low, mapping->phys_addr,
                             interval->high - interval->low + 1);
    /* unmap previous entry and map again */
    virtio_iommu_notify_unmap(mr, interval->low, interval->high - interval->low + 1);

    virtio_iommu_notify_map(mr, interval->low, mapping->phys_addr,
                            interval->high - interval->low + 1);
    return false;
}

static void virtio_iommu_replay(IOMMUMemoryRegion *mr, IOMMUNotifier *n)
{
    IOMMUDevice *sdev = container_of(mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;
    uint32_t sid;
    viommu_endpoint *ep;

    sid = virtio_iommu_get_sid(sdev);

    qemu_mutex_lock(&s->mutex);

    ep = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(sid));
    if (!ep || !ep->domain) {
        goto unlock;
    }

    g_tree_foreach(ep->domain->mappings, virtio_iommu_remap, mr);

unlock:
    qemu_mutex_unlock(&s->mutex);
}

static void virtio_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu_mr,
                                             IOMMUNotifierFlag old,
                                             IOMMUNotifierFlag new)
{
    IOMMUDevice *sdev = container_of(iommu_mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;
    VirtioIOMMUNotifierNode *node = NULL;
    VirtioIOMMUNotifierNode *next_node = NULL;

    if (old == IOMMU_NOTIFIER_NONE) {
        trace_virtio_iommu_notify_flag_add(iommu_mr->parent_obj.name);
        node = g_malloc0(sizeof(*node));
        node->iommu_dev = sdev;
        QLIST_INSERT_HEAD(&s->notifiers_list, node, next);
        return;
    }

    /* update notifier node with new flags */
    QLIST_FOREACH_SAFE(node, &s->notifiers_list, next, next_node) {
        if (node->iommu_dev == sdev) {
            if (new == IOMMU_NOTIFIER_NONE) {
                trace_virtio_iommu_notify_flag_del(iommu_mr->parent_obj.name);
                QLIST_REMOVE(node, next);
                g_free(node);
            }
            return;
        }
    }
}

static void virtio_iommu_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOIOMMU *s = VIRTIO_IOMMU(dev);

    QLIST_INIT(&s->notifiers_list);
    virtio_init(vdev, "virtio-iommu", VIRTIO_ID_IOMMU,
                sizeof(struct virtio_iommu_config));

    s->req_vq = virtio_add_queue(vdev, VIOMMU_DEFAULT_QUEUE_SIZE,
                             virtio_iommu_handle_command);
    s->event_vq = virtio_add_queue(vdev, VIOMMU_DEFAULT_QUEUE_SIZE, NULL);

    s->config.page_size_mask = TARGET_PAGE_MASK;
    s->config.input_range.end = -1UL;
    s->config.domain_range.end = 32;
    s->config.probe_size = VIOMMU_PROBE_SIZE;

    virtio_add_feature(&s->features, VIRTIO_RING_F_EVENT_IDX);
    virtio_add_feature(&s->features, VIRTIO_RING_F_INDIRECT_DESC);
    virtio_add_feature(&s->features, VIRTIO_F_VERSION_1);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_INPUT_RANGE);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_DOMAIN_RANGE);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_MAP_UNMAP);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_BYPASS);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_MMIO);
    virtio_add_feature(&s->features, VIRTIO_IOMMU_F_PROBE);

    qemu_mutex_init(&s->mutex);

    memset(s->as_by_bus_num, 0, sizeof(s->as_by_bus_num));
    s->as_by_busptr = g_hash_table_new_full(NULL, NULL, NULL, g_free);

    if (s->primary_bus) {
        pci_setup_iommu(s->primary_bus, virtio_iommu_find_add_as, s);
    } else {
        error_setg(errp, "VIRTIO-IOMMU is not attached to any PCI bus!");
    }

    s->domains = g_tree_new_full((GCompareDataFunc)int_cmp,
                                 NULL, NULL, virtio_iommu_put_domain);
    s->endpoints = g_tree_new_full((GCompareDataFunc)int_cmp,
                                   NULL, NULL, virtio_iommu_put_endpoint);
}

static void virtio_iommu_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOIOMMU *s = VIRTIO_IOMMU(dev);

    g_tree_destroy(s->domains);
    g_tree_destroy(s->endpoints);

    virtio_cleanup(vdev);
}

static void virtio_iommu_device_reset(VirtIODevice *vdev)
{
    trace_virtio_iommu_device_reset();
}

static void virtio_iommu_set_status(VirtIODevice *vdev, uint8_t status)
{
    trace_virtio_iommu_device_status(status);
}

static void virtio_iommu_instance_init(Object *obj)
{
}

#define VMSTATE_INTERVAL                               \
{                                                      \
    .name = "interval",                                \
    .version_id = 1,                                   \
    .minimum_version_id = 1,                           \
    .fields = (VMStateField[]) {                       \
        VMSTATE_UINT64(low, viommu_interval),          \
        VMSTATE_UINT64(high, viommu_interval),         \
        VMSTATE_END_OF_LIST()                          \
    }                                                  \
}

#define VMSTATE_MAPPING                               \
{                                                     \
    .name = "mapping",                                \
    .version_id = 1,                                  \
    .minimum_version_id = 1,                          \
    .fields = (VMStateField[]) {                      \
        VMSTATE_UINT64(phys_addr, viommu_mapping),    \
        VMSTATE_UINT32(flags, viommu_mapping),        \
        VMSTATE_END_OF_LIST()                         \
    },                                                \
}

static const VMStateDescription vmstate_interval_mapping[2] = {
    VMSTATE_MAPPING,   /* value */
    VMSTATE_INTERVAL   /* key   */
};

static int domain_preload(void *opaque)
{
    viommu_domain *domain = opaque;

    domain->mappings = g_tree_new_full((GCompareDataFunc)interval_cmp,
                                       NULL, g_free, g_free);
    return 0;
}

static const VMStateDescription vmstate_endpoint = {
    .name = "endpoint",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(id, viommu_endpoint),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_domain = {
    .name = "domain",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = domain_preload,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(id, viommu_domain),
        VMSTATE_GTREE_V(mappings, viommu_domain, 1,
                        vmstate_interval_mapping,
                        viommu_interval, viommu_mapping),
        VMSTATE_QLIST_V(endpoint_list, viommu_domain, 1,
                        vmstate_endpoint, viommu_endpoint, next),
        VMSTATE_END_OF_LIST()
    }
};

static gboolean reconstruct_ep_domain_link(gpointer key, gpointer value,
                                           gpointer data)
{
    viommu_domain *d = (viommu_domain *)value;
    viommu_endpoint *iter, *tmp;
    viommu_endpoint *ep = (viommu_endpoint *)data;

    QLIST_FOREACH_SAFE(iter, &d->endpoint_list, next, tmp) {
        if (iter->id == ep->id) {
            /* remove the ep */
            QLIST_REMOVE(iter, next);
            g_free(iter);
            /* replace it with the good one */
            QLIST_INSERT_HEAD(&d->endpoint_list, ep, next);
            /* update the domain */
            ep->domain = d;
            return true; /* stop the search */
        }
    }
    return false; /* continue the traversal */
}

static gboolean fix_endpoint(gpointer key, gpointer value, gpointer data)
{
    VirtIOIOMMU *s = (VirtIOIOMMU *)data;

    g_tree_foreach(s->domains, reconstruct_ep_domain_link, value);
    return false;
}

static int iommu_post_load(void *opaque, int version_id)
{
    VirtIOIOMMU *s = opaque;

    g_tree_foreach(s->endpoints, fix_endpoint, s);
    return 0;
}

static const VMStateDescription vmstate_virtio_iommu_device = {
    .name = "virtio-iommu-device",
    .minimum_version_id = 1,
    .version_id = 1,
    .post_load = iommu_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_GTREE_DIRECT_KEY_V(domains, VirtIOIOMMU, 1,
                                   &vmstate_domain, viommu_domain),
        VMSTATE_GTREE_DIRECT_KEY_V(endpoints, VirtIOIOMMU, 1,
                                   &vmstate_endpoint, viommu_endpoint),
        VMSTATE_END_OF_LIST()
    },
};


static const VMStateDescription vmstate_virtio_iommu = {
    .name = "virtio-iommu",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_iommu_properties[] = {
    DEFINE_PROP_LINK("primary-bus", VirtIOIOMMU, primary_bus, "PCI", PCIBus *),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_iommu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_iommu_properties;
    dc->vmsd = &vmstate_virtio_iommu;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_iommu_device_realize;
    vdc->unrealize = virtio_iommu_device_unrealize;
    vdc->reset = virtio_iommu_device_reset;
    vdc->get_config = virtio_iommu_get_config;
    vdc->set_config = virtio_iommu_set_config;
    vdc->get_features = virtio_iommu_get_features;
    vdc->set_features = virtio_iommu_set_features;
    vdc->set_status = virtio_iommu_set_status;
    vdc->vmsd = &vmstate_virtio_iommu_device;
}

static void virtio_iommu_memory_region_class_init(ObjectClass *klass,
                                                  void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = virtio_iommu_translate;
    imrc->replay = virtio_iommu_replay;
    imrc->notify_flag_changed = virtio_iommu_notify_flag_changed;
}

static const TypeInfo virtio_iommu_info = {
    .name = TYPE_VIRTIO_IOMMU,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOIOMMU),
    .instance_init = virtio_iommu_instance_init,
    .class_init = virtio_iommu_class_init,
};

static const TypeInfo virtio_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_VIRTIO_IOMMU_MEMORY_REGION,
    .class_init = virtio_iommu_memory_region_class_init,
};


static void virtio_register_types(void)
{
    type_register_static(&virtio_iommu_info);
    type_register_static(&virtio_iommu_memory_region_info);
}

type_init(virtio_register_types)
