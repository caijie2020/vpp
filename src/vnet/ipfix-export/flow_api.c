/*
 *------------------------------------------------------------------
 * flow_api.c - flow api
 *
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <vnet/vnet.h>
#include <vlibmemory/api.h>
#include <vnet/ip/ip_types_api.h>
#include <vnet/udp/udp_local.h>

#include <vnet/interface.h>
#include <vnet/api_errno.h>

#include <vnet/fib/fib_table.h>
#include <vnet/ipfix-export/flow_report.h>
#include <vnet/ipfix-export/flow_report_classify.h>

#include <vnet/format_fns.h>
#include <vnet/ipfix-export/ipfix_export.api_enum.h>
#include <vnet/ipfix-export/ipfix_export.api_types.h>

#define REPLY_MSG_ID_BASE frm->msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_set_ipfix_exporter_t_handler (vl_api_set_ipfix_exporter_t * mp)
{
  vlib_main_t *vm = vlib_get_main ();
  flow_report_main_t *frm = &flow_report_main;
  vl_api_registration_t *reg;
  vl_api_set_ipfix_exporter_reply_t *rmp;
  ip4_address_t collector, src;
  u16 collector_port = UDP_DST_PORT_ipfix;
  u32 path_mtu;
  u32 template_interval;
  u8 udp_checksum;
  u32 fib_id;
  u32 fib_index = ~0;
  int rv = 0;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  if (mp->src_address.af == ADDRESS_IP6
      || mp->collector_address.af == ADDRESS_IP6)
    {
      rv = VNET_API_ERROR_UNIMPLEMENTED;
      goto out;
    }

  ip4_address_decode (mp->collector_address.un.ip4, &collector);
  collector_port = ntohs (mp->collector_port);
  if (collector_port == (u16) ~ 0)
    collector_port = UDP_DST_PORT_ipfix;
  ip4_address_decode (mp->src_address.un.ip4, &src);
  fib_id = ntohl (mp->vrf_id);

  ip4_main_t *im = &ip4_main;
  if (fib_id == ~0)
    {
      fib_index = ~0;
    }
  else
    {
      uword *p = hash_get (im->fib_index_by_table_id, fib_id);
      if (!p)
	{
	  rv = VNET_API_ERROR_NO_SUCH_FIB;
	  goto out;
	}
      fib_index = p[0];
    }

  path_mtu = ntohl (mp->path_mtu);
  if (path_mtu == ~0)
    path_mtu = 512;		// RFC 7011 section 10.3.3.
  template_interval = ntohl (mp->template_interval);
  if (template_interval == ~0)
    template_interval = 20;
  udp_checksum = mp->udp_checksum;

  if (collector.as_u32 != 0 && src.as_u32 == 0)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto out;
    }

  if (path_mtu > 1450 /* vpp does not support fragmentation */ )
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto out;
    }

  if (path_mtu < 68)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto out;
    }

  /* Reset report streams if we are reconfiguring IP addresses */
  if (frm->ipfix_collector.as_u32 != collector.as_u32 ||
      frm->src_address.as_u32 != src.as_u32 ||
      frm->collector_port != collector_port)
    vnet_flow_reports_reset (frm);

  frm->ipfix_collector.as_u32 = collector.as_u32;
  frm->collector_port = collector_port;
  frm->src_address.as_u32 = src.as_u32;
  frm->fib_index = fib_index;
  frm->path_mtu = path_mtu;
  frm->template_interval = template_interval;
  frm->udp_checksum = udp_checksum;

  /* Turn on the flow reporting process */
  vlib_process_signal_event (vm, flow_report_process_node.index, 1, 0);

out:
  REPLY_MACRO (VL_API_SET_IPFIX_EXPORTER_REPLY);
}

