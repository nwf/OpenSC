/*
 * reader-pcsc.c: Reader driver for PC/SC Lite
 *
 * Copyright (C) 2002  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "internal.h"
#include "log.h"
#include "opensc.h"
#include "ctbcs.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <winscard.h>

/* Default timeout value for SCardGetStatusChange
 * Needs to be increased for some broken PC/SC
 * Lite implementations.
 */
#ifndef SC_CUSTOM_STATUS_TIMEOUT
#define SC_STATUS_TIMEOUT 0
#else
#define SC_STATUS_TIMEOUT SC_CUSTOM_STATUS_TIMEOUT
#endif

#ifdef _WIN32
/* Some windows specific kludge */
#define SCARD_PROTOCOL_ANY (SCARD_PROTOCOL_T0)
#define SCARD_SCOPE_GLOBAL SCARD_SCOPE_USER

/* Error printing */
#define PCSC_ERROR(ctx, desc, rv) error(ctx, desc ": %lx\n", rv);

#else

#define PCSC_ERROR(ctx, desc, rv) error(ctx, desc ": %s\n", pcsc_stringify_error(rv));

#endif

#define GET_SLOT_PTR(s, i) (&(s)->slot[(i)])
#define GET_PRIV_DATA(r) ((struct pcsc_private_data *) (r)->drv_data)
#define GET_SLOT_DATA(r) ((struct pcsc_slot_data *) (r)->drv_data)

struct pcsc_global_private_data {
	SCARDCONTEXT pcsc_ctx;
	int apdu_fix;  /* flag to indicate whether to 'fix' some T=0 APDUs */
};

struct pcsc_private_data {
	SCARDCONTEXT pcsc_ctx;
	char *reader_name;
	struct pcsc_global_private_data *gpriv;
};

struct pcsc_slot_data {
	SCARDHANDLE pcsc_card;
};

static int pcsc_ret_to_error(long rv)
{
	switch (rv) {
	case SCARD_W_REMOVED_CARD:
		return SC_ERROR_CARD_REMOVED;
	case SCARD_W_RESET_CARD:
		return SC_ERROR_CARD_RESET;
	case SCARD_E_NOT_TRANSACTED:
		return SC_ERROR_TRANSMIT_FAILED;
	default:
		return SC_ERROR_UNKNOWN;
	}
}

static unsigned int pcsc_proto_to_opensc(DWORD proto)
{
	switch (proto) {
	case SCARD_PROTOCOL_T0:
		return SC_PROTO_T0;
	case SCARD_PROTOCOL_T1:
		return SC_PROTO_T1;
	case SCARD_PROTOCOL_RAW:
		return SC_PROTO_RAW;
	default:
		return 0;
	}
}

static DWORD opensc_proto_to_pcsc(unsigned int proto)
{
	switch (proto) {
	case SC_PROTO_T0:
		return SCARD_PROTOCOL_T0;
	case SC_PROTO_T1:
		return SCARD_PROTOCOL_T1;
	case SC_PROTO_RAW:
		return SCARD_PROTOCOL_RAW;
	default:
		return 0;
	}
}

