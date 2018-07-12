/******************************************************************************
************************* IMPORTANT NOTE -- READ ME!!! ************************
*******************************************************************************
* THIS SOFTWARE IMPLEMENTS A **DRAFT** STANDARD, BSR E1.33 REV. 63. UNDER NO
* CIRCUMSTANCES SHOULD THIS SOFTWARE BE USED FOR ANY PRODUCT AVAILABLE FOR
* GENERAL SALE TO THE PUBLIC. DUE TO THE INEVITABLE CHANGE OF DRAFT PROTOCOL
* VALUES AND BEHAVIORAL REQUIREMENTS, PRODUCTS USING THIS SOFTWARE WILL **NOT**
* BE INTEROPERABLE WITH PRODUCTS IMPLEMENTING THE FINAL RATIFIED STANDARD.
*******************************************************************************
* Copyright 2018 ETC Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************
* This file is a part of RDMnet. For more information, go to:
* https://github.com/ETCLabs/RDMnet
******************************************************************************/

#include "rdmnet/brokerprot.h"
#include "rdmnet/connection.h"
#include "brokerprotpriv.h"
#include "lwpa_pack.h"

/* Suppress strncpy() warning on Windows/MSVC. */
#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif

/***************************** Private macros ********************************/

#define pack_broker_header(length, vector, buf) \
  do                                            \
  {                                             \
    (buf)[0] = 0xf0;                            \
    pdu_pack_ext_len(buf, length);              \
    pack_16b(&(buf)[3], vector);                \
  } while (0)

#define pack_client_entry_header(length, vector, cidptr, buf) \
  do                                                          \
  {                                                           \
    (buf)[0] = 0xf0;                                          \
    pdu_pack_ext_len(buf, length);                            \
    pack_32b(&(buf)[3], vector);                              \
    memcpy(&(buf)[7], (cidptr)->data, CID_BYTES);             \
  } while (0)

/*********************** Private function prototypes *************************/

static size_t calc_client_connect_len(const ClientConnectMsg *data);
static lwpa_error_t do_send(int handle, RdmnetConnection *conn, const uint8_t *data, size_t datasize);
static size_t pack_broker_header_with_rlp(const RootLayerPdu *rlp, uint8_t *buf, size_t buflen, uint32_t vector);
static lwpa_error_t send_broker_header(int handle, RdmnetConnection *conn, const RootLayerPdu *rlp, uint8_t *buf,
                                       size_t buflen, uint32_t vector);
static lwpa_error_t send_connect_reply_internal(RdmnetConnection *conn, const ClientConnectMsg *data);

/*************************** Function definitions ****************************/

lwpa_error_t do_send(int handle, RdmnetConnection *conn, const uint8_t *data, size_t datasize)
{
  if (conn)
    return lwpa_send(conn->sock, data, datasize, 0);
  else
    return rdmnet_send_partial_message(handle, data, datasize);
}

/***************************** Broker PDU Header *****************************/

size_t pack_broker_header_with_rlp(const RootLayerPdu *rlp, uint8_t *buf, size_t buflen, uint32_t vector)
{
  uint8_t *cur_ptr = buf;
  size_t data_size = root_layer_buf_size(rlp, 1);

  if (data_size == 0)
    return 0;

  data_size = pack_tcp_preamble(cur_ptr, buflen, data_size);
  if (data_size == 0)
    return 0;
  cur_ptr += data_size;
  buflen -= data_size;

  data_size = pack_root_layer_header(cur_ptr, buflen, rlp);
  if (data_size == 0)
    return 0;
  cur_ptr += data_size;
  buflen -= data_size;

  pack_broker_header(rlp->datalen, vector, cur_ptr);
  cur_ptr += BROKER_PDU_HEADER_SIZE;
  return cur_ptr - buf;
}

lwpa_error_t send_broker_header(int handle, RdmnetConnection *conn, const RootLayerPdu *rlp, uint8_t *buf,
                                size_t buflen, uint32_t vector)
{
  int res;
  size_t data_size = root_layer_buf_size(rlp, 1);

  if (data_size == 0)
  {
    if (!conn)
      rdmnet_end_message(handle);
    return LWPA_PROTERR;
  }

  /* Pack and send the TCP preamble. */
  data_size = pack_tcp_preamble(buf, buflen, data_size);
  if (data_size == 0)
    return LWPA_PROTERR;
  res = do_send(handle, conn, buf, data_size);
  if (res < 0)
    return res;

  /* Pack and send the Root Layer PDU header */
  data_size = pack_root_layer_header(buf, buflen, rlp);
  if (data_size == 0)
    return LWPA_PROTERR;
  res = do_send(handle, conn, buf, data_size);
  if (res < 0)
    return res;

  /* Pack and send the Broker PDU header */
  pack_broker_header(rlp->datalen, vector, buf);
  res = do_send(handle, conn, buf, BROKER_PDU_HEADER_SIZE);
  if (res < 0)
    return res;

  return LWPA_OK;
}