static void
vl_api_ipfix_exporter_dump_t_handler (vl_api_ipfix_exporter_dump_t * mp)
{
  flow_report_main_t *frm = &flow_report_main;
  vl_api_registration_t *reg;
  vl_api_ipfix_exporter_details_t *rmp;
  ip4_main_t *im = &ip4_main;
  ip46_address_t collector = {.as_u64[0] = 0,.as_u64[1] = 0 };
  ip46_address_t src = {.as_u64[0] = 0,.as_u64[1] = 0 };
  u32 vrf_id;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));
  rmp->_vl_msg_id =
    ntohs ((REPLY_MSG_ID_BASE) + VL_API_IPFIX_EXPORTER_DETAILS);
  rmp->context = mp->context;

  memcpy (&collector.ip4, &frm->ipfix_collector, sizeof (ip4_address_t));
  ip_address_encode (&collector, IP46_TYPE_IP4, &rmp->collector_address);

  rmp->collector_port = htons (frm->collector_port);

  memcpy (&src.ip4, &frm->src_address, sizeof (ip4_address_t));
  ip_address_encode (&src, IP46_TYPE_IP4, &rmp->src_address);

  if (frm->fib_index == ~0)
    vrf_id = ~0;
  else
    vrf_id = im->fibs[frm->fib_index].ft_table_id;
  rmp->vrf_id = htonl (vrf_id);
  rmp->path_mtu = htonl (frm->path_mtu);
  rmp->template_interval = htonl (frm->template_interval);
  rmp->udp_checksum = (frm->udp_checksum != 0);

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
  vl_api_set_ipfix_classify_stream_t_handler
  (vl_api_set_ipfix_classify_stream_t * mp)
{
  vl_api_set_ipfix_classify_stream_reply_t *rmp;
  flow_report_classify_main_t *fcm = &flow_report_classify_main;
  flow_report_main_t *frm = &flow_report_main;
  u32 domain_id = 0;
  u32 src_port = UDP_DST_PORT_ipfix;
  int rv = 0;

  domain_id = ntohl (mp->domain_id);
  src_port = ntohs (mp->src_port);

  if (fcm->src_port != 0 &&
      (fcm->domain_id != domain_id || fcm->src_port != (u16) src_port))
    {
      int rv = vnet_stream_change (frm, fcm->domain_id, fcm->src_port,
				   domain_id, (u16) src_port);
      ASSERT (rv == 0);
    }

  fcm->domain_id = domain_id;
  fcm->src_port = (u16) src_port;

  REPLY_MACRO (VL_API_SET_IPFIX_CLASSIFY_STREAM_REPLY);
}