static int pcsc_transmit(struct sc_reader *reader, struct sc_slot_info *slot,
			 const u8 *sendbuf, size_t sendsize,
			 u8 *recvbuf, size_t *recvsize,
			 int control)
{
	SCARD_IO_REQUEST sSendPci, sRecvPci;
	DWORD dwSendLength, dwRecvLength;
	LONG rv;
	SCARDHANDLE card;
	struct pcsc_slot_data *pslot = GET_SLOT_DATA(slot);
	struct pcsc_private_data *prv = GET_PRIV_DATA(reader);

	assert(pslot != NULL);
	card = pslot->pcsc_card;

	sSendPci.dwProtocol = opensc_proto_to_pcsc(slot->active_protocol);
	sSendPci.cbPciLength = sizeof(sSendPci);
	sRecvPci.dwProtocol = opensc_proto_to_pcsc(slot->active_protocol);
	sRecvPci.cbPciLength = sizeof(sRecvPci);
	
	if (prv->gpriv->apdu_fix && sendsize >= 6) {
		/* Check if the APDU in question is of Case 4 */
		const u8 *p = sendbuf;
		int lc;
		
		p += 4;
		lc = *p;
		if (lc == 0)
			lc = 256;
		if (sendsize == lc + 6) {
			/* Le is present, cut it out */
			debug(reader->ctx, "Cutting out Le byte from Case 4 APDU\n");
			sendsize--;
		}
	}
	
	dwSendLength = sendsize;
	dwRecvLength = *recvsize;

        if (dwRecvLength > 255)
		dwRecvLength = 255;

	if (!control) {
		rv = SCardTransmit(card, &sSendPci, sendbuf, dwSendLength,
				   &sRecvPci, recvbuf, &dwRecvLength);
	} else {
#ifndef _WIN32
		rv = SCardControl(card, sendbuf, dwSendLength,
				  recvbuf, &dwRecvLength);
#else
		rv = SCardControl(card, 0, sendbuf, dwSendLength,
				  recvbuf, dwRecvLength, &dwRecvLength);
#endif
	}

	if (rv != SCARD_S_SUCCESS) {
		switch (rv) {
		case SCARD_W_REMOVED_CARD:
			return SC_ERROR_CARD_REMOVED;
		case SCARD_W_RESET_CARD:
			return SC_ERROR_CARD_RESET;
		case SCARD_E_NOT_TRANSACTED:
#if 0
			FIXME
			if (sc_detect_card(card->ctx, card->reader) != 1)
				return SC_ERROR_CARD_REMOVED;
#endif
			return SC_ERROR_TRANSMIT_FAILED;
                default:
                        PCSC_ERROR(reader->ctx, "SCardTransmit failed", rv);
			return SC_ERROR_TRANSMIT_FAILED;
		}
	}
	if (dwRecvLength < 2)
		return SC_ERROR_UNKNOWN_DATA_RECEIVED;
	*recvsize = dwRecvLength;
	
	return 0;
}

static int refresh_slot_attributes(struct sc_reader *reader, struct sc_slot_info *slot)
{
	struct pcsc_private_data *priv = GET_PRIV_DATA(reader);
	LONG ret;
	SCARD_READERSTATE_A rgReaderStates[SC_MAX_READERS];

	rgReaderStates[0].szReader = priv->reader_name;
	rgReaderStates[0].dwCurrentState = SCARD_STATE_UNAWARE;
	rgReaderStates[0].dwEventState = SCARD_STATE_UNAWARE;
	ret = SCardGetStatusChange(priv->pcsc_ctx, SC_STATUS_TIMEOUT, rgReaderStates, 1);
	if (ret != 0) {
		PCSC_ERROR(reader->ctx, "SCardGetStatusChange failed", ret);
		return pcsc_ret_to_error(ret);
	}
	if (rgReaderStates[0].dwEventState & SCARD_STATE_PRESENT) {
		slot->flags |= SC_SLOT_CARD_PRESENT;
		slot->atr_len = rgReaderStates[0].cbAtr;
		if (slot->atr_len > SC_MAX_ATR_SIZE)
			slot->atr_len = SC_MAX_ATR_SIZE;
		memcpy(slot->atr, rgReaderStates[0].rgbAtr, slot->atr_len);
	} else {
		slot->flags &= ~SC_SLOT_CARD_PRESENT;
	}

	return 0;
}

static int pcsc_detect_card_presence(struct sc_reader *reader, struct sc_slot_info *slot)
{
	int rv;

	if ((rv = refresh_slot_attributes(reader, slot)) < 0)
		return rv;
	return (slot->flags & SC_SLOT_CARD_PRESENT)? 1 : 0;
}