/******************************* Client Connect ******************************/

size_t calc_client_connect_len(const ClientConnectMsg *data)
{
  size_t res = BROKER_PDU_HEADER_SIZE + CLIENT_CONNECT_DATA_MIN_SIZE;

  if (is_rpt_client_entry(&data->client_entry))
  {
    res += RPT_CLIENT_ENTRY_DATA_SIZE;
    return res;
  }
  else if (is_ept_client_entry(&data->client_entry))
  {
    EptSubProtocol *prot = get_ept_client_entry_data(&data->client_entry)->protocol_list;
    for (; prot; prot = prot->next)
      res += EPT_PROTOCOL_ENTRY_SIZE;
    return res;
  }
  else
    /* Should never happen */
    return 0;
}

lwpa_error_t send_client_connect(RdmnetConnection *conn, const ClientConnectMsg *data)
{
  int res;
  RootLayerPdu rlp;
  uint8_t buf[CLIENT_CONNECT_COMMON_FIELD_SIZE];
  uint8_t *cur_ptr;

  if (!(is_rpt_client_entry(&data->client_entry) || is_ept_client_entry(&data->client_entry)))
  {
    return LWPA_PROTERR;
  }

  rlp.sender_cid = conn->local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = calc_client_connect_len(data);

  res = send_broker_header(-1, conn, &rlp, buf, CLIENT_CONNECT_COMMON_FIELD_SIZE, VECTOR_BROKER_CONNECT);
  if (res != LWPA_OK)
    return res;

  /* Pack and send the common fields for the Client Connect message. */
  cur_ptr = buf;
  strncpy((char *)cur_ptr, data->scope, E133_SCOPE_STRING_PADDED_LENGTH);
  cur_ptr[E133_SCOPE_STRING_PADDED_LENGTH - 1] = '\0';
  cur_ptr += E133_SCOPE_STRING_PADDED_LENGTH;
  pack_16b(cur_ptr, data->e133_version);
  cur_ptr += 2;
  strncpy((char *)cur_ptr, data->search_domain, E133_DOMAIN_STRING_PADDED_LENGTH);
  cur_ptr[E133_DOMAIN_STRING_PADDED_LENGTH - 1] = '\0';
  cur_ptr += E133_DOMAIN_STRING_PADDED_LENGTH;
  *cur_ptr++ = data->connect_flags;
  res = lwpa_send(conn->sock, buf, cur_ptr - buf, 0);
  if (res < 0)
    return res;

  /* Pack and send the beginning of the Client Entry PDU */
  pack_client_entry_header(rlp.datalen - (BROKER_PDU_HEADER_SIZE + CLIENT_CONNECT_COMMON_FIELD_SIZE),
                           data->client_entry.client_protocol, &data->client_entry.client_cid, buf);
  res = lwpa_send(conn->sock, buf, CLIENT_ENTRY_HEADER_SIZE, 0);

  if (is_rpt_client_entry(&data->client_entry))
  {
    /* Pack and send the RPT client entry */
    const ClientEntryDataRpt *rpt_data = get_rpt_client_entry_data(&data->client_entry);
    cur_ptr = buf;
    pack_16b(cur_ptr, rpt_data->client_uid.manu);
    cur_ptr += 2;
    pack_32b(cur_ptr, rpt_data->client_uid.id);
    cur_ptr += 4;
    *cur_ptr++ = rpt_data->client_type;
    memcpy(cur_ptr, rpt_data->binding_cid.data, CID_BYTES);
    cur_ptr += CID_BYTES;
    res = lwpa_send(conn->sock, buf, RPT_CLIENT_ENTRY_DATA_SIZE, 0);
    if (res < 0)
      return res;
  }
  else /* is EPT client entry */
  {
    /* Pack and send the EPT client entry */
    const ClientEntryDataEpt *ept_data = get_ept_client_entry_data(&data->client_entry);
    const EptSubProtocol *prot = ept_data->protocol_list;
    for (; prot; prot = prot->next)
    {
      cur_ptr = buf;
      pack_32b(cur_ptr, prot->protocol_vector);
      cur_ptr += 4;
      strncpy((char *)cur_ptr, prot->protocol_string, EPT_PROTOCOL_STRING_PADDED_LENGTH);
      cur_ptr[EPT_PROTOCOL_STRING_PADDED_LENGTH - 1] = '\0';
      cur_ptr += EPT_PROTOCOL_STRING_PADDED_LENGTH;
      res = lwpa_send(conn->sock, buf, EPT_PROTOCOL_ENTRY_SIZE, 0);
      if (res < 0)
        return res;
    }
  }
  lwpa_timer_reset(&conn->send_timer);
  return LWPA_OK;
}