static void
  vl_api_ipfix_classify_stream_dump_t_handler
  (vl_api_ipfix_classify_stream_dump_t * mp)
{
  flow_report_classify_main_t *fcm = &flow_report_classify_main;
  vl_api_registration_t *reg;
  vl_api_ipfix_classify_stream_details_t *rmp;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));
  rmp->_vl_msg_id = ntohs (VL_API_IPFIX_CLASSIFY_STREAM_DETAILS);
  rmp->context = mp->context;
  rmp->domain_id = htonl (fcm->domain_id);
  rmp->src_port = htons (fcm->src_port);

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
  vl_api_ipfix_classify_table_add_del_t_handler
  (vl_api_ipfix_classify_table_add_del_t * mp)
{
  vl_api_ipfix_classify_table_add_del_reply_t *rmp;
  vl_api_registration_t *reg;
  flow_report_classify_main_t *fcm = &flow_report_classify_main;
  flow_report_main_t *frm = &flow_report_main;
  vnet_flow_report_add_del_args_t args;
  ipfix_classify_table_t *table;
  int is_add;
  u32 classify_table_index;
  u8 ip_version;
  u8 transport_protocol;
  int rv = 0;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  classify_table_index = ntohl (mp->table_id);
  ip_version = (mp->ip_version == ADDRESS_IP4) ? 4 : 6;
  transport_protocol = mp->transport_protocol;
  is_add = mp->is_add;

  if (fcm->src_port == 0)
    {
      /* call set_ipfix_classify_stream first */
      rv = VNET_API_ERROR_UNSPECIFIED;
      goto out;
    }

  clib_memset (&args, 0, sizeof (args));

  table = 0;
  int i;
  for (i = 0; i < vec_len (fcm->tables); i++)
    if (ipfix_classify_table_index_valid (i))
      if (fcm->tables[i].classify_table_index == classify_table_index)
	{
	  table = &fcm->tables[i];
	  break;
	}

  if (is_add)
    {
      if (table)
	{
	  rv = VNET_API_ERROR_VALUE_EXIST;
	  goto out;
	}
      table = ipfix_classify_add_table ();
      table->classify_table_index = classify_table_index;
    }
  else
    {
      if (!table)
	{
	  rv = VNET_API_ERROR_NO_SUCH_ENTRY;
	  goto out;
	}
    }

  table->ip_version = ip_version;
  table->transport_protocol = transport_protocol;

  args.opaque.as_uword = table - fcm->tables;
  args.rewrite_callback = ipfix_classify_template_rewrite;
  args.flow_data_callback = ipfix_classify_send_flows;
  args.is_add = is_add;
  args.domain_id = fcm->domain_id;
  args.src_port = fcm->src_port;

  rv = vnet_flow_report_add_del (frm, &args, NULL);

  /* If deleting, or add failed */
  if (is_add == 0 || (rv && is_add))
    ipfix_classify_delete_table (table - fcm->tables);

out:
  REPLY_MACRO (VL_API_SET_IPFIX_CLASSIFY_STREAM_REPLY);
}

static void
send_ipfix_classify_table_details (u32 table_index,
				   vl_api_registration_t * reg, u32 context)
{
  flow_report_classify_main_t *fcm = &flow_report_classify_main;
  vl_api_ipfix_classify_table_details_t *mp;

  ipfix_classify_table_t *table = &fcm->tables[table_index];

  mp = vl_msg_api_alloc (sizeof (*mp));
  clib_memset (mp, 0, sizeof (*mp));
  mp->_vl_msg_id = ntohs (VL_API_IPFIX_CLASSIFY_TABLE_DETAILS);
  mp->context = context;
  mp->table_id = htonl (table->classify_table_index);
  mp->ip_version = (table->ip_version == 4) ? ADDRESS_IP4 : ADDRESS_IP6;
  mp->transport_protocol = table->transport_protocol;

  vl_api_send_msg (reg, (u8 *) mp);
}

static void
  vl_api_ipfix_classify_table_dump_t_handler
  (vl_api_ipfix_classify_table_dump_t * mp)
{
  flow_report_classify_main_t *fcm = &flow_report_classify_main;
  vl_api_registration_t *reg;
  u32 i;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  for (i = 0; i < vec_len (fcm->tables); i++)
    if (ipfix_classify_table_index_valid (i))
      send_ipfix_classify_table_details (i, reg, mp->context);
}

static void
vl_api_ipfix_flush_t_handler (vl_api_ipfix_flush_t * mp)
{
  flow_report_main_t *frm = &flow_report_main;
  vl_api_ipfix_flush_reply_t *rmp;
  vl_api_registration_t *reg;
  vlib_main_t *vm = vlib_get_main ();
  int rv = 0;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  /* poke the flow reporting process */
  vlib_process_signal_event (vm, flow_report_process_node.index,
			     1 /* type_opaque */ , 0 /* data */ );

  REPLY_MACRO (VL_API_IPFIX_FLUSH_REPLY);
}

#include <vnet/ipfix-export/ipfix_export.api.c>
static clib_error_t *
flow_api_hookup (vlib_main_t * vm)
{
  flow_report_main_t *frm = &flow_report_main;
  /*
   * Set up the (msg_name, crc, message-id) table
   */
  REPLY_MSG_ID_BASE = setup_message_id_table ();

  return 0;
}

VLIB_API_INIT_FUNCTION (flow_api_hookup);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