static int pcsc_connect(struct sc_reader *reader, struct sc_slot_info *slot)
{
	DWORD active_proto;
	SCARDHANDLE card_handle;
	LONG rv;
	struct pcsc_private_data *priv = GET_PRIV_DATA(reader);
	struct pcsc_slot_data *pslot = GET_SLOT_DATA(slot);
	int r;

	if (pslot != NULL)
		return SC_ERROR_SLOT_ALREADY_CONNECTED;
	r = refresh_slot_attributes(reader, slot);
	if (r)
		return r;
	if (!(slot->flags & SC_SLOT_CARD_PRESENT))
		return SC_ERROR_CARD_NOT_PRESENT;
	pslot = (struct pcsc_slot_data *) malloc(sizeof(struct pcsc_slot_data));
	if (pslot == NULL)
		return SC_ERROR_OUT_OF_MEMORY;
        rv = SCardConnect(priv->pcsc_ctx, priv->reader_name,
			  SCARD_SHARE_SHARED, SCARD_PROTOCOL_ANY,
                          &card_handle, &active_proto);
	if (rv != 0) {
		PCSC_ERROR(reader->ctx, "SCardConnect failed", rv);
		free(pslot);
		return pcsc_ret_to_error(rv);
	}
	slot->active_protocol = pcsc_proto_to_opensc(active_proto);
	slot->drv_data = pslot;
	pslot->pcsc_card = card_handle;

	return 0;
}

static int pcsc_disconnect(struct sc_reader *reader, struct sc_slot_info *slot,
			   int action)
{
	struct pcsc_slot_data *pslot = GET_SLOT_DATA(slot);

	/* FIXME: check action */
	SCardDisconnect(pslot->pcsc_card, SCARD_LEAVE_CARD);
	free(pslot);
	slot->drv_data = NULL;
	return 0;
}
                                          
static int pcsc_lock(struct sc_reader *reader, struct sc_slot_info *slot)
{
	long rv;
	struct pcsc_slot_data *pslot = GET_SLOT_DATA(slot);

	assert(pslot != NULL);
        rv = SCardBeginTransaction(pslot->pcsc_card);
        if (rv != SCARD_S_SUCCESS) {
		PCSC_ERROR(reader->ctx, "SCardBeginTransaction failed", rv);
                return pcsc_ret_to_error(rv);
        }
	return 0;
}

static int pcsc_unlock(struct sc_reader *reader, struct sc_slot_info *slot)
{
	long rv;
	struct pcsc_slot_data *pslot = GET_SLOT_DATA(slot);

	assert(pslot != NULL);
	rv = SCardEndTransaction(pslot->pcsc_card, SCARD_LEAVE_CARD);
	if (rv != SCARD_S_SUCCESS) {
		PCSC_ERROR(reader->ctx, "SCardEndTransaction failed", rv);
                return pcsc_ret_to_error(rv);
	}
	return 0;
}

static int pcsc_release(struct sc_reader *reader)
{
	struct pcsc_private_data *priv = GET_PRIV_DATA(reader);

	free(priv->reader_name);
	free(priv);
	return 0;
}

static struct sc_reader_operations pcsc_ops;

static const struct sc_reader_driver pcsc_drv = {
	"PC/SC Lite Resource Manager",
	"pcsc",
	&pcsc_ops
};