/******************************* Connect Reply *******************************/

size_t pack_connect_reply(uint8_t *buf, size_t buflen, const LwpaCid *local_cid, const ConnectReplyMsg *data)
{
  RootLayerPdu rlp;
  uint8_t *cur_ptr = buf;
  size_t data_size;

  if (!buf || buflen < CONNECT_REPLY_FULL_MSG_SIZE || !local_cid || !data)
    return 0;

  rlp.sender_cid = *local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_PDU_HEADER_SIZE + CONNECT_REPLY_DATA_SIZE;

  /* Try to pack all the header data */
  data_size = pack_broker_header_with_rlp(&rlp, buf, buflen, VECTOR_BROKER_CONNECT_REPLY);
  if (data_size == 0)
    return 0;
  cur_ptr += data_size;

  /* Pack the Connect Reply data fields */
  pack_16b(cur_ptr, data->connect_status);
  cur_ptr += 2;
  pack_16b(cur_ptr, data->e133_version);
  cur_ptr += 2;
  pack_16b(cur_ptr, data->broker_uid.manu);
  cur_ptr += 2;
  pack_32b(cur_ptr, data->broker_uid.id);
  cur_ptr += 4;

  return cur_ptr - buf;
}

lwpa_error_t send_connect_reply(int handle, const LwpaCid *local_cid, const ConnectReplyMsg *data)
{
  int res;
  RootLayerPdu rlp;
  uint8_t buf[RLP_HEADER_SIZE_EXT_LEN];
  uint8_t *cur_ptr;

  if (!local_cid || !data)
    return LWPA_INVALID;

  rlp.sender_cid = *local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_PDU_HEADER_SIZE + CONNECT_REPLY_DATA_SIZE;

  res = rdmnet_start_message(handle);
  if (res != LWPA_OK)
    return res;

  res = send_broker_header(handle, NULL, &rlp, buf, RLP_HEADER_SIZE_EXT_LEN, VECTOR_BROKER_CONNECT_REPLY);
  if (res != LWPA_OK)
  {
    rdmnet_end_message(handle);
    return res;
  }

  /* Pack and send the Connect Reply data fields */
  cur_ptr = buf;
  pack_16b(cur_ptr, data->connect_status);
  cur_ptr += 2;
  pack_16b(cur_ptr, data->e133_version);
  cur_ptr += 2;
  pack_16b(cur_ptr, data->broker_uid.manu);
  cur_ptr += 2;
  pack_32b(cur_ptr, data->broker_uid.id);
  cur_ptr += 4;

  res = do_send(handle, NULL, buf, cur_ptr - buf);
  if (res < 0)
  {
    rdmnet_end_message(handle);
    return res;
  }

  return rdmnet_end_message(handle);
}

/***************************** Fetch Client List *****************************/

lwpa_error_t send_fetch_client_list(int handle, const LwpaCid *local_cid)
{
  int res;
  RootLayerPdu rlp;
  uint8_t buf[RLP_HEADER_SIZE_EXT_LEN];

  if (!local_cid)
    return LWPA_INVALID;

  rlp.sender_cid = *local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_PDU_HEADER_SIZE;

  res = rdmnet_start_message(handle);
  if (res != LWPA_OK)
    return res;

  res = send_broker_header(handle, NULL, &rlp, buf, RLP_HEADER_SIZE_EXT_LEN, VECTOR_BROKER_FETCH_CLIENT_LIST);
  if (res != LWPA_OK)
    return res;

  return rdmnet_end_message(handle);
}

/**************************** Client List Messages ***************************/

size_t calc_client_entry_buf_size(ClientEntryData *client_entry_list)
{
  size_t res = 0;
  ClientEntryData *cur_entry = client_entry_list;

  for (; cur_entry; cur_entry = cur_entry->next)
  {
    if (cur_entry->client_protocol == E133_CLIENT_PROTOCOL_RPT)
    {
      res += RPT_CLIENT_ENTRY_SIZE;
    }
    else
    {
      /* TODO */
      return 0;
    }
  }
  return res;
}

size_t bufsize_client_list(ClientEntryData *client_entry_list)
{
  return (client_entry_list ? (BROKER_PDU_FULL_HEADER_SIZE + calc_client_entry_buf_size(client_entry_list)) : 0);
}

size_t pack_client_list(uint8_t *buf, size_t buflen, const LwpaCid *local_cid, uint16_t vector,
                        ClientEntryData *client_entry_list)
{
  RootLayerPdu rlp;
  uint8_t *cur_ptr = buf;
  uint8_t *buf_end = buf + buflen;
  size_t data_size;
  ClientEntryData *cur_entry;

  if (!buf || buflen < BROKER_PDU_FULL_HEADER_SIZE || !local_cid || !client_entry_list ||
      (vector != VECTOR_BROKER_CONNECTED_CLIENT_LIST && vector != VECTOR_BROKER_CLIENT_ADD &&
       vector != VECTOR_BROKER_CLIENT_REMOVE && vector != VECTOR_BROKER_CLIENT_ENTRY_CHANGE))
  {
    return 0;
  }

  rlp.sender_cid = *local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_PDU_HEADER_SIZE + calc_client_entry_buf_size(client_entry_list);

  /* Try to pack all the header data */
  data_size = pack_broker_header_with_rlp(&rlp, buf, buflen, vector);
  if (data_size == 0)
    return 0;
  cur_ptr += data_size;

  for (cur_entry = client_entry_list; cur_entry; cur_entry = cur_entry->next)
  {
    /* Check bounds */
    if (cur_ptr + CLIENT_ENTRY_HEADER_SIZE > buf_end)
      return 0;

    /* Pack the common client entry fields. */
    *cur_ptr = 0xf0;
    pdu_pack_ext_len(cur_ptr, RPT_CLIENT_ENTRY_SIZE);
    cur_ptr += 3;
    pack_32b(cur_ptr, cur_entry->client_protocol);
    cur_ptr += 4;
    memcpy(cur_ptr, cur_entry->client_cid.data, CID_BYTES);
    cur_ptr += CID_BYTES;

    if (cur_entry->client_protocol == E133_CLIENT_PROTOCOL_RPT)
    {
      ClientEntryDataRpt *rpt_data = get_rpt_client_entry_data(cur_entry);

      /* Check bounds. */
      if (cur_ptr + RPT_CLIENT_ENTRY_DATA_SIZE > buf_end)
        return 0;

      /* Pack the RPT Client Entry data */
      pack_16b(cur_ptr, rpt_data->client_uid.manu);
      cur_ptr += 2;
      pack_32b(cur_ptr, rpt_data->client_uid.id);
      cur_ptr += 4;
      *cur_ptr++ = rpt_data->client_type;
      memcpy(cur_ptr, rpt_data->binding_cid.data, CID_BYTES);
      cur_ptr += CID_BYTES;
    }
    else
    {
      /* TODO EPT */
      return 0;
    }
  }
  return cur_ptr - buf;
}

lwpa_error_t send_disconnect(RdmnetConnection *conn, const DisconnectMsg *data)
{
  int res;
  RootLayerPdu rlp;
  uint8_t buf[RLP_HEADER_SIZE_EXT_LEN];

  rlp.sender_cid = conn->local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_DISCONNECT_MSG_SIZE;

  res = send_broker_header(-1, conn, &rlp, buf, RLP_HEADER_SIZE_EXT_LEN, VECTOR_BROKER_DISCONNECT);
  if (res != LWPA_OK)
    return res;

  pack_16b(buf, data->disconnect_reason);
  res = lwpa_send(conn->sock, buf, 2, 0);
  if (res < 0)
    return res;

  lwpa_timer_reset(&conn->send_timer);
  return LWPA_OK;
}

lwpa_error_t send_null(RdmnetConnection *conn)
{
  int res;
  RootLayerPdu rlp;
  uint8_t buf[RLP_HEADER_SIZE_EXT_LEN];

  rlp.sender_cid = conn->local_cid;
  rlp.vector = VECTOR_ROOT_BROKER;
  rlp.datalen = BROKER_NULL_MSG_SIZE;

  res = send_broker_header(-1, conn, &rlp, buf, RLP_HEADER_SIZE_EXT_LEN, VECTOR_BROKER_NULL);
  if (res != LWPA_OK)
    return res;

  lwpa_timer_reset(&conn->send_timer);
  return LWPA_OK;
}