static int pcsc_init(struct sc_context *ctx, void **reader_data)
{
	LONG rv;
	DWORD reader_buf_size;
	char *reader_buf, *p;
	LPCSTR mszGroups = NULL;
	SCARDCONTEXT pcsc_ctx;
	int r, i, apdu_fix;
	struct pcsc_global_private_data *gpriv;
	scconf_block **blocks = NULL, *conf_block = NULL;

        rv = SCardEstablishContext(SCARD_SCOPE_GLOBAL,
                                   NULL,
                                   NULL,
				   &pcsc_ctx);
	if (rv != SCARD_S_SUCCESS)
		return pcsc_ret_to_error(rv);
	SCardListReaders(pcsc_ctx, NULL, NULL,
			 (LPDWORD) &reader_buf_size);
	if (reader_buf_size < 2) {
		SCardReleaseContext(pcsc_ctx);
		return 0;	/* No readers configured */
	}
	gpriv = (struct pcsc_global_private_data *) malloc(sizeof(struct pcsc_global_private_data));
	if (gpriv == NULL) {
		SCardReleaseContext(pcsc_ctx);
		return SC_ERROR_OUT_OF_MEMORY;
	}
	gpriv->pcsc_ctx = pcsc_ctx;
	gpriv->apdu_fix = 0;
	*reader_data = gpriv;
	
	reader_buf = (char *) malloc(sizeof(char) * reader_buf_size);
	SCardListReaders(pcsc_ctx, mszGroups, reader_buf,
			 (LPDWORD) &reader_buf_size);
	p = reader_buf;
	do {
		struct sc_reader *reader = (struct sc_reader *) malloc(sizeof(struct sc_reader));
		struct pcsc_private_data *priv = (struct pcsc_private_data *) malloc(sizeof(struct pcsc_private_data));
		struct sc_slot_info *slot;
		
		if (reader == NULL || priv == NULL) {
			if (reader)
				free(reader);
			if (priv)
				free(priv);
			break;
		}
		memset(reader, 0, sizeof(*reader));
		reader->drv_data = priv;
		reader->ops = &pcsc_ops;
		reader->driver = &pcsc_drv;
		reader->slot_count = 1;
		reader->name = strdup(p);
		priv->gpriv = gpriv;
		priv->pcsc_ctx = pcsc_ctx;
		priv->reader_name = strdup(p);
		r = _sc_add_reader(ctx, reader);
		if (r) {
			free(priv->reader_name);
			free(priv);
			free(reader->name);
			free(reader);
			break;
		}
		slot = &reader->slot[0];
		slot->id = 0;
		refresh_slot_attributes(reader, slot);
		slot->capabilities = 0;
		slot->atr_len = 0;
		slot->drv_data = NULL;

		while (*p++ != 0);
	} while (p < (reader_buf + reader_buf_size - 1));
	free(reader_buf);
	
	for (i = 0; ctx->conf_blocks[i] != NULL; i++) {
		blocks = scconf_find_blocks(ctx->conf, ctx->conf_blocks[i],
					    "reader_driver", "pcsc");
		conf_block = blocks[0];
		free(blocks);
		if (conf_block != NULL)
			break;
	}
	if (conf_block == NULL)
		return 0;
	apdu_fix = scconf_get_bool(conf_block, "apdu_fix", 0);
	if (apdu_fix)
		gpriv->apdu_fix = apdu_fix;
	
	return 0;
}

static int pcsc_finish(struct sc_context *ctx, void *prv_data)
{
	struct pcsc_global_private_data *priv = (struct pcsc_global_private_data *) prv_data;

	if (priv) {
		SCardReleaseContext(priv->pcsc_ctx);
		free(priv);
	}
	
	return 0;
}

const struct sc_reader_driver * sc_get_pcsc_driver()
{
	pcsc_ops.init = pcsc_init;
	pcsc_ops.finish = pcsc_finish;
	pcsc_ops.transmit = pcsc_transmit;
	pcsc_ops.detect_card_presence = pcsc_detect_card_presence;
	pcsc_ops.lock = pcsc_lock;
	pcsc_ops.unlock = pcsc_unlock;
	pcsc_ops.release = pcsc_release;
	pcsc_ops.connect = pcsc_connect;
	pcsc_ops.disconnect = pcsc_disconnect;
	pcsc_ops.enter_pin = ctbcs_pin_cmd;
	
	return &pcsc_drv;
}
